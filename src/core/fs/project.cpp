#include "core/fs/project.h"

#include <cctype>
#include <stdexcept>

#include "core/util/charset.h"

// ---------------------------------------------------------------------------
// INI parser (folded from the standalone config/ini unit; see project.h for
// the documented parser semantics).
// ---------------------------------------------------------------------------

namespace oa::fs {

namespace {

std::string trim(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(uint8_t(s[b]))) ++b;
    while (e > b && std::isspace(uint8_t(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

} // namespace

IniFile IniFile::parse(std::string_view text, std::string* error) {
    IniFile out;
    Section* cur = nullptr;
    size_t line_no = 0;
    auto fail = [&](const std::string& msg) {
        if (error) *error = "ini line " + std::to_string(line_no) + ": " + msg;
        out = IniFile{};
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        ++line_no;
        const size_t nl = text.find('\n', pos);
        std::string_view line =
            text.substr(pos, nl == std::string_view::npos ? text.size() - pos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;
        if (trimmed[0] == '[') {
            const size_t close = trimmed.find(']');
            if (close == std::string::npos) {
                fail("unterminated section header");
                return out;
            }
            Section s;
            s.name = uppercase(trim(trimmed.substr(1, close - 1)));
            out.sections.push_back(std::move(s));
            cur = &out.sections.back();
            continue;
        }
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            // Parser ignores keyless lines rather than failing.
            continue;
        }
        const std::string key = uppercase(trim(trimmed.substr(0, eq)));
        if (key.empty()) continue;
        std::string value = trim(trimmed.substr(eq + 1));
        // strip inline comment when a ';' starts a comment outside quotes
        {
            bool in_quote = false;
            for (size_t i = 0; i < value.size(); ++i) {
                const char c = value[i];
                if (c == '"') {
                    in_quote = !in_quote;
                } else if (c == ';' && !in_quote) {
                    value = trim(std::string_view(value).substr(0, i));
                    break;
                }
            }
        }
        // unquote
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (cur) {
            cur->kv[key] = value;
        } else {
            // Keys before any section: parser keeps them in a
            // nameless "root" section we model as "".
            Section root;
            root.name = "";
            root.kv[key] = value;
            out.sections.push_back(std::move(root));
            cur = &out.sections.back();
        }
    }
    return out;
}

std::string ini_get(const IniFile& ini, std::string_view section, std::string_view key,
                    const std::string& fallback) {
    const IniFile::Section* s = ini.section(section);
    if (!s) return fallback;
    const std::string* v = s->find(key);
    return v ? *v : fallback;
}

bool parse_bool(const std::string& value, bool fallback) {
    if (value.empty()) return fallback;
    const std::string lower = IniFile::uppercase(value);
    return lower == "1" || lower == "TRUE" || lower == "ON" || lower == "YES";
}

int parse_int(const std::string& value, int fallback) {
    if (value.empty()) return fallback;
    try {
        size_t used = 0;
        const int v = std::stoi(value, &used);
        return used == value.size() ? v : fallback;
    } catch (...) {
        return fallback;
    }
}

} // namespace oa::fs

// ---------------------------------------------------------------------------
// Project layer (system.ini -> ProjectConfig).
// ---------------------------------------------------------------------------

namespace oa::fs {

namespace {
std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return out;
}
} // namespace

Project Project::open(const fs::IFileSystem& fs, std::string_view platform) {
    const auto bytes = fs.read("system.ini");
    if (!bytes) {
        throw std::runtime_error("system.ini not found in mounted resources");
    }
    // Behavior: detect the CHARSET= line inside the target
    // platform section by ASCII scan, then decode the whole file with it.
    std::string section_name;
    {
        const std::string want = IniFile::uppercase(platform);
        section_name = want;
    }
    // ASCII scan for charset within the target section.
    std::string charset = "Shift_JIS";
    {
        const std::string text(bytes->begin(), bytes->end());
        const std::string mark = "[" + section_name + "]";
        const size_t sec = text.find(mark);
        if (sec != std::string::npos) {
            size_t next = text.find('[', sec + mark.size());
            const std::string_view body =
                std::string_view(text).substr(sec, next == std::string::npos ? text.size() - sec
                                                                             : next - sec);
            const size_t cs = body.find("CHARSET");
            if (cs != std::string_view::npos) {
                const size_t eq = body.find('=', cs);
                if (eq != std::string_view::npos) {
                    size_t b = eq + 1;
                    while (b < body.size() && (body[b] == ' ' || body[b] == '\t')) ++b;
                    size_t e = b;
                    while (e < body.size() && body[e] != '\r' && body[e] != '\n' &&
                           body[e] != ';') {
                        ++e;
                    }
                    std::string v(body.substr(b, e - b));
                    while (!v.empty() && v.back() == ' ') v.pop_back();
                    if (!v.empty()) charset = v;
                }
            }
        }
    }
    const std::string text = util::decode_to_utf8(
        std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()),
        charset);

    std::string err;
    IniFile ini = IniFile::parse(text, &err);
    if (!err.empty()) {
        throw std::runtime_error("system.ini parse error: " + err);
    }

    Project p;
    p.ini = std::move(ini);
    ProjectConfig& c = p.config;
    const std::string section = IniFile::uppercase(platform);
    const auto* sec = p.ini.section(section);
    if (!sec) {
        throw std::runtime_error("system.ini has no section [" + section + "] for platform " +
                                 std::string(platform));
    }
    c.platform = lower_ascii(platform);
    c.env = sec->kv;

    const auto need = [&](const char* key) -> std::string {
        const std::string* v = sec->find(key);
        if (!v) {
            throw std::runtime_error(std::string("system.ini [") + section + "] missing " +
                                     key);
        }
        return *v;
    };
    c.stage_width = parse_int(need("WIDTH"), 1280);
    c.stage_height = parse_int(need("HEIGHT"), 720);
    c.boot_script = need("BOOT");
    c.fps = parse_int(ini_get(p.ini, section, "FPS"), 60);
    const std::string cs = ini_get(p.ini, section, "CHARSET", "");
    if (!cs.empty()) c.charset = cs;
    c.savepath = ini_get(p.ini, section, "SAVEPATH");
    c.title = ini_get(p.ini, section, "TITLE");
    c.frameless = parse_bool(ini_get(p.ini, section, "FRAMELESS"));
    c.resizable = parse_bool(ini_get(p.ini, section, "RESIZABLE"));
    c.fixed_aspect_ratio =
        parse_bool(ini_get(p.ini, section, "FIXED_ASPECT_RATIO"));
    c.sidecut = parse_bool(ini_get(p.ini, section, "SIDECUT"));
    c.power_saving = parse_bool(ini_get(p.ini, section, "POWER_SAVING"));
    c.no_save = parse_bool(ini_get(p.ini, section, "NO_SAVE"));
    c.side_picture = ini_get(p.ini, section, "SIDE_PICTURE");
    c.prevent_multiple_process_set =
        sec->find("PREVENT_MULTIPLE_PROCESS") != nullptr;
    return p;
}

} // namespace oa::fs
