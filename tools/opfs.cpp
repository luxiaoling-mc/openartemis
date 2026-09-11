// opfs — command-line PFS survey tool (C++ twin of tools/pfs.py).
//
// Usage:
//   opfs info <archive>
//   opfs find <archive> [substr]
//   opfs get  <archive> <name> [out]      (out omitted: print text head / hexdump)
//   opfs dump <archive> <name> <out>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "core/fs/physfs_fs.h"

namespace {

void print_usage() {
    std::fprintf(stderr,
                 "usage: opfs <info|find|get|dump|extract> <archive> [args]\n"
                 "  opfs info <archive>\n"
                 "  opfs find <archive> [substr]\n"
                 "  opfs get <archive> <name> [out]\n"
                 "  opfs dump <archive> <name> <out>\n"
                 "  opfs extract <archive> <dir>\n"
                 "      Extract every entry of the archive into <dir> (created on\n"
                 "      demand; backslash separators become '/'), byte-exact. This\n"
                 "      is the folder-start companion: `openartemis <dir>` boots the\n"
                 "      extracted project directly (QA-queue U11).\n");
}

bool looks_like_text(const std::vector<uint8_t>& data, size_t limit) {
    limit = std::min(limit, data.size());
    if (limit == 0) return true;
    // UTF-8 decode attempt (no replacement char) => text.
    size_t i = 0;
    bool all_utf8 = true;
    while (i < limit) {
        const uint8_t c = data[i];
        int extra = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            all_utf8 = false;
            break;
        }
        if (i + extra >= limit) {
            all_utf8 = false;
            break;
        }
        for (int k = 1; k <= extra; ++k) {
            if ((data[i + k] & 0xC0) != 0x80) {
                all_utf8 = false;
                break;
            }
        }
        if (!all_utf8) break;
        i += extra + 1;
    }
    if (all_utf8) return true;
    // Fallback: ASCII printable dominance.
    size_t printable = 0;
    for (size_t k = 0; k < limit; ++k) {
        const uint8_t b = data[k];
        if (b == 9 || b == 10 || b == 13 || (b >= 32 && b < 127)) ++printable;
    }
    return limit > 0 && printable * 4 > limit * 3;
}

int cmd_info(const std::string& archive) {
    const oa::fs::PfsInfo pfs = oa::fs::scan_pfs_file(archive);
    std::printf("archive: %s\n", archive.c_str());
    std::printf("version: pf%c\n", pfs.version);
    std::printf("index_size: %u\n", pfs.index_size);
    std::printf("file_count: %zu\n", pfs.entries.size());
    std::printf("total_size: %llu\n", (unsigned long long)pfs.total_size);
    return 0;
}

int cmd_find(const std::string& archive, const std::string& substr) {
    const oa::fs::PfsInfo pfs = oa::fs::scan_pfs_file(archive);
    const std::string needle = oa::fs::normalize_path(substr);
    for (const auto& e : pfs.entries) {
        if (needle.empty() ||
            oa::fs::normalize_path(e.path).find(needle) != std::string::npos) {
            std::printf("%12u  %s\n", e.size, e.path.c_str());
        }
    }
    return 0;
}

int cmd_get(const std::string& archive, const std::string& name, const std::string* out,
            bool all) {
    // Size lookup for the hexdump addressing; content comes from the
    // mounted view (decrypted payloads).
    const oa::fs::PfsInfo pfs = oa::fs::scan_pfs_file(archive);
    const std::string want = oa::fs::normalize_path(name);
    uint32_t entry_offset = 0;
    for (const auto& e : pfs.entries) {
        if (e.path.empty()) continue;
        if (oa::fs::normalize_path(e.path) == want) {
            entry_offset = e.offset;
            break;
        }
    }
    oa::fs::PhysFileSystem fs(archive, false);
    const std::optional<std::vector<uint8_t>> data = fs.read(name);
    if (!data) {
        std::fprintf(stderr, "not found: %s\n", name.c_str());
        return 1;
    }
    if (out) {
        std::FILE* f = std::fopen(out->c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot open output: %s\n", out->c_str());
            return 1;
        }
        std::fwrite(data->data(), 1, data->size(), f);
        std::fclose(f);
        std::fprintf(stderr, "%s (%zu bytes) -> %s\n", name.c_str(), data->size(),
                     out->c_str());
        return 0;
    }
    const size_t limit = all ? data->size() : std::min<size_t>(20000, data->size());
    if (looks_like_text(*data, limit)) {
        std::fwrite(data->data(), 1, limit, stdout);
        std::fputc('\n', stdout);
        return 0;
    }
    std::printf("<binary %zu bytes; hexdump of first 512>\n", data->size());
    const size_t hex = std::min<size_t>(512, data->size());
    for (size_t j = 0; j < hex; j += 16) {
        std::printf("%08llx  ", (unsigned long long)(entry_offset + j));
        for (size_t k = j; k < j + 16; ++k) {
            if (k < hex) {
                std::printf("%02x ", (*data)[k]);
            } else {
                std::printf("   ");
            }
            if (k == j + 7) std::fputc(' ', stdout);
        }
        std::fputc(' ', stdout);
        for (size_t k = j; k < j + 16 && k < hex; ++k) {
            std::fputc((*data)[k] >= 32 && (*data)[k] < 127 ? char((*data)[k]) : '.',
                       stdout);
        }
        std::fputc('\n', stdout);
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
    const std::string archive = argv[2];
    try {
        if (cmd == "info") {
            return cmd_info(archive);
        }
        if (cmd == "find") {
            return cmd_find(archive, argc > 3 ? argv[3] : "");
        }
        if (cmd == "get") {
            if (argc < 4) {
                print_usage();
                return 2;
            }
            const std::string name = argv[3];
            std::string out;
            const std::string* out_ptr = argc > 4 ? &(out = argv[4]) : nullptr;
            return cmd_get(archive, name, out_ptr, false);
        }
        if (cmd == "extract") {
            if (argc < 4) {
                print_usage();
                return 2;
            }
            const std::string dir = argv[3];
            const size_t n = oa::fs::extract_pfs_archive(archive, dir);
            std::fprintf(stderr, "%zu files extracted -> %s\n", n, dir.c_str());
            return 0;
        }
        if (cmd == "dump") {
            if (argc < 5) {
                print_usage();
                return 2;
            }
            const std::string name = argv[3];
            const std::string out = argv[4];
            return cmd_get(archive, name, &out, true);
        }
        print_usage();
        return 2;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "opfs: %s\n", ex.what());
        return 1;
    }
}
