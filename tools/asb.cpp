// asb — Artemis script binary (ASB) tool.
//
// ASB is the compiled Artemis script container (labels + tags). The engine's
// decoder (oa::runtime::decode_asb) turns it back into Artemis *text* syntax
// (**the same text the regular script parser eats**), so this tool is a thin,
// useful shell around it: inspect, count, extract, or dump to stdout.
//
// Usage:
//   asb info    <file.asb>          header/entry stats + tag histogram
//   asb labels  <file.asb>          label list
//   asb decode  <file.asb> [out.iet]   (default: stdout; "-" = stdout)
//   asb extract <dir> [outdir]      decode every *.asb under <dir> recursively
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/runtime/runtime_iet.h"

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace {

void print_usage() {
    std::fprintf(stderr,
        "usage: asb <info|labels|decode|extract> <file|dir> [args]\n"
        "  asb info    <file.asb>              entry/tag statistics\n"
        "  asb labels  <file.asb>              label list\n"
        "  asb decode  <file.asb> [out.iet|-]  Artemis text (default: stdout)\n"
        "  asb extract <dir> [outdir]          decode every *.asb under <dir>\n"
        "      outdir defaults to <dir>_iet (mirrors the input tree; files keep\n"
        "      their relative path with the extension replaced by .iet).\n");
}

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
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

bool write_file(const std::string& path, const std::string& text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t put = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return put == text.size();
}

void mkdir_p(const std::string& dir) {
    std::string cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/' || dir[i] == '\\') {
            if (!cur.empty() && cur != ".") {
#if defined(_WIN32)
                _mkdir(cur.c_str());
#else
                mkdir(cur.c_str(), 0777);
#endif
            }
            if (i < dir.size()) cur += dir[i];
        } else {
            cur += dir[i];
        }
    }
}

std::string lower_ext(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

std::string replace_ext(const std::string& path, const std::string& ext) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path + ext;
    return path.substr(0, dot) + ext;
}

/// Non-empty, trimmed line.
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

struct Stats {
    size_t lines = 0;
    size_t labels = 0;
    size_t tags = 0;
    std::map<std::string, size_t> tag_hist;
    std::vector<std::string> label_names;
};

Stats collect(const std::string& text) {
    Stats st;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string line =
            trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (!line.empty()) {
            ++st.lines;
            if (line[0] == '*') {
                ++st.labels;
                st.label_names.push_back(line.substr(1));
            } else if (line[0] == '[') {
                ++st.tags;
                const size_t sp = line.find_first_of(" \t]");
                const std::string tag = line.substr(1, sp == std::string::npos ? std::string::npos
                                                                               : sp - 1);
                ++st.tag_hist[tag];
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return st;
}

std::string decode_or_die(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        std::exit(1);
    }
    return oa::runtime::decode_asb(bytes);
}

int cmd_info(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    if (bytes.size() < 9 || std::memcmp(bytes.data(), "ASB\0", 4) != 0) {
        std::fprintf(stderr, "not an ASB container (bad magic): %s\n", path.c_str());
        return 1;
    }
    const uint8_t flag = bytes[4];
    uint32_t entries = 0;
    std::memcpy(&entries, bytes.data() + 5, 4);
    const std::string text = oa::runtime::decode_asb(bytes);
    const Stats st = collect(text);
    std::printf("file:     %s (%zu bytes)\n", path.c_str(), bytes.size());
    std::printf("flag:     %u\n", unsigned(flag));
    std::printf("entries:  %u (container header)\n", entries);
    std::printf("decoded:  %zu lines (%zu labels, %zu tags, %zu bytes)\n", st.lines, st.labels,
                st.tags, text.size());
    std::vector<std::pair<std::string, size_t>> hist(st.tag_hist.begin(), st.tag_hist.end());
    std::sort(hist.begin(), hist.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::printf("tags (%zu distinct):\n", hist.size());
    for (const auto& [tag, n] : hist) std::printf("  %6zu  [%s]\n", n, tag.c_str());
    return 0;
}

int cmd_labels(const std::string& path) {
    const Stats st = collect(decode_or_die(path));
    for (const std::string& l : st.label_names) std::printf("%s\n", l.c_str());
    std::fprintf(stderr, "%zu labels in %s\n", st.labels, path.c_str());
    return 0;
}

int cmd_decode(const std::string& path, const std::string* out) {
    const std::string text = decode_or_die(path);
    if (out && *out != "-") {
        const size_t slash = out->find_last_of("/\\");
        if (slash != std::string::npos) mkdir_p(out->substr(0, slash));
        if (!write_file(*out, text)) {
            std::fprintf(stderr, "cannot write: %s\n", out->c_str());
            return 1;
        }
        std::fprintf(stderr, "%s -> %s (%zu bytes)\n", path.c_str(), out->c_str(), text.size());
        return 0;
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
}

int cmd_extract(const std::string& dir, const std::string& outdir) {
    struct Item {
        std::string rel;
    };
    std::vector<Item> items;

#if defined(_WIN32)
    std::vector<std::pair<std::string, std::string>> todo{{dir, ""}};
    while (!todo.empty()) {
        const auto [abs, rel] = todo.back();
        todo.pop_back();
        WIN32_FIND_DATAA fd;
        const std::string pat = abs + "\\*";
        HANDLE h = FindFirstFileA(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            const std::string child_abs = abs + "\\" + name;
            const std::string child_rel = rel.empty() ? name : rel + "\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                todo.emplace_back(child_abs, child_rel);
            } else if (lower_ext(name) == ".asb") {
                items.push_back({child_rel});
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    std::vector<std::pair<std::string, std::string>> todo{{dir, ""}};
    while (!todo.empty()) {
        const auto [abs, rel] = todo.back();
        todo.pop_back();
        DIR* d = opendir(abs.c_str());
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            const std::string child_abs = abs + "/" + name;
            const std::string child_rel = rel.empty() ? name : rel + "/" + name;
            struct stat sb{};
            if (stat(child_abs.c_str(), &sb) != 0) continue;
            if (S_ISDIR(sb.st_mode)) todo.emplace_back(child_abs, child_rel);
            else if (lower_ext(name) == ".asb") items.push_back({child_rel});
        }
        closedir(d);
    }
#endif
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.rel < b.rel;
    });
    size_t ok = 0, failed = 0;
    for (const Item& it : items) {
        const std::string src = dir + "/" + it.rel;
        const std::string dst = outdir + "/" + replace_ext(it.rel, ".iet");
        const size_t slash = dst.find_last_of("/\\");
        if (slash != std::string::npos) mkdir_p(dst.substr(0, slash));
        try {
            const std::string text = decode_or_die(src);
            if (write_file(dst, text)) {
                ++ok;
                std::fprintf(stderr, "%-48s -> %s\n", it.rel.c_str(), dst.c_str());
            } else {
                ++failed;
                std::fprintf(stderr, "%-48s write failed\n", it.rel.c_str());
            }
        } catch (const std::exception& ex) {
            ++failed;
            std::fprintf(stderr, "%-48s %s\n", it.rel.c_str(), ex.what());
        }
    }
    std::fprintf(stderr, "%zu decoded, %zu failed (%zu .asb found in %s)\n", ok, failed,
                 items.size(), dir.c_str());
    return failed ? 1 : 0;
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
        if (cmd == "labels") return cmd_labels(target);
        if (cmd == "decode") {
            if (argc > 3) {
                const std::string out = argv[3];
                return cmd_decode(target, &out);
            }
            return cmd_decode(target, nullptr);
        }
        if (cmd == "extract") {
            const std::string outdir = argc > 3 ? argv[3] : target + "_iet";
            return cmd_extract(target, outdir);
        }
        print_usage();
        return 2;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "asb: %s\n", ex.what());
        return 1;
    }
}
