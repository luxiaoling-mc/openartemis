#pragma once
// Engine save formats — runtime save contract (part of the
// runtime_save family; implementation lives in runtime_save.cpp, binary
// since the payload became a single hand-written binary document;
// design/table: core/util/binary_stream.h):
//   - sys files (saveg.dat / system.dat): one binary document whose payload
//     is a flat domain map {stripped-key: value}
//   - numbered saves (SaveData): one binary document whose payload is the
//     tagged map described below
// Only the LOCAL variable domain travels with numbered saves (g./s. persist
// via saveg/system.dat; numbered_save keeps persistent domains out of the
// snapshot).
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime_iet.h"

namespace oa::runtime {

/// Logical save schema version (= the binary header version; must match
/// oa::util::kFormatVersion). Bump on any layout change below.
/// 维护契约：任何字段增删改都必须同步 bump 本常量，并检查
/// 读侧对"失效 id"的容忍（层 id 属数据，但读档后可能不存在——要么重建、
/// 要么显式判空跳过；旧档只补默认值，不写迁移器）。
constexpr int kSaveFormatVersion = 1;

// ---------------------------------------------------------------------------
// Audio replay snapshot ( AudioSnapshot / AudioChannelSnapshot).
// ---------------------------------------------------------------------------
struct AudioChannelSnap {
    std::string id;
    std::string file;
    bool loop_play = false;
    int32_t gain = 1000; // raw Artemis scale
    int32_t pan = 0;
    bool skippable = false;
};

struct AudioSnap {
    std::optional<AudioChannelSnap> bgm;
    std::vector<AudioChannelSnap> se;
    std::vector<AudioChannelSnap> voice;
    bool empty() const { return !bgm && se.empty() && voice.empty(); }
};

// ---------------------------------------------------------------------------
// Scene snapshot (openartemis-lite): verbatim layer props replay. Layer
// ids/dotted parents come back via Compositor::create autovivify; root props
// ("!") are carried separately.
// ---------------------------------------------------------------------------
/// One [lyevent] row on a layer (misc-B: layer event registrations travel
/// with the scene snapshot — Layer.event_handlers is serialized as
/// part of Scene, compositor/).
struct LayerEventHandlerSnap {
    std::string type; // "click" / "rollover" / "rollout" / "dragin" / ...
    bool enabled = true;
    bool penetration = false;
    std::string handler;
    std::string file;
    std::string label;
    bool call = false;
    std::map<std::string, std::string> params;
    std::map<std::string, std::string> filter_params;
};

struct LayerSnap {
    std::string id;
    std::map<std::string, std::string> props;
    std::vector<LayerEventHandlerSnap> handlers;
};

// ---------------------------------------------------------------------------
// One numbered save.
//
// Binary payload layout:
//   "local_variables": map<string, Value>     (Value tags: nil/bool/int/
//                                               float64/string)
//   "current_script":  string
//   "current_line":    int (>= 0)
//   "call_stack":      array of { "script": string, "return_line": int }
//   "scene"?:          { "root_props": map<string,string>,
//                        "layers": array of layer maps }
//   "audio"?:          { "bgm"?: channel, "se": [...], "voice": [...] }
// A channel map: id/file strings, loop_play/skippable bools, gain/pan ints.
// Unknown keys are skipped on read (forward tolerance within a version).
// ---------------------------------------------------------------------------
struct SaveData {
    int version = kSaveFormatVersion;
    std::map<std::string, oa::runtime::Value> local_variables; // local domain keys
    std::string current_script;
    size_t current_line = 0;
    std::vector<std::pair<std::string, size_t>> call_stack; // {script, return_line}
    // Scene (optional; saves without a snapshot restore script/Lua state
    // only).
    bool has_scene = false;
    std::map<std::string, std::string> root_props;
    std::vector<LayerSnap> layers;
    // Audio (optional).
    bool has_audio = false;
    AudioSnap audio;

    /// Encode to one binary document.
    std::string encode() const;
    /// Decode one binary document; throws oa::util::FormatError
    /// (a std::runtime_error) on malformed data or a newer format version.
    static SaveData decode(const std::string& bytes);
};

/// Flat domain-map binary document for saveg.dat / system.dat (keys stripped
/// of the "g." / "s." prefix, syssave/sysload).
std::string encode_domain_map(const std::map<std::string, oa::runtime::Value>& vars);
/// Decode a domain-map binary document; throws oa::util::FormatError.
std::map<std::string, oa::runtime::Value> decode_domain_map(const std::string& bytes);

} // namespace oa::runtime

// ---------------------------------------------------------------------------
// Save-domain storage seam (moved from core/runtime/runtime_save.h): hosts/tests inject a
// SaveStore; DirSaveStore rides the shared PhysicsFS READ-WRITE mount.
// ---------------------------------------------------------------------------
namespace oa::runtime {

/// Key-value byte persistence over relative paths under the save root.
class SaveStore {
public:
    virtual ~SaveStore() = default;
    /// Write bytes (creating parent directories as needed). False on failure.
    virtual bool write(const std::string& rel_path, const std::vector<uint8_t>& data) = 0;
    /// Read bytes; nullopt when missing or unreadable.
    virtual std::optional<std::vector<uint8_t>> read(const std::string& rel_path) const = 0;
    /// Delete; false when missing/failed.
    virtual bool remove(const std::string& rel_path) = 0;
    /// Whether the path exists (save-slot checks).
    virtual bool exists(const std::string& rel_path) const { return read(rel_path).has_value(); }
    /// Whether writes actually persist somewhere (autosave gating).
    virtual bool persistent() const { return true; }
    /// Local calendar components [y,mo,d,h,mi,s] of the file's modification
    /// time (var system=file_update_time hook). nullopt when missing or the
    /// store cannot report times.
    virtual std::optional<std::array<int64_t, 6>> modification_time(
        const std::string& rel_path) const {
        (void)rel_path;
        return std::nullopt;
    }
};

/// No-op store (default): save operations run their logic but persist nothing.
/// Keeps headless runs side-effect free; hosts/tests install a real store.
class NullSaveStore final : public SaveStore {
public:
    bool write(const std::string&, const std::vector<uint8_t>&) override { return false; }
    std::optional<std::vector<uint8_t>> read(const std::string&) const override {
        return std::nullopt;
    }
    bool remove(const std::string&) override { return false; }
    bool exists(const std::string&) const override { return false; }
    bool persistent() const override { return false; }
};

/// Read-write PhysicsFS-backed store rooted at a host-chosen directory
/// (app sandbox / tmp dir in tests). `root` may be empty (store inert like
/// Null). All bytes flow through the shared PhysicsFS session.
class DirSaveStore final : public SaveStore {
public:
    explicit DirSaveStore(std::string root);
    bool write(const std::string& rel_path, const std::vector<uint8_t>& data) override;
    std::optional<std::vector<uint8_t>> read(const std::string& rel_path) const override;
    bool remove(const std::string& rel_path) override;
    bool exists(const std::string& rel_path) const override;
    std::optional<std::array<int64_t, 6>> modification_time(
        const std::string& rel_path) const override;
    const std::string& root() const { return root_; }

private:
    std::string root_;
    std::unique_ptr<oa::fs::WritableMount> mount_;
};

} // namespace oa::runtime
