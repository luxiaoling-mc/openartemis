// Unit tests for util/sha1 + the PhysicsFS-backed PFS reader (physfs_fs):
// pf6/pf8 (XOR), standalone sibling volume chains, raw byte-split volumes
// and the read-write mount category. Plain asserts; return 0 on success.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime_save.h"
#include "core/util/sha1.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::string hex(const oa::util::Sha1Digest& d) {
    char out[41];
    for (int i = 0; i < 20; ++i) std::snprintf(out + i * 2, 3, "%02x", d[i]);
    return out;
}

namespace sfs = std::filesystem;

struct TmpDir {
    std::string path;
    TmpDir(const char* tag) {
        sfs::path base = sfs::temp_directory_path();
        for (int i = 0;; ++i) {
            const std::string cand =
                (base / (std::string("oa_fs_") + tag + "_" + std::to_string(i))).string();
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
    std::string file(const std::string& rel) const { return path + "/" + rel; }
};

void put_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

void put_bytes(const std::string& path, std::string_view text) {
    std::ofstream f(path, std::ios::binary);
    f.write(text.data(), std::streamsize(text.size()));
}

void put_le32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 24));
}

void append_entry(std::vector<uint8_t>& out, const std::string& path, uint32_t offset,
                  uint32_t size) {
    put_le32(out, uint32_t(path.size()));
    out.insert(out.end(), path.begin(), path.end());
    put_le32(out, 0); // reserved
    put_le32(out, offset);
    put_le32(out, size);
}

// Build a self-contained pf6 archive over {name -> payload}.
std::vector<uint8_t> build_pf6(const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<uint8_t> out;
    out.push_back('p');
    out.push_back('f');
    out.push_back('6');
    put_le32(out, 0); // index_size placeholder
    put_le32(out, uint32_t(files.size()));
    std::vector<size_t> recs;
    for (const auto& [name, payload] : files) {
        (void)payload;
        recs.push_back(out.size());
        append_entry(out, name, 0, 0);
    }
    std::vector<size_t> starts;
    for (const auto& [name, payload] : files) {
        (void)name;
        starts.push_back(out.size());
        out.insert(out.end(), payload.begin(), payload.end());
    }
    const uint32_t isz = uint32_t(out.size() - 7);
    auto put32 = [&](size_t at, uint32_t v) {
        out[at] = uint8_t(v);
        out[at + 1] = uint8_t(v >> 8);
        out[at + 2] = uint8_t(v >> 16);
        out[at + 3] = uint8_t(v >> 24);
    };
    put32(3, isz);
    for (size_t i = 0; i < files.size(); ++i) {
        const size_t pl = files[i].first.size();
        put32(recs[i] + 4 + uint32_t(pl) + 4, uint32_t(starts[i]));
        put32(recs[i] + 4 + uint32_t(pl) + 8, uint32_t(files[i].second.size()));
    }
    return out;
}

void test_sha1_vectors() {
    check(hex(oa::util::sha1_of("", 0)) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1 empty");
    const char* abc = "abc";
    check(hex(oa::util::sha1_of(abc, 3)) == "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1 abc");
    const char* fox = "The quick brown fox jumps over the lazy dog";
    check(hex(oa::util::sha1_of(fox, std::strlen(fox))) ==
              "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12",
          "sha1 fox");
}

void test_normalize() {
    check(oa::fs::normalize_path("A\\B\\C.txt") == "a/b/c.txt", "normalize mixed");
    check(oa::fs::normalize_path("system/First.IET") == "system/first.iet",
          "normalize ascii");
}

void test_pf6_basic() {
    TmpDir dir("pf6");
    const std::string path = dir.file("t.pfs");
    std::vector<uint8_t> file;
    const char* payload_ini = "; fake system.ini\r\n";
    const char* payload_png = "\x89PNG fake image bytes";
    const char* name_ini = "system.ini";
    const char* name_img = "Image\\BG\\Test_01.PNG";
    const char* name_dir = "empty\\dir\\";
    file.push_back('p');
    file.push_back('f');
    file.push_back('6');
    put_le32(file, 0); // index_size placeholder
    put_le32(file, 3); // file_count
    const size_t rec0 = file.size();
    append_entry(file, name_ini, 0, 0);
    const size_t rec1 = file.size();
    append_entry(file, name_img, 0, 0);
    append_entry(file, name_dir, 0, 0);
    const size_t data0 = file.size();
    file.insert(file.end(), payload_ini, payload_ini + std::strlen(payload_ini));
    const size_t data1 = file.size();
    file.insert(file.end(), payload_png, payload_png + std::strlen(payload_png));
    const uint32_t isz = uint32_t(file.size() - 7);
    file[3] = uint8_t(isz);
    file[4] = uint8_t(isz >> 8);
    file[5] = uint8_t(isz >> 16);
    file[6] = uint8_t(isz >> 24);
    auto set = [&](size_t at, uint32_t v) {
        file[at] = uint8_t(v);
        file[at + 1] = uint8_t(v >> 8);
        file[at + 2] = uint8_t(v >> 16);
        file[at + 3] = uint8_t(v >> 24);
    };
    const size_t pl_ini = std::strlen(name_ini), pl_img = std::strlen(name_img);
    set(rec0 + 4 + pl_ini + 4, uint32_t(data0));
    set(rec0 + 4 + pl_ini + 8, uint32_t(std::strlen(payload_ini)));
    set(rec1 + 4 + pl_img + 4, uint32_t(data1));
    set(rec1 + 4 + pl_img + 8, uint32_t(std::strlen(payload_png)));
    put_bytes(path, file);

    const oa::fs::PfsInfo meta = oa::fs::scan_pfs_file(path);
    check(meta.version == '6', "pf6 version");
    check(meta.entries.size() == 3, "pf6 entry count");
    check(meta.index_size == isz, "pf6 index size");

    oa::fs::PhysFileSystem fs(path, false);
    check(std::string(fs.kind()) == "pfs", "kind() == pfs");

    auto ini = fs.read("SYSTEM.INI");
    check(ini.has_value() && ini->size() == std::strlen(payload_ini), "read case-folded");
    if (ini) {
        check(std::memcmp(ini->data(), payload_ini, ini->size()) == 0, "read content");
        auto tail = fs.read_range("system.ini", 3, 5);
        check(tail.has_value() && tail->size() == 5 &&
                  std::memcmp(tail->data(), payload_ini + 3, 5) == 0,
              "read_range chunked");
        auto past = fs.read_range("system.ini", 100, 8);
        check(past.has_value() && past->empty(), "read_range past end -> empty");
    }
    auto png = fs.read("image\\bg\\test_01.png");
    check(png.has_value() && png->size() == std::strlen(payload_png) &&
              std::memcmp(png->data(), payload_png, png->size()) == 0,
          "slash/backslash + case-insensitive payload");
    check(fs.exists("empty/dir"), "dir record exists");
    check(!fs.exists("nope"), "missing stays missing");
    check(!fs.read("empty/dir").has_value(), "dir record not openable");
    auto children = fs.list("");
    check(children.has_value(), "root listing");
    if (children) {
        // File children only: system.ini (Test_01.PNG nests under Image/,
        // the dir record is filtered).
        check(children->size() == 1 && (*children)[0] == "system.ini",
              "root list = files only");
    }
}

void test_pf8_encryption() {
    TmpDir dir("pf8");
    const std::string path = dir.file("t.pfs");
    std::vector<uint8_t> file;
    const char* pa = "alpha payload 0123456789";
    const char* pb = "bravo: the quick brown fox jumps over the lazy dog";
    const char* na = "a.dat";
    const char* nb = "b\\c.dat";
    file.push_back('p');
    file.push_back('f');
    file.push_back('8');
    put_le32(file, 0); // index_size placeholder
    put_le32(file, 2); // file_count
    const size_t rec0 = file.size();
    append_entry(file, na, 0, 0);
    const size_t rec1 = file.size();
    append_entry(file, nb, 0, 0);
    file.resize(file.size() + 32, 0xEE); // index region padding
    const uint32_t isz = uint32_t(file.size() - 7);
    file[3] = uint8_t(isz);
    file[4] = uint8_t(isz >> 8);
    file[5] = uint8_t(isz >> 16);
    file[6] = uint8_t(isz >> 24);
    const size_t data_a = file.size();
    const size_t data_b = data_a + std::strlen(pa);
    auto set = [&](size_t at, uint32_t v) {
        file[at] = uint8_t(v);
        file[at + 1] = uint8_t(v >> 8);
        file[at + 2] = uint8_t(v >> 16);
        file[at + 3] = uint8_t(v >> 24);
    };
    set(rec0 + 4 + std::strlen(na) + 4, uint32_t(data_a));
    set(rec0 + 4 + std::strlen(na) + 8, uint32_t(std::strlen(pa)));
    set(rec1 + 4 + std::strlen(nb) + 4, uint32_t(data_b));
    set(rec1 + 4 + std::strlen(nb) + 8, uint32_t(std::strlen(pb)));
    // Key over the final index region, then append encrypted payloads.
    const auto key = oa::util::sha1_of(file.data() + 7, isz);
    auto encrypt = [&](const char* plain, size_t n) {
        std::vector<uint8_t> out(plain, plain + n);
        for (size_t i = 0; i < n; ++i) out[i] ^= key[i % key.size()];
        return out;
    };
    auto ea = encrypt(pa, std::strlen(pa));
    file.insert(file.end(), ea.begin(), ea.end());
    auto eb = encrypt(pb, std::strlen(pb));
    file.insert(file.end(), eb.begin(), eb.end());
    put_bytes(path, file);

    const oa::fs::PfsInfo meta = oa::fs::scan_pfs_file(path);
    check(meta.version == '8', "pf8 version");
    oa::fs::PhysFileSystem fs(path, false);
    auto a = fs.read("a.dat");
    check(a.has_value() && a->size() == std::strlen(pa) &&
              std::memcmp(a->data(), pa, a->size()) == 0,
          "pf8 decrypt whole");
    if (a) {
        auto c = fs.read_range("a.dat", 6, 7); // unaligned XOR phase
        check(c.has_value() && c->size() == 7 &&
                  std::memcmp(c->data(), pa + 6, 7) == 0,
              "pf8 decrypt chunk unaligned");
        auto h = fs.read_range("a.dat", 0, 4);
        check(h.has_value() && h->size() == 4 &&
                  std::memcmp(h->data(), pa, 4) == 0,
              "pf8 decrypt chunk head");
    }
    auto b = fs.read("b/c.dat");
    check(b.has_value() && b->size() == std::strlen(pb) &&
              std::memcmp(b->data(), pb, b->size()) == 0,
          "pf8 decrypt second entry");
}

// Standalone sibling volumes: base + root.pfs.000 are each complete
// archives; the engine must resolve names across the chain (later wins).
void test_overlay_chain() {
    TmpDir dir("chain");
    const std::string base_path = dir.file("t.pfs");
    const std::string vol_path = base_path + ".000";
    put_bytes(base_path, build_pf6({
                              {"system.ini", "; fake system.ini\r\n"},
                              {"shared.dat", "base version"},
                              {"only-base.dat", "base only payload"},
                          }));
    put_bytes(vol_path, build_pf6({
                             {"shared.dat", "volume version"},
                             {"image/bg/black.png", "\x89PNG vol black"},
                             {"only-vol.dat", "volume only payload"},
                         }));

    oa::fs::PhysFileSystem fs(base_path, false);
    auto ini = fs.read("system.ini");
    check(ini.has_value() && ini->size() == 19, "chain fs: base file readable");
    auto sh = fs.read("shared.dat");
    check(sh.has_value() && std::string(sh->begin(), sh->end()) == "volume version",
          "chain fs: later pack wins");
    auto onlybase = fs.read("only-base.dat");
    check(onlybase.has_value() &&
              std::string(onlybase->begin(), onlybase->end()) == "base only payload",
          "chain fs: base-only file readable");
    auto black = fs.read("image/bg/black.png");
    check(black.has_value() && black->size() == 14, "chain fs: volume-only file readable");
    check(!fs.exists("nope/missing.png"), "chain fs: missing stays missing");
    auto children = fs.list("");
    check(children.has_value(), "chain fs: root listing");
    if (children) {
        check(children->size() == 4 &&
                  std::find(children->begin(), children->end(), "system.ini") !=
                      children->end() &&
                  std::find(children->begin(), children->end(), "shared.dat") !=
                      children->end() &&
                  std::find(children->begin(), children->end(), "only-vol.dat") !=
                      children->end(),
              "chain fs: listing merges base + overlay");
    }
    auto bg = fs.list("image/bg");
    check(bg.has_value() && bg->size() == 1 && (*bg)[0] == "black.png",
          "chain fs: overlay subdir listing");
}

// Volume parts with a numbering GAP (.001 missing): sibling discovery must
// not stop at the first absent part — every present standalone pack joins
// the chain in ascending numeric order (NUKITASHI1 ships .000/.001/.002 +
// .010/.011, NUKITASHI2 .000 + .020; the engine previously mounted only the
// contiguous prefix .000/.001/.002 and silently dropped the later parts).
void test_overlay_chain_numbering_gaps() {
    TmpDir dir("chaingap");
    const std::string base_path = dir.file("t.pfs");
    put_bytes(base_path, build_pf6({
                              {"system.ini", "; fake system.ini\r\n"},
                              {"shared.dat", "base version"},
                              {"only-base.dat", "base only payload"},
                          }));
    put_bytes(base_path + ".000", build_pf6({
                                      {"shared.dat", "vol 000 version"},
                                      {"only-vol000.dat", "vol 000 payload"},
                                  }));
    put_bytes(base_path + ".002", build_pf6({
                                      {"shared.dat", "vol 002 version"},
                                      {"only-vol002.dat", "vol 002 payload"},
                                  }));

    oa::fs::PhysFileSystem fs(base_path, false);
    auto sh = fs.read("shared.dat");
    check(sh.has_value() && std::string(sh->begin(), sh->end()) == "vol 002 version",
          "gap chain: highest present part wins (later pack)");
    auto v0 = fs.read("only-vol000.dat");
    check(v0.has_value() && std::string(v0->begin(), v0->end()) == "vol 000 payload",
          "gap chain: earlier part file readable across the gap");
    auto v2 = fs.read("only-vol002.dat");
    check(v2.has_value() && std::string(v2->begin(), v2->end()) == "vol 002 payload",
          "gap chain: part after the gap readable");
    auto onlybase = fs.read("only-base.dat");
    check(onlybase.has_value() &&
              std::string(onlybase->begin(), onlybase->end()) == "base only payload",
          "gap chain: base-only file readable");
    check(!fs.exists("nope/missing.png"), "gap chain: missing stays missing");
    auto children = fs.list("");
    check(children.has_value(), "gap chain: root listing");
    if (children) {
        check(children->size() == 5 &&
                  std::find(children->begin(), children->end(), "system.ini") !=
                      children->end() &&
                  std::find(children->begin(), children->end(), "shared.dat") !=
                      children->end() &&
                  std::find(children->begin(), children->end(), "only-vol000.dat") !=
                      children->end() &&
                  std::find(children->begin(), children->end(), "only-vol002.dat") !=
                      children->end(),
              "gap chain: listing merges base + every part");
    }
}

// Raw byte-split volumes: a sibling with no pf header stays a byte
// continuation; entries straddling the boundary must read through.
void test_raw_byte_split_volumes() {
    TmpDir dir("rawsplit");
    const std::string base_path = dir.file("t.pfs");
    const std::string vol_path = base_path + ".000";
    std::vector<uint8_t> file = build_pf6({{"head.dat", "0123456789"},
                                           {"tail.dat", "789abcdef"}});
    const uint32_t base_size = uint32_t(file.size()) - uint32_t(std::strlen("789abcdef"));
    auto put32 = [&](size_t at, uint32_t v) {
        file[at] = uint8_t(v);
        file[at + 1] = uint8_t(v >> 8);
        file[at + 2] = uint8_t(v >> 16);
        file[at + 3] = uint8_t(v >> 24);
    };
    const size_t rec_tail = 11 + (4 + 8 + 12);
    put32(rec_tail + 4 + 8 + 4, base_size - 3);
    put32(rec_tail + 4 + 8 + 8, uint32_t(std::strlen("789abcdef")));
    file.resize(base_size);
    put_bytes(base_path, file);
    {
        std::ofstream f(vol_path, std::ios::binary);
        f.write("abcdef", 6);
    }

    const oa::fs::PfsInfo meta = oa::fs::scan_pfs_file(base_path);
    check(meta.total_size == uint64_t(base_size) + 6,
          "raw split: volumes concatenated");
    oa::fs::PhysFileSystem fs(base_path, false);
    auto head = fs.read("head.dat");
    check(head.has_value() && head->size() == 10 &&
              std::memcmp(head->data(), "0123456789", 10) == 0,
          "raw split: in-base entry");
    auto tail = fs.read("tail.dat");
    check(tail.has_value() && tail->size() == 9 &&
              std::memcmp(tail->data(), "789abcdef", 9) == 0,
          "raw split: entry crossing the volume boundary");
}

// Dir-first sidecar: loose files in the archive's real parent directory
// shadow pack content; the directory-only source mode covers plain trees.
void test_dir_first_sidecar_and_dir_mode() {
    TmpDir dir("sidecar");
    const std::string pfs_path = dir.file("game.pfs");
    put_bytes(pfs_path, build_pf6({
                             {"system.ini", "; from pack\r\n"},
                             {"shared.dat", "pack version"},
                             {"only-pack.dat", "pack payload"},
                         }));
    // Loose files next to the archive.
    put_bytes(dir.file("shared.dat"), "loose override");
    put_bytes(dir.file("loose-only.dat"), "loose extra");

    oa::fs::PhysFileSystem fs(pfs_path); // sidecar_dir default on
    check(std::string(fs.kind()) == "pfs+dir", "kind() == pfs+dir");
    auto sh = fs.read("shared.dat");
    check(sh.has_value() && std::string(sh->begin(), sh->end()) == "loose override",
          "loose dir file wins over pack");
    auto lo = fs.read("loose-only.dat");
    check(lo.has_value() && std::string(lo->begin(), lo->end()) == "loose extra",
          "loose-only file visible");
    auto pack = fs.read("only-pack.dat");
    check(pack.has_value(), "pack-only file still visible");
    auto ini = fs.read("system.ini");
    check(ini.has_value(), "pack system.ini readable");
    auto children = fs.list("");
    check(children.has_value(), "sidecar root listing");
    if (children) {
        // dir files (shared.dat shadowed once, loose-only.dat, game.pfs
        // itself) + pack-only files (only-pack.dat, system.ini).
        check(children->size() == 5, "sidecar listing merges dir + pack once");
    }
}

// Directory-only source mode over its own tree (a real dir that is NOT
// simultaneously mounted as a pfs sidecar by a live instance).
void test_dir_mode_isolated() {
    TmpDir dir("dirmode");
    put_bytes(dir.file("shared.dat"), "loose content");
    put_bytes(dir.file("loose-only.dat"), "extra");
    put_bytes(dir.file("game.pfs"), "not a real pack; stays a plain file");

    oa::fs::PhysFileSystem dirfs(dir.path);
    check(std::string(dirfs.kind()) == "dir", "dir kind");
    auto sh = dirfs.read("SHARED.DAT");
    check(sh.has_value() && std::string(sh->begin(), sh->end()) == "loose content",
          "dir read case-insensitive");
    check(dirfs.exists("only-pack.dat") == false, "dir mode does not see the pack");
    auto names = dirfs.list("");
    check(names.has_value() && names->size() == 3, "dir root listing files only");
}

// Read-write category: DirSaveStore/WritableMount round trip.
void test_writable_mount() {
    TmpDir dir("rw");
    oa::runtime::DirSaveStore store(dir.path);
    check(store.persistent(), "store persistent");
    check(store.write("save/slot1.dat", {1, 2, 3, 4}), "write nested");
    const auto back = store.read("save/slot1.dat");
    check(back.has_value() && back->size() == 4 && (*back)[0] == 1 && (*back)[3] == 4,
          "read back");
    check(store.exists("save/slot1.dat"), "exists after write");
    const auto mt = store.modification_time("save/slot1.dat");
    check(mt.has_value(), "modification time present");
    check(!store.write("../escape", {1}), "traversal write rejected");
    check(!store.read("../escape").has_value(), "traversal read rejected");
    check(store.remove("save/slot1.dat"), "remove");
    check(!store.exists("save/slot1.dat"), "gone after remove");
    check(!store.remove("save/slot1.dat"), "second remove false");
}


// Save root == game source directory (desktop default): the read-only
// PhysFileSystem already mounted the real dir, so the store must ADOPT that
// mountpoint instead of silently failing its own duplicate mount.
void test_writable_mount_same_dir_as_game_source() {
    TmpDir dir("rwshare");
    put_bytes(dir.file("system.ini"), "[WINDOWS]\n");
    {
        oa::fs::PhysFileSystem assets(dir.path); // dir mount, registry user #1
        check(assets.read("system.ini").has_value(), "assets read before save");
        oa::runtime::DirSaveStore store(dir.path); // adopts the same mountpoint
        check(store.write("save/slot1.dat", {9, 8, 7}), "same-dir save write");
        const auto back = store.read("save/slot1.dat");
        check(back.has_value() && back->size() == 3 && (*back)[0] == 9 && (*back)[2] == 7,
              "same-dir save read back through the shared mount");
        check(store.exists("save/slot1.dat"), "same-dir save exists");
    }
    // Store and assets destroyed: a fresh store over the dir still works.
    {
        oa::runtime::DirSaveStore store(dir.path);
        check(store.read("save/slot1.dat").has_value(), "file persisted on disk");
        check(store.remove("save/slot1.dat"), "cleanup remove");
    }
}
} // namespace

int main() {
    try {
        test_sha1_vectors();
        test_normalize();
        test_pf6_basic();
        test_pf8_encryption();
        test_overlay_chain();
        test_overlay_chain_numbering_gaps();
        test_raw_byte_split_volumes();
        test_dir_first_sidecar_and_dir_mode();
        test_dir_mode_isolated();
        test_writable_mount();
        test_writable_mount_same_dir_as_game_source();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "EXC: %s\n", ex.what());
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "pfs_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("pfs_test: all ok\n");
    return 0;
}
