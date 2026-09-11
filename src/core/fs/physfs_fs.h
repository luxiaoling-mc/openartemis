#pragma once
// Single project-source virtual file system on top of PhysicsFS, split into
// the two mount categories the engine needs:
//
//   READ-ONLY (game assets)   — PhysFileSystem below: one project source
//     (real directory, or an Artemis PFS pack, or pack + its own real parent
//     directory overlay where loose files win). Also served by the custom
//     PHYSFS_Archiver in physfs_fs.cpp.
//   READ-WRITE (saves + file API) — WritableMount below: one writable real
//     directory (PhysicsFS write dir) mounted read-back at a private point.
//
// All game-visible file bytes go through this one PhysicsFS session, so the
// android/wasm hosts only need to hand the right real roots in; no engine
// code touches the OS filesystem directly.
//
// The engine previously layered its own readers (DirFileSystem /
// PfsFileSystem / FallbackFileSystem + pfs_archive + stdio DirSaveStore);
// this header replaces that whole family.
//
// Case folding: PhysicsFS lookups are exact-case, but the IFileSystem
// contract is case-insensitive virtual paths. PhysFileSystem therefore
// resolves every virtual path against the mounted tree per component
// (enumerate + ASCII-fold match, cached per directory).
//
// scan_pfs_file / extract_pfs_archive keep the metadata/self-extract
// surfaces the opfs tool and boot-subset tests need (they no longer have a
// standalone archive object to poke at).
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/fs/fs.h"

namespace oa::fs {

/// One parsed PFS index record (metadata + extraction surface).
struct PfsEntryInfo {
    std::string path;      // virtual path as stored (backslashes)
    uint32_t reserved = 0;
    uint32_t offset = 0;   // absolute container offset of payload
    uint32_t size = 0;     // payload size (decrypted)
};

/// Parsed index metadata of a PFS source (single-file + volume handling).
struct PfsInfo {
    char version = '6';            // '2' | '6' | '8'
    uint32_t index_size = 0;
    uint64_t total_size = 0;       // container bytes (raw split volumes merged)
    std::vector<PfsEntryInfo> entries; // base pack index (chain: base only)
};

/// PFS virtual-path normalization: `\` -> `/`, ASCII upper -> lower, and a
/// trailing separator of a directory entry is dropped.
std::string normalize_path(std::string_view path);

/// Parse a PFS source's index for tools/tests. Resolves sibling volumes the
/// same way the mount path does (standalone chain / raw byte split). Throws
/// std::runtime_error when the source is not a readable PFS archive.
PfsInfo scan_pfs_file(const std::string& path);

/// Extract every entry of a PFS source into `dir` (created on demand),
/// byte-exact, preserving relative paths (backslashes become '/'). `filter`:
/// return false to skip an entry (default: extract all). Returns the number
/// of files written. Throws std::runtime_error on I/O failure.
size_t extract_pfs_archive(
    const std::string& archive_path, const std::string& dir,
    const std::function<bool(const PfsEntryInfo&)>& filter = nullptr);

// ---------------------------------------------------------------------------
// Read-write category (saves / future file API).
//
// PhysicsFS splits IO into two mount classes: the read-only search path
// (PhysFileSystem above, game assets) and ONE writable real directory.
// WritableMount is the second category: its real root becomes the PhysicsFS
// write dir (writes/mkdir/delete land there) AND is mounted at a private
// mountpoint so reads/modification-time queries go through the same session.
// Every file in the engine therefore flows through PhysicsFS on all
// platforms (android/wasm write roots arrive via oa::plat::default_save_root
// etc.); nothing below touches std::filesystem for game-visible bytes except
// the one-time real-root bootstrap.
// ---------------------------------------------------------------------------

class WritableMount {
public:
    /// `real_root` empty -> inert store (nothing persists).
    explicit WritableMount(std::string real_root);
    ~WritableMount();

    WritableMount(const WritableMount&) = delete;
    WritableMount& operator=(const WritableMount&) = delete;

    bool valid() const;
    const std::string& root() const { return root_; }

    bool write(const std::string& rel_path, const std::vector<uint8_t>& data);
    std::optional<std::vector<uint8_t>> read(const std::string& rel_path) const;
    bool remove(const std::string& rel_path);
    bool exists(const std::string& rel_path) const;
    /// Local calendar components [y,mo,d,h,mi,s] of the file's modification
    /// time; nullopt when unavailable.
    std::optional<std::array<int64_t, 6>> modification_time(
        const std::string& rel_path) const;

private:
    struct Impl;
    std::string root_;
    std::unique_ptr<Impl> d_;
};

class PhysFileSystem final : public IFileSystem {
public:
    /// Mount one project source.
    ///  * source_path names a DIRECTORY: the tree is the file system.
    ///  * source_path names a PFS ARCHIVE: pack (+ volume set) is mounted;
    ///    with sidecar_dir (default) the archive's own real parent directory
    ///    is mounted at the same point with higher priority when it exists.
    /// Throws std::runtime_error when the source cannot be mounted.
    explicit PhysFileSystem(std::string source_path, bool sidecar_dir = true);

    ~PhysFileSystem() override;

    PhysFileSystem(const PhysFileSystem&) = delete;
    PhysFileSystem& operator=(const PhysFileSystem&) = delete;

    std::optional<std::vector<uint8_t>> read(std::string_view path) const override;
    /// Read a byte range of a file (decryption-safe chunked access);
    /// nullopt when the file is missing, shorter bytes at EOF.
    std::optional<std::vector<uint8_t>> read_range(std::string_view path,
                                                   uint64_t offset, size_t len) const;
    bool exists(std::string_view path) const override;
    std::optional<std::vector<std::string>> list(std::string_view dir) const override;
    const char* kind() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace oa::fs
