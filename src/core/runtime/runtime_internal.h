#pragma once
// RuntimeState implementation surface (runtime_game.h → this → runtime*.cpp).
// State buckets / load contracts: docs/RUNTIME_STATE.md.
#include <deque>
#include <memory>

#include "core/render/renderer.h" // RuntimeState owns the RenderEngine directly
#include "core/runtime/runtime_game.h"
// anim/transition 收为 render 内部实现，实现面需要 TweenDone 的完整定义。
#include "core/render/render_internal.h"

namespace oa::runtime {

/// Host input accumulated between ticks (main.cpp converts SDL -> these).
struct HostInput {
    FrameInput frame;
    std::set<int> kbd_down;
    std::set<int> mouse_held;             // 2 right / 3 middle
    std::map<int, int> wheel_hold_frames; // vk -> frames left
    bool left_down = false;
    bool left_pressed = false;
};

struct GameRuntime::RuntimeState {
    // [B] E: injected owner; the host object outlives load.
    GameRuntime* rt_ = nullptr;

    // Domain sub-states; RuntimeState keeps only frame/wait/dispatch glue.
    struct PendingEmoteMethod {
        std::string method;
        std::string s;
        std::string s2;
        double n1 = 0, n2 = 0, n3 = 0;
    };
    struct MediaDomain {
        oa::media::AudioEngine audio_;
        oa::media::VideoEngine video_;
        std::unique_ptr<oa::media::MediaPlayers> media_players_;
        std::unique_ptr<oa::media::DecodePool> decode_pool_;
        std::map<std::string, EmoteLayerState> emote_layers_;
        std::map<std::string, oa::emote::EmotePlayer::ViewState> emote_view_cache_;
        std::map<std::string, EmoteLayerState> parked_emote_;
        std::map<std::string, std::vector<PendingEmoteMethod>> pending_emote_methods_;
        size_t emote_layer_events_ = 0;
        uint64_t voice_serial_ = 0;
        bool video_finished_ = false;
        std::optional<float> last_bgm_volume_;
        std::optional<float> last_se_volume_;
        size_t media_events_ = 0;
        size_t media_handler_dispatches_ = 0;
    };
    struct ScreenshotBuffer {
        bool valid = false;
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };
    struct SaveDomain {
        std::shared_ptr<oa::runtime::SaveStore> save_store_;
        std::string savepath_ = "save";
        ScreenshotBuffer screenshot_;
        CaptureFn capture_fn_;
        bool capture_pending_ = false;
        int autosave_allow_ = 0;
        bool autosaved_current_wait_ = false;
        std::string autosave_wait_sig_;
    };
    struct PointerDomain {
        oa::render::Compositor::QuadSizeFn hit_size_;
        oa::render::Compositor::AlphaSamplerFn alpha_sampler_;
        void* compositor_userdata_ = nullptr;
        std::set<std::string> hovered_;
        std::string drag_layer_;
        double drag_start_mouse_x_ = 0, drag_start_mouse_y_ = 0;
        double drag_origin_left_ = 0, drag_origin_top_ = 0;
        int mouse_prev_x_ = 0, mouse_prev_y_ = 0;
        bool mouse_inited_ = false;
        bool left_down_prev_ = false;
        size_t pointer_dispatch_count_ = 0;
        std::function<void(const PointerDispatch&)> pointer_observer_;
        std::function<void(int, int)> pointer_warp_cb_;
        size_t pointer_warp_requests_ = 0;
        bool pointer_warp_pending_ = false;
        int pointer_warp_x_ = 0, pointer_warp_y_ = 0;
        int raw_mouse_x_ = 0, raw_mouse_y_ = 0;
    };
    struct ControlDomain {
        bool skip_active_ = false;
        bool skip_allowed_ = true;
        bool skip_enabled_ = false;
        bool automode_ = false;
        bool automode_allowed_ = true;
        std::string automode_syncse_;
        uint64_t auto_elapsed_ms_ = 0;
        bool control_skip_pressed_ = false;
        bool control_skip_blocked_ = false;
        bool last_control_skip_effective_ = false;
        bool rclick_allowed_ = false;
        std::string rclick_file_;
        bool hide_allowed_ = false;
        bool hide_active_ = false;
        std::vector<std::string> hide_window_;
        std::map<std::string, bool> hide_snapshot_;
        std::map<int, std::set<int>> keymap_;
        std::string active_wait_icon_;
        bool exskip_active_ = false;
        int script_status_ = 0;
    };
    struct InputDomain {
        std::map<int, int> overrides_;
        std::map<int, int> prev_overrides_;
        std::set<int> keys_down_;
        std::set<int> down_edges_;
        std::set<int> up_edges_;
        int mouse_x_ = 0, mouse_y_ = 0;
        bool decide_edge_ = false;
    };

    // [C] H: 媒体/解码/emote 资源态；读档 reset_domains 清
    MediaDomain media_;
    // [B] H/E: save store / savepath / capture hooks；[C]: screenshot + autosave
    SaveDomain save_;
    // [B] H: hit/alpha providers + observer/warp hooks；[C]: pointer transient
    PointerDomain pointer_;
    // [B] L: config rows (skip/automode/rclick/hide/keymap)；[C]: in-flight flags
    ControlDomain control_;
    // [C] E: per-tick input snapshot
    InputDomain input_state_;

    // Public-method mirrors (signatures match runtime_game.h; docs live there).
    bool exit_requested() const { return exit_requested_; }
    void key_event(int vk, bool down);
    void mouse_move(float window_x, float window_y);
    void mouse_button(int vk, bool down);
    void mouse_wheel(int direction);
    void tick_input(uint64_t delta_ms);
    void finish_frame();
    std::vector<oa::runtime::Event> drain_events() {
        std::vector<oa::runtime::Event> out(pending_events_.begin(), pending_events_.end());
        pending_events_.clear();
        return out;
    }
    oa::runtime::Interpreter& interpreter() { return *interpreter_; }
    const oa::runtime::Interpreter& interpreter() const { return *interpreter_; }
    bool has_interpreter() const { return interpreter_ != nullptr; }
    std::string resolve_magic_path(const std::string& name) const;
    std::optional<std::string> host_variable(const std::string& key) const;
    std::optional<std::vector<uint8_t>> read_save_file(
        const std::string& rel_path) const;
    void bind_text_metrics(
        std::function<std::tuple<double, double, double>()> fn);
    const std::shared_ptr<const oa::fs::IFileSystem>& fs() const { return fs_; }
    oa::render::Compositor& scene() { return renderer_.scene(); }
    const oa::render::Compositor& scene() const { return renderer_.scene(); }
    oa::render::Transition& transition() { return renderer_.transition(); }
    const oa::render::Transition& transition() const { return renderer_.transition(); }
    oa::render::Transition& transition_state() { return renderer_.transition(); }
    size_t scene_layer_events() const { return renderer_.stats().layers; }
    size_t scene_tween_events() const { return renderer_.stats().tweens; }
    size_t tween_completion_calls() const { return renderer_.stats().tween_done; }
    const oa::runtime::WaitReason* current_wait() const { return wait_ ? &*wait_ : nullptr; }
    uint64_t now_ms() const { return now_ms_; }
    bool attach_window(void* native_window, const std::string& backend_kind);
    int stage_width() const { return rt_->project_.config.stage_width; }
    int stage_height() const { return rt_->project_.config.stage_height; }
    bool vsync_active() const { return renderer_.vsync_active(); }
    bool frame_presented() const { return renderer_.frame_presented(); }
    std::optional<std::vector<uint8_t>> capture_frame(uint32_t* w, uint32_t* h) {
        return renderer_.capture_frame(w, h);
    }
    void request_repaint() { renderer_.request_repaint(); }
    void set_transition_capture_callback(std::function<void()> cb) {
        transition_capture_cb_ = std::move(cb);
    }
    void mark_transition_captured() { renderer_.transition().mark_captured(now_ms_); }
    void set_hit_providers(oa::render::Compositor::QuadSizeFn size,
                           oa::render::Compositor::AlphaSamplerFn alpha, void* userdata) {
        pointer_.hit_size_ = std::move(size);
        pointer_.alpha_sampler_ = std::move(alpha);
        pointer_.compositor_userdata_ = userdata;
    }
    void set_pointer_observer(std::function<void(const PointerDispatch&)> cb) {
        pointer_.pointer_observer_ = std::move(cb);
    }
    const std::set<std::string>& hovered_layers() const { return pointer_.hovered_; }
    bool is_hovered(const std::string& id) const { return pointer_.hovered_.count(id) > 0; }
    size_t pointer_dispatch_count() const { return pointer_.pointer_dispatch_count_; }
    void set_pointer_warp_callback(std::function<void(int, int)> cb) {
        pointer_.pointer_warp_cb_ = std::move(cb);
    }
    size_t pointer_warp_requests() const { return pointer_.pointer_warp_requests_; }
    std::pair<int, int> mouse_point() const { return {input_state_.mouse_x_, input_state_.mouse_y_}; }
    bool skip_active() const {
        return control_.skip_allowed_ && (control_.skip_enabled_ || control_skip_effective());
    }
    bool automode_active() const { return control_.automode_allowed_ && control_.automode_; }
    bool hide_active() const { return control_.hide_active_; }
    bool control_skip_effective() const {
        return control_.control_skip_pressed_ && !control_.control_skip_blocked_ && control_.skip_allowed_;
    }
    bool rclick_allowed() const { return control_.rclick_allowed_; }
    const oa::render::TextEngine& text() const { return renderer_.text(); }
    oa::render::TextEngine& text() { return renderer_.text(); }
    uint64_t text_revision() const { return renderer_.text().revision(); }
    size_t text_events() const { return renderer_.stats().text; }
    size_t message_switch_events() const { return renderer_.stats().message_switch; }
    oa::media::AudioEngine& audio() { return media_.audio_; }
    const oa::media::AudioEngine& audio() const { return media_.audio_; }
    oa::media::VideoEngine& video() { return media_.video_; }
    const oa::media::VideoEngine& video() const { return media_.video_; }
    const std::map<std::string, EmoteLayerState>& emote_layers() const {
        return media_.emote_layers_;
    }
    size_t emote_layer_events() const { return media_.emote_layer_events_; }
    oa::media::MediaPlayers& media_players() { return *media_.media_players_; }
    void set_media_loader(
        std::function<std::optional<std::vector<uint8_t>>(const std::string&)> loader) {
        if (media_.media_players_) media_.media_players_->set_loader(std::move(loader));
    }
    size_t media_events() const { return media_.media_events_; }
    size_t media_handler_dispatches() const { return media_.media_handler_dispatches_; }
    void set_save_store(std::shared_ptr<oa::runtime::SaveStore> store) {
        save_.save_store_ = std::move(store);
    }
    const std::shared_ptr<oa::runtime::SaveStore>& save_store() const { return save_.save_store_; }
    void set_frame_capture(CaptureFn fn) { save_.capture_fn_ = std::move(fn); }
    const std::string& savepath() const { return save_.savepath_; }
    void apply_save_event(const oa::runtime::Event& e) {
        handle_save_tag(e);
    }

    // ---- 实现在各域 .cpp 里的对外方法（签名同上）------------------------
    void open_project(std::string_view platform);
    void boot_project();
    void restart_project();
    void dispatch_tween_done(const std::vector<oa::render::TweenDone>& done);
    void tick(uint64_t delta_ms, const FrameInput& input);
    bool waiting_stop() const;
    void apply_override(int key, int status);
    void transition_begin(const std::map<std::string, std::string>& params);
    void begin_all_delete(uint64_t time_ms);
    double all_delete_fade() const;
    void force_pointer_drag(const std::string& layer_id);
    bool apply_text_event(const oa::runtime::Event& e);
    void enable_decode_pool(int threads = -1);
    void post_frame_capture();
    void sysload();
    bool syssave();
    bool save_game_to(const std::string& file);
    LoadResult load_game(const std::string& file, int64_t trans_type);
    bool load_game_from(const std::string& file, int64_t trans_type);
    bool save_file_exists(const std::string& file) const;
    bool apply_media_event(const oa::runtime::Event& e);
    /// 视频源线程产出的纹理先入队，主线程（advance_media_frame）挂到层上，
    /// 场景数据只在主线程被写。
    void queue_host_texture(const std::string& layer_id,
                            oa::render::HostTextureRef tex) {
        std::lock_guard<std::mutex> lk(host_tex_mu_);
        pending_host_tex_[layer_id] = std::move(tex);
    }

    void advance_media_frame(uint64_t delta_ms);
    void sound_finished(oa::media::SoundCategory cat, const std::string& id);
    bool media_se_wait_finished(const std::string& id, bool time_given,
                                uint64_t time_ms) const;
    // [wait se=ID time=N] deadline when no live channel exists.
    mutable std::map<std::string, uint64_t> se_wait_deadline_;

    // Bucket annotations below; full ownership/load contract:
    // docs/RUNTIME_STATE.md. Keep declaration order stable (dtor order).
    void wire_lua_host();
    bool resolve_wait(uint64_t delta_ms, const FrameInput& in, bool clicked);
    void advance_wait();
    /// Park on a Stop wait named by `id` and clear its timer.
    void arm_stop_wait(std::string id);
    /// Fixed order: engine → media → text → scene → control → host stream.
    oa::runtime::CallbackResult dispatch_interpreter_event(
        const oa::runtime::Event& e);
    bool dispatch_engine_event(const oa::runtime::Event& e,
                               oa::runtime::CallbackResult* out);
    bool dispatch_media_event(const oa::runtime::Event& e,
                              oa::runtime::CallbackResult* out);
    /// Pause verdict for host-stream events (wait/reset/trans).
    oa::runtime::CallbackResult host_event_result(const oa::runtime::Event& e) const;
    /// Save/load internals ------------------------------------------------
    void dispatch_save_events();
    void dispatch_pending_events();
    void handle_save_tag(const oa::runtime::Event& e);
    /// Consume [exit]/[reset]/[gotitle] from the pending host stream.
    void dispatch_control_events();
    /// Re-run the boot head with the Lua VM kept; full restart on failure.
    void reboot_boot_keep_vm(const char* reason);
    /// Clear engine domains; does not touch the interpreter.
    void reset_domains();
    void handle_file_operation(const std::map<std::string, std::string>& params);
    /// Materialize/bind the active message layer's scene node.
    void ensure_active_message_node();
    void handle_save_screenshot(const std::string& file, const std::string& width,
                                const std::string& height);
    /// Render one PSB static pose and bind it to its carrier layer.
    void apply_emote_static(const oa::runtime::Event& e);
    /// Advance emote player clocks and re-render throttled poses.
    void advance_emote_players(uint64_t delta_ms);
    /// Route an EmoteLayer method call; queue it if the layer is not ready.
    bool emote_method_dispatch(const std::string& id, const std::string& method,
                               const std::string& s, const std::string& s2,
                               double n1, double n2, double n3, double* out,
                               std::string* sout);
    std::string qualify_save_file(const std::string& file) const;
    void maybe_autosave_for_wait();
    // Load hardening (implementation: runtime_save.cpp).
    /// Clear the [C] ephemeral subset before restoring the scene snapshot.
    size_t reset_ephemeral_on_load();
    /// Preflight the saved script position; failure is atomic.
    bool preflight_load_position(const oa::runtime::SaveData& data, std::string* why);
    /// Log-only post-decode sanity check; returns the issue count.
    size_t self_check_save_data(const oa::runtime::SaveData& data) const;
    /// Bounded tag-queue drain; logs and counts guard hits.
    size_t drain_tag_queue_bounded(const char* phase);
    // [C] E: 读档自检问题计数（诊断；只累加，不改变行为）
    size_t load_self_check_issues_ = 0;
    // [C] E: 有界队列排空命中 guard 的次数（诊断）
    size_t tag_drain_truncations_ = 0;
    /// [alldelete] completion (fade clock lives in RenderEngine).
    void finish_all_delete();
    /// Media internals -----------------------------------------------------
    void finish_sound(bool bgm, const std::string& id);
    void finish_video(const std::string& id);
    void dispatch_media_handler(const oa::media::SoundFinishHandler& h);
    void dispatch_media_handler(const oa::media::VideoFinishHandler& h);
    void sync_system_audio_volumes();
    void set_bgm_loop_file(const std::string& file, bool loop_play);
/// get_sound_info snapshot: BGM + SE bus.
    oa::runtime::InterpreterHooks::SoundInfoSnap sound_info_snapshot_for_hook() const;
    /// Apply a scene event; true = consumed (never reaches the host stream).
    bool apply_scene_event(const oa::runtime::Event& e);
    /// Control/config rows and [lydrag]; runs after the scene domain.
    bool apply_control_event(const oa::runtime::Event& e);
    /// [lyc2]: merge `<file>.csv` placement keys unless explicit.
    void apply_lyc2_csv(std::map<std::string, std::string>* props);
    /// R6 order: reveal → scene → emote → click-wait icon → media.
    void advance_content_planes(uint64_t delta_ms);
    /// Advance tweens and deliver finished handlers.
    void advance_scene_frame();
    /// Deferred handler-tween completions; delivered only while their layer
    /// survives the frame (cancelled and natural finishes use the same rule).
    struct PendingTweenCancel {
        std::string id;                        // tween target layer id
        const oa::render::Layer* node = nullptr; // layer identity at cancel
        std::vector<oa::render::TweenDone> done;
    };
    // [C] E: 在飞 tween 完成投递；元素含 const Layer*（场景清场后悬垂）——读档须 clear
    std::vector<PendingTweenCancel> pending_tween_cancels_;
    void flush_pending_tween_cancels();
    /// Drop deferred completions on scene teardown.
    void drop_pending_tween_cancels();
    /// Run until a park; map trans/video/alldelete/sync-tween waits.
    void run_until_wait();
    /// Drain queued tags while parked; may replace or clear `wait_`.
    void drain_parked_queue();
    /// Hit test → hover diff → click/drag/push; reports claimed edges.
    struct PointerConsumption {
        bool layer_handled = false;   // a click-handler row dispatched
        bool drag_handled = false;    // a drag chain ran on this edge
        bool left_push_handled = false; // global push key 1 dispatch handled
        bool role_advance = false;    // a keyconfig role-0 key was pressed
        bool input_swallowed = false; // hide-mode click recovery consumed it
    };
    PointerConsumption process_pointer_input(const oa::runtime::FrameInput& in);
    /// Run one registered row through filter + calllua; true = handled.
    /// row 按值传：下面的 Lua 回调可以重新注册/删除 handler，寄存器里的
    /// 那一行随时会失效（fuzz 抓到的 heap-use-after-free）。
    bool dispatch_layer_row(const std::string& id, oa::render::LayerEventHandler row,
                            const std::string& event_type,
                            const std::vector<std::pair<std::string, std::string>>& runtime_params,
                            PointerDispatch::Kind kind);
    bool dispatch_input_row(const std::string& filter_name,
                            oa::render::InputHandler row,
                            const std::vector<std::pair<std::string, std::string>>& runtime_params,
                            PointerDispatch::Kind kind);
    /// drag begin/continue/finish.
    bool drag_begin(const std::string& layer_id, bool forced);
    void drag_update(double mx, double my);
    void drag_end();
    void notify_pointer(const PointerDispatch& d);
    /// config-tag application shared by apply_scene_event + tests.
    bool apply_control_config(const oa::runtime::Event& e);
    /// Synthetic return frame for a parked input helper that queues tags
    /// (single marker, never nested). Lifecycle: arm → settle → refresh.
    struct InlineEventFrame {
        std::string script;
        size_t line = 0;
        std::vector<oa::runtime::CallFrame> stack; // stack snapshot at push
        bool claimed_by_jump = false;
    };
    void refresh_inline_event_frame();
    void begin_inline_event_frame();
    void settle_inline_event_frame(bool paused, bool has_queued, bool saw_call,
                                   bool saw_jump);
    bool inline_event_marker_active() const;

    // [B] H: 宿主注入的文件系统；读档保留
    std::shared_ptr<const oa::fs::IFileSystem> fs_;
    // Render state (scene/text/transition/textures) lives in render.
    oa::render::RenderEngine renderer_;
    // 视频源线程 → 主线程的纹理挂载队列（场景数据只在主线程写）
    std::mutex host_tex_mu_;
    std::map<std::string, oa::render::HostTextureRef> pending_host_tex_;
    // [C] H: 资源自然尺寸探测缓存（TextureKey → {w,h}；未知文件缓存 {0,0}）
    std::map<oa::render::TextureKey, std::pair<double, double>> image_size_cache_;
    std::pair<double, double> bound_image_size(const oa::render::Layer& l);
    // [B] E: open_project 的平台串；[reset] 重启复用
    std::string platform_;
    // [C] E: 本 tick 是否有新玩家输入（决定停驻队列是否放行）
    bool input_this_tick_ = false;
    // [B] E+L: 宿主重建 VM（boot）；读档只 restore_position + Lua restore()
    std::unique_ptr<oa::runtime::Interpreter> interpreter_;
    // [B] E: 读档后由恢复位置的那一行重跑重导出
    std::optional<oa::runtime::WaitReason> wait_;
    // [C] E: 帧号（e:getFrameNumber）
    uint64_t tick_count_ = 0;
    // [C] E: wait_ 的派生量；读档清零
    uint64_t wait_remaining_ms_ = 0;
    // [C] E: 本 tick 已武装的 wait 至少存活一帧
    bool wait_armed_this_tick_ = false;
    // [B] E: [exit] 控制闩；reset_domains 清
    bool exit_requested_ = false;
    // [C] E: 单调引擎时钟（绝不进档）
    uint64_t now_ms_ = 0;

    // Control-domain private machinery
    void set_skip_mode(bool enabled);
    void set_automode_mode(bool enabled);
    /// Automode interval (s.automodewait; 900 ms fallback).
    uint64_t automode_wait_ms() const;
    /// automode voice gate — any channel listed in automode_syncse_ playing.
    bool automode_sync_blocked() const;
    // FPM e:debugSkip exskip fast-forward state machine.
    void start_debug_skip();
    void end_debug_skip();
    void update_control_skip_hold(const std::set<int>& keys_down);
    void dispatch_mode_input(const std::string& event_name);
    void reveal_all_text();
    bool key_is_toggle(int key, int role_out) const;
    bool has_role(int role, int key) const;
    bool handle_role_key_edge(int key);
    void apply_keyconfig(const std::map<std::string, std::string>& params);
    void apply_rclick_config(const std::map<std::string, std::string>& params);
    void apply_hide_config(const std::map<std::string, std::string>& params);
    void apply_skip_config(const std::map<std::string, std::string>& params);
    void apply_automode_config(const std::map<std::string, std::string>& params);
    void apply_exec_command(const std::map<std::string, std::string>& params);
    void apply_mouse_config(const std::map<std::string, std::string>& params);
    bool trigger_rclick();
    void toggle_hide_mode();
    void exit_hide_mode();
    void apply_hide_visibility(bool hidden);
    /// [glyph] click-wait icon per-frame driving; no-op unless configured.
    void advance_click_wait();

    // [C] H: 宿主事件泵队列（读档按入口快照迭代，不就地清）
    std::deque<oa::runtime::Event> pending_events_;
    // [B] H: 宿主捕获钩子；读档保留
    std::function<void()> transition_capture_cb_;
    // [B] E: 合成 CallFrame 标记（script/line/stack 快照）；读档 reset
    std::optional<InlineEventFrame> inline_event_frame_;
    // [C] E: [lytween sync=1] 停驻目标层；读档 reset
    std::optional<std::string> pending_sync_tween_layer_;
    // [C] E: drain_parked_queue 重入闩（排空期间的 lytween 不武装停驻）
    bool in_parked_drain_ = false;
    // [C] E: 宿主输入泵（边沿在每次 tick 后清）
    HostInput input_;
    // message-layer text domain lives in renderer_.text().

    /// 构造：注入反指与文件系统（原 GameRuntime 构造函数的初始化列表）。
    RuntimeState(GameRuntime* rt, std::shared_ptr<const oa::fs::IFileSystem> fs);
    ~RuntimeState();
};

} // namespace oa::runtime
