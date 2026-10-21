// oasave — save-file inspector (oa::runtime formats, research/64).
//
// Four document kinds live under a save root:
//   * numbered saves  ("save/NN.dat" or a slot file): SaveData — local
//     variables + script position + optional scene snapshot + audio state.
//   * saveg.dat       : the g. (global) domain map (keys stripped of "g.")
//   * system.dat      : the s. (system) domain map
//   * official BOW*   : the NATIVE engine's container (system.dat / saveg.dat /
//     a numbered slot written by the SDK exe). Different format family — see
//     docs/OFFICIAL_SAVES.md; `oasave bow` dumps its field directory.
// The tool auto-detects which one it is (official container, then SaveData,
// then domain map) so a file can just be pointed at.
//
// Usage:
//   oasave info  <file>                 document kind + counts
//   oasave vars  <file> [substr]        variables (key = value)
//   oasave scene <file>                 root props + layer snapshot rows
//   oasave audio <file>                 audio snapshot
//   oasave bow   <file>                 official container: fields + variables
//   oasave dir   <savedir>              survey every *.dat in a save directory
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/runtime/runtime_save.h"
#include "core/runtime/runtime_save_official.h"
#include "core/runtime/runtime_iet.h"
#include "core/util/binary_stream.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace {

void print_usage() {
    std::fprintf(stderr,
        "usage: oasave <info|vars|scene|audio|bow|dir> <file|savedir> [args]\n"
        "  oasave info  <file>            document kind + counts\n"
        "  oasave vars  <file> [substr]   variables (key = value)\n"
        "  oasave scene <file>            root props + layer snapshot rows\n"
        "  oasave audio <file>            audio snapshot\n"
        "  oasave bow   <file>            official BOW* container: field table + vars\n"
        "  oasave dir   <savedir>         survey every .dat/.sav in a directory\n");
}

bool read_file(const std::string& path, std::string* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    out->resize(size_t(n));
    const size_t got = n ? std::fread(out->data(), 1, size_t(n), f) : 0;
    std::fclose(f);
    out->resize(got);
    return true;
}

std::string short_value(const oa::runtime::Value& v, size_t limit = 60) {
    std::string s = v.to_display();
    if (s.size() > limit) s = s.substr(0, limit - 1) + "…";
    return s;
}

/// Auto-detected document: numbered SaveData, a flat domain map, or an official
/// (native engine) BOW* container.
struct Doc {
    bool is_save_data = false;
    bool is_official = false;
    oa::runtime::SaveData save;
    std::map<std::string, oa::runtime::Value> domain;
    oa::runtime::OfficialSave official;
    std::string kind; // "SaveData" / "domain-map" / "official BOW*"
};

const char* official_kind_name(oa::runtime::OfficialSaveKind k) {
    switch (k) {
        case oa::runtime::OfficialSaveKind::System: return "official BOWX (system.dat)";
        case oa::runtime::OfficialSaveKind::Global: return "official BOWG (saveg.dat)";
        case oa::runtime::OfficialSaveKind::Slot: return "official BOWS (numbered save)";
    }
    return "official BOW*";
}

/// 官方值可能是 Pluto 二进制块/CP932 文本：非可打印字节一律 \xNN，避免把
/// 终端搅乱（也可粘贴回脚本里）。
std::string escape_bytes(const std::string& s, size_t limit = 72) {
    std::string out;
    for (size_t i = 0; i < s.size() && out.size() < limit; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c < 0x7f && c != '\\') {
            out.push_back(char(c));
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        }
    }
    if (s.size() > limit) out += "…";
    return out;
}

/// Both document kinds are an 'OASB' map at the root (research/64), so a plain
/// "try SaveData, then domain map" auto-detect mis-reads a saveg/system map as
/// an empty numbered save. Peek the top-level keys instead: SaveData always
/// carries "current_script"/"current_line"/"local_variables".
bool looks_like_save_data(const std::string& bytes) {
    try {
        oa::util::Reader r(bytes);
        const size_t n = r.map_entries();
        for (size_t i = 0; i < n; ++i) {
            const std::string k = r.str();
            if (k == "current_script" || k == "current_line") return true;
            r.skip_value();
        }
    } catch (const std::exception&) {
        return false;
    }
    return false;
}

bool load_doc(const std::string& path, Doc* doc, std::string* err) {
    std::string bytes;
    if (!read_file(path, &bytes)) {
        *err = "cannot read " + path;
        return false;
    }
    // 官方容器必须先认：它的 magics（BOW*）和 OASB 完全不相交，先判省得
    // "SaveData decode failed" 这种误导性的报错。
    if (oa::runtime::is_official_save(bytes)) {
        try {
            doc->official = oa::runtime::decode_official_save(bytes);
            doc->is_official = true;
            doc->kind = official_kind_name(doc->official.kind);
            return true;
        } catch (const std::exception& ex) {
            *err = std::string("official BOW* decode failed: ") + ex.what();
            return false;
        }
    }
    const bool numbered = looks_like_save_data(bytes);
    try {
        if (numbered) {
            doc->save = oa::runtime::SaveData::decode(bytes);
            doc->is_save_data = true;
            doc->kind = "SaveData (numbered save)";
            return true;
        }
        doc->domain = oa::runtime::decode_domain_map(bytes);
        doc->kind = "domain map (saveg.dat / system.dat)";
        return true;
    } catch (const std::exception& ex) {
        // Wrong guess (or a corrupt file): report which shape failed.
        *err = std::string(numbered ? "SaveData decode failed: "
                                    : "domain-map decode failed: ") +
               ex.what();
        return false;
    }
}

int cmd_info(const std::string& path) {
    Doc doc;
    std::string err;
    if (!load_doc(path, &doc, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return 1;
    }
    std::printf("file: %s\n", path.c_str());
    std::printf("kind: %s\n", doc.kind.c_str());
    if (doc.is_official) {
        const oa::runtime::OfficialSave& o = doc.official;
        std::printf("version:        %u\n", o.version);
        std::printf("variables:      %zu\n", o.variables.size());
        std::printf("read-line sets: %zu (", o.read_lines.size());
        size_t lines = 0;
        for (const auto& [script, rows] : o.read_lines) {
            (void)script;
            lines += rows.size();
        }
        std::printf("%zu line(s))\n", lines);
        std::printf("objects:        %zu\n", o.field_ids.size());
        size_t shown = 0;
        for (const auto& [start, ids] : o.field_ids) {
            if (shown++ >= 12) {
                std::printf("  ... %zu more object(s)\n", o.field_ids.size() - 12);
                break;
            }
            std::printf("  object@%-8u fields:", start);
            for (uint32_t id : ids) std::printf(" %u", id);
            std::printf("\n");
        }
        return 0;
    }
    if (doc.is_save_data) {
        const oa::runtime::SaveData& s = doc.save;
        std::printf("version:        %d\n", s.version);
        std::printf("script:         %s:%zu\n", s.current_script.c_str(), s.current_line);
        std::printf("call_stack:     %zu\n", s.call_stack.size());
        std::printf("local vars:     %zu\n", s.local_variables.size());
        std::printf("scene:          %s", s.has_scene ? "yes" : "no");
        if (s.has_scene)
            std::printf(" (root props %zu, layers %zu)", s.root_props.size(), s.layers.size());
        std::printf("\naudio:          %s", s.has_audio ? "yes" : "no");
        if (s.has_audio) {
            const oa::runtime::AudioSnap& a = s.audio;
            std::printf(" (bgm %s, se %zu, voice %zu)", a.bgm ? a.bgm->file.c_str() : "-",
                        a.se.size(), a.voice.size());
        }
        std::printf("\n");
    } else {
        std::printf("variables:      %zu\n", doc.domain.size());
    }
    return 0;
}

/// 官方容器专用视图：info（含 object/字段目录）+ 已读行集合展开；
/// BOWS 再展开"命令栈 → 快照"的导入结果（位置/图层/文本/BGM）。
/// BOWS 的图层命令栈字段就是靠 info 里那张字段目录定位的。
int cmd_bow(const std::string& path) {
    Doc doc;
    std::string err;
    if (!load_doc(path, &doc, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return 1;
    }
    if (!doc.is_official) {
        std::fprintf(stderr, "%s: not an official BOW* container\n", path.c_str());
        return 1;
    }
    (void)cmd_info(path);
    if (!doc.official.read_lines.empty()) {
        std::printf("read-line sets:\n");
        for (const auto& [script, rows] : doc.official.read_lines) {
            std::printf("  %-36s %zu line(s)", script.c_str(), rows.size());
            if (!rows.empty()) {
                const auto [lo, hi] = std::minmax_element(rows.begin(), rows.end());
                std::printf(" (%u..%u)", *lo, *hi);
            }
            std::printf("\n");
        }
    }
    if (doc.official.kind != oa::runtime::OfficialSaveKind::Slot) return 0;
    // BOWS：把命令栈翻成快照，逐项展示（这就是读档时用的映射）。
    const auto slot = oa::runtime::decode_official_slot(
        [&] {
            std::string bytes;
            (void)read_file(path, &bytes);
            return bytes;
        }());
    std::printf("slot position:  %s:%u%s\n", slot.frames.empty() ? "(none)" : slot.frames.front().script.c_str(),
                slot.frames.empty() ? 0u : slot.frames.front().line,
                slot.frames_truncated ? "  [call stack truncated]" : "");
    std::printf("frames:         %zu%s\n", slot.frames.size(),
                slot.frames_truncated ? "  [truncated: unknown frame shape]" : "");
    for (size_t i = 0; i < slot.frames.size(); ++i) {
        const auto& f = slot.frames[i];
        std::printf("  [%zu] %s:%u  return_line=%u  pending %zu", i, f.script.c_str(), f.line,
                    f.return_line, f.pending.size());
        if (f.has_current) std::printf("  at-tag=%s", f.current.name.c_str());
        std::printf("\n");
    }
    std::printf("system frame:   %s\n",
                slot.has_system_frame
                    ? (slot.system_frame.script + ":" + std::to_string(slot.system_frame.line))
                          .c_str()
                    : "(none)");
    std::printf("image layers:   %zu\n", slot.image_layers.size());
    size_t shown = 0;
    for (const auto& l : slot.image_layers) {
        if (shown++ >= 8) {
            std::printf("  ... %zu more\n", slot.image_layers.size() - 8);
            break;
        }
        std::printf("  %-28s %zu cmd(s)", l.id.c_str(), l.commands.size());
        for (const auto& c : l.commands) {
            if (c.name != "lyc" && c.name != "lyprop" && c.name != "lyevent") continue;
            std::printf(" %s{", c.name.c_str());
            size_t n = 0;
            for (const auto& [k, v] : c.attrs) {
                if (n++) std::printf(", ");
                std::printf("%s=%s", k.c_str(), v.c_str());
            }
            std::printf("}");
        }
        std::printf("\n");
    }
    std::printf("text layers:    %zu (active '%s')\n", slot.text_layers.size(),
                slot.current_message_layer.c_str());
    for (const auto& l : slot.text_layers) {
        std::string text;
        for (const auto& c : l.commands) {
            if (c.name != "print") continue;
            const auto it = c.attrs.find("data");
            if (it != c.attrs.end()) text += it->second;
        }
        const auto rect = slot.text_rects.find(l.id);
        std::printf("  %-28s %zu cmd(s) text=%s", l.id.c_str(), l.commands.size(),
                    escape_bytes(text, 60).c_str());
        if (rect != slot.text_rects.end()) {
            std::printf(" box=%.0f,%.0f %.0fx%.0f", double(rect->second.left),
                        double(rect->second.top), double(rect->second.width),
                        double(rect->second.height));
        }
        std::printf("\n");
    }
    if (!slot.bgm.empty()) {
        std::printf("bgm:            %s {", slot.bgm.front().name.c_str());
        size_t n = 0;
        for (const auto& [k, v] : slot.bgm.front().attrs) {
            if (n++) std::printf(", ");
            std::printf("%s=%s", k.c_str(), v.c_str());
        }
        std::printf("}\n");
    }
    return 0;
}

int cmd_vars(const std::string& path, const std::string& filter) {
    Doc doc;
    std::string err;
    if (!load_doc(path, &doc, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return 1;
    }
    if (doc.is_official) {
        // 官方值是原始字节（文本 / Pluto 块）：显示转义形式，并标出导入时会
        // 推断成数值的那些（int/float）。
        const auto& rows = doc.official.variables;
        size_t shown = 0;
        for (const auto& [k, v] : rows) {
            if (!filter.empty() && k.find(filter) == std::string::npos) continue;
            const auto typed = oa::runtime::infer_official_value(v);
            const char* tag = typed.kind == oa::runtime::ValueKind::Int     ? "int"
                              : typed.kind == oa::runtime::ValueKind::Float ? "float"
                                                                            : "str";
            std::printf("%-40s = %s  [%s]\n", k.c_str(), escape_bytes(v).c_str(), tag);
            ++shown;
        }
        std::fprintf(stderr, "%zu/%zu variables%s\n", shown, rows.size(),
                     filter.empty() ? "" : (" matching '" + filter + "'").c_str());
        return 0;
    }
    const std::map<std::string, oa::runtime::Value>* vars = nullptr;
    if (doc.is_save_data) vars = &doc.save.local_variables;
    else vars = &doc.domain;
    size_t shown = 0;
    for (const auto& [k, v] : *vars) {
        if (!filter.empty() && k.find(filter) == std::string::npos) continue;
        std::printf("%-40s = %s\n", k.c_str(), short_value(v).c_str());
        ++shown;
    }
    std::fprintf(stderr, "%zu/%zu variables%s\n", shown, vars->size(),
                 filter.empty() ? "" : (" matching '" + filter + "'").c_str());
    return 0;
}

int cmd_scene(const std::string& path) {
    Doc doc;
    std::string err;
    if (!load_doc(path, &doc, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return 1;
    }
    if (!doc.is_save_data || !doc.save.has_scene) {
        std::fprintf(stderr, "%s: no scene snapshot in this document\n", path.c_str());
        return 1;
    }
    const oa::runtime::SaveData& s = doc.save;
    std::printf("root props (%zu):\n", s.root_props.size());
    for (const auto& [k, v] : s.root_props) std::printf("  %s = %s\n", k.c_str(), v.c_str());
    std::printf("layers (%zu):\n", s.layers.size());
    for (const oa::runtime::LayerSnap& l : s.layers) {
        std::printf("  %-32s props=%zu handlers=%zu\n", l.id.c_str(), l.props.size(),
                    l.handlers.size());
        for (const auto& [k, v] : l.props) std::printf("      %s = %s\n", k.c_str(), v.c_str());
    }
    return 0;
}

int cmd_audio(const std::string& path) {
    Doc doc;
    std::string err;
    if (!load_doc(path, &doc, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return 1;
    }
    if (!doc.is_save_data || !doc.save.has_audio) {
        std::fprintf(stderr, "%s: no audio snapshot in this document\n", path.c_str());
        return 1;
    }
    const oa::runtime::AudioSnap& a = doc.save.audio;
    if (a.bgm)
        std::printf("bgm:   %s (loop=%d gain=%d)\n", a.bgm->file.c_str(), int(a.bgm->loop_play),
                    a.bgm->gain);
    else
        std::printf("bgm:   -\n");
    for (const oa::runtime::AudioChannelSnap& c : a.se)
        std::printf("se[%s]: %s (loop=%d gain=%d)\n", c.id.c_str(), c.file.c_str(),
                    int(c.loop_play), c.gain);
    for (const oa::runtime::AudioChannelSnap& c : a.voice)
        std::printf("voice[%s]: %s\n", c.id.c_str(), c.file.c_str());
    return 0;
}

int cmd_dir(const std::string& dir) {
    std::vector<std::string> files;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            files.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            struct stat sb{};
            if (stat((dir + "/" + e->d_name).c_str(), &sb) == 0 && S_ISREG(sb.st_mode))
                files.push_back(e->d_name);
        }
        closedir(d);
    }
#endif
    if (files.empty()) {
        std::fprintf(stderr, "no files in %s\n", dir.c_str());
        return 1;
    }
    std::sort(files.begin(), files.end());
    std::printf("%-28s %-34s %s\n", "file", "kind", "size/counts");
    for (const std::string& name : files) {
        const std::string path = dir + "/" + name;
        // 槽位缩略图与存档同目录（官方与我们都写普通 PNG）：不是存档文档，
        // 认出来直接标一行，别用 "unrecognized" 制造噪声。
        {
            std::string bytes;
            if (read_file(path, &bytes) && bytes.size() > 8 &&
                bytes.rfind("\x89PNG\r\n\x1a\n", 0) == 0) {
                std::printf("%-28s %-34s %zu bytes\n", name.c_str(), "PNG thumbnail",
                            bytes.size());
                continue;
            }
        }
        Doc doc;
        std::string err;
        if (!load_doc(path, &doc, &err)) {
            std::printf("%-28s %-34s %s\n", name.c_str(), "(unrecognized)", err.c_str());
            continue;
        }
        std::string detail;
        if (doc.is_official) {
            const oa::runtime::OfficialSave& o = doc.official;
            size_t lines = 0;
            for (const auto& [script, rows] : o.read_lines) {
                (void)script;
                lines += rows.size();
            }
            detail = "official v" + std::to_string(o.version) +
                     " vars=" + std::to_string(o.variables.size()) +
                     " scripts=" + std::to_string(o.read_lines.size()) +
                     " reads=" + std::to_string(lines);
        } else if (doc.is_save_data) {
            detail = "vars=" + std::to_string(doc.save.local_variables.size()) +
                     " scene=" + (doc.save.has_scene ? "y" : "n") +
                     " audio=" + (doc.save.has_audio ? "y" : "n") +
                     " at " + doc.save.current_script + ":" +
                     std::to_string(doc.save.current_line);
        } else {
            detail = "vars=" + std::to_string(doc.domain.size());
        }
        std::printf("%-28s %-34s %s\n", name.c_str(), doc.kind.c_str(), detail.c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return 2;
    }
    const std::string cmd = argv[1];
    const std::string target = argv[2];
    try {
        if (cmd == "info") return cmd_info(target);
        if (cmd == "vars") return cmd_vars(target, argc > 3 ? argv[3] : "");
        if (cmd == "scene") return cmd_scene(target);
        if (cmd == "audio") return cmd_audio(target);
        if (cmd == "bow") return cmd_bow(target);
        if (cmd == "dir") return cmd_dir(target);
        print_usage();
        return 2;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "oasave: %s\n", ex.what());
        return 1;
    }
}
