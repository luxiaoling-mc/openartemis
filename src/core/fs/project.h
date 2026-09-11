#pragma once
// Project layer: tolerant INI parsing + system.ini -> ProjectConfig.
//
// The generic INI reader (parser semantics below) was folded into this
// header when the config parser lost every consumer but Project::open; its
// INI types live directly in oa::fs (the folder's namespace).
//
// INI semantics:
//   - comments start with ';' or '#'
//   - section headers [NAME], section/key names ASCII-uppercased
//   - 'key = value' lines; inline comments after ';'
//   - blank lines ignored
//   - bool values: 1/true/on/yes (any case) are true.
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/fs/fs.h"

namespace oa::fs {

struct IniFile {
    struct Section {
        std::string name;                          // uppercased
        std::map<std::string, std::string> kv;     // uppercased keys, trimmed values
        const std::string* find(std::string_view key) const {
            auto it = kv.find(uppercase(key));
            return it == kv.end() ? nullptr : &it->second;
        }
    };

    std::vector<Section> sections;

    static std::string uppercase(std::string_view s) {
        std::string out(s);
        for (char& c : out) {
            if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        }
        return out;
    }

    /// Parse text content; returns false and appends a human message on
    /// malformed structure (unterminated section etc.).
    static IniFile parse(std::string_view text, std::string* error = nullptr);

    const Section* section(std::string_view name) const {
        const std::string want = uppercase(name);
        for (const auto& s : sections) {
            if (s.name == want) return &s;
        }
        return nullptr;
    }
};

/// Returns value of `key` inside `section` (both case-insensitive), or
/// `fallback` when absent.
std::string ini_get(const IniFile& ini, std::string_view section, std::string_view key,
                    const std::string& fallback = std::string());

/// Parse an ini boolean (1/true/on/yes true; everything else false).
bool parse_bool(const std::string& value, bool fallback = false);

int parse_int(const std::string& value, int fallback = 0);

} // namespace oa::fs

namespace oa::fs {

struct ProjectConfig {
    int stage_width = 1280;
    int stage_height = 720;
    int fps = 60;
    std::string charset = "Shift_JIS";        // default
    std::string boot_script;                  // required, e.g. system/first.iet
    std::string savepath;                     // raw SAVEPATH (sanitized later)
    std::string title;
    bool frameless = false;
    bool resizable = false;
    bool fixed_aspect_ratio = false;
    bool sidecut = false;
    bool power_saving = false;
    bool no_save = false;
    bool prevent_multiple_process_set = false;
    std::string side_picture;
    std::string platform;                     // lowercase, e.g. "windows"
    // Every key/value of the selected platform section (as parsed).
    std::map<std::string, std::string> env;
};

struct Project {
    IniFile ini;
    ProjectConfig config;

    /// Load `system.ini` from `fs` with host `platform` ("windows", ...).
    /// Throws std::runtime_error when system.ini is missing, when the
    /// platform section is missing, or on required-key absence.
    static Project open(const fs::IFileSystem& fs, std::string_view platform);
};

} // namespace oa::fs
