#include "core/fs/physfs_fs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <errno.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "physfs.h"
}

#include "core/util/sha1.h"

// ---------------------------------------------------------------------------
// PhysicsFS-backed project source (see physfs_fs.h for the model).
//
// The custom PFS archiver below serves pf2/pf6/pf8 packs (index layout and
// pf8 XOR payloads ported from the old pfs_archive.cpp reader). Volume sets
// are classified on the host side BEFORE mounting (same rules as the old
// PfsArchive ctor): standalone sibling packs are mounted as separate
// archives (later pack wins), raw byte splits become one concatenated
// container. Sibling discovery is presence-based (all existing <base>.NNN
// parts, numbering gaps allowed — see sibling_volumes). Every archive
// mounts through PHYSFS_mountIo under a unique per-instance name, and each
// PhysFileSystem instance reads only its own mountpoint subtree — many live
// instances (unit tests!) never contaminate each other.
// ---------------------------------------------------------------------------

namespace oa::fs {

namespace {

namespace sfs = std::filesystem;

// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------

constexpr size_t kPfsHeaderSize = 11; // 'pf' + version + index_size u32le + file_count u32le

uint32_t read_u32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

std::string ascii_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return out;
}

std::string physfs_error() {
    const char* s = PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode());
    return s ? s : "unknown";
}

// ---------------------------------------------------------------------------
// Path encoding (research/129): the engine's path strings are UTF-8 on every
// platform (research/74 §编码: "游戏侧 SJIS/CP932 → 内部 UTF-8；无 wchar/ANSI
// codepage"), and PFS index entry names are raw UTF-8 bytes. Two native APIs
// on Windows disagree with that contract, because they decode `char*` with the
// process ANSI code page (ACP, 936 on the dev box) instead of UTF-8:
//   * std::filesystem::path(std::string) throws "No mapping for the Unicode
//     character exists in the target multi-byte code page";
//   * std::fopen / UCRT narrow file APIs fail with errno=EILSEQ (42,
//     "Illegal byte sequence").
// `path(std::u8string)` is specified to decode char8_t input as UTF-8 (MSVC:
// MultiByteToWideChar(CP_UTF8)), independent of the ACP *and* of the C locale
// — which matters because __std_fs_code_page() follows setlocale(LC_ALL,
// ".UTF-8") at runtime (see research/129 §1 for the probe).
//
// POSIX: value_type is char and paths are opaque byte strings, so both helpers
// below are the identity in bytes — the same behaviour as the pre-fix
// std::filesystem/std::fopen calls.
// ---------------------------------------------------------------------------

/// Interpret UTF-8 path bytes as a native path (Windows: UTF-16).
sfs::path native_path_from_utf8(std::string_view utf8) {
    const std::u8string u8(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return sfs::path(u8);
}

/// Open `path` for binary writing: the wide API on Windows (the narrow one
/// decodes with the ACP), the byte API on POSIX.
std::FILE* fopen_write_binary(const sfs::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

// ---------------------------------------------------------------------------
// PFS index model + parsing (layout identical to old pfs_archive.cpp)
// ---------------------------------------------------------------------------

struct ParsedEntry {
    std::string path; // as stored
    uint32_t reserved = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool is_dir = false; // stored path ends with a separator (dir record)
};

struct ParsedArchive {
    char version = '6';
    uint32_t index_size = 0;
    std::vector<ParsedEntry> entries;
    std::vector<uint8_t> xor_key; // pf8 only (20 bytes)
};

template <typename Reader>
ParsedArchive parse_index(Reader& src) {
    uint8_t head[kPfsHeaderSize];
    src.read_at(0, head, kPfsHeaderSize);
    if (head[0] != 'p' || head[1] != 'f') {
        throw std::runtime_error("not a PFS archive (magic mismatch)");
    }
    ParsedArchive out;
    const char v = char(head[2]);
    if (v != '2' && v != '6' && v != '8') {
        throw std::runtime_error(std::string("unsupported PFS version byte: ") + v);
    }
    out.version = v;
    out.index_size = read_u32le(head + 3);
    const uint32_t file_count = read_u32le(head + 7);

    uint64_t pos = kPfsHeaderSize;
    out.entries.reserve(file_count);
    for (uint32_t i = 0; i < file_count; ++i) {
        uint8_t len_bytes[4];
        src.read_at(pos, len_bytes, 4);
        pos += 4;
        const uint32_t path_len = read_u32le(len_bytes);
        if (path_len > 16 * 1024 * 1024) {
            throw std::runtime_error("corrupt PFS index (path length " +
                                     std::to_string(path_len) + ")");
        }
        ParsedEntry e;
        e.path.assign(path_len, '\0');
        if (path_len > 0) {
            src.read_at(pos, e.path.data(), path_len);
            pos += path_len;
        }
        uint8_t rest[12];
        src.read_at(pos, rest, 12);
        pos += 12;
        e.reserved = read_u32le(rest);
        e.offset = read_u32le(rest + 4);
        e.size = read_u32le(rest + 8);
        if (!e.path.empty() && (e.path.back() == '\\' || e.path.back() == '/')) {
            e.is_dir = true;
        }
        out.entries.push_back(std::move(e));
    }
    if (out.version == '8') {
        // XOR key = SHA1(file bytes [7, 7 + index_size)).
        std::vector<uint8_t> index_data(out.index_size);
        src.read_at(kPfsHeaderSize - 4, index_data.data(), index_data.size());
        const oa::util::Sha1Digest d = oa::util::sha1_of(index_data.data(), index_data.size());
        out.xor_key.assign(d.begin(), d.end());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Multi-file container over real volume files (volume concatenation)
// ---------------------------------------------------------------------------

class FileSet {
public:
    explicit FileSet(std::vector<std::string> paths) {
        for (auto& p : paths) {
            auto f = std::make_unique<std::ifstream>(p, std::ios::binary);
            if (!*f) throw std::runtime_error("cannot open PFS volume: " + p);
            f->seekg(0, std::ios::end);
            const auto end = f->tellg();
            if (end <= 0) throw std::runtime_error("empty PFS volume: " + p);
            specs_.push_back(VolumeFile{p, uint64_t(end)});
            streams_.push_back(std::move(f));
        }
    }

    /// Independent handles to the same volume set (PHYSFS_Io duplication /
    /// per-open-file isolation). One FileSet must never serve two streams.
    FileSet(const FileSet& other) : specs_(other.specs_) {
        for (const auto& v : specs_) {
            auto f = std::make_unique<std::ifstream>(v.path, std::ios::binary);
            if (!*f) throw std::runtime_error("cannot open PFS volume: " + v.path);
            streams_.push_back(std::move(f));
        }
    }

    uint64_t total_size() const {
        uint64_t t = 0;
        for (const auto& v : specs_) t += v.size;
        return t;
    }

    void read_at(uint64_t pos, void* buf, size_t len) const {
        uint8_t* out = static_cast<uint8_t*>(buf);
        uint64_t global = pos;
        for (size_t i = 0; i < specs_.size(); ++i) {
            if (global >= specs_[i].size) {
                global -= specs_[i].size;
                continue;
            }
            const size_t avail = size_t(specs_[i].size - global);
            const size_t take = std::min(len, avail);
            streams_[i]->seekg(std::streamoff(global));
            streams_[i]->read(reinterpret_cast<char*>(out), std::streamsize(take));
            if (size_t(streams_[i]->gcount()) != take) {
                throw std::runtime_error("short read from PFS volume " + specs_[i].path);
            }
            out += take;
            len -= take;
            global = 0;
            if (len == 0) return;
        }
        if (len != 0) throw std::runtime_error("read past end of PFS archive");
    }

private:
    struct VolumeFile {
        std::string path;
        uint64_t size = 0;
    };
    std::vector<VolumeFile> specs_;
    std::vector<std::unique_ptr<std::ifstream>> streams_;
};

// ---------------------------------------------------------------------------
// PHYSFS_Io implementation
// ---------------------------------------------------------------------------

/// One PHYSFS_Io instance: its own volume handles + a window into the
/// container (`lo`..`lo+size`) + current position inside the window.
struct IoContext {
    IoContext(std::shared_ptr<FileSet> files, uint64_t lo, uint64_t size,
              const std::vector<uint8_t>* key)
        : files(std::move(files)), lo(lo), size(size), key(key) {}
    IoContext(const IoContext& o)
        : files(std::make_shared<FileSet>(*o.files)), lo(o.lo), size(o.size),
          key(o.key), pos(o.pos) {}

    std::shared_ptr<FileSet> files;
    uint64_t lo = 0;
    uint64_t size = 0;
    const std::vector<uint8_t>* key = nullptr; // pf8 payload key (archive-owned)
    uint64_t pos = 0;
};

PHYSFS_Io* make_container_io(IoContext* ctx);

PHYSFS_sint64 io_read(PHYSFS_Io* io, void* buf, PHYSFS_uint64 len) {
    auto* ctx = static_cast<IoContext*>(io->opaque);
    if (len == 0 || ctx->pos >= ctx->size) return 0;
    const size_t take = size_t(std::min<PHYSFS_uint64>(ctx->size - ctx->pos, len));
    try {
        ctx->files->read_at(ctx->lo + ctx->pos, buf, take);
    } catch (const std::exception&) {
        PHYSFS_setErrorCode(PHYSFS_ERR_IO);
        return -1;
    }
    if (ctx->key && !ctx->key->empty()) {
        // pf8 XOR: the key phase runs over the payload (window) offset.
        uint8_t* p = static_cast<uint8_t*>(buf);
        const auto& key = *ctx->key;
        const uint64_t key_off = ctx->pos % key.size();
        for (size_t i = 0; i < take; ++i) p[i] ^= key[(key_off + i) % key.size()];
    }
    ctx->pos += take;
    return PHYSFS_sint64(take);
}

PHYSFS_sint64 io_write(PHYSFS_Io*, const void*, PHYSFS_uint64) {
    PHYSFS_setErrorCode(PHYSFS_ERR_READ_ONLY);
    return -1;
}

int io_seek(PHYSFS_Io* io, PHYSFS_uint64 offset) {
    auto* ctx = static_cast<IoContext*>(io->opaque);
    if (offset > ctx->size) {
        PHYSFS_setErrorCode(PHYSFS_ERR_PAST_EOF);
        return 0;
    }
    ctx->pos = offset;
    return 1;
}

PHYSFS_sint64 io_tell(PHYSFS_Io* io) {
    return PHYSFS_sint64(static_cast<IoContext*>(io->opaque)->pos);
}

PHYSFS_sint64 io_length(PHYSFS_Io* io) {
    return PHYSFS_sint64(static_cast<IoContext*>(io->opaque)->size);
}

PHYSFS_Io* io_duplicate(PHYSFS_Io* io) {
    try {
        auto* dup = new IoContext(*static_cast<IoContext*>(io->opaque));
        return make_container_io(dup);
    } catch (const std::exception&) {
        PHYSFS_setErrorCode(PHYSFS_ERR_OUT_OF_MEMORY);
        return nullptr;
    }
}

int io_flush(PHYSFS_Io*) { return 1; }

void io_destroy(PHYSFS_Io* io) {
    delete static_cast<IoContext*>(io->opaque);
    delete io;
}

PHYSFS_Io* make_container_io(IoContext* ctx) {
    static PHYSFS_Io s_io = [] {
        PHYSFS_Io io;
        std::memset(&io, 0, sizeof(io));
        io.read = io_read;
        io.write = io_write;
        io.seek = io_seek;
        io.tell = io_tell;
        io.length = io_length;
        io.duplicate = io_duplicate;
        io.flush = io_flush;
        io.destroy = io_destroy;
        return io;
    }();
    auto* io = new PHYSFS_Io(s_io);
    io->opaque = ctx;
    return io;
}

// ---------------------------------------------------------------------------
// PFS archiver (registers under the "PFS" extension)
// ---------------------------------------------------------------------------

struct PfsArchiveBox {
    ParsedArchive data;
    std::shared_ptr<FileSet> files; // container handle set (for entry opens)
    std::map<std::string, size_t> by_name; // normalized path -> entry index
};

void* arch_open(PHYSFS_Io* io, const char* name, int, int* claimed) {
    (void)name;
    *claimed = 0;
    
    try {
        uint8_t head[2];
        if (io->seek(io, 0) == 0 || io->read(io, head, 2) != 2) {
            
            return nullptr;
        }
        if (head[0] != 'p' || head[1] != 'f') return nullptr; // not ours
        *claimed = 1;
        

        struct IoReader {
            PHYSFS_Io* io;
            void read_at(uint64_t pos, void* buf, size_t len) {
                if (io->seek(io, pos) == 0)
                    throw std::runtime_error("PFS container seek failed");
                const PHYSFS_sint64 got =
                    io->read(io, buf, PHYSFS_uint64(len));
                if (got < 0 || PHYSFS_uint64(got) != len)
                    throw std::runtime_error("short read from PFS container");
            }
        } reader{io};

        // Parse first (may throw); only then allocate the archive box.
        ParsedArchive parsed = parse_index(reader);
        auto* box = new PfsArchiveBox();
        box->data = std::move(parsed);
        box->files = static_cast<IoContext*>(io->opaque)->files;
        for (size_t i = 0; i < box->data.entries.size(); ++i) {
            const ParsedEntry& e = box->data.entries[i];
            if (e.path.empty()) continue;
            box->by_name.emplace(normalize_path(e.path), i); // first wins
        }
        return box;
    } catch (const std::exception&) {
        PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT);
        return nullptr;
    }
}

std::set<std::string> arch_children(const ParsedArchive& data,
                                    const std::string& dirname) {
    std::set<std::string> out;
    const std::string prefix = dirname.empty() ? std::string() : dirname + "/";
    for (const auto& e : data.entries) {
        if (e.path.empty()) continue;
        const std::string n = normalize_path(e.path);
        if (n.rfind(prefix, 0) != 0) continue;
        const std::string rest = n.substr(prefix.size());
        if (rest.empty()) continue; // the directory record itself
        const size_t slash = rest.find('/');
        out.insert(slash == std::string::npos ? rest : rest.substr(0, slash));
    }
    return out;
}

PHYSFS_EnumerateCallbackResult arch_enumerate(void* opaque, const char* dirname,
                                              PHYSFS_EnumerateCallback cb,
                                              const char*, void* data) {
    const auto* box = static_cast<const PfsArchiveBox*>(opaque);
    const auto kids = arch_children(box->data, normalize_path(dirname));
    for (const auto& name : kids) {
        const PHYSFS_EnumerateCallbackResult r = cb(data, dirname, name.c_str());
        if (r != PHYSFS_ENUM_OK) {
            if (r == PHYSFS_ENUM_ERROR) PHYSFS_setErrorCode(PHYSFS_ERR_APP_CALLBACK);
            return r;
        }
    }
    return PHYSFS_ENUM_OK;
}

const ParsedEntry* arch_find(const PfsArchiveBox& box, const std::string& want,
                             bool* is_implicit_dir) {
    *is_implicit_dir = false;
    const auto it = box.by_name.find(want);
    if (it != box.by_name.end()) return &box.data.entries[it->second];
    // Implicit directory: `want` is a proper prefix of some entry path.
    const std::string prefix = want + "/";
    for (const auto& e : box.data.entries) {
        const std::string n = normalize_path(e.path);
        if (n.rfind(prefix, 0) == 0) {
            *is_implicit_dir = true;
            return nullptr;
        }
    }
    return nullptr;
}

PHYSFS_Io* arch_open_read(void* opaque, const char* fnm) {
    auto* box = static_cast<PfsArchiveBox*>(opaque);
    
    bool implicit = false;
    const ParsedEntry* e = arch_find(*box, normalize_path(fnm), &implicit);
    if (!e) {
        PHYSFS_setErrorCode(implicit ? PHYSFS_ERR_NOT_A_FILE : PHYSFS_ERR_NOT_FOUND);
        return nullptr;
    }
    if (e->is_dir || e->offset == 0) {
        // Dir records / marker entries are not openable (mirrors the old
        // reader, which skipped offset==0 entries so overlay chains resolve
        // past them).
        PHYSFS_setErrorCode(e->is_dir ? PHYSFS_ERR_NOT_A_FILE : PHYSFS_ERR_NOT_FOUND);
        return nullptr;
    }
    try {
        // Per-open-file isolation: duplicate the volume handles.
        auto files = std::make_shared<FileSet>(*box->files);
        auto* ctx = new IoContext(std::move(files), e->offset, e->size,
                                  box->data.xor_key.empty() ? nullptr
                                                            : &box->data.xor_key);
        return make_container_io(ctx);
    } catch (const std::exception&) {
        PHYSFS_setErrorCode(PHYSFS_ERR_IO);
        return nullptr;
    }
}

int arch_stat(void* opaque, const char* fnm, PHYSFS_Stat* stat) {
    auto* box = static_cast<PfsArchiveBox*>(opaque);
    std::memset(stat, 0, sizeof(*stat));
    stat->readonly = 1;
    stat->modtime = stat->createtime = stat->accesstime = -1;
    const std::string want = normalize_path(fnm);
    if (want.empty()) { // the archive root itself
        stat->filetype = PHYSFS_FILETYPE_DIRECTORY;
        return 1;
    }
    bool implicit = false;
    const ParsedEntry* e = arch_find(*box, want, &implicit);
    if (implicit || (e && e->is_dir)) {
        stat->filetype = PHYSFS_FILETYPE_DIRECTORY;
        return 1;
    }
    if (!e) {
        PHYSFS_setErrorCode(PHYSFS_ERR_NOT_FOUND);
        return 0;
    }
    stat->filetype = PHYSFS_FILETYPE_REGULAR;
    stat->filesize = e->size;
    return 1;
}

void arch_close(void* opaque) { delete static_cast<PfsArchiveBox*>(opaque); }

const PHYSFS_Archiver g_pfs_archiver = [] {
    PHYSFS_Archiver a;
    std::memset(&a, 0, sizeof(a));
    a.version = 0;
    a.info.extension = "PFS";
    a.info.description = "Artemis PFS pack (pf2/pf6/pf8)";
    a.info.author = "openartemis";
    a.info.url = "https://github.com/openartemis";
    a.info.supportsSymlinks = 0;
    a.openArchive = arch_open;
    a.enumerate = arch_enumerate;
    a.openRead = arch_open_read;
    a.openWrite = [](void*, const char*) -> PHYSFS_Io* {
        PHYSFS_setErrorCode(PHYSFS_ERR_READ_ONLY);
        return nullptr;
    };
    a.openAppend = [](void*, const char*) -> PHYSFS_Io* {
        PHYSFS_setErrorCode(PHYSFS_ERR_READ_ONLY);
        return nullptr;
    };
    a.remove = [](void*, const char*) -> int {
        PHYSFS_setErrorCode(PHYSFS_ERR_READ_ONLY);
        return 0;
    };
    a.mkdir = [](void*, const char*) -> int {
        PHYSFS_setErrorCode(PHYSFS_ERR_READ_ONLY);
        return 0;
    };
    a.stat = arch_stat;
    a.closeArchive = arch_close;
    return a;
}();

// ---------------------------------------------------------------------------
// Process-wide PhysicsFS state
// ---------------------------------------------------------------------------

int g_phys_refs = 0;
int g_seq = 0; // mountpoint + mount-name sequence

struct SharedDir {
    std::string mp;
    int users = 0;
};
std::map<std::string, SharedDir> g_shared_dirs; // real dir path -> mount info

void physfs_acquire() {
    if (g_phys_refs++ == 0) {
        // PHYSFS_init needs a base directory: its POSIX layer probes
        // /proc/self/exe first and, with no /proc (Emscripten) or an
        // argv0 == NULL, `calculateBaseDir` bails with ARGV0_IS_NULL —
        // PHYSFS_init then returns 0 *without* an error code and nothing can
        // be mounted (wasm: "PHYSFS_init failed: no error").
        // Passing an argv0 that contains a dirsep makes PhysicsFS derive the
        // directory part itself ("/openartemis" -> "/"); only PHYSFS_getBaseDir
        // reads it and the engine never does. Desktop keeps the historical
        // NULL + /proc probe.
#if defined(__EMSCRIPTEN__)
        static const char* const kArgv0Hint = "/openartemis";
#else
        static const char* const kArgv0Hint = nullptr;
#endif
        if (!PHYSFS_init(kArgv0Hint)) {
            g_phys_refs = 0;
            throw std::runtime_error(std::string("PHYSFS_init failed: ") + physfs_error());
        }
        // The old directory layer followed symlinks inside the tree; keep
        // that reachable (PhysicsFS still contains them below the mount).
        PHYSFS_permitSymbolicLinks(1);
        if (!PHYSFS_registerArchiver(&g_pfs_archiver)) {
            throw std::runtime_error(std::string("PHYSFS_registerArchiver failed: ") +
                                     physfs_error());
        }
    }
}

void physfs_release() {
    if (g_phys_refs > 0 && --g_phys_refs == 0) {
        g_shared_dirs.clear();
        PHYSFS_deinit();
    }
}

std::string fresh_name(const char* suffix) {
    return std::string("__oa") + std::to_string(g_seq++) + suffix;
}

// ---------------------------------------------------------------------------
// Source planning: classify a PFS path into mountable container(s)
// ---------------------------------------------------------------------------

/// Discover sibling parts <base>.NNN (three digits) present next to the
/// archive. Presence-based: a missing middle number does NOT stop the scan,
/// because real Artemis volume sets ship with numbering gaps (NUKITASHI1:
/// .000/.001/.002 + .010/.011; NUKITASHI2: .000 + .020). Every present part
/// joins the set in ascending numeric order, so chain/merge semantics stay
/// "later part number wins / later bytes follow" exactly as for a
/// contiguous set.
std::vector<std::string> sibling_volumes(const std::string& base_path) {
    std::vector<std::string> out;
    const sfs::path base(base_path);
    const std::string stem = base.filename().string();
    const std::string dir =
        base.parent_path().empty() ? "." : base.parent_path().string();
    std::vector<uint32_t> nums;
    std::error_code ec;
    sfs::directory_iterator it(dir, ec);
    if (ec) return out;
    for (const auto& de : it) {
        const std::string fn = de.path().filename().string();
        if (fn.size() != stem.size() + 4) continue;
        if (fn.compare(0, stem.size(), stem) != 0 || fn[stem.size()] != '.') continue;
        const char* s = fn.c_str() + stem.size() + 1;
        if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9' || s[2] < '0' ||
            s[2] > '9') continue;
        nums.push_back(uint32_t((s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0')));
    }
    if (nums.empty()) return out;
    std::sort(nums.begin(), nums.end());
    for (const uint32_t n : nums) {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), ".%03u", n);
        const std::string cand = (sfs::path(dir) / (stem + suffix)).string();
        std::error_code fec;
        if (!sfs::is_regular_file(cand, fec) || fec) continue; // dirs aren't volumes
        out.push_back(cand);
    }
    return out;
}

ParsedArchive parse_container(const std::vector<std::string>& paths) {
    FileSet files(paths);
    return parse_index(files);
}

bool is_standalone_pack(const std::string& p) {
    try {
        const ParsedArchive a = parse_container({p});
        FileSet f({p});
        const uint64_t total = f.total_size();
        for (const auto& e : a.entries) {
            if (uint64_t(e.offset) + e.size > total) return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// Classify one PFS source: mount order is lowest priority first.
struct PlannedSource {
    std::vector<std::vector<std::string>> containers;
    ParsedArchive meta; // base (chain) or merged (raw split) index
};

PlannedSource plan_pfs_source(const std::string& path) {
    PlannedSource plan;
    const std::vector<std::string> siblings = sibling_volumes(path);
    if (siblings.empty()) {
        plan.containers.push_back({path});
        plan.meta = parse_container(plan.containers.back());
        return plan;
    }
    // Standalone chain? Every sibling must be a complete pack and the base
    // must hold its own whole index + payloads.
    bool chain = true;
    for (const auto& s : siblings) {
        if (!is_standalone_pack(s)) {
            chain = false;
            break;
        }
    }
    if (chain) {
        try {
            ParsedArchive base = parse_container({path});
            FileSet f({path});
            const uint64_t base_size = f.total_size();
            for (const auto& e : base.entries) {
                if (uint64_t(e.offset) + e.size > base_size) {
                    chain = false;
                    break;
                }
            }
            if (chain) {
                plan.containers.push_back({path});
                for (const auto& s : siblings) plan.containers.push_back({s});
                plan.meta = std::move(base);
                return plan;
            }
        } catch (const std::exception&) {
            chain = false;
        }
    }
    // Raw byte split: base + every sibling concatenated.
    std::vector<std::string> merged{path};
    merged.insert(merged.end(), siblings.begin(), siblings.end());
    plan.containers.push_back(std::move(merged));
    plan.meta = parse_container(plan.containers.back());
    return plan;
}

bool stat_full(const std::string& full, PHYSFS_Stat* st) {
    return PHYSFS_stat(full.c_str(), st) != 0;
}

} // namespace

// ---------------------------------------------------------------------------
// oa::fs public metadata / extraction helpers
// ---------------------------------------------------------------------------

std::string normalize_path(std::string_view path) {
    std::string out = ascii_lower(path);
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    // Directory entries "dir\\" and queries "dir" are equal.
    if (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

PfsInfo scan_pfs_file(const std::string& path) {
    const PlannedSource plan = plan_pfs_source(path);
    PfsInfo out;
    out.version = plan.meta.version;
    out.index_size = plan.meta.index_size;
    // Standalone chain reports the base container; raw splits merge volumes.
    FileSet f(plan.containers[0]);
    out.total_size = f.total_size();
    out.entries.reserve(plan.meta.entries.size());
    for (const auto& e : plan.meta.entries) {
        PfsEntryInfo info;
        info.path = e.path;
        info.reserved = e.reserved;
        info.offset = e.offset;
        info.size = e.size;
        out.entries.push_back(std::move(info));
    }
    return out;
}

size_t extract_pfs_archive(const std::string& archive_path, const std::string& dir,
                           const std::function<bool(const PfsEntryInfo&)>& filter) {
    // `dir` comes from the caller (argv / temp dir): a NATIVE path string, so
    // it keeps its existing decoding. Only the PFS entries below are UTF-8.
    auto mkdirs = [](const sfs::path& path) {
        sfs::path cur;
        for (const auto& part : path) {
            cur /= part;
            std::error_code ec;
            if (!sfs::exists(cur, ec)) sfs::create_directory(cur, ec);
        }
    };
    const sfs::path root(dir);
    mkdirs(root);
    const PfsInfo info = scan_pfs_file(archive_path);
    PhysFileSystem fs(archive_path, false);
    size_t written = 0;
    for (const auto& e : info.entries) {
        if (e.path.empty() || e.path.back() == '\\' || e.path.back() == '/') continue;
        if (filter && !filter(e)) continue;
        std::string rel;
        rel.reserve(e.path.size());
        for (const char c : e.path) rel.push_back(c == '\\' ? '/' : c);
        const std::optional<std::vector<uint8_t>> data = fs.read(rel);
        if (!data) throw std::runtime_error("extract: unreadable entry: " + rel);
        // Diagnostic label stays the UTF-8 bytes (never a lossy narrow render
        // of the native path, which itself could throw on Windows).
        const std::string out_label = dir + "/" + rel;
        sfs::path out;
        try {
            sfs::path entry = native_path_from_utf8(rel);
            // Index paths are relative; `root / entry` would DISCARD root for
            // an absolute entry (leading '/', UNC, "C:.."), while the pre-fix
            // string concat always kept the entry below `dir`. Keep that
            // containment property.
            if (!entry.is_relative()) entry = entry.relative_path();
            out = root / entry;
        } catch (const std::exception& ex) {
            throw std::runtime_error("extract: cannot map entry path: " + out_label + " (" +
                                     ex.what() + ")");
        }
        mkdirs(out.parent_path());
        std::FILE* f = fopen_write_binary(out);
        if (!f) {
            throw std::runtime_error("extract: cannot open output: " + out_label + " (" +
                                     std::strerror(errno) + ")");
        }
        if (!data->empty() &&
            std::fwrite(data->data(), 1, data->size(), f) != data->size()) {
            std::fclose(f);
            throw std::runtime_error("extract: short write: " + out_label);
        }
        std::fclose(f);
        ++written;
    }
    return written;
}

// ---------------------------------------------------------------------------
// PhysFileSystem
// ---------------------------------------------------------------------------

struct PhysFileSystem::Impl {
    std::string mp;                 // own mountpoint subtree ("/__oaN")
    std::string kind;               // "dir" | "pfs" | "pfs+dir"
    std::vector<std::string> mounted;  // physfs mount names this object added
    std::vector<std::string> dir_keys; // real-dir registry keys referenced
    // Case-fold cache: key = mp + '|' + exact virtual dir, value = children.
    mutable std::map<std::string, std::vector<std::string>> dir_cache;

    ~Impl() {
        for (auto it = dir_keys.rbegin(); it != dir_keys.rend(); ++it) {
            auto sd = g_shared_dirs.find(*it);
            if (sd != g_shared_dirs.end() && --sd->second.users <= 0) {
                PHYSFS_unmount(sd->first.c_str());
                g_shared_dirs.erase(sd);
            }
        }
        for (auto it = mounted.rbegin(); it != mounted.rend(); ++it) {
            PHYSFS_unmount(it->c_str());
        }
        physfs_release();
    }

    /// Adopt a live mount of `realdir` (same real dir shared by another
    /// instance): PhysicsFS refuses duplicate directory mounts. Fresh
    /// mounts register in g_shared_dirs; unmounts happen in the destructor
    /// when the last user goes away.
    bool adopt_or_mount_dir(const std::string& realdir) {
        const auto existing = g_shared_dirs.find(realdir);
        if (existing != g_shared_dirs.end()) {
            mp = existing->second.mp;
            ++existing->second.users;
            dir_keys.push_back(realdir);
            return true;
        }
        std::error_code ec;
        if (!sfs::is_directory(realdir, ec) || ec) {
            throw std::runtime_error("cannot mount directory: " + realdir);
        }
        if (!PHYSFS_mount(realdir.c_str(), mp.c_str(), 0)) {
            throw std::runtime_error("cannot mount directory " + realdir + ": " +
                                     physfs_error());
        }
        g_shared_dirs.emplace(realdir, SharedDir{mp, 1});
        dir_keys.push_back(realdir);
        return false;
    }

    void mount_pfs_container(const std::vector<std::string>& paths, bool prepend) {
        try {
            auto files = std::make_shared<FileSet>(paths);
            const uint64_t total = files->total_size();
            auto* ctx = new IoContext(std::move(files), 0, total, nullptr);
            PHYSFS_Io* io = make_container_io(ctx);
            const std::string name = fresh_name(".pfs");
            // mountIo takes ownership on success and destroys io on unmount;
            // on failure it does NOT destroy the io.
            if (!PHYSFS_mountIo(io, name.c_str(), mp.c_str(), prepend ? 0 : 1)) {
                io_destroy(io);
                throw std::runtime_error("cannot mount PFS archive: " + physfs_error());
            }
            
            mounted.push_back(name);
        } catch (const std::runtime_error&) {
            throw;
        }
    }

    const std::vector<std::string>& children(const std::string& full_dir) const {
        const std::string key = mp + "|" + full_dir;
        auto it = dir_cache.find(key);
        if (it != dir_cache.end()) return it->second;
        std::set<std::string> names;
        if (char** list = PHYSFS_enumerateFiles(full_dir.c_str())) {
            for (char** q = list; *q; ++q) names.emplace(*q);
            PHYSFS_freeList(list);
        }
        
        std::vector<std::string> sorted(names.begin(), names.end());
        return dir_cache.emplace(key, std::move(sorted)).first->second;
    }

    /// Case-insensitive resolve of `rel` to an exact-case path under mp.
    /// Returns nullopt when absent (or on escape attempts).
    std::optional<std::string> resolve(std::string_view rel) const {
        std::vector<std::string> comps;
        std::string cur;
        for (const char c : rel) {
            if (c == '/' || c == '\\') {
                if (!cur.empty()) comps.push_back(std::move(cur));
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) comps.push_back(std::move(cur));

        std::string full = mp;
        for (const auto& want : comps) {
            if (want.empty() || want == "." || want == "..") return std::nullopt;
            const std::string low = ascii_lower(want);
            bool found = false;
            for (const auto& name : children(full)) {
                if (ascii_lower(name) == low) {
                    full += '/';
                    full += name;
                    found = true;
                    break;
                }
            }
            if (!found) return std::nullopt;
        }
        return full;
    }
};

PhysFileSystem::PhysFileSystem(std::string source_path, bool sidecar_dir)
    : d_(new Impl()) {
    while (source_path.size() > 1 &&
           (source_path.back() == '/' || source_path.back() == '\\')) {
        source_path.pop_back();
    }
    physfs_acquire();
    d_->mp = "/" + fresh_name(""); // e.g. /__oa0
    try {
        std::error_code ec;
        const bool is_dir = sfs::is_directory(source_path, ec) && !ec;
        if (is_dir) {
            d_->adopt_or_mount_dir(source_path);
            d_->kind = "dir";
        } else {
            const std::string parent = sfs::path(source_path).parent_path().string();
            bool have_sidecar = false;
            if (sidecar_dir && !parent.empty()) {
                std::error_code sec;
                have_sidecar = sfs::is_directory(parent, sec) && !sec;
            }
            // Resolve a possible shared sidecar directory FIRST: adopting an
            // existing mountpoint moves mp before any archive mounts below.
            bool adopted = false;
            if (have_sidecar) adopted = d_->adopt_or_mount_dir(parent);
            const PlannedSource plan = plan_pfs_source(source_path);
            bool first = true;
            for (const auto& container : plan.containers) {
                d_->mount_pfs_container(container, /*prepend=*/!first);
                first = false;
            }
            d_->kind = "pfs";
            if (have_sidecar) {
                // dir-first: loose files in the archive's own
                // real directory overlay the pack (mounted last = highest
                // search-path priority).
                if (!adopted) d_->adopt_or_mount_dir(parent);
                d_->kind = "pfs+dir";
            }
        }
    } catch (...) {
        d_.reset(); // unmounts everything + releases physicsfs
        throw;
    }
}

PhysFileSystem::~PhysFileSystem() = default;

std::optional<std::vector<uint8_t>> PhysFileSystem::read(std::string_view path) const {
    const std::optional<std::string> full = d_->resolve(path);
    if (!full) return std::nullopt;
    PHYSFS_File* f = PHYSFS_openRead(full->c_str());
    if (!f) {
        
        return std::nullopt;
    }
    const PHYSFS_sint64 len = PHYSFS_fileLength(f);
    std::vector<uint8_t> data;
    if (len > 0) {
        if (uint64_t(len) > std::numeric_limits<size_t>::max()) {
            PHYSFS_close(f);
            return std::nullopt;
        }
        data.resize(size_t(len));
        PHYSFS_sint64 got = 0;
        while (got < len) {
            const PHYSFS_sint64 r =
                PHYSFS_readBytes(f, data.data() + got, PHYSFS_uint64(len - got));
            if (r <= 0) break;
            got += r;
        }
        if (got != len) {
            PHYSFS_close(f);
            return std::nullopt;
        }
    }
    PHYSFS_close(f);
    return data;
}

std::optional<std::vector<uint8_t>> PhysFileSystem::read_range(std::string_view path,
                                                               uint64_t offset,
                                                               size_t len) const {
    const std::optional<std::string> full = d_->resolve(path);
    if (!full) return std::nullopt;
    PHYSFS_File* f = PHYSFS_openRead(full->c_str());
    if (!f) return std::nullopt;
    const PHYSFS_sint64 length = PHYSFS_fileLength(f);
    std::vector<uint8_t> data;
    if (offset >= PHYSFS_uint64(length)) { // at/past EOF: empty range, no error
        PHYSFS_close(f);
        return data;
    }
    const uint64_t avail = PHYSFS_uint64(length) - offset;
    len = size_t(std::min<uint64_t>(len, avail));
    if (len > 0) {
        data.resize(len);
        if (PHYSFS_seek(f, PHYSFS_uint64(offset)) == 0) {
            PHYSFS_close(f);
            return std::nullopt;
        }
        PHYSFS_sint64 got = 0;
        while (got < PHYSFS_sint64(len)) {
            const PHYSFS_sint64 r =
                PHYSFS_readBytes(f, data.data() + got, PHYSFS_uint64(len - size_t(got)));
            if (r <= 0) break;
            got += r;
        }
        data.resize(size_t(got)); // EOF tail: return the bytes we actually got
    }
    PHYSFS_close(f);
    return data;
}

bool PhysFileSystem::exists(std::string_view path) const {
    const std::optional<std::string> full = d_->resolve(path);
    if (!full) return false;
    PHYSFS_Stat st;
    return stat_full(*full, &st);
}

std::optional<std::vector<std::string>> PhysFileSystem::list(std::string_view dir) const {
    std::string full = d_->mp;
    if (!dir.empty()) {
        const std::optional<std::string> resolved = d_->resolve(dir);
        if (!resolved) return std::nullopt;
        full = *resolved;
    }
    // Contract: direct FILE children only (directories are not listed).
    std::vector<std::string> out;
    for (const auto& name : d_->children(full)) {
        PHYSFS_Stat st;
        if (!stat_full(full + "/" + name, &st)) continue;
        if (st.filetype == PHYSFS_FILETYPE_REGULAR ||
            st.filetype == PHYSFS_FILETYPE_SYMLINK) {
            out.push_back(name);
        }
    }
    return out;
}

const char* PhysFileSystem::kind() const { return d_->kind.c_str(); }

// ---------------------------------------------------------------------------
// WritableMount (read-write category: saves / file API)
// ---------------------------------------------------------------------------

struct WritableMount::Impl {
    std::string root; // real root (write dir identity)
    std::string mp;   // read-back mountpoint (shared with other mounts of the same dir)
    bool mounted = false; // this instance called PHYSFS_mount itself

    ~Impl() {
        // Clear the process-wide write dir if it still points at us.
        const char* cur = PHYSFS_getWriteDir();
        if (cur && root == cur) PHYSFS_setWriteDir(nullptr);
        // Release the shared real-dir registration: the LAST user unmounts.
        const auto sd = g_shared_dirs.find(root);
        if (sd != g_shared_dirs.end() && --sd->second.users <= 0) {
            PHYSFS_unmount(root.c_str());
            g_shared_dirs.erase(sd);
        } else if (sd == g_shared_dirs.end() && mounted) {
            PHYSFS_unmount(root.c_str());
        }
        physfs_release();
    }
};

WritableMount::WritableMount(std::string real_root) : root_(std::move(real_root)) {
    if (root_.empty()) return; // inert store
    physfs_acquire();
    d_.reset(new Impl());
    d_->root = root_;
    try {
        // One-time real-root bootstrap; every game-visible byte after this
        // flows through PhysicsFS.
        std::error_code ec;
        sfs::create_directories(root_, ec);
        if (ec) {
            d_.reset(); // dtor releases the physfs reference
            root_.clear();
            return;
        }
        // PhysicsFS refuses to mount the same real directory twice (dedupe
        // by archive): when another live mount (e.g. the read-only game
        // source, which is the desktop default save root) already owns this
        // directory, ADOPT its mountpoint instead of failing silently -
        // otherwise read-back through our own mountpoint would see nothing.
        const auto existing = g_shared_dirs.find(root_);
        if (existing != g_shared_dirs.end()) {
            d_->mp = existing->second.mp;
            ++existing->second.users;
            return;
        }
        d_->mp = "/" + fresh_name("r");
        if (!PHYSFS_mount(root_.c_str(), d_->mp.c_str(), 1)) {
            d_.reset();
            root_.clear();
            return;
        }
        d_->mounted = true;
        g_shared_dirs.emplace(root_, SharedDir{d_->mp, 1});
    } catch (...) {
        d_.reset();
        root_.clear();
    }
}
WritableMount::~WritableMount() = default;

bool WritableMount::valid() const { return d_ != nullptr && !root_.empty(); }

namespace {

bool rel_clean(std::string_view rel) {
    if (rel.empty() || rel.front() == '/' || rel.front() == '\\') return false;
    const sfs::path relp{std::string(rel)};
    for (const auto& seg : relp) {
        if (seg == "..") return false;
    }
    return true;
}

bool ensure_write_root(const std::string& root) {
    const char* cur = PHYSFS_getWriteDir();
    if (cur && root == cur) return true;
    if (cur && !PHYSFS_setWriteDir(nullptr)) return false;
    return PHYSFS_setWriteDir(root.c_str()) != 0;
}

/// Create every ancestor directory of `rel` inside the write dir.
bool mkdir_parents(const std::string& rel) {
    size_t pos = 0;
    while ((pos = rel.find('/', pos)) != std::string::npos) {
        if (pos > 0 && !PHYSFS_mkdir(rel.substr(0, pos).c_str())) return false;
        ++pos;
    }
    return true;
}

std::optional<std::array<int64_t, 6>> calendar_of(PHYSFS_sint64 secs) {
    if (secs < 0) return std::nullopt;
    const std::time_t t = std::time_t(secs);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::array<int64_t, 6> out;
    out[0] = tm.tm_year + 1900;
    out[1] = tm.tm_mon + 1;
    out[2] = tm.tm_mday;
    out[3] = tm.tm_hour;
    out[4] = tm.tm_min;
    out[5] = tm.tm_sec;
    return out;
}

} // namespace

bool WritableMount::write(const std::string& rel_path,
                          const std::vector<uint8_t>& data) {
    if (!valid() || !rel_clean(rel_path)) return false;
    if (!ensure_write_root(root_)) return false;
    if (!mkdir_parents(rel_path)) return false;
    PHYSFS_File* f = PHYSFS_openWrite(rel_path.c_str());
    if (!f) return false;
    bool ok = true;
    if (!data.empty()) {
        PHYSFS_sint64 wrote = 0;
        while (wrote < PHYSFS_sint64(data.size())) {
            const PHYSFS_sint64 r = PHYSFS_writeBytes(
                f, data.data() + wrote, PHYSFS_uint64(data.size() - size_t(wrote)));
            if (r <= 0) {
                ok = false;
                break;
            }
            wrote += r;
        }
    }
    if (PHYSFS_close(f) == 0) ok = false;
    return ok;
}

std::optional<std::vector<uint8_t>> WritableMount::read(
    const std::string& rel_path) const {
    if (!valid() || !rel_clean(rel_path)) return std::nullopt;
    const std::string full = d_->mp + "/" + rel_path;
    PHYSFS_File* f = PHYSFS_openRead(full.c_str());
    if (!f) return std::nullopt;
    const PHYSFS_sint64 len = PHYSFS_fileLength(f);
    std::vector<uint8_t> data;
    bool ok = true;
    if (len > 0) {
        data.resize(size_t(len));
        PHYSFS_sint64 got = 0;
        while (got < len) {
            const PHYSFS_sint64 r =
                PHYSFS_readBytes(f, data.data() + got, PHYSFS_uint64(len - got));
            if (r <= 0) {
                ok = false;
                break;
            }
            got += r;
        }
        if (got != len) ok = false;
    }
    PHYSFS_close(f);
    if (!ok) return std::nullopt;
    return data;
}

bool WritableMount::remove(const std::string& rel_path) {
    if (!valid() || !rel_clean(rel_path)) return false;
    if (!ensure_write_root(root_)) return false;
    return PHYSFS_delete(rel_path.c_str()) != 0;
}

bool WritableMount::exists(const std::string& rel_path) const {
    if (!valid() || !rel_clean(rel_path)) return false;
    const std::string full = d_->mp + "/" + rel_path;
    PHYSFS_Stat st;
    return PHYSFS_stat(full.c_str(), &st) != 0;
}

std::optional<std::array<int64_t, 6>> WritableMount::modification_time(
    const std::string& rel_path) const {
    if (!valid() || !rel_clean(rel_path)) return std::nullopt;
    const std::string full = d_->mp + "/" + rel_path;
    PHYSFS_Stat st;
    if (PHYSFS_stat(full.c_str(), &st) == 0) return std::nullopt;
    return calendar_of(st.modtime);
}

} // namespace oa::fs
