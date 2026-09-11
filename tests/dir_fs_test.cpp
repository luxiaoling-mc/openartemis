// Directory-source PhysFileSystem tests (real temp tree, no archive):
// reads, case-insensitive virtual paths, listing, escape rejection.
// Plain asserts; return 0 on success. Portable (std::filesystem tmp dirs).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/fs/physfs_fs.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

namespace sfs = std::filesystem;

struct TmpDir {
    std::string path;
    TmpDir() {
        sfs::path base = sfs::temp_directory_path();
        for (int i = 0;; ++i) {
            const std::string cand = (base / ("oa_dirfs_" + std::to_string(i))).string();
            std::error_code ec;
            if (sfs::create_directory(cand, ec) && !ec) {
                path = cand;
                break;
            }
        }
    }
    ~TmpDir() {
        std::error_code ec;
        sfs::remove_all(path, ec);
    }
    void write(const std::string& rel, const std::string& data) {
        const sfs::path full = sfs::path(path) / rel;
        std::error_code ec;
        sfs::create_directories(full.parent_path(), ec);
        std::FILE* f = std::fopen(full.string().c_str(), "wb");
        if (f) {
            std::fwrite(data.data(), 1, data.size(), f);
            std::fclose(f);
        }
    }
};

} // namespace

int main() {
    try {
        TmpDir root;
        root.write("system.ini", "[WINDOWS]\n");
        root.write("Image/BG/Test_01.PNG", "png-bytes");
        root.write("empty_dir_marker/.keep", "");
        root.write("caps.TXT", "caps content");

        oa::fs::PhysFileSystem fs(root.path);
        check(std::string(fs.kind()) == "dir", "kind() == dir");

        // Exact-case reads.
        auto ini = fs.read("system.ini");
        check(ini.has_value() && std::string(ini->begin(), ini->end()) == "[WINDOWS]\n",
              "exact-case read");
        // Case-insensitive reads incl. mixed separators.
        auto png = fs.read("IMAGE/bg/test_01.png");
        check(png.has_value() &&
                  std::string(png->begin(), png->end()) == "png-bytes",
              "case-insensitive nested read");
        check(fs.read("caps.txt").has_value(), "case-insensitive extension read");
        check(fs.exists("image/bg/Test_01.PNG"), "exists case-insensitive");
        check(fs.exists("image/bg"), "directory exists");
        check(!fs.exists("nope/missing.png"), "missing");
        // Escape / absolute handling: everything is relative to the mount.
        check(!fs.exists("../etc/passwd"), "escape '..' rejected");
        check(!fs.exists("/etc/passwd"), "absolute path treated as relative-miss");
        // Listing: direct FILE children only, original case preserved.
        auto root_list = fs.list("");
        check(root_list.has_value(), "root listing");
        if (root_list) {
            // system.ini + caps.TXT (Image/ and empty_dir_marker are dirs).
            check(root_list->size() == 2, "root has 2 files");
            bool has_caps = false;
            for (const auto& n : *root_list) {
                if (n == "caps.TXT") has_caps = true;
                if (n == "Image" || n == "empty_dir_marker") {
                    check(false, "directories not listed");
                }
            }
            check(has_caps, "original file case preserved in listing");
        }
        auto img = fs.list("image/bg");
        check(img.has_value() && img->size() == 1 && (*img)[0] == "Test_01.PNG",
              "subdir listing case-insensitive");
        // A second instance over the same real dir is content-identical.
        oa::fs::PhysFileSystem fs2(root.path);
        check(fs2.read("system.ini").has_value(), "second dir mount readable");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "EXC: %s\n", ex.what());
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "dir_fs_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("dir_fs_test: all ok\n");
    return 0;
}
