#pragma once
// runtime_internal.h —— GameRuntime 的**内部实现面**（"小接口，大文件"）。
//
// 分层（每层只 include 上一层）：
//   runtime.h            对外契约：公开类型 + 公开方法声明。调用方只 include 它。
//   runtime_internal.h   实现面：RuntimeState（状态 + 实现方法）。只有
//                        src/core/runtime/*.cpp 允许 include。
//   runtime.cpp          公开薄转发（对外接口段）+ 内核/控制域实现
//   runtime_media.cpp    媒体域实现   runtime_save.cpp  存档域实现
//
// RuntimeState 是 GameRuntime 的私有嵌套类型：外部 TU 既不能命名它、也访问不到它的
// 成员，因此这里用 struct（成员可见性对封装无影响）。公开方法在 RuntimeState 里各有
// 一份实现镜像（见下面的"对外方法的实现镜像"段）；GameRuntime 的同名方法只是
// `return s_->f(...)`（runtime.cpp 的对外接口段）。
// 公开头里的内联体已逐字搬入镜像段，公开头只留签名 —— 因此 56 个调用方一行不改。
//
// "原 GameRuntime private 段"一节是**逐字搬移**：成员声明顺序不变 ⇒ 构造/析构顺序
// 逐位不变。
// ===========================================================================
// 维护契约（改本文件前必读）
// ---------------------------------------------------------------------------
// RuntimeState 的每个成员都属于下列**三桶之一**，且只有一个恢复责任人。
// 新增/修改字段：必须在声明前补 `// [桶]` 标注 + 恢复责任人；缺标注 = 不通过。
//
//   [A] Serializable（进存档，可跨进程恢复）
//       —— A 面数据 = SaveData（runtime_save.h 手写二进制编解码）
//          + sys 域（saveg.dat/system.dat）。**本类没有自有 A 字段**：A 面一律
//          以"组件投影"形式存在，投影源 = interpreter_（local 变量域 /
//          current_script / current_line / call_stack）、scene_（LayerSnap +
//          root_props）、audio_（AudioSnap）；采集/回放在 runtime_save.cpp
//          (collect_* / restore_*)，不在本类里。
//       硬约束：A 面只放纯数据（值 / 字符串 / 属性表 / id）。
//          ★ 不得放句柄、指针、资源对象（纹理、播放器、引擎实例、线程）——
//            它们进不了存档，跨进程也无意义。
//          ★ 层 / 节点 **id 属数据**，允许进 A；但凡 A 面（含磁盘上的旧档）
//            里的 id，恢复路径**必须容忍"读档后该 id 不存在"**：显式重建，
//            或显式判空跳过；禁止经 set_props 这类"缺失即物化"的入口回写
//            （会凭空造出幽灵层）。
//       责任人：engine（runtime_save.cpp 的 save/load 面）+ Lua onLoad 的确定性
//          回填（framework 自管表：FPM fileio.lua store()/restore()）。
//
//   [B] RebuiltOnLoad（不进存档；重放 / 重建，或按已证规则"特判保留"）
//       入口默认收紧：新字段默认归 A；归 B 必须同时给出
//         ① 一条**已验证**的重建/保留路径（代码位置 + 测试或旅程证据），
//         ② 确定性前提（同一 script position 重放得到同一状态）。
//       反例：全局输入注册表（setonpush 行）重放不足——
//         quickload 尾巴从不重发键行，只能"清场景前保留现场 + 场景恢复后
//         原样回写"；这是 B 的"特判保留"子类唯一合法形态。
//       责任人三选一，禁止双写：engine reset（runtime 自己重建/保留）/
//         Lua onLoad（framework 回填链）/ host（宿主重启资源后回写）。
//
//   [C] Ephemeral（瞬态 / 资源态；读档一律重置，或由宿主/Lua 重启）
//       —— 在飞动画与等待计时、指针与按键瞬时态、媒体与解码器、纹理与
//          GPU/组烘焙缓存、事件泵缓存、诊断计数。
//       责任人：engine reset（= load_game_from / reset_domains 的显式清单）
//         或 host（重新泵帧 / 重建纹理）。C 桶成员不得被 A 面引用。
//
// 顺序契约（改 load 顺序前必读；两处都是既有实现依赖）：
//   1) B-preserve 的**回写**必须发生在场景快照恢复**之后**
//      （输入注册表在 clear_scene 前取现场、restore_scene_snapshot 后回写）。
//   2) Lua onLoad（restore()）跑在位置恢复之后、引擎音频快照恢复之前
//      （runtime_save.cpp load_game_from：restore() → 队列排空 → 音频快照），
//      framework 在 onLoad 里排的 tag 会先于引擎音频快照生效。
//
// 读档失败契约：`load_game_from` / `load_game` 的失败是
//   **原子的**——位置预检（当前脚本可载入 + 行号在界内）在**任何清场之前**完成；
//   预检失败 ⇒ 运行时状态一个字节都不动。函数返回后若成功，才允许出现
//   "已清场 + 已恢复" 的组合。禁止"场景已清、快照已灌、位置恢复失败"的半恢复。
//
// 在飞动画契约（冻结语义）：tween / [anime] / transition 的**在飞
//   状态**属 [C]，不进存档，也不在读档后重放或清零重算：`clear_scene()` 随节点
//   销毁 tween/anime 桶，恢复出来的层只带存档时刻的 props（= 动画中途的姿态）。
//   ⇒ 读档后动画**冻结在存档时刻**（既不续播也不回退到起点）；重放无源数据，
//   补建动画属于新特性（反过度设计红线）。等待侧同理：`pending_tween_cancels_`
//   与 `pending_sync_tween_layer_` 在读档清场里丢弃。
//
// interpreter tag 队列契约（② 特判保留）：
//   读档**不清** `Interpreter::tag_queue_`（它不属于任何桶的"重置面"）：
//   ① 队列是"点击 → estag 链 → [load]"的同一 tick 残留，清它 = 丢掉触发读档那一步
//      自己排的收尾 tag；
//   ② FPM 的重建链依赖它——`restore()` 末尾排 `tag{"call", ui.asb, load_next}`，
//      由读档后的排空（runtime_save.cpp 有界排空）执行，这是 UI 重建的唯一入口。
//   顺序：队列在**位置恢复之后**排空（排空里的 jump/call 覆盖恢复位置是 FPM 既定
//   行为，见 load_next）；旧队列与新队列按 FIFO 并存。
//
// SaveData 布局契约：
//   任何字段增删改 → 同步 bump oa::runtime::kSaveFormatVersion（runtime_save.h，
//   必须与 oa::util::kFormatVersion 一致）；旧档只做"补默认值 + 失效 id
//   容错"，不写迁移器（反过度设计红线）。
//   版本门与未来跨版本位（当前处于存档定义阶段，格式未
//   冻结 ⇒ 跨版本兼容**不做**）：`oa::runtime::check_version` 是唯一的版本分派点
//   （新于本版本 → FormatError；旧于本版本 → 按缺省值读，不迁移）。未来做跨版本
//   时只在这一点加分派，不改 decode 的字段读取顺序。
// ===========================================================================
#include <memory>

#include "core/runtime/runtime.h"
// anim/transition 收为 render 内部实现，实现面（本源文件 +
// 各域 .cpp）需要它们的完整定义：RuntimeState 直接持有 oa::render::Transition，
// 并按值搬运 std::vector<oa::render::TweenDone>。公开头一律不 include 本头。
#include "core/render/render_internal.h"

namespace oa::runtime {

struct GameRuntime::RuntimeState {
    /// 反指宿主（GameRuntime 构造时注入）：公开面 project_ 等经它访问。
    // [B] E: 构造注入的反指；读档不动（宿主对象活过读档）
    GameRuntime* rt_ = nullptr;

    // ======================================================================
    // 对外方法的实现镜像
    //    签名与 runtime.h 的公开声明逐字一致（文档注释见 runtime.h，此处不重复）；
    //    原公开头里的内联体原样保留在这里。
    // ======================================================================
    bool exit_requested() const { return exit_requested_; }
    std::vector<oa::runtime::Event> drain_events() {
        std::vector<oa::runtime::Event> out(pending_events_.begin(), pending_events_.end());
        pending_events_.clear();
        return out;
    }
    oa::runtime::Interpreter& interpreter() { return *interpreter_; }
    const oa::runtime::Interpreter& interpreter() const { return *interpreter_; }
    const std::shared_ptr<const oa::fs::IFileSystem>& fs() const { return fs_; }
    const oa::render::Compositor& scene() const { return scene_; }
    size_t scene_layer_events() const { return scene_layer_events_; }
    size_t scene_tween_events() const { return scene_tween_events_; }
    size_t tween_completion_calls() const { return tween_completion_calls_; }
    const oa::runtime::WaitReason* current_wait() const { return wait_ ? &*wait_ : nullptr; }
    uint64_t now_ms() const { return now_ms_; }
    void set_transition_capture_callback(std::function<void()> cb) {
        transition_capture_cb_ = std::move(cb);
    }
    void mark_transition_captured() { transition_.mark_captured(now_ms_); }
    void set_hit_providers(oa::render::Compositor::QuadSizeFn size,
                           oa::render::Compositor::AlphaSamplerFn alpha, void* userdata) {
        hit_size_ = std::move(size);
        alpha_sampler_ = std::move(alpha);
        compositor_userdata_ = userdata;
    }
    void set_pointer_observer(std::function<void(const PointerDispatch&)> cb) {
        pointer_observer_ = std::move(cb);
    }
    const std::set<std::string>& hovered_layers() const { return hovered_; }
    bool is_hovered(const std::string& id) const { return hovered_.count(id) > 0; }
    size_t pointer_dispatch_count() const { return pointer_dispatch_count_; }
    void set_pointer_warp_callback(std::function<void(int, int)> cb) {
        pointer_warp_cb_ = std::move(cb);
    }
    size_t pointer_warp_requests() const { return pointer_warp_requests_; }
    std::pair<int, int> mouse_point() const { return {mouse_x_, mouse_y_}; }
    bool skip_active() const {
        return skip_allowed_ && (skip_enabled_ || control_skip_effective());
    }
    bool automode_active() const { return automode_allowed_ && automode_; }
    bool hide_active() const { return hide_active_; }
    bool control_skip_effective() const {
        return control_skip_pressed_ && !control_skip_blocked_ && skip_allowed_;
    }
    bool rclick_allowed() const { return rclick_allowed_; }
    const oa::render::TextEngine& text() const { return text_; }
    oa::render::TextEngine& text() { return text_; }
    uint64_t text_revision() const { return text_.revision(); }
    size_t text_events() const { return text_events_; }
    size_t message_switch_events() const { return message_switch_events_; }
    oa::media::AudioEngine& audio() { return audio_; }
    const oa::media::AudioEngine& audio() const { return audio_; }
    oa::media::VideoEngine& video() { return video_; }
    const oa::media::VideoEngine& video() const { return video_; }
    const std::map<std::string, EmoteLayerState>& emote_layers() const {
        return emote_layers_;
    }
    size_t emote_layer_events() const { return emote_layer_events_; }
    oa::media::MediaPlayers& media_players() { return *media_players_; }
    void set_media_loader(
        std::function<std::optional<std::vector<uint8_t>>(const std::string&)> loader) {
        if (media_players_) media_players_->set_loader(std::move(loader));
    }
    size_t media_events() const { return media_events_; }
    size_t media_handler_dispatches() const { return media_handler_dispatches_; }
    void set_save_store(std::shared_ptr<oa::runtime::SaveStore> store) {
        save_store_ = std::move(store);
    }
    const std::shared_ptr<oa::runtime::SaveStore>& save_store() const { return save_store_; }
    void set_frame_capture(CaptureFn fn) { capture_fn_ = std::move(fn); }
    const std::string& savepath() const { return savepath_; }
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
    void advance_media_frame(uint64_t delta_ms);
    void sound_finished(oa::media::SoundCategory cat, const std::string& id);
    bool media_se_wait_finished(const std::string& id, bool time_given,
                                uint64_t time_ms) const;

    // ======================================================================
    // 原 GameRuntime private 段（逐字搬移）
    // ======================================================================
    // ---------------------------------------------------------------------
    // 三桶分区图 ——**注解分区，不是内存布局分区**
    //   物理顺序 = 逐字搬移的原 private 段，构造/析构顺序依赖
    //   它：禁止重排、禁止把桶固化成具名子结构。桶只以 `// [X]` 逐字段注解
    //   + 本图表达。标注口径 = **读档时该成员的恢复方式**：
    //     [A] 随存档往返（本类无自有 A 字段：A 面全部是组件投影，见文件头）；
    //     [B] 重放/重建/特判保留（不写进存档）；[C] 重置或由宿主重启。
    //   A 面投影源（字段 → SaveData 键，均在 runtime_save.cpp 采集/回放）：
    //     interpreter_ → local_variables / current_script / current_line /
    //                    call_stack        （save_game_to）
    //     scene_       → scene{root_props, layers} （collect_scene_snapshot）
    //     audio_       → audio{bgm, se, voice}     （collect_audio_snapshot）
    //   ★ 读档清场清单（[C] 桶）见 reset_ephemeral_on_load；改
    //     load_game_from / reset_domains 任一处的清场集合时，两处必须对齐。
    // ---------------------------------------------------------------------
    void wire_lua_host();
    bool resolve_wait(uint64_t delta_ms, const FrameInput& in, bool clicked);
    void advance_wait();
    /// Park the script on a Stop wait (the id names the reason) and clear the
    /// timer — the shared arming step of every Stop park in run_until_wait /
    /// drain_parked_queue (seven identical 4-line blocks are merged here).
    void arm_stop_wait(std::string id);
    /// Save/load internals ------------------------------------------------
    void dispatch_save_events();
    void handle_save_tag(const oa::runtime::Event& e);
    /// Control-event dispatch ([reset]/[gotitle]/[exit]): consumes
    /// engine control events from the pending host stream after the script
    /// phase each tick. [exit] raises exit_requested_; [reset] restarts the
    /// project
    /// (fresh interpreter + boot); [gotitle] clears the domains and starts
    /// the BOOT script at its *title label.
    void dispatch_control_events();
    /// Clear the engine-owned domains a [reset]/[gotitle] overwrites (shared
    /// by restart_project and the GoTitle path). Does NOT touch the
    /// interpreter.
    void reset_domains();
    void handle_file_operation(const std::map<std::string, std::string>& params);
    /// lazily materialize the active message layer's scene node
    /// (binding + revive) when content arrives before any chgmsg switch —
    /// keeps the invariant "every drawable message owns a node", the single
    /// prerequisite for text painting at the node slot only.
    void ensure_active_message_node();
    void handle_save_screenshot(const std::string& file, const std::string& width,
                                const std::string& height);
    /// render the static pose of one emote PSB and bind it
    /// to its carrier layer (e:createEmoteLayer -> "emotestatic" engine tag).
    void apply_emote_static(const oa::runtime::Event& e);
    /// advance every emote player clock (timeline frames)
    /// and re-render throttled pose changes.
    void advance_emote_players(uint64_t delta_ms);
    /// route an EmoteLayer Lua method call to the layer's player; when
    /// the layer does not exist yet (methods called right after
    /// e:createEmoteLayer), the call is queued and replayed once the layer
    /// materializes. `s2` is the second string argument (setVariableDiff's
    /// dstLabel), `sout` the string result of getVariableLabelAt.
    bool emote_method_dispatch(const std::string& id, const std::string& method,
                               const std::string& s, const std::string& s2,
                               double n1, double n2, double n3, double* out,
                               std::string* sout);
    struct PendingEmoteMethod {
        std::string method;
        std::string s;
        std::string s2;
        double n1 = 0, n2 = 0, n3 = 0;
    };
    // [C] E: emote 方法补发队列；随 emote 层存亡——读档须与 emote_layers_ 同清
    std::map<std::string, std::vector<PendingEmoteMethod>> pending_emote_methods_;
    std::string qualify_save_file(const std::string& file) const;
    void maybe_autosave_for_wait();
    // ---------------------------------------------------------------------
    // 读档加固（实现全在 runtime_save.cpp）
    // ---------------------------------------------------------------------
    /// 读档清场：[C] 桶 engine 清单的**读档子集**（= reset_domains 的 C 面
    /// + 读档专属裁定）。必须在
    /// `restore_scene_snapshot` **之前**调用（② 桶回写契约在先清后恢复）。
    /// 返回清掉的条目数（诊断/测试）。
    size_t reset_ephemeral_on_load();
    /// 读档**预检**：当前脚本可载入（失败 = 返回 false，读档整体失败且状态不动）。
    /// 行号越界**不**算失败：解释器把它当"脚本结束"（行为有定义），只打报告。
    /// 只在预检通过后才允许清场 ⇒ 失败是原子的（运行时一字节不动）。
    /// `why` 收失败原因（可空）。
    bool preflight_load_position(const oa::runtime::SaveData& data, std::string* why);
    /// A 面（SaveData）decode 后自检：层 id 合法性/重复、空场景/空音频/空脚本、
    /// call_stack 空 script、空音频 file、旧保留命名空间 file 前缀（记录用）。
    /// **只 log 不 throw**（旧档/手写档容忍）；返回问题条目数。
    size_t self_check_save_data(const oa::runtime::SaveData& data) const;
    /// 有界 tag 队列排空（guard = kTagDrainGuard）：返回轮数；命中 guard 时打
    /// 一条可观测日志并累加 tag_drain_truncations_（不再静默截断）。
    size_t drain_tag_queue_bounded(const char* phase);
    // [C] E: 读档自检问题计数（诊断；只累加，不改变行为）
    size_t load_self_check_issues_ = 0;
    // [C] E: 有界队列排空命中 guard 的次数（诊断）
    size_t tag_drain_truncations_ = 0;
    // [B] E: open_project 建立的会话配置（[reset] 重启复用）；读档保留
    std::string savepath_ = "save";
    // [B] H: 宿主注入的存档 Store；读档保留
    std::shared_ptr<oa::runtime::SaveStore> save_store_;
    // [B] L: [autosave allow] 脚本配置；读档保留（无 fixture 证据）
    int autosave_allow_ = 0;
    // [C] E: 单个 wait 的自动存档去重闩
    bool autosaved_current_wait_ = false;
    // [C] E: 同上（wait 签名）
    std::string autosave_wait_sig_;
    struct ScreenshotBuffer {
        bool valid = false;
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };
    // [C] H: 宿主截图缓冲（资源：w/h/rgba）
    ScreenshotBuffer screenshot_;
    // [B] H: 宿主取帧钩子（渲染器重建后必须重挂）；读档保留
    CaptureFn capture_fn_;
    // [C] E: 在飞取帧请求；读档须清
    bool capture_pending_ = false;
    /// [alldelete] state ---------------------------------------------------
    // [C] E: [alldelete] 淡出时钟；读档须清——否则淡出到期 finish_all_delete 会抹掉刚恢复的场景
    uint64_t alldelete_start_ms_ = 0;
    // [C] E: 同上（淡出时长）
    uint64_t alldelete_duration_ms_ = 0;
    // [C] E: 同上（在飞标志）
    bool alldelete_active_ = false;
    void finish_all_delete();
    /// Media internals -----------------------------------------------------
    void finish_sound(bool bgm, const std::string& id);
    void finish_video(const std::string& id);
    void dispatch_media_handler(const oa::media::SoundFinishHandler& h);
    void dispatch_media_handler(const oa::media::VideoFinishHandler& h);
    void sync_system_audio_volumes();
    void set_bgm_loop_file(const std::string& file, bool loop_play);
/// e:var system=get_sound_info host snapshot (misc-B):
/// BGM + SE bus; voice channels not
/// surfaced unless riding the SE bus via ":vo/" files.
    oa::runtime::InterpreterHooks::SoundInfoSnap sound_info_snapshot_for_hook() const;
    // [B] E: A 面投影源（AudioSnap）；读档 = stop_all_sounds + restore_audio_snapshot 重放；解码/淡出时钟等非投影部分归 [C]
    oa::media::AudioEngine audio_;
    // [C] H: 无 A 面投影；读档须 stop_all_videos
    oa::media::VideoEngine video_;
    // [C] H: 解码播放器（线程/设备）；读档 stop_all
    std::unique_ptr<oa::media::MediaPlayers> media_players_;
    // Decode pool (audio/video worker host). Declared BEFORE the media
    // engines so it is destroyed LAST: the engines retire their workers in
    // their destructors, then the pool joins them. Null/0 threads = all
    // decoding stays on the caller's thread (headless/tests unchanged).
    // [C] H: 解码线程池；宿主 enable_decode_pool 启停，读档不动
    std::unique_ptr<oa::media::DecodePool> decode_pool_;
    // [C] E: emote 播放器（资源）+ 层 id；读档须 clear
    std::map<std::string, EmoteLayerState> emote_layers_;
    // [C] E: 诊断计数
    size_t emote_layer_events_ = 0;
    // [C] E: 语音 id 发生器（单调；读档不恢复）
    uint64_t voice_serial_ = 0;
    // [C] E: 视频结束闩（全屏 EOF → 释放任意 Stop 停驻）；读档清
    // （reset_domains 也漏了它，见文件头 [C] 清单对齐契约）
    bool video_finished_ = false;
    // [C] E: 音量同步缓存（最后写入值）
    std::optional<float> last_bgm_volume_;
    // [C] E: 音量同步缓存（最后写入值）
    std::optional<float> last_se_volume_;
    // [C] E: 诊断计数
    size_t media_events_ = 0;
    // [C] E: 诊断计数
    size_t media_handler_dispatches_ = 0;
    /// Apply one interpreter event to the scene at dispatch time. Returns true
    /// when the event belongs to the compositor boundary and was consumed.
    /// Host event stream never sees these.
    bool apply_scene_event(const oa::runtime::Event& e);
    /// "内容在场面"的每帧推进唯一
    /// 逐面序列(reveal → scene 轨 → emote 双时钟之一 → click-wait 图标 →
    /// 媒体帧末;相位保真红线 R6 —— 顺序逐帧不变,重排即改动画
    /// 相位)。每帧由 tick 调用一次;宿主上传泵面(emote/video)不在此序列
    /// (main.cpp tick 后、渲染前)。
    void advance_content_planes(uint64_t delta_ms);
    /// End-of-frame scene bookkeeping: advance every tween to now_ms_ and
    /// dispatch finished handlers to Lua. Runs on every tick, waiting or not.
    void advance_scene_frame();
    /// handler-tween completions (script-timer rounds) end
    /// cleanly only while the tween's layer survives. A [lytweendel]-cancelled
    /// tween that still carries a completion handler ends its round (FPM
    /// sample preview refills after a mid-round slider drag); a naturally
    /// finished one must deliver exactly the same way. Neither is delivered
    /// when the frames in between tore the tween's layer down — config page
    /// switches run config_delsample() (lytweendel) BEFORE the csvbtn3
    /// [lydel2 500]+rebuild, and a completion delivered after that would
    /// reprint the sample text onto the NEXT page (snll/NekoMiko sample
    /// bleed onto every config page — cancelled rounds; a
    /// natural finish racing the switch would reprint + rearm a fresh round
    /// on the new page). Both are deferred to a frame-end scene
    /// advance and dropped when the layer no longer exists there;
    /// same-frame same-page cancels (FPM r10e drag) and normal page-2
    /// rounds keep their layer and deliver as before (natural finishes one
    /// frame later).
    struct PendingTweenCancel {
        std::string id;                        // tween target layer id
        const oa::render::Layer* node = nullptr; // layer identity at cancel
        std::vector<oa::render::TweenDone> done;
    };
    // [C] E: 在飞 tween 完成投递；元素含 const Layer*（场景清场后悬垂）——读档须 clear
    std::vector<PendingTweenCancel> pending_tween_cancels_;
    void flush_pending_tween_cancels();
    /// Drop deferred tween completions on a scene teardown ([reset]/
    /// [gotitle]/[alldelete] wipe every node; the flush check would drop
    /// them anyway, this keeps the bookkeeping empty).
    void drop_pending_tween_cancels();
    /// Run the interpreter until it parks (mirroring tick's wait mapping,
    /// incl. [trans] -> Stop{trans}, and arming the
    /// [lytween sync=1] Stop{tween:} park when the script completes without
    /// parking itself).
    void run_until_wait();
    /// Drain the interpreter tag queue while parked; may replace/clear wait_.
    /// Runs on every parked tick with a non-empty queue — the interpreter
    /// drains the
    /// queue in every wait state, including [stop]/[trans]
    /// parks (FPM title interactions queue tags while parked). A queued wait
    /// replaces wait_; a
    /// queued jump/call/return moves the interpreter position and clears wait_.
    void drain_parked_queue();
    /// Pointer chain: hit test -> hover set diff ->
    /// click/drag/push dispatch. Returns which dispatches claimed the
    /// left-down edge (for the `clicked` formula).
    struct PointerConsumption {
        bool layer_handled = false;   // a click-handler row dispatched
        bool drag_handled = false;    // a drag chain ran on this edge
        bool left_push_handled = false; // global push key 1 dispatch handled
        bool role_advance = false;    // a keyconfig role-0 key was pressed
        bool input_swallowed = false; // hide-mode click recovery consumed it
    };
    PointerConsumption process_pointer_input(const oa::runtime::FrameInput& in);
    /// Run one registered row through the event filter + calllua semantics.
    /// Returns true when the dispatch "handled" the event (filter verdict 1,
    /// or the handler was actually queued/called).
    bool dispatch_layer_row(const std::string& id, const oa::render::LayerEventHandler& row,
                            const std::string& event_type,
                            const std::vector<std::pair<std::string, std::string>>& runtime_params,
                            PointerDispatch::Kind kind);
    bool dispatch_input_row(const std::string& filter_name,
                            const oa::render::InputHandler& row,
                            const std::vector<std::pair<std::string, std::string>>& runtime_params,
                            PointerDispatch::Kind kind);
    /// drag begin/continue/finish.
    bool drag_begin(const std::string& layer_id, bool forced);
    void drag_update(double mx, double my);
    void drag_end();
    void notify_pointer(const PointerDispatch& d);
    /// config-tag application shared by apply_scene_event + tests.
    bool apply_control_config(const oa::runtime::Event& e);
    /// inline-event return frames: when an input row's
    /// Lua helper (fn.push/estag UI flows) queues engine tags while the
    /// scenario is parked, a synthetic marker CallFrame is pushed above the
    /// parked position so the helper's trailing [return] pops the marker and
    /// the interpreter resumes the parked line — the story wait survives
    /// window open/close cycles instead of unwinding a real frame per helper.
    /// Marker lifecycle (single marker, never nested):
    ///  - arm (begin): only when no marker is active and THIS row's Lua fn
    ///    actually queued tags; pushes {script, parked_line} above the live
    ///    stack and records the pre-arm snapshot.
    ///  - settle (per drained queue round): drops the
    ///    record when the marker frame left the stack (its own [return] ran);
    ///    detaches the marker (leaving queued-call real frames intact) when a
    ///    queued [call] ran while unclaimed; claims it (claimed_by_jump) when
    ///    a queued [jump] ran so it survives the helper's own waits/drains;
    ///    keeps it while claimed/paused/more tags queued; otherwise (drain
    ///    with no structural tag) tears it down and restores the pre-arm park.
    ///  - refresh (every tick start): drops the record once the marker frame
    ///    is no longer at its recorded stack index.
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
    // natural (texture) size cache for get_layer_info's last
    // width/height fallback (explicit > clip > bound image size) — resource
    // read key -> {w,h} (0,0 cached when the file is unknown).
    // 键 = TextureKey(只有 Asset 域参与文件探测;宿主供帧域
    // 恒 {0,0},与旧保留命名空间探测恒失败逐位等价,同时消除"层 id 与资源
    // 名同形"的撞名面)。
    // [C] H: 资源尺寸探测缓存（键 = TextureKey，已无层 id 撞名面）
    std::map<oa::render::TextureKey, std::pair<double, double>> image_size_cache_;
    std::pair<double, double> bound_image_size(const oa::render::Layer& l);
    // [B] E: open_project 的平台串；[reset] 重启复用
    std::string platform_; // project platform of open_project (restart re-open)
    // [B] E+L: 宿主重建 VM（boot）；读档只 restore_position + Lua restore() 回填。A 面投影源；Lua 全局/闭包与 tag 队列归 [B]（无存档面）
    std::unique_ptr<oa::runtime::Interpreter> interpreter_;
    // [B] E: 读档 reset 后由恢复位置的那一行重跑重导出（停驻时 current_line 指停驻行）
    std::optional<oa::runtime::WaitReason> wait_;
    // [C] E: 帧号（e:getFrameNumber）；读档不恢复
    uint64_t tick_count_ = 0; // per-tick frame number for e:getFrameNumber
    // [C] E: wait_ 的派生量；读档清零
    uint64_t wait_remaining_ms_ = 0;
    // [B] E: [exit] 控制闩；读档保留（reset_domains 清）
    bool exit_requested_ = false;
    // [C] E: 每 tick 由 skip_active() 重算
    bool skip_active_ = false;   // effective skip (recomputed each tick)
    // [C] E: e:debugSkip 快进态；读档清。**不调用
    // end_debug_skip()**：那会 fire onDebugSkipOut → Lua exskip_end 的
    // quickjump 重放会把解释器拉回 backlog 点，覆盖刚恢复的位置；读档只静默
    // 落旗（Lua 侧 flg.exskip 由框架自身在下一屏收尾，FPM mainloop 只在
    // flg.exskip 为真时抑制画面，静默落旗不会留下黑屏臂）
    bool exskip_active_ = false; // FPM e:debugSkip fast-forward;
                                 // Lua flg.exskip is the script-side half
    // [C] E: 读档显式关闭
    bool automode_ = false;      // automode active (auto-advance waits)
    // [C] E: 读档清零
    uint64_t auto_elapsed_ms_ = 0;
    // [automode syncse=<ids>] — the FPM-family Lua contract
    // (autoskip.lua: automode_start/clickAutomode queue the ids of the voice
    // channels playing on the CURRENT page before every automode click park
    // when conf.autostop==1, and clear it (syncse="") on page release /
    // automode stop). While non-empty, the automode page flip holds until
    // every listed channel stopped (automode_sync_blocked).
    // [C] E: 当前页语音 id 闸；读档清——automode_ 读档显式关，闸与模式同域
    // 同清
    std::string automode_syncse_; // comma-separated sound ids; "" = no gate
    // [C] E: 单调引擎时钟（绝不进档）
    uint64_t now_ms_ = 0;
    // Control domain
    // [B] L: [skip allow] 配置（boot 脚本重挂）；读档保留
    bool skip_allowed_ = true;   // [skip allow] gate
    // [C] E: 命令 skip 开关；读档清——只清派生
    // skip_active_ 不够：skip_active() = skip_allowed_ && (skip_enabled_ ||
    // control_skip_effective())，下一 tick 又从旧开关重算为真
    bool skip_enabled_ = false;  // command skip ([exec skip] / roles 12-13)
    // [B] L: [automode allow] 配置（boot 脚本重挂）
    bool automode_allowed_ = true; // [automode allow] gate
    // [C] E: 每 tick 由 keys_down_ 重算
    bool control_skip_pressed_ = false; // a role-14 key is held (Ctrl…)
    // [C] E: 锁到松键；读档清（键仍按住的下一 tick 由
    // keys_down_ 重新建立）
    bool control_skip_blocked_ = false; // lock until release (skip disallowed)
    // [B] L: [rclick allow] 配置（boot 脚本重挂）
    bool rclick_allowed_ = false; // [rclick allow]
    // [B] L: [rclick file] 脚本名（非层 id）
    std::string rclick_file_;    // [rclick file] ("" = default script)
    // [B] L: [hide allow] 配置（boot 脚本重挂）
    bool hide_allowed_ = false;  // [hide allow]
    // [C] E: hide 模式在飞态；读档清：模式闩与 automode_ /
    // skip_active_ 同域同类（"同域清一半"面）；存档时刻的可见性已经
    // 逐字在 props 里，恢复场景即恢复画面
    bool hide_active_ = false;   // hide mode: window layers hidden, click recovers
    // [B] L: hide 窗口层 id 表 = **脚本配置**（[hide window=]，与 [skip allow]
    // 同类，boot/config 脚本重挂）；读档**保留**：读档没有重发
    // 路径（FPM init.lua:259 只在 boot 发 [hide allow=0]；清掉 = 会话配置
    // 丢失），且唯一消费者 apply_hide_visibility 已经 find 守卫
    // （runtime.cpp 中 `if (!l) continue;`）⇒ 不可能物化幽灵层。
    std::vector<std::string> hide_window_;
    // [C] E: hide 前的层可见性现场（层 id→bool）；读档清：它是
    // **已消失场景**的现场，恢复后的同 id 层语义可能不同，退出 hide 时回写会
    // 把旧可见性写到新层上（错值，非幽灵层——回写前有 find 守卫）
    std::map<std::string, bool> hide_snapshot_;
    // [B] L: keyconfig 角色→键表；会话态，读档保留（全局输入注册表同类）
    std::map<int, std::set<int>> keymap_; // keyconfig role -> key ids
    // [C] E: 上一帧有效 skip 边沿
    bool last_control_skip_effective_ = false;
    // Control-domain private machinery
    void set_skip_mode(bool enabled);
    void set_automode_mode(bool enabled);
    // automode pacing interval — the script var s.automodewait
    // (FPM clickAutomode writes it at every click park: automode_vowait for a
    // voiced line under autostop, getASpeed() otherwise), 900 ms fallback.
    uint64_t automode_wait_ms() const;
    // automode voice gate — true while any channel listed in
    // automode_syncse_ is still playing (voice or SE bus, same lookup as
    // media_se_wait_finished).
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
    // [glyph] click-wait icon per-frame driving
    // (advance_click_wait): while a Generic/Generic0 click wait parks and the
    // current text page is fully revealed, move+show the icon layer from
    // click_wait_placement(false); hide it when the wait leaves or reveal is
    // incomplete. No-op unless [glyph] configured icon layers (FPM has none).
    void advance_click_wait();
    // [C] E: click-wait 图标层 id；读档清——advance_click_wait
    // 的隐藏路径对旧 id 直接 set_props（layer.cpp 的 `lyprop on a missing id
    // materializes the node`），读档后 wait_ 已重置 ⇒ 下一个 tick 必走隐藏路径
    // ⇒ 凭空物化幽灵层。清 + 该路径补 find 守卫（双保险）
    std::string active_wait_icon_; // 当前显示中的图标图层 id（"" = 无）
    // input state (physical + overrides across frames)
    // [C] E: 每帧末清空
    std::map<int, int> overrides_;
    // [C] E: 上一帧快照
    std::map<int, int> prev_overrides_;
    // [C] E: 每 tick 由 FrameInput 覆盖
    std::set<int> keys_down_;
    // [C] E: 同上（本帧按下边沿）
    std::set<int> down_edges_;
    // [C] E: 同上（本帧抬起边沿）
    std::set<int> up_edges_;
    // [C] E: 每 tick 覆盖（宿主指针）
    int mouse_x_ = 0;
    // [C] E: 同上
    int mouse_y_ = 0;
    // decide-edge: override with bit 32 that appeared this frame
    // [C] E: 每 tick 重算（overrides_ 位 32 边沿）
    bool decide_edge_ = false;
    // script status set by Lua (0 running; 4 = forced stop)
    // [C] E: Lua 写的剧本状态（0 跑 / 4 强制停）；**读档清 0**。
    // 理由：① ==4 是引擎唯一的"整步停摆"闸
    // （runtime.cpp 的 `if (script_status_ != 4)`），读档后没人能保证复位 ⇒ 挂起
    // 不可自愈；② 复位者属于**读档前那一屏**（FPM adv/vsync.lua 的
    // imageCacheStart 链），跨读档无意义；③ FPM 无 getScriptStatus 读者
    // （全树 grep 为空，唯一消费者是上述引擎闸）⇒ 清 0 不改变任何 Lua 语义。
    int script_status_ = 0;
    // [C] H: 宿主事件泵队列。读档**不从本队列里清**：读档是在
    // dispatch_save_events 的派发循环里被调用的，就地清 = 迭代器失效（UB）+ 丢掉
    // 读档链自己产生的事件；改为在派发循环里按**入口快照**迭代且把"读档期间新产生
    // 的事件"留在队列尾部（确定性，不再依赖 deque 的迭代器失效行为）
    std::deque<oa::runtime::Event> pending_events_;
    // runtime-owned Artemis scene compositor + diagnostics counters
    // [B] E: A 面投影源（LayerSnap/root_props）；读档 = clear + restore_scene_snapshot 重放；不随投影恢复的面（消息槽/绑定/输入行/动画/内容角色）见 runtime_save.cpp
    oa::render::Compositor scene_;
    // [C] E: 诊断计数
    size_t scene_layer_events_ = 0;
    // [C] E: 诊断计数
    size_t scene_tween_events_ = 0;
    // [C] E: 诊断计数
    size_t tween_completion_calls_ = 0;
    // scene transition state + capture hook
    // [C] E: 在飞转场；读档 clear（type 1/2 由 load 重新起）
    oa::render::Transition transition_;
    // [B] H: 宿主捕获钩子；读档保留
    std::function<void()> transition_capture_cb_;
    // pointer-dispatch state
    // [B] H: 宿主命中尺寸提供者（渲染器重建后重挂）
    oa::render::Compositor::QuadSizeFn hit_size_;
    // [B] H: 宿主 alpha 采样器
    oa::render::Compositor::AlphaSamplerFn alpha_sampler_;
    // [B] H: 上两者的 userdata
    void* compositor_userdata_ = nullptr;
    // [C] E: 层 id 集；读档 clear（已做）
    std::set<std::string> hovered_;
    // [C] E: 层 id；读档 clear（已做）
    std::string drag_layer_;
    // [C] E: 拖拽起点（与 drag_start_mouse_y_ 同行声明）
    double drag_start_mouse_x_ = 0, drag_start_mouse_y_ = 0;
    // [C] E: 拖拽原点（与 drag_origin_top_ 同行声明）
    double drag_origin_left_ = 0, drag_origin_top_ = 0;
    // [C] E: 指针去重快照（与 mouse_prev_y_ 同行声明）
    int mouse_prev_x_ = 0, mouse_prev_y_ = 0; // last processed pointer position
    // [C] E: 首帧闩
    bool mouse_inited_ = false;
    // [C] E: 左键边沿闩
    bool left_down_prev_ = false;
    // [C] E: 诊断计数
    size_t pointer_dispatch_count_ = 0;
    // [B] H: 宿主指针观察者；读档保留
    std::function<void(const PointerDispatch&)> pointer_observer_;
    // [mouse] pointer-warp host hook + accepted-request counter.
    // [B] H: 宿主指针 warp 钩子；读档保留
    std::function<void(int, int)> pointer_warp_cb_;
    // [C] E: 诊断计数
    size_t pointer_warp_requests_ = 0;
    // Warp-emulation state: a [mouse] request normally reaches the engine as
    // fresh OS-motion feedback. Hosts that cannot warp the OS cursor (hidden
    // window / headless) never produce feedback; while no fresh raw pointer
    // input arrives the runtime then advances its own pointer to the
    // requested target through the normal pointer chain (hover included), and
    // the first raw input change clears the pending warp (tick()).
    // [C] E: warp 仿真在飞态
    bool pointer_warp_pending_ = false;
    // [C] E: warp 目标（与 pointer_warp_y_ 同行声明）
    int pointer_warp_x_ = 0;
    int pointer_warp_y_ = 0;
    // [C] E: 宿主原始指针快照（与 raw_mouse_y_ 同行声明）
    int raw_mouse_x_ = 0;
    int raw_mouse_y_ = 0;
    // inline-event marker state (see decl above)
    // [B] E: 合成 CallFrame 标记（script/line/stack 快照）；读档须 reset——停驻位置变了，记录的栈索引失去意义
    std::optional<InlineEventFrame> inline_event_frame_;
    // [lytween sync=1] arm state — the layer whose tween completion parks
    // the script only when the current run does not park on a real wait first
    // (Event::LayerTween{sync} dispatch rule).
    // [C] E: 层 id；读档 reset（已做）
    std::optional<std::string> pending_sync_tween_layer_;
    // True while drain_parked_queue runs: scene events consumed from drained
    // tags must not arm a [lytween sync] park (that only arms when no wait
    // reason is parked at dispatch time).
    // [C] E: 队列排空重入闩
    bool in_parked_drain_ = false;
    // message-layer text domain
    // [B] L: 消息窗口由 Lua onLoad 重建（fileio.lua restore → ui.asb load_next → restoreText/restoreNext）；引擎侧读档清页内容、层配置会话级保留
    oa::render::TextEngine text_;
    // [C] E: 诊断计数
    size_t text_events_ = 0;
    // [C] E: 诊断计数
    size_t message_switch_events_ = 0;

    /// 构造：注入反指与文件系统（原 GameRuntime 构造函数的初始化列表）。
    RuntimeState(GameRuntime* rt, std::shared_ptr<const oa::fs::IFileSystem> fs);
};

} // namespace oa::runtime
