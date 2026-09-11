// oasave — save-file inspector (oa::runtime formats, research/64).
//
// Three document kinds live under a save root:
//   * numbered saves  ("save/NN.dat" or a slot file): SaveData — local
//     variables + script position + optional scene snapshot + audio state.
//   * saveg.dat       : the g. (global) domain map (keys stripped of "g.")
//   * system.dat      : the s. (system) domain map
// The tool auto-detects which one it is (SaveData first, then domain map) so
// a file can just be pointed at.
//
// Usage:
//   oasave info  <file>                 document kind + counts
//   oasave vars  <file> [substr]        variables (key = value)
//   oasave scene <file>                 root props + layer snapshot rows
//   oasave audio <file>                 audio snapshot
//   oasave dir   <savedir>              survey every *.dat in a save directory
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/runtime/runtime_save.h"
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
        "usage: oasave <info|vars|scene|audio|dir> <file|savedir> [args]\n"
        "  oasave info  <file>            document kind + counts\n"
        "  oasave vars  <file> [substr]   variables (key = value)\n"
        "  oasave scene <file>            root props + layer snapshot rows\n"
        "  oasave audio <file>            audio snapshot\n"
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

/// Auto-detected document: numbered SaveData or a flat domain map.
struct Doc {
    bool is_save_data = false;
    oa::runtime::SaveData save;
    std::map<std::string, oa::runtime::Value> domain;
    std::string kind; // "SaveData" / "domain-map"
};

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

int cmd_vars(const std::string& path, const std::string& filter) {
    Doc doc;
    std::string err;
    if (!load_doc(path, &doc, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return 1;
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
        Doc doc;
        std::string err;
        if (!load_doc(path, &doc, &err)) {
            std::printf("%-28s %-34s %s\n", name.c_str(), "(unrecognized)", err.c_str());
            continue;
        }
        std::string detail;
        if (doc.is_save_data) {
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
        if (cmd == "dir") return cmd_dir(target);
        print_usage();
        return 2;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "oasave: %s\n", ex.what());
        return 1;
    }
}
