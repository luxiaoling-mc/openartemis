#pragma once
// Engine surface: GameRuntime = project + interpreter + Artemis waits + render
// host. Host apps / the SDL layer include runtime.h (App) instead of this.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/emote/emote_player.h"
#include "core/fs/fs.h"
#include "core/fs/project.h"
#include "core/media/audio.h" // AudioEngine + MediaPlayers (merged)
#include "core/media/decode_pool.h"
#include "core/media/video.h"
#include "core/render/layer.h"
#include "core/render/render_host.h"
#include "core/render/text.h"
#include "core/render/texture_key.h"
#include "core/runtime/runtime_iet.h"
#include "core/runtime/runtime_save.h"

namespace oa::runtime {

/// Per-tick physical input summary (Windows virtual key codes).
struct FrameInput {
    std::vector<int> key_down_edges; // pressed this tick
    std::vector<int> key_up_edges;   // released this tick
    std::set<int> keys_down;         // currently held
    int mouse_x = 0;
    int mouse_y = 0;
    bool left_down = false;
    bool left_click_edge = false;
    bool right_click_edge = false;
};

/// Load outcome; every failure is atomic (runtime state unchanged).
enum class LoadResult {
    Ok = 0,
    MissingFile,         // save unreadable: missing file, no store, no interpreter
    Corrupt,             // decode failed (corrupt / newer format / invalid binary)
    PositionUnavailable, // preflight failed: the saved script position cannot load
};

/// [trans] read-only projection; the write side lives in RuntimeState.
class TransitionStatus {
public:
    bool active() const;
    bool is_in_progress(uint64_t clock_ms) const; // occupies the stage while capturing
    double progress(uint64_t clock_ms) const;    // 0..1 overlay progress
    int type() const;                           // 0 = none
    const std::string& rule() const;            // type=2 rule image ("" when idle)
    int vague() const;                          // type=2 dissolve blur, default 32

private:
    friend class GameRuntime;
    explicit TransitionStatus(const oa::render::Transition* state) : state_(state) {}
    const oa::render::Transition* state_ = nullptr;
};

class GameRuntime : public oa::render::RenderHost {
public:
    explicit GameRuntime(std::shared_ptr<const oa::fs::IFileSystem> fs);
    ~GameRuntime();

    // ---- project lifecycle ------------------------------------------------
    /// Parse system.ini and build the interpreter/hooks. Does not run boot.
    /// Throws on failure; re-open replaces the interpreter.
    void open_project(std::string_view platform);
    /// Load system saves, wire Lua hooks and position the BOOT script.
    void boot_project();
    /// Parsed project (system.ini config + mounts); valid after open_project.
    const oa::fs::Project& project() const { return project_; }

    // ---- frame loop -------------------------------------------------------
    /// Advance one frame: onEnterFrame → drains → steps → waits.
    void tick(uint64_t delta_ms, const FrameInput& input);
    /// Host path: uses the input accumulated by the methods below.
    void tick(uint64_t delta_ms);
    void key_event(int vk, bool down);
    void mouse_move(float window_x, float window_y);
    void mouse_button(int vk, bool down); // 1 left, 2 right, 3 middle
    void mouse_wheel(int direction);      // > 0 up, < 0 down
    bool exit_requested() const;
    /// Host events since the last drain; scene/text/media events never appear.
    std::vector<oa::runtime::Event> drain_events();

    // ---- engine access ----------------------------------------------------
    oa::runtime::Interpreter& interpreter();
    const oa::runtime::Interpreter& interpreter() const;
    const std::shared_ptr<const oa::fs::IFileSystem>& fs() const;
    /// Create the backend on the host window and start media players.
    bool attach_window(void* native_window, const std::string& renderer_kind);
    int stage_width() const override;
    int stage_height() const override;
    bool vsync_active() const;
    bool frame_presented() const;
    void request_repaint();
    /// Stage frame capture used by --dump / parity runs.
    std::optional<std::vector<uint8_t>> capture_frame(uint32_t* w, uint32_t* h);

    // ---- 供渲染器直接回调的宿主面 ---------------------------------------
    /// Monotonic tick clock (ms), the single animation/transition clock.
    uint64_t now_ms() const override;
    std::string resolve_magic_path(const std::string& name) const override;
    std::optional<std::string> host_variable(const std::string& key) const override;
    std::optional<std::vector<uint8_t>> read_save_file(
        const std::string& rel_path) const override;
    void bind_text_metrics(
        std::function<std::tuple<double, double, double>()> fn) override;
    /// Apply [trans]: type 0 clears, otherwise starts a capture transition.
    void transition_begin(const std::map<std::string, std::string>& params) override;
    oa::render::Transition& transition_state();
    /// [takess] capture runs after presenting the current frame.
    void post_frame_capture() override;
    using EmoteLayerState = oa::emote::LayerState;
    const std::map<std::string, EmoteLayerState>& emote_layers() const;
    oa::media::VideoEngine& video();

    // ---- scene / text / transition views ----------------------------------
    // Scene/text/transition live in the runtime-owned RenderEngine.
    oa::render::Compositor& scene();
    const oa::render::Compositor& scene() const;
    oa::render::TextEngine& text();
    const oa::render::TextEngine& text() const;
    /// Read-only [trans] projection; write entries stay in RuntimeState.
    TransitionStatus transition();
    TransitionStatus transition() const;
    /// Host snapshot hook invoked right after a non-zero [trans] begins.
    void set_transition_capture_callback(std::function<void()> cb);
    /// Host finished snapshotting the previous frame into the capture texture.
    void mark_transition_captured();
    /// Global scene/text fade-out then clear; time=0 clears immediately.
    /// Pauses on Stop{"alldelete"} while the fade runs.
    void begin_all_delete(uint64_t time_ms);
    /// Global opacity multiplier for the fade (1.0 when idle).
    double all_delete_fade() const;

    // ---- wait machine / control probes ------------------------------------
    /// Is the interpreter parked in a [stop]-class wait (title etc.)?
    bool waiting_stop() const;
    /// Current wait reason when parked (nullptr when executing).
    const oa::runtime::WaitReason* current_wait() const;
    /// Lua-side override status bits (e:overrideKey). Frame edge bookkeeping
    /// happens in tick; bits: 2 isPush 4 isDown 8 downEdge 16 upEdge 32 decide.
    void apply_override(int key, int status);
    bool skip_active() const;
    bool automode_active() const;
    bool hide_active() const;
    bool control_skip_effective() const;
    bool rclick_allowed() const;

    // ---- pointer probes / host hooks --------------------------------------
    /// Host hit providers: texture size + pixel-alpha sampler (both optional).
    void set_hit_providers(oa::render::Compositor::QuadSizeFn size,
                           oa::render::Compositor::AlphaSamplerFn alpha, void* userdata);
    /// One pointer-chain Lua dispatch (host diagnostics).
    struct PointerDispatch {
        enum class Kind { HoverIn, HoverOut, Click, Push, DragIn, DragMove, DragOut };
        Kind kind = Kind::Click;
        std::string layer;    // layer id ("" for global push)
        std::string function; // Lua function dispatched ("" when none)
        std::string key;      // button key / push key when meaningful
        bool ran_lua = false; // call_function executed (function existed)
    };
    /// Called synchronously when the pointer chain dispatches a handler.
    void set_pointer_observer(std::function<void(const PointerDispatch&)> cb);
    bool is_hovered(const std::string& id) const;
    /// Total Lua dispatches made by the pointer chain (diagnostics/tests).
    size_t pointer_dispatch_count() const;
    /// Host cursor warp hook ([mouse]); the runtime emulates the move when
    /// the host cannot warp (hidden/headless).
    void set_pointer_warp_callback(std::function<void(int, int)> cb);
    /// [mouse] warp requests accepted since boot (diagnostics/tests).
    size_t pointer_warp_requests() const;
    /// Current engine pointer in stage coordinates (per-tick FrameInput).
    std::pair<int, int> mouse_point() const;

    // ---- diagnostics counters (host prints / tests) -----------------------
    size_t scene_layer_events() const;
    size_t scene_tween_events() const;
    size_t tween_completion_calls() const;
    size_t text_events() const;
    size_t message_switch_events() const;
    size_t emote_layer_events() const;
    size_t media_events() const;
    size_t media_handler_dispatches() const;

    // ---- media domain -----------------------------------------------------
    oa::media::AudioEngine& audio();
    const oa::media::AudioEngine& audio() const;
    const oa::media::VideoEngine& video() const;
    oa::media::MediaPlayers& media_players();
    /// Override the media asset loader (defaults to the project filesystem +
    /// magic-path resolution). Tests inject synthetic assets here.
    void set_media_loader(
        std::function<std::optional<std::vector<uint8_t>>(const std::string&)> loader);
    /// Decode pool: < 0 = platform default, 0 = decode on the caller thread.
    void enable_decode_pool(int threads = -1);
    /// Drive one media event directly (identical path to interpreter dispatch).
    bool apply_media_event(const oa::runtime::Event& e);
    /// Drive one interpreter event through the full domain routing (engine →
    /// media → text → scene → control). Hosts/tools that need to raise a tag
    /// without a script (e.g. emotestatic/lyc) use this; scripts go through
    /// the interpreter callback. Waits land in drain_events() as usual.
    oa::runtime::CallbackResult apply_event(const oa::runtime::Event& e);
    /// Finish one sound channel and dispatch its registered handler.
    void sound_finished(oa::media::SoundCategory cat, const std::string& id);

    // ---- save domain ------------------------------------------------------
    /// Install the save backend (host/tests). Null store by default.
    void set_save_store(std::shared_ptr<oa::runtime::SaveStore> store);
    /// Host frame capture for [takess]/[savess] (RGBA). Absent => savess
    /// writes a deterministic placeholder thumbnail.
    using CaptureFn = std::function<std::optional<std::vector<uint8_t>>(uint32_t*, uint32_t*)>;
    void set_frame_capture(CaptureFn fn);
    /// syssave: persist g./s. domains into saveg.dat/system.dat.
    bool syssave();
    /// Numbered save (host/tests may call directly).
    bool save_game_to(const std::string& file);
    /// Load a numbered save; authoritative entry, failures are atomic.
    LoadResult load_game(const std::string& file, int64_t trans_type);
    /// `load_game(...) == LoadResult::Ok`.
    bool load_game_from(const std::string& file, int64_t trans_type);
    /// Current logical save path prefix (sanitized from the ini SAVEPATH).
    const std::string& savepath() const;
    /// Drive one save-domain event directly (tests/hosts).
    void apply_save_event(const oa::runtime::Event& e);

private:
    // Parsed before s_ so the state (which holds a pointer back) dies first.
    oa::fs::Project project_;
    // Implementation lives in RuntimeState (runtime_internal.h).
    struct RuntimeState;
    std::unique_ptr<RuntimeState> s_;
};

} // namespace oa::runtime
