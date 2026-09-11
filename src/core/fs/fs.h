#pragma once
// Virtual file system seam. The engine only talks to IFileSystem; hosts
// mount a PFS archive and/or a real directory (patch/override). Paths are
// virtual (e.g. "system.ini", "image/bg/bg001_a.png"), slash separated,
// matched case-insensitively by implementations.
//
// The save domain lives in THIS directory too (store.*): a save
// IS a filesystem — SaveStore is the read/write seam over relative save paths
// and DirSaveStore rides the same shared PhysicsFS session as the project
// source (core/fs/physfs_fs.h, WritableMount). fs/project.* is the project/ini
// reader, also pure filesystem work. (The save *format* moved out of this
// directory: it is core/runtime/runtime_save.{h,cpp} now.)
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::fs {

class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    /// Read a whole file; nullopt when missing.
    virtual std::optional<std::vector<uint8_t>> read(std::string_view path) const = 0;

    /// Whether a file (or directory) exists.
    virtual bool exists(std::string_view path) const { return read(path).has_value(); }

    /// List the direct file children of a virtual directory ("" = root).
    /// Paths come back slash-separated relative to `dir` (no trailing
    /// slash), matching the read() virtual-path convention. Nullopt when
    /// the backend cannot list (callers then fall back to candidate names).
    virtual std::optional<std::vector<std::string>> list(std::string_view dir) const {
        (void)dir;
        return std::nullopt;
    }

    /// Human name for logs.
    virtual const char* kind() const = 0;
};

} // namespace oa::fs
