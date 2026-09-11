#pragma once
// GameRuntime: host composition gluing Project + Interpreter + input state
// together with the Artemis wait-state machine. The SDL
// host (src/app) drives tick; media/render/save bridges attach later.
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/fs/fs.h"
#include "core/fs/project.h"
#include "core/media/audio.h"       // AudioEngine + MediaPlayers (merged)
#include "core/emote/emote_player.h"
#include "core/media/decode_pool.h"
#include "core/media/video.h"
#include "core/runtime/runtime_save.h"
#include "core/runtime/runtime_save.h"
#include "core/render/layer.h"
#include "core/render/texture_key.h"
#include "core/runtime/runtime_iet.h"
#include "core/render/text.h"

// anim/transition 收为 render 内部实现：公开面只需要这两个
// **名字**（[trans] 只读投影持有的状态指针 / tween 完成回执的容器元素），
// 定义在 core/render/render_internal.h —— 本头不 include 它。
namespace oa::render {
class Transition;
struct TweenDone;
} // namespace oa::render

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

/// Sanitize the project ini SAVEPATH into a clean relative prefix (
/// empty -> "save").
std::string sanitize_savepath(const std::string& raw);

/// 读档结果是**可判定**的（调用方按值分支，不再"只打日志"）。
/// 失败一律是**原子的**：位置预检在任何清场之前完成，任一失败 ⇒ 运行时状态
/// 不被改动（不存在"场景已清、快照已灌、位置恢复失败"的半恢复）。
enum class LoadResult {
    Ok = 0,
    /// 存档不可读：文件不存在（路径不合法）/ 未注入 save store / 无解释器。
    MissingFile,
    /// 解码失败（损坏 / 版本高于本引擎 / 非法二进制）。
    Corrupt,
    /// 预检失败：存档里的当前脚本无法载入（脚本缺失/解析错误）。
    PositionUnavailable,
};

// ---------------------------------------------------------------------------
// [trans] 只读状态投影
//
// transition 语义机（oa::render::Transition）已收为 render 内部实现：写入口
// （start / clear / mark_captured / skip_by_input / clear_finished）只留在
// runtime 实现面（runtime_internal.h 的 RuntimeState），宿主能看到的只有下面
// 6 个只读查询。GameRuntime::transition() 按值返回它（成本 = 一个指针），
// 原调用点 `rt.transition().is_in_progress(rt.now_ms())` 因此一行不改。
// 本类只持 runtime 内部状态的指针，生命周期跟随该 GameRuntime。
// ---------------------------------------------------------------------------
class TransitionStatus {
public:
    /// 是否存在活动 transition。
    bool active() const;
    /// 此刻是否占据画面（等待捕获期间为 true）。
    bool is_in_progress(uint64_t clock_ms) const;
    /// 0..1 覆盖层进度（elapsed/duration 夹取；待捕获/无活动时为 0）。
    double progress(uint64_t clock_ms) const;
    /// 活动 transition 类型（0 = 无）。
    int type() const;
    /// type=2 的 rule 图像路径（无活动时为静态空串）。
    const std::string& rule() const;
    /// rule 溶解柔化 0-255（无活动时为默认 32）。
    int vague() const;

private:
    friend class GameRuntime;
    explicit TransitionStatus(const oa::render::Transition* state) : state_(state) {}
    const oa::render::Transition* state_ = nullptr;
};

class GameRuntime {
public:
    explicit GameRuntime(std::shared_ptr<const oa::fs::IFileSystem> fs);
    ~GameRuntime();

    /// Parsed project (system.ini config + mounts). Valid after open_project.
    oa::fs::Project project_;
    /// Mount a project: parse system.ini, build the interpreter and install
    /// the runtime hooks (loader/save-area/media/backlog/layer-info) and the
    /// interpreter event callback. Does NOT position or run the BOOT script:
    /// that is boot_project's job (two-phase start so the host can install
    /// boot-time consumers — save store, render/font metrics — in between).
    /// Throws on failure. Re-opening replaces the interpreter and resets the
    /// boot-time state; call boot_project again after re-opening.
    void open_project(std::string_view platform);
    /// Second boot phase: load the system saves (sysload), wire the Lua host
    /// service hooks and position the interpreter at the BOOT script entry.
    /// Script execution still starts on the first tick. Call exactly once
    /// per open_project (no-op-safe against an interpreter-less runtime).
    void boot_project();

    /// Full engine restart ([reset] tag — FPM's system/ui.asb *go_title flow):
    /// stop all media, clear
    /// scene/text/transition/pointer/control domains, drop the inline-event
    /// marker, then re-open the project with a FRESH interpreter (fresh Lua
    /// VM — the boot script's [lua] init blocks re-run) and boot again. The
    /// host's save store / render / hit providers survive (open_project only
    /// re-wires the interpreter-side hooks). Pending host events are dropped.
    void restart_project();

    /// Deliver lytween completion handlers (natural finishes and R10e
    /// lytweendel-cancelled timer rounds) as queued engine tags.
    void dispatch_tween_done(const std::vector<oa::render::TweenDone>& done);

    /// Advance one frame: onEnterFrame -> queued drains -> steps -> waits.
    /// `clicked` normally derives from input edges; pass explicit for tests.
    void tick(uint64_t delta_ms, const FrameInput& input);

    bool exit_requested() const;
    /// Events emitted since the last drain (host consumes them each frame).
    /// Scene events (Layer*/lytween/lytweendel/tweenset) never appear here:
    /// the runtime applies them to its Compositor at dispatch time.
    std::vector<oa::runtime::Event> drain_events();
    oa::runtime::Interpreter& interpreter();
    const oa::runtime::Interpreter& interpreter() const;
    const std::shared_ptr<const oa::fs::IFileSystem>& fs() const;

    // ------------------------------------------------------------------
    // Compositor ownership: GameRuntime owns the
    // scene and consumes every scene-mutating interpreter event itself. The
    // host only READS the scene (draw order / world geometry / hit tests) for
    // rendering and input dispatch.
    // ------------------------------------------------------------------
    const oa::render::Compositor& scene() const;
    /// Counters for the host's diagnostics (layer_ev / tween_ev prints).
    size_t scene_layer_events() const;
    size_t scene_tween_events() const;
    size_t tween_completion_calls() const;
    /// Scene tween timeline advanced every frame inside tick; completion
    /// handlers ([lytween handler=calllua function=...]) are dispatched to Lua
    /// by the runtime.

    /// Is the interpreter parked in a [stop]-class wait (title etc.)?
    bool waiting_stop() const;
    /// Current wait reason when parked (nullptr when executing).
    const oa::runtime::WaitReason* current_wait() const;

    /// Lua-side override status bits (e:overrideKey). Frame edge bookkeeping
    /// happens in tick; bits: 2 isPush 4 isDown 8 downEdge 16 upEdge 32 decide.
    void apply_override(int key, int status);

    // -- transitions --------------------------------------------------------
    /// [trans] 状态的只读投影：语义机本体与其全部写入口都在
    /// 实现面（RuntimeState），宿主只查询"是否在放 / 进度 / 类型 / rule /
    /// vague"；应用 [trans] 事件仍走 transition_begin，渲染仍走
    /// set_transition_capture_callback / mark_transition_captured。返回按值
    /// （一个状态指针），生命周期跟随本 runtime。
    TransitionStatus transition();
    TransitionStatus transition() const;
    /// Monotonic tick clock (ms), the single animation/transition clock.
    uint64_t now_ms() const;

    /// Apply a [trans] event (verbatim params map). type=0 clears; type!=0
    /// starts a capture-needing transition at the current clock. When no
    /// capture callback is installed the capture is treated as completed
    /// immediately (headless hosts have nothing to snapshot); windowed hosts
    /// install capture_callback and mark_transition_captured themselves.
    void transition_begin(const std::map<std::string, std::string>& params);
    /// Host snapshot hook invoked right after a non-zero [trans] begins.
    void set_transition_capture_callback(std::function<void()> cb);
    /// Host finished snapshotting the previous frame into the capture texture.
    void mark_transition_captured();

    // -- [alldelete time=] (whole-scene fade-out + clear) -------------------
    /// Engine-native [alldelete]: every scene layer (message-text overlay
    /// nodes included) fades to 0 over `time_ms` on one global opacity ramp,
    /// then the scene AND the text engine are cleared (FPM's go_title /
    /// go_exit / suspend flows run [alldelete time=1500] to produce the black
    /// transition page; without it the engine-owned text overlays outlived
    /// the Lua-side scene clear and painted story text over the black page).
    /// Script pauses (Stop{id:"alldelete"}) while the fade runs;
    /// time==0 clears instantly without pausing. Hosts multiply every scene
    /// draw (flat and recursive, text included) by all_delete_fade.
    void begin_all_delete(uint64_t time_ms);
    /// Global opacity multiplier for the fade (1.0 when idle).
    double all_delete_fade() const;

    // ------------------------------------------------------------------
    // Pointer dispatch: the runtime owns hit
    // testing + rollover/rollout/click/drag/push Lua dispatch. The host feeds
    // per-tick FrameInput (mouse position, button edges) and provides texture
    // sizes (hit geometry) + pixel alpha sampling for clickablethreshold.
    // ------------------------------------------------------------------

    /// Host hit-provider setter: texture size resolution (layers without
    /// width/height/clip) + texture pixel-alpha sampler (clickablethreshold).
    /// Either may stay empty; empty alpha = layer-alpha fallback.
    void set_hit_providers(oa::render::Compositor::QuadSizeFn size,
                           oa::render::Compositor::AlphaSamplerFn alpha, void* userdata);
    /// One Lua dispatch / handler invocation made by the input chain, for
    /// host diagnostics (the runtime calls Lua itself; observers only print).
    struct PointerDispatch {
        enum class Kind { HoverIn, HoverOut, Click, Push, DragIn, DragMove, DragOut };
        Kind kind = Kind::Click;
        std::string layer;    // layer id ("" for global push)
        std::string function; // Lua function dispatched ("" when none)
        std::string key;      // button key / push key when meaningful
        bool ran_lua = false; // call_function executed (function existed)
    };
    /// Observer for diagnostics (prints etc.); called synchronously when the
    /// runtime dispatches a handler.
    void set_pointer_observer(std::function<void(const PointerDispatch&)> cb);
    /// Current hover set (top + penetration rollover-enabled layers).
    const std::set<std::string>& hovered_layers() const;
    bool is_hovered(const std::string& id) const;
    /// Total Lua dispatches made by the pointer chain (diagnostics/tests).
    size_t pointer_dispatch_count() const;
    /// [mouse] config-tag pointer-warp hook: the host moves the OS pointer to
    /// the requested stage coordinates (SDL warp; SDL then synthesizes real
    /// mouse-motion feedback so rollover/rollout/click dispatch stays
    /// untouched). FPM's exit-confirm dialog uses this — yesno_active ->
    /// mouse_autocursor flies the cursor onto the YES button in eased steps.
    /// The engine pointer stays FrameInput-driven; on hosts that cannot warp
    /// the OS cursor (hidden window / headless) the runtime emulates the move
    /// itself while no fresh raw input arrives (warp-emulation state below).
    void set_pointer_warp_callback(std::function<void(int, int)> cb);
    /// [mouse] warp requests accepted since boot (diagnostics/tests).
    size_t pointer_warp_requests() const;
    /// Current engine pointer in stage coordinates (per-tick FrameInput).
    std::pair<int, int> mouse_point() const;
    // ------------------------------------------------------------------
    // Control-domain queries. Effective flags the wait machine consults.
    // ------------------------------------------------------------------
    bool skip_active() const;
    bool automode_active() const;
    bool hide_active() const;
    bool control_skip_effective() const;
    bool rclick_allowed() const;

    /// [lydrag] forced drag consumed from the event stream.
    void force_pointer_drag(const std::string& layer_id);

    // ------------------------------------------------------------------
    // Message-layer text domain: the runtime consumes the
    // text tags (print/chgmsg/rt/rp/font/... ) into oa::render::TextEngine at
    // dispatch time — they never reach the host event stream
    // (apply_text_event). Hosts read text/text_revision for the
    // glyph layout + static-frame dirty signal.
    // ------------------------------------------------------------------
    const oa::render::TextEngine& text() const;
    oa::render::TextEngine& text();
    /// 文本内容修改版本（宿主静态帧跳过信号；跨帧比较判脏）。
    uint64_t text_revision() const;
    /// 诊断计数（原宿主 text_ev/switch_ev 语义迁移到 runtime）。
    size_t text_events() const;
    size_t message_switch_events() const;
    /// 文本事件应用（返回 true = 已消费，不再进宿主事件流）。
    bool apply_text_event(const oa::runtime::Event& e);

    // ------------------------------------------------------------------
    // Media domain:
    // audio/video interpreter events are consumed at dispatch time into the
    // audio/video engines (apply_media_event), and
    // per-frame advance_media_frame runs the fade clock, decodes playing
    // channels (MediaPlayers), dispatches finish handlers and advances the
    // video logic backend. Hosts never see media events in the event stream.
    // ------------------------------------------------------------------
    oa::media::AudioEngine& audio();
    const oa::media::AudioEngine& audio() const;
    oa::media::VideoEngine& video();
    const oa::media::VideoEngine& video() const;

    // ------------------------------------------------------------------
    // E-mote static layers. e:createEmoteLayer (the real
    // game Lua chain emote.lua -> e:createEmoteLayer) enqueues a lyc2 +
    // "emotestatic" engine tag pair; the runtime decodes the first PSB file,
    // renders the static pose (R1 fit mapping) at the
    // requested canvas size, stores the RGBA frame here and binds the layer
    // to that emote canvas: 层的 LayerContent
    // 状态 = EmoteCanvas;读取域键 = EmoteContent::canvas_key(id),不再有
    // 保留命名空间字符串.Hosts upload that frame once per revision (same
    // pattern as the video layers) so the layer becomes visible.
    // Playback/param APIs (playTimeline etc.) stay engine-side no-ops;
    // only the static pose is rendered.
    // ------------------------------------------------------------------
    struct EmoteLayerState {
        int width = 0, height = 0;
        uint64_t revision = 0; // last composed render revision
        std::string file;      // first psb path
        /// The game drives this player's timeline clock itself
        /// (em:progress calls every vsync — 甜蜜女友3-style frameworks):
        /// advance_emote_players must NOT auto-advance it as well (double
        /// speed). Set on the first explicit progress() dispatch.
        bool host_clock = false;
        /// Frame data owner: the player renders into its own rgba buffer and
        /// the revision above tracks it (hosts upload once per revision).
        std::shared_ptr<oa::emote::EmotePlayer> player;
    };
    const std::map<std::string, EmoteLayerState>& emote_layers() const;
    size_t emote_layer_events() const;
    oa::media::MediaPlayers& media_players();
    /// Override the media asset loader (defaults to the project filesystem +
    /// magic-path resolution). Tests inject synthetic assets here.
    void set_media_loader(
        std::function<std::optional<std::vector<uint8_t>>(const std::string&)> loader);
    /// Media events consumed at dispatch time (diagnostics/tests).
    size_t media_events() const;
    /// Registered media finish handlers dispatched (handlers that produced a
    /// queued tag or jump).
    size_t media_handler_dispatches() const;

    /// Enable the media decode pool (threads < 0: OA_DECODE_THREADS /
    /// platform default; native = several workers, wasm default = 1; 0
    /// disables = decode on the caller's thread). Audio/video decode runs
    /// on pool workers while the tick/rendering thread stays authoritative.
    void enable_decode_pool(int threads = -1);

    // ------------------------------------------------------------------
    // Save domain: the
    // runtime persists engine state through an injected oa::runtime::SaveStore
    // (default NullSaveStore — headless runs are side-effect free). System
    // saves (g./s. domains) live in saveg.dat/system.dat and are loaded at
    // boot; numbered saves are binary (SaveData). The App/tests install a
    // real store; the logical savepath prefix comes from the project ini.
    // ------------------------------------------------------------------
    /// Install the save backend (host/tests). Null store by default.
    void set_save_store(std::shared_ptr<oa::runtime::SaveStore> store);
    const std::shared_ptr<oa::runtime::SaveStore>& save_store() const;
    /// Host frame capture for [takess]/[savess] (RGBA). Absent => savess
    /// writes a deterministic placeholder thumbnail.
    using CaptureFn = std::function<std::optional<std::vector<uint8_t>>(uint32_t*, uint32_t*)>;
    void set_frame_capture(CaptureFn fn);
    /// [takess] defers the actual read to the host's end-of-present
    /// (dispatch runs before this frame's render, so the host captures the
    /// just-drawn frame — the pre-save-dialog scene; the same
    /// pre-UI frame).
    /// Hosts call post_frame_capture right after presenting the frame.
    void post_frame_capture();
    /// Boot-time system save load (called by open_project).
    void sysload();
    /// syssave: persist g./s. domains into saveg.dat/system.dat.
    bool syssave();
    /// Numbered save / load (host/tests may call directly).
    bool save_game_to(const std::string& file);
    /// 读档：返回可判定的结果；失败原子（状态不变）。这是
    /// 权威入口，`load_game_from` 是它的 bool 投影（保留旧签名）。
    LoadResult load_game(const std::string& file, int64_t trans_type);
    /// `load_game(...) == LoadResult::Ok`。
    bool load_game_from(const std::string& file, int64_t trans_type);
    /// Current logical save path prefix (sanitized from the ini SAVEPATH).
    const std::string& savepath() const;
    /// Whether numbered-save files exist (save slot list checks).
    bool save_file_exists(const std::string& file) const;
    /// Drive one save-domain event directly (tests/hosts; identical path to
    /// the interpreter dispatch).
    void apply_save_event(const oa::runtime::Event& e);
    /// Consume one audio/video interpreter event; true = handled.
    bool apply_media_event(const oa::runtime::Event& e);
    /// Per-frame media pump (call once per tick, after wait resolution).
    void advance_media_frame(uint64_t delta_ms);
    /// Playback-end report for one sound channel (EOF from the decode pump,
    /// or a host/sink stop): ends the channel, dispatches its registered
    /// finish handler and releases [wait se=]/[wait voice=] rows. `cat`
    /// picks the space: Bgm = the single BGM slot (id unused), Se/Voice
    /// share the id-keyed channel map.
    void sound_finished(oa::media::SoundCategory cat, const std::string& id);
    /// Release predicate for [wait se=...].
    bool media_se_wait_finished(const std::string& id, bool time_given,
                                uint64_t time_ms) const;

private:
    // 私有状态与实现全部搬到 runtime_internal.h 的 RuntimeState（"小接口，大文件"）：
    // 本类只持有它并对公开方法做薄转发（定义见 runtime.cpp 的对外接口段）。
    // 析构在 runtime.cpp 内定义，这里只需前向声明（unique_ptr 的不完整类型要求）。
    struct RuntimeState;
    std::unique_ptr<RuntimeState> s_;
};

} // namespace oa::runtime
