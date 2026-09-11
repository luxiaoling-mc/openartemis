// Save-domain glue.
//
// SaveGame/LoadGame dispatch under the openartemis host model: real
// persistence goes through an injected oa::runtime::SaveStore (the default null
// store keeps headless runs side-effect free). Save files are binary
// documents (codec core/util/binary_stream.h; layout in runtime_save.h). 
// Semantics (files/ layout/domains/onSave-onLoad) are documented in 
// runtime_save.h.
#include "core/runtime/runtime_internal.h"
#include "core/runtime/runtime_save.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <cstring>
#include <limits>

#include "core/util/binary_stream.h"

#include "core/media/image.h"
#include "core/render/layer.h"
#include "core/render/text.h"

namespace oa::runtime {

namespace {
constexpr const char* kSavegFile = "saveg.dat";
constexpr const char* kSystemFile = "system.dat";
constexpr const char* kAutosaveFile = "autosave.dat";

bool is_input_wait(const oa::runtime::WaitReason* w) {
    using K = oa::runtime::WaitReason::Kind;
    return w && (w->kind == K::Generic || w->kind == K::Generic0 || w->kind == K::KeyWait);
}

/// Drop dangerous/Windows-only segments from the ini SAVEPATH.
std::string sanitize_segments(const std::string& raw) {
    if (raw.empty()) return "save";
    std::string normalized = raw;
    for (char& c : normalized)
        if (c == '\\') c = '/';
    std::vector<std::string> cleaned;
    size_t start = 0;
    while (start <= normalized.size()) {
        const size_t slash = normalized.find('/', start);
        const std::string seg = normalized.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? normalized.size() + 1 : slash + 1;
        std::string t = seg;
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        if (t.empty() || t == "." || t == ".." || t.back() == ':') continue;
        cleaned.push_back(t);
    }
    if (cleaned.empty()) return "save";
    std::string out;
    for (size_t i = 0; i < cleaned.size(); ++i) {
        if (i) out += '/';
        out += cleaned[i];
    }
    return out;
}

bool clean_rel_path(const std::string& file, std::string* out) {
    if (file.empty()) return false;
    std::string n = file;
    while (!n.empty() && n.front() == '/') n.erase(n.begin());
    for (char& c : n)
        if (c == '\\') c = '/';
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= n.size()) {
        const size_t slash = n.find('/', start);
        const std::string seg = n.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? n.size() + 1 : slash + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == ".." || seg.find(':') != std::string::npos) return false;
        parts.push_back(seg);
    }
    if (parts.empty()) return false;
    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) joined += '/';
        joined += parts[i];
    }
    *out = joined;
    return true;
}
long long save_i64(const std::string& v, long long dflt) {
    if (v.empty()) return dflt;
    try {
        size_t used = 0;
        const long long r = std::stoll(v, &used);
        return used == v.size() ? r : dflt;
    } catch (...) {
        return dflt;
    }
}

} // namespace

std::string sanitize_savepath(const std::string& raw) { return sanitize_segments(raw); }

std::string GameRuntime::RuntimeState::qualify_save_file(const std::string& file) const {
    std::string clean;
    if (!clean_rel_path(file, &clean)) return std::string();
    if (savepath_.empty() || clean == savepath_ || clean.rfind(savepath_ + "/", 0) == 0) {
        return clean;
    }
    return savepath_ + "/" + clean;
}

bool GameRuntime::RuntimeState::save_file_exists(const std::string& file) const {
    const std::string path = qualify_save_file(file);
    if (path.empty() || !save_store_) return false;
    return save_store_->exists(path);
}

// ---------------------------------------------------------------------------
// System saves (saveg.dat / system.dat)
// ---------------------------------------------------------------------------

void GameRuntime::RuntimeState::sysload() {
    if (!save_store_) return;
    for (const bool is_global : {true, false}) {
        const std::string rel = is_global ? kSavegFile : kSystemFile;
        const std::string path = qualify_save_file(rel);
        if (path.empty()) continue;
        const auto bytes = save_store_->read(path);
        if (!bytes) continue; // first boot: no file is the default state
        try {
            const auto map = oa::runtime::decode_domain_map(
                std::string(bytes->begin(), bytes->end()));
            const std::string prefix = is_global ? "g." : "s.";
            for (const auto& [k, v] : map) {
                interpreter_->set_variable(prefix + k, v);
            }
            std::fprintf(stderr, "[runtime] sysload %s (%zu entries)\n", path.c_str(),
                         map.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[runtime] sysload decode %s failed: %s\n", path.c_str(),
                         e.what());
        }
    }
}

bool GameRuntime::RuntimeState::syssave() {
    if (!save_store_ || !interpreter_) return false;
    const auto& vars = interpreter_->variables();
    const std::map<std::string, oa::runtime::Value> global(vars.global.begin(),
                                                          vars.global.end());
    const std::map<std::string, oa::runtime::Value> system(vars.system.begin(),
                                                          vars.system.end());
    bool all_ok = true;
    const auto write_map = [&](const char* file, const std::map<std::string,
                                                                oa::runtime::Value>& m) {
        const std::string path = qualify_save_file(file);
        if (path.empty()) return;
        // Strip the domain prefix off each key.
        std::map<std::string, oa::runtime::Value> stripped;
        for (const auto& [k, v] : m) {
            stripped[k.size() > 2 ? k.substr(2) : k] = v;
        }
        const std::string doc = oa::runtime::encode_domain_map(stripped);
        const std::vector<uint8_t> bytes(doc.begin(), doc.end());
        if (!save_store_->write(path, bytes)) all_ok = false;
    };
    write_map(kSavegFile, global);
    write_map(kSystemFile, system);
    return all_ok;
}

// ---------------------------------------------------------------------------
// Numbered saves / loads
// ---------------------------------------------------------------------------

namespace {

/// 有界 tag 队列排空的轮数上限（= 原静默 guard 128）。
constexpr size_t kTagDrainGuard = 128;

/// 层 id 合法性（A 面只存名字；恢复必须容忍"读档后该 id 不
/// 存在"，但**畸形 id 不许进 ensure_path**——那条路会物化空名/错名节点）。
/// 判据来自 Compositor::ensure_path 的前缀切分：id 自身或任一前缀为空 =
/// 物化出空名节点。空 id / 前导 '.' / 空段（".."）都命中；尾点（"a."）是
/// 合法的嵌套 id（ensure_path 对它有一致的解释），不拒。
bool layer_id_ok(const std::string& id) {
    if (id.empty()) return false;
    if (id.front() == '.') return false;
    if (id.find("..") != std::string::npos) return false;
    return true;
}

/// 宿主供帧层旧存档的保留命名空间
/// 前缀。**仅用于自检记录**（跨版本兼容当前不做，见 runtime_internal.h 的
/// 版本门契约）：不迁移、不改写 props，只在日志里点出来。
bool legacy_reserved_file(const std::string& file) {
    return file.rfind("__video_layer__:", 0) == 0 ||
           file.rfind("__emote_layer__:", 0) == 0;
}

void collect_scene_snapshot(oa::runtime::SaveData* data, const oa::render::Compositor& scene) {
    data->has_scene = true;
    const oa::render::Layer& root = scene.root_props();
    data->root_props = root.props;
    for (const oa::render::Layer* l : scene.draw_order()) {
        if (scene.is_message_slot(l->id)) continue; // 槽=运行时结构,文本流重建
        oa::runtime::LayerSnap snap;
        snap.id = l->id;
        snap.props = l->props;
        for (const auto& [type, row] : l->event_handlers) {
            oa::runtime::LayerEventHandlerSnap h;
            h.type = type;
            h.enabled = row.enabled;
            h.penetration = row.penetration;
            h.handler = row.handler;
            h.file = row.file;
            h.label = row.label;
            h.call = row.call;
            h.params = row.params;
            h.filter_params = row.filter_params;
            snap.handlers.push_back(std::move(h));
        }
        data->layers.push_back(std::move(snap));
    }
}

void collect_audio_snapshot(oa::runtime::SaveData* data, const oa::media::AudioEngine& audio) {
    data->has_audio = true;
    const auto& st = audio.state();
    const auto snap_channel = [](const oa::media::SoundChannel& ch) {
        oa::runtime::AudioChannelSnap s;
        s.id = ch.id;
        s.file = ch.file;
        s.loop_play = ch.loop_play;
        s.gain = ch.raw_gain;
        s.pan = ch.raw_pan;
        s.skippable = ch.skippable;
        return s;
    };
    if (st.bgm_channel && st.bgm_channel->playing) {
        data->audio.bgm = snap_channel(*st.bgm_channel);
    }
    for (const auto& [id, ch] : st.se_channels) {
        (void)id;
        if (ch.playing) data->audio.se.push_back(snap_channel(ch));
    }
    for (const auto& [id, ch] : st.voice_channels) {
        (void)id;
        if (ch.playing) data->audio.voice.push_back(snap_channel(ch));
    }
    std::sort(data->audio.se.begin(), data->audio.se.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(data->audio.voice.begin(), data->audio.voice.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
}

/// 场景快照恢复的显式处理统计（失效 id 跳过/重复 id 去重都必须
/// 有账可查；"缺失即物化"的入口 set_props 在这里一次都不用）。
struct SceneRestoreStats {
    size_t created = 0;
    size_t skipped_invalid = 0; // 畸形 id：不物化，跳过
    size_t skipped_dup = 0;     // 重复 id：首个生效，其余跳过（避免逐键合并）
};

SceneRestoreStats restore_scene_snapshot(oa::render::Compositor* scene,
                                        const oa::runtime::SaveData& data) {
    SceneRestoreStats st;
    if (!data.has_scene) return st;
    scene->clear_scene();
    if (!data.root_props.empty()) scene->set_root_props(data.root_props);
    std::set<std::string> seen;
    for (const auto& ls : data.layers) {
        // A 面数据里的 id 只当数据看：畸形 → 显式跳过 + 报告（不 ensure_path）。
        if (!layer_id_ok(ls.id)) {
            ++st.skipped_invalid;
            std::fprintf(stderr,
                         "[runtime] load: layer snapshot id '%s' is malformed; "
                         "skipped (no node materialized)\n",
                         ls.id.c_str());
            continue;
        }
        // 重复 id：collect_scene_snapshot 不会产出（draw_order 唯一），手写/损坏
        // 档可能有 ⇒ 首个生效（确定性），其余跳过并报告。
        if (!seen.insert(ls.id).second) {
            ++st.skipped_dup;
            std::fprintf(stderr,
                         "[runtime] load: duplicate layer snapshot id '%s'; "
                         "first occurrence wins, later one skipped\n",
                         ls.id.c_str());
            continue;
        }
        scene->create(ls.id, ls.props);
        ++st.created;
        for (const auto& h : ls.handlers) {
            oa::render::LayerEventHandler row;
            row.enabled = h.enabled;
            row.penetration = h.penetration;
            row.handler = h.handler;
            row.file = h.file;
            row.label = h.label;
            row.call = h.call;
            row.params = h.params;
            row.filter_params = h.filter_params;
            scene->restore_event_handler_row(ls.id, h.type, std::move(row));
        }
    }
    return st;
}

void restore_audio_snapshot(oa::media::AudioEngine* audio, const oa::runtime::AudioSnap& snap) {
    audio->stop_all_sounds();
    if (snap.bgm) {
        oa::media::BgmConfig cfg;
        cfg.loop_play = snap.bgm->loop_play;
        cfg.gain = snap.bgm->gain;
        cfg.pan = snap.bgm->pan;
        audio->play_bgm(snap.bgm->file, cfg);
        if (snap.bgm->loop_play) {
            if (auto loop_file = oa::media::ab_loop_file(snap.bgm->file)) {
                if (audio->state().bgm_channel) {
                    audio->state().bgm_channel->loop_file = std::move(loop_file);
                }
            }
        }
    }
    for (const auto& se : snap.se) {
        oa::media::SeConfig cfg;
        cfg.loop_play = se.loop_play;
        cfg.gain = se.gain;
        cfg.pan = se.pan;
        cfg.skippable = se.skippable;
        audio->play_se(se.id, se.file, cfg);
    }
    for (const auto& v : snap.voice) {
        oa::media::SeConfig cfg;
        cfg.loop_play = v.loop_play;
        cfg.gain = v.gain;
        cfg.pan = v.pan;
        cfg.skippable = v.skippable;
        audio->play_voice(v.id, v.file, cfg);
    }
}

const char* wait_kind_name(oa::runtime::WaitReason::Kind k) {
    using K = oa::runtime::WaitReason::Kind;
    switch (k) {
        case K::Generic: return "generic";
        case K::Generic0: return "wt0";
        case K::Timed: return "timed";
        case K::Stop: return "stop";
        case K::Se: return "se";
        case K::VideoLayer: return "video";
        case K::ScenarioTween: return "scenario";
        case K::KeyWait: return "key";
    }
    return "?";
}

} // namespace

bool GameRuntime::RuntimeState::save_game_to(const std::string& file) {
    if (!save_store_ || !interpreter_) return false;
    // onSave (store): serialize sys/gscr/conf into g.* vars; run it with the
    // tag queue isolated so the queued UI continuation is not consumed.
    // The FPM handler is `store(e, p)` reading p.file (fileio.lua:5-10), so
    // the callback needs the standard calllua argument shape, not the
    // no-argument fire_event used for onEnterFrame (total-acceptance: the
    // save screen's store crashed with p=nil).
    std::vector<oa::runtime::Instruction> pending = interpreter_->take_tag_queue();
    {
        const std::map<std::string, std::string> onsave_params{{"file", file}};
        // research/130: a failing save handler abandons the callback, not the
        // app (original runtime: calllua logs and continues).
        try {
            (void)interpreter_->lua_bridge().call_function("store", onsave_params);
        } catch (const oa::runtime::LuaError& e) {
            interpreter_->lua_bridge().report_dispatch_error("calllua", "store", e.what());
        }
    }
    (void)drain_tag_queue_bounded("save"); // 有界排空（命中 guard 可观测）
    interpreter_->restore_tag_queue(std::move(pending));

    oa::runtime::SaveData data;
    data.local_variables = interpreter_->variables().local;
    if (const std::string* s = interpreter_->current_script()) {
        data.current_script = *s;
    }
    data.current_line = interpreter_->current_line();
    for (const auto& f : interpreter_->call_stack()) {
        data.call_stack.emplace_back(f.script, f.return_line);
    }
    collect_scene_snapshot(&data, scene_);
    collect_audio_snapshot(&data, audio_);
    const std::string doc = data.encode();
    const std::string path = qualify_save_file(file);
    if (path.empty()) {
        std::fprintf(stderr, "[runtime] save: illegal file name '%s'\n", file.c_str());
        return false;
    }
    if (!save_store_->write(path, std::vector<uint8_t>(doc.begin(), doc.end()))) {
        std::fprintf(stderr, "[runtime] save: write failed: %s\n", path.c_str());
        return false;
    }
    std::fprintf(stderr, "[runtime] saved %s (%zu local vars, script %s:%zu)\n",
                 path.c_str(), data.local_variables.size(), data.current_script.c_str(),
                 data.current_line);
    // A numbered save also refreshes the system save (slot index lives in
    // g.system).
    (void)syssave();
    return true;
}

// ---------------------------------------------------------------------------
// 读档加固：清场清单 / 原子失败 / A 面自检 / 有界排空
// ---------------------------------------------------------------------------

size_t GameRuntime::RuntimeState::reset_ephemeral_on_load() {
    // 读档清场 = [C] 桶的引擎责任人清单（契约见 runtime_internal.h 文件头）。
    // 本清单与 reset_domains()（runtime.cpp）**必须对齐**，任何一处增删
    // 条目时另一处同步裁定。逐项落地（"清/留 + 理由 + 证据"）：
    //   [C] video_.stop_all_videos() + video_finished_
    //   [C] emote_layers_ + pending_emote_methods_
    //   [C] drop_pending_tween_cancels()
    //   [C] inline_event_frame_.reset()
    //   [留] interpreter tag 队列（② 特判保留：FPM restore() 的
    //        tag{"call", ui.asb, load_next} 靠它；契约见文件头）
    //   [C] alldelete_* + capture_pending_
    //   [C] active_wait_icon_ / hide_snapshot_ / hide_active_；hide_window_ **留**
    //   [C] skip_enabled_ / exskip_active_ / control_skip_blocked_
    //   [C] script_status_ / automode_syncse_ / 指针按键瞬时态；pending_events_ 留
    // 顺序：媒体 → 文本/场景 → 动画投递 → 控制域 → 指针瞬时态；场景清场发生在
    // ② 桶回写（输入注册表）**之前**（顺序契约 1）。
    size_t cleared = 0;
    const auto clr = [&cleared] { ++cleared; };

    // ---- 媒体面（资源态：播放器/解码通道/宿主供帧绑定）----
    audio_.stop_all_sounds(); clr();
    if (media_players_) { media_players_->stop_all(); clr(); }
    video_.stop_all_videos(); clr();  // [C]：通道绑在已清场的层 id 上
    video_finished_ = false; clr();   // [C]（结束闩；reset_domains 亦漏，见文件头契约）
    emote_layers_.clear(); clr();     // [C]：播放器（③ 资源）+ 旧层 id
    pending_emote_methods_.clear(); clr(); // [C]：方法补发队列（同生共死）

    // ---- 文本 / 场景容器 ----
    text_.clear_scene(); clr();
    scene_.clear_scene(); clr();      // 节点销毁 ≡ tween/anime 桶随之消亡
    drop_pending_tween_cancels(); clr(); // [C]：元素持已被释放的 const Layer*
    transition_.clear(); clr();
    hovered_.clear(); drag_layer_.clear(); clr();
    pending_sync_tween_layer_.reset(); clr();
    inline_event_frame_.reset(); clr();  // [C]：记录的是旧停驻栈索引
    screenshot_.valid = false; clr();
    capture_pending_ = false; clr();     // [C]：否则读档后会截一帧错场景
    alldelete_active_ = false;           // [C]：到期 finish_all_delete 会抹掉
    alldelete_start_ms_ = 0;             //        刚恢复的场景
    alldelete_duration_ms_ = 0; clr();
    wait_.reset(); wait_remaining_ms_ = 0; clr();

    // ---- 控制域（模式闩；配置面 [* allow] 属 ② 保留，不在此）----
    skip_active_ = false; clr();              // 派生量
    skip_enabled_ = false; clr();             // [C]：只清派生量会让 skip 复活
    // [C]：exskip 只静默落旗。**不能**调 end_debug_skip()：它会 fire
    // onDebugSkipOut → FPM exskip_end 的 quickjump 重放把解释器拉回 backlog 点，
    // 覆盖刚恢复的位置（读档 = 显式定位，优先级最高）。
    exskip_active_ = false; clr();
    control_skip_pressed_ = false; clr();     // 下一 tick 由 keys_down_ 重算
    control_skip_blocked_ = false; clr();     // [C]：锁到松键的闩跨读档无意义
    last_control_skip_effective_ = false; clr();
    automode_ = false; auto_elapsed_ms_ = 0; clr();
    automode_syncse_.clear(); clr();          // [C]：闸随 automode 同域同清
    hide_active_ = false; clr();              // [C]：模式闩（与 automode 同类）
    hide_snapshot_.clear(); clr();            // [C]：已消失场景的可见性现场
    script_status_ = 0; clr();                // [C]：==4 会让读档后剧本不推进
    active_wait_icon_.clear(); clr();         // [C]：隐藏路径对旧 id set_props

    // ---- 指针 / 按键瞬时态（[C]；每 tick 由 FrameInput 重算的派生量）----
    overrides_.clear(); prev_overrides_.clear(); clr();
    keys_down_.clear(); down_edges_.clear(); up_edges_.clear(); clr();
    decide_edge_ = false; clr();

    // 明确**保留**（每条都有理由）：
    //   hide_window_ / hide_allowed_ / skip_allowed_ / automode_allowed_ /
    //   rclick_allowed_ / rclick_file_ / keymap_   —— [B] 桶脚本配置：读档没有
    //     重发路径（FPM init.lua 只在 boot 发），清掉 = 会话配置
    //     丢失；hide_window_ 的唯一消费者 apply_hide_visibility 已有 find 守卫。
    //   mouse_x_/mouse_y_/mouse_prev_x_/mouse_prev_y_/mouse_inited_/
    //   left_down_prev_ —— 镜像物理指针：读档不改变指针位置/按键，清它们会在
    //     读档后注入一次假 pointer re-seed（假 hover 面）。
    //   exit_requested_ —— [exit] 控制闩（②：宿主退出请求不因读档失效）。
    //   pending_events_ —— 见 dispatch_save_events（入口快照 + 新事件留队尾）。
    //   interpreter_->tag_queue_ —— [留]，② 特判保留（文件头契约）。
    return cleared;
}

bool GameRuntime::RuntimeState::preflight_load_position(const oa::runtime::SaveData& data,
                                                       std::string* why) {
    if (!interpreter_) {
        if (why) *why = "no interpreter";
        return false;
    }
    // 空 current_script = "未定位"（合法：只有变量/场景的档）。
    if (data.current_script.empty()) return true;
    std::string err;
    if (!interpreter_->try_load_script(data.current_script, &err)) {
        if (why) *why = "script '" + data.current_script + "' unavailable: " + err;
        return false;
    }
    const oa::runtime::Script* s = interpreter_->get_script(data.current_script);
    if (s && data.current_line >= s->instructions.size()) {
        // 同版本内的数据漂移（脚本改过 ⇒ 行号越界）**不是**失败：解释器把越界
        // 位置当"脚本结束"（runtime_iet.cpp 的 Completed 分支），行为有定义。
        // 只报告，让调用方和日志看得见（不静默）。
        std::fprintf(stderr,
                     "[runtime] load: preflight: current_line %zu is beyond script '%s' "
                     "end (%zu instructions); the position resolves to script-completed\n",
                     data.current_line, data.current_script.c_str(), s->instructions.size());
    }
    return true;
}

size_t GameRuntime::RuntimeState::self_check_save_data(const oa::runtime::SaveData& data) const {
    // 只 log 不 throw：A 面是数据，坏数据要"有定义地
    // 容忍"，不是让读档炸掉。返回问题/记录条目数（诊断计数 load_self_check_issues_）。
    size_t issues = 0;
    std::set<std::string> ids;
    for (const auto& ls : data.layers) {
        if (!layer_id_ok(ls.id)) {
            ++issues;
            std::fprintf(stderr,
                         "[runtime] load: self-check: layer id '%s' is malformed "
                         "(empty id / leading '.' / empty path segment); it will be "
                         "skipped, no node materialized\n",
                         ls.id.c_str());
            continue;
        }
        if (!ids.insert(ls.id).second) {
            ++issues;
            std::fprintf(stderr,
                         "[runtime] load: self-check: duplicate layer id '%s'; the first "
                         "occurrence wins\n",
                         ls.id.c_str());
        }
        const auto fit = ls.props.find("file");
        if (fit != ls.props.end() && legacy_reserved_file(fit->second)) {
            // 旧存档形态（宿主供帧层的保留命名空间）。跨版本兼容不做
            // ⇒ **只记录不改写**（不迁移、不剥前缀）。
            ++issues;
            std::fprintf(stderr,
                         "[runtime] load: self-check: legacy reserved file prop on layer "
                         "'%s' (legacy save form); kept verbatim — cross-version "
                         "migration is out of scope\n",
                         ls.id.c_str());
        }
    }
    if (!data.has_scene) {
        ++issues;
        std::fprintf(stderr,
                     "[runtime] load: self-check: save has no scene snapshot; the scene "
                     "is cleared and only script/Lua state is restored\n");
    } else if (data.layers.empty()) {
        ++issues;
        std::fprintf(stderr,
                     "[runtime] load: self-check: empty scene snapshot (0 layers)\n");
    }
    if (data.has_audio && data.audio.empty()) {
        ++issues;
        std::fprintf(stderr, "[runtime] load: self-check: empty audio snapshot\n");
    }
    if (data.current_script.empty() && !data.call_stack.empty()) {
        ++issues;
        std::fprintf(stderr,
                     "[runtime] load: self-check: %zu call-stack frame(s) with an empty "
                     "current script\n",
                     data.call_stack.size());
    }
    for (const auto& [script, line] : data.call_stack) {
        (void)line;
        if (script.empty()) {
            ++issues;
            std::fprintf(stderr,
                         "[runtime] load: self-check: call-stack frame with an empty "
                         "script name (frame is kept verbatim)\n");
            break;
        }
    }
    const auto check_ch = [&issues](const oa::runtime::AudioChannelSnap& c, const char* kind) {
        if (c.file.empty()) {
            ++issues;
            std::fprintf(stderr,
                         "[runtime] load: self-check: %s channel '%s' has an empty file; "
                         "replay will be a no-op\n",
                         kind, c.id.c_str());
        }
    };
    if (data.has_audio) {
        if (data.audio.bgm) check_ch(*data.audio.bgm, "bgm");
        for (const auto& c : data.audio.se) check_ch(c, "se");
        for (const auto& c : data.audio.voice) check_ch(c, "voice");
    }
    return issues;
}

size_t GameRuntime::RuntimeState::drain_tag_queue_bounded(const char* phase) {
    size_t rounds = 0;
    while (interpreter_->has_queued_tags() && rounds < kTagDrainGuard) {
        (void)interpreter_->run_queued();
        ++rounds;
    }
    if (interpreter_->has_queued_tags()) {
        // 旧实现是 `guard++ < 128` 的静默截断：超限时剩下的 tag 直接留到下一
        // tick（甚至被后续清场带走）。现在截断**可观测**（日志 + 计数）。
        ++tag_drain_truncations_;
        std::fprintf(stderr,
                     "[runtime] %s: tag queue drain hit the %zu-round guard; %zu tag(s) "
                     "still queued (they run on the next drain round)\n",
                     phase, kTagDrainGuard, interpreter_->queued_tag_count());
    }
    return rounds;
}

LoadResult GameRuntime::RuntimeState::load_game(const std::string& file, int64_t trans_type) {
    // 失败语义：**原子**。位置预检在任何清场之前完成；任一失败
    // ⇒ 运行时状态不被改动，调用方按 LoadResult 分支（不再"只打日志"）。
    if (!save_store_ || !interpreter_) {
        std::fprintf(stderr, "[runtime] load: no save store / interpreter; load unavailable\n");
        return LoadResult::MissingFile;
    }
    const std::string path = qualify_save_file(file);
    const auto bytes = path.empty() ? std::optional<std::vector<uint8_t>>() : save_store_->read(path);
    if (!bytes) {
        std::fprintf(stderr, "[runtime] load: missing save file: %s\n", path.c_str());
        return LoadResult::MissingFile;
    }
    oa::runtime::SaveData data;
    try {
        data = oa::runtime::SaveData::decode(std::string(bytes->begin(), bytes->end()));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[runtime] load: corrupt save %s: %s\n", path.c_str(), e.what());
        return LoadResult::Corrupt;
    }
    // ① A 面自检（只 log 不 throw）。
    load_self_check_issues_ += self_check_save_data(data);
    // 位置预检：**必须先于任何清场**（否则 = 场景已清、快照已灌、位置失败的
    // 半恢复）。
    {
        std::string why;
        if (!preflight_load_position(data, &why)) {
            std::fprintf(stderr,
                         "[runtime] load: position preflight failed (%s); runtime state "
                         "unchanged\n",
                         why.c_str());
            return LoadResult::PositionUnavailable;
        }
    }

    // ② 桶特判保留的现场：全局输入注册表必须在清场
    // 之前取，场景快照恢复之后回写（顺序契约 1）。诊断：在飞动画（冻结语义，
    // 见文件头"在飞动画契约"）在清场里连同节点消亡，只记不重放。
    const auto live_input = scene_.input_handlers();
    const bool had_tweens = scene_.has_tweens();
    const bool had_anime = scene_.has_anime();

    // [C] 桶读档清场（逐项见 reset_ephemeral_on_load）。
    const size_t cleared = reset_ephemeral_on_load();

    const SceneRestoreStats rst = restore_scene_snapshot(&scene_, data);
    for (const auto& [k, row] : live_input) {
        scene_.restore_input_row(k.first, k.second, row);
    }

    // Restore variables (local domain only; current g./s. survive) and the
    // execution position.
    interpreter_->variables().reset_local_temp();
    for (const auto& [k, v] : data.local_variables) {
        interpreter_->set_variable(k, v);
    }
    std::vector<oa::runtime::CallFrame> stack;
    for (const auto& [script, line] : data.call_stack) {
        oa::runtime::CallFrame f;
        f.script = script;
        f.return_line = line;
        stack.push_back(std::move(f));
    }
    try {
        interpreter_->restore_position(data.current_script, data.current_line, stack);
    } catch (const std::exception& e) {
        // 预检已保证脚本可载入 ⇒ 这里只在引擎内部不一致（如内存故障）时到达。
        // 不静默：明确报错 + 返回可判定的失败码（此时状态 = 已清场并已按快照
        // 恢复，没有第二个可回滚的现场，文档里逐字写明）。
        std::fprintf(stderr,
                     "[runtime] load: position restore failed AFTER a successful preflight "
                     "(%s); scene is the restored snapshot but the position is unset\n",
                     e.what());
        return LoadResult::PositionUnavailable;
    }

    // onLoad (restore): unpack restored variables back into the Lua tables.
    // FPM handler `restore(e, p)` reads p.file too (fileio.lua:12) — same
    // calllua shape as onSave.
    {
        const std::map<std::string, std::string> onload_params{{"file", file}};
        // research/130: same policy as onSave above.
        try {
            (void)interpreter_->lua_bridge().call_function("restore", onload_params);
        } catch (const oa::runtime::LuaError& e) {
            interpreter_->lua_bridge().report_dispatch_error("calllua", "restore", e.what());
        }
    }
    // tag 队列不清（② 特判保留）——这里排空的是"读档链自己排的" tag，
    // 顺序契约 2：Lua onLoad → 队列排空 → 引擎音频快照。有界排空可观测。
    (void)drain_tag_queue_bounded("load");
    // Reload sys/gscr/conf Lua tables from the (restored) g.* stash vars.
    try {
        interpreter_->lua_bridge().run_code(
            "if type(fload_pluto) == \"function\" and type(init) == \"table\" then\n"
            "  if init.save_system then sys = fload_pluto(init.save_system) or sys or {} end\n"
            "  if init.save_global then gscr = fload_pluto(init.save_global) or gscr or {} end\n"
            "  if init.save_config then conf = fload_pluto(init.save_config) or conf or {} end\n"
            "end\n",
            "reload_persistent");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[runtime] load: persistent tables reload skipped: %s\n",
                     e.what());
    }

    if (data.has_audio) restore_audio_snapshot(&audio_, data.audio);
    sync_system_audio_volumes();

    // [load type=..] runs one transition after the restore (type 0 instant).
    if (trans_type == 1 || trans_type == 2) {
        std::map<std::string, std::string> params;
        params["type"] = std::to_string(trans_type);
        params["input"] = "1";
        transition_begin(params);
    }
    std::fprintf(stderr,
                 "[runtime] loaded %s (script %s:%zu; cleared %zu ephemeral slot(s), "
                 "restored %zu layer(s)%s%s, in-flight tween=%d anime=%d dropped)\n",
                 path.c_str(), data.current_script.c_str(), data.current_line, cleared,
                 rst.created,
                 rst.skipped_invalid ? " + skipped-invalid" : "",
                 rst.skipped_dup ? " + skipped-duplicate" : "",
                 had_tweens ? 1 : 0, had_anime ? 1 : 0);
    if (rst.skipped_invalid || rst.skipped_dup) {
        std::fprintf(stderr,
                     "[runtime] load: %zu malformed and %zu duplicate layer snapshot(s) "
                     "were skipped (explicit handling; no ghost nodes)\n",
                     rst.skipped_invalid, rst.skipped_dup);
    }
    return LoadResult::Ok;
}

bool GameRuntime::RuntimeState::load_game_from(const std::string& file, int64_t trans_type) {
    return load_game(file, trans_type) == LoadResult::Ok;
}

// ---------------------------------------------------------------------------
// Event dispatch
// ---------------------------------------------------------------------------

void GameRuntime::RuntimeState::dispatch_save_events() {
    if (pending_events_.empty()) return;
    // 按**入口快照**迭代。存档/读档派发会在处理过程中推进解释器
    // （读档的 onLoad 链 + tag 队列排空），其事件回调会 push_back 到
    // pending_events_ —— 就地 range-for 遍历一个正在被 push_back 的 deque 是
    // 迭代器失效（UB），而旧实现的尾部 `swap(keep)` 还会把处理期间新产生的事件
    // 整批丢掉（读档链自己排的 [trans]/[wt]/[save] 就这么消失了）。现在：
    //   - 入口快照决定"本轮处理哪些事件"（不会在派发中途自反馈）；
    //   - 处理期间新产生的事件按产生顺序留在队列**尾部**，交给下一次派发
    //     （同一 tick 的 dispatch_control_events 或下一 tick 的宿主 drain）。
    std::deque<oa::runtime::Event> in;
    in.swap(pending_events_);
    std::deque<oa::runtime::Event> keep;
    for (const auto& e : in) {
        const bool is_save =
            e.tag == "save" || e.tag == "load" || e.tag == "syssave" ||
            e.tag == "autosave" || e.tag == "takess" || e.tag == "savess" ||
            e.tag == "file";
        if (!is_save) {
            keep.push_back(e);
            continue;
        }
        handle_save_tag(e);
    }
    for (auto& e : pending_events_) keep.push_back(std::move(e));
    pending_events_.swap(keep);
}

void GameRuntime::RuntimeState::handle_save_tag(const oa::runtime::Event& e) {
    auto get = [&e](const char* k) -> std::string {
        const auto it = e.params.find(k);
        return it == e.params.end() ? std::string() : it->second;
    };
    if (e.tag == "save") {
        const std::string file = get("file");
        if (file.empty()) {
            (void)syssave(); // no-file [save] == syssave
        } else {
            (void)save_game_to(file);
        }
        return;
    }
    if (e.tag == "syssave") {
        (void)syssave();
        return;
    }
    if (e.tag == "load") {
        const std::string file = get("file");
        int64_t type = -1; // none
        const std::string t = get("type");
        if (!t.empty()) {
            try {
                type = std::stoll(t);
            } catch (...) {
                type = -1;
            }
        }
        if (file.empty()) {
            std::fprintf(stderr, "[runtime] load: empty file ignored\n");
            return;
        }
        // 读档结果是可判定的，调用方**必须**分支处理（旧实现
        // ignore 返回值 ⇒ 预检/损坏失败时只有一行日志，界面以为已经读档）。
        switch (load_game(file, type)) {
            case LoadResult::Ok:
                break;
            case LoadResult::MissingFile:
                std::fprintf(stderr, "[runtime] load: FAILED (file missing); state unchanged\n");
                break;
            case LoadResult::Corrupt:
                std::fprintf(stderr, "[runtime] load: FAILED (corrupt/unsupported save); "
                                     "state unchanged\n");
                break;
            case LoadResult::PositionUnavailable:
                std::fprintf(stderr, "[runtime] load: FAILED (position unavailable); "
                                     "no half-restore\n");
                break;
        }
        return;
    }
    if (e.tag == "autosave") {
        const std::string allow = get("allow");
        autosave_allow_ = allow.empty() ? 1 : int(save_i64(allow, 1));
        return;
    }
    if (e.tag == "takess") {
        // [takess]: the host snapshots the frame it is about to present.
        screenshot_.valid = false;
        capture_pending_ = capture_fn_ != nullptr;
        return;
    }
    if (e.tag == "savess") {
        handle_save_screenshot(get("file"), get("width"), get("height"));
        return;
    }
    if (e.tag == "file") {
        handle_file_operation(e.params);
        return;
    }
}

void GameRuntime::RuntimeState::handle_file_operation(const std::map<std::string, std::string>& params) {
    auto get = [&params](const char* k) -> std::string {
        const auto it = params.find(k);
        return it == params.end() ? std::string() : it->second;
    };
    const std::string command = get("command");
    if (command == "delete") {
        const std::string path = qualify_save_file(get("target"));
        if (!path.empty() && save_store_) save_store_->remove(path);
        return;
    }
    if (command == "copy" || command == "move") {
        const std::string src = qualify_save_file(get("src"));
        const std::string dst = qualify_save_file(get("dst"));
        if (!src.empty() && !dst.empty() && save_store_) {
            if (auto data = save_store_->read(src)) {
                if (save_store_->write(dst, *data)) {
                    if (command == "move") save_store_->remove(src);
                }
            }
        }
    }
}

namespace {
/// 可分离双线性重采样（Triangle 的实用等价：两遍一维线性插值；
/// imageops FilterType::Triangle，）。dims 相等时原样拷贝。
std::vector<uint8_t> resize_rgba(const uint8_t* src, uint32_t sw, uint32_t sh,
                                 uint32_t dw, uint32_t dh) {
    std::vector<uint8_t> out(size_t(dw) * dh * 4);
    if (sw == dw && sh == dh) {
        std::memcpy(out.data(), src, out.size());
        return out;
    }
    // Horizontal pass into tmp (dw x sh).
    std::vector<uint8_t> tmp(size_t(dw) * sh * 4);
    for (uint32_t y = 0; y < sh; ++y) {
        const uint8_t* row = src + size_t(y) * sw * 4;
        for (uint32_t x = 0; x < dw; ++x) {
            const double fx = (double(x) + 0.5) * double(sw) / double(dw) - 0.5;
            double sx = fx < 0 ? 0 : fx;
            if (sx > sw - 1) sx = sw - 1;
            const uint32_t x0 = uint32_t(sx);
            const uint32_t x1 = x0 + 1 < sw ? x0 + 1 : x0;
            const double t = sx - x0;
            uint8_t* d = tmp.data() + (size_t(y) * dw + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const double v = row[x0 * 4 + c] * (1.0 - t) + row[x1 * 4 + c] * t;
                d[c] = uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5));
            }
        }
    }
    // Vertical pass.
    for (uint32_t y = 0; y < dh; ++y) {
        const double fy = (double(y) + 0.5) * double(sh) / double(dh) - 0.5;
        double sy = fy < 0 ? 0 : fy;
        if (sy > sh - 1) sy = sh - 1;
        const uint32_t y0 = uint32_t(sy);
        const uint32_t y1 = y0 + 1 < sh ? y0 + 1 : y0;
        const double t = sy - y0;
        for (uint32_t x = 0; x < dw; ++x) {
            uint8_t* d = out.data() + (size_t(y) * dw + x) * 4;
            const uint8_t* p0 = tmp.data() + (size_t(y0) * dw + x) * 4;
            const uint8_t* p1 = tmp.data() + (size_t(y1) * dw + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const double v = p0[c] * (1.0 - t) + p1[c] * t;
                d[c] = uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5));
            }
        }
    }
    return out;
}
} // namespace

void GameRuntime::RuntimeState::post_frame_capture() {
    if (!capture_pending_ || !capture_fn_) return;
    capture_pending_ = false;
    uint32_t w = 0, h = 0;
    if (auto rgba = capture_fn_(&w, &h)) {
        screenshot_.valid = !rgba->empty() && w > 0 && h > 0 &&
                            rgba->size() == size_t(w) * h * 4;
        screenshot_.w = w;
        screenshot_.h = h;
        screenshot_.rgba = screenshot_.valid ? std::move(*rgba) : std::vector<uint8_t>();
    }
}

void GameRuntime::RuntimeState::handle_save_screenshot(const std::string& file, const std::string& width,
                                         const std::string& height) {
    if (file.empty()) return;
    // Target size from the [savess] params; falls back to the captured frame
    // size, then the placeholder size.
    uint32_t w = 320;
    uint32_t h = 180;
    if (screenshot_.valid && screenshot_.w > 0) w = screenshot_.w;
    if (screenshot_.valid && screenshot_.h > 0) h = screenshot_.h;
    if (!width.empty()) {
        try {
            const long v = std::stol(width);
            if (v > 0) w = uint32_t(v);
        } catch (...) {
        }
    }
    if (!height.empty()) {
        try {
            const long v = std::stol(height);
            if (v > 0) h = uint32_t(v);
        } catch (...) {
        }
    }
    std::vector<uint8_t> rgba;
    if (screenshot_.valid && !screenshot_.rgba.empty()) {
        rgba = resize_rgba(screenshot_.rgba.data(), screenshot_.w, screenshot_.h, w, h);
    } else {
        // Deterministic placeholder: slate background with a border.
        rgba.assign(size_t(w) * h * 4, 0x2A);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = (size_t(y) * w + x) * 4;
                if (y == 0 || y == h - 1 || x == 0 || x == w - 1) {
                    rgba[i] = rgba[i + 1] = rgba[i + 2] = uint8_t('o');
                }
                rgba[i + 3] = 255;
            }
        }
    }
    std::string name = file;
    const bool has_png = name.size() > 4 && name.rfind(".png") == name.size() - 4;
    if (!has_png) name += ".png";
    const std::vector<uint8_t> png = oa::media::encode_png(w, h, rgba);
    if (png.empty()) return;
    const std::string path = qualify_save_file(name);
    if (path.empty() || !save_store_) return;
    (void)save_store_->write(path, png);
}

void GameRuntime::RuntimeState::maybe_autosave_for_wait() {
    // Reset the once-per-wait latch whenever the parked wait identity changes
    // or no input wait is parked ( maybe_autosave_on_input_wait runs
    // only when a run establishes a new wait,  + ).
    std::string sig;
    if (const oa::runtime::WaitReason* w = wait_ ? &*wait_ : nullptr) {
        sig = std::string(wait_kind_name(w->kind)) + ":" + w->id;
    }
    if (sig != autosave_wait_sig_) {
        autosave_wait_sig_ = sig;
        autosaved_current_wait_ = false;
    }
    if (autosave_allow_ == 2 && is_input_wait(wait_ ? &*wait_ : nullptr) &&
        !autosaved_current_wait_ && save_store_ && save_store_->persistent()) {
        autosaved_current_wait_ = true;
        (void)save_game_to(kAutosaveFile);
    }
}

} // namespace oa::runtime

// ---------------------------------------------------------------------------
// Save format implementation: the
// binary codec for SaveData / sys-domain documents.
// ---------------------------------------------------------------------------

namespace oa::runtime {

namespace {

using oa::util::FormatError;
using oa::util::Reader;
using oa::util::Writer;

// Payload field names.
constexpr const char* kLocalVars = "local_variables";
constexpr const char* kCurrentScript = "current_script";
constexpr const char* kCurrentLine = "current_line";
constexpr const char* kCallStack = "call_stack";
constexpr const char* kScript = "script";
constexpr const char* kReturnLine = "return_line";
constexpr const char* kScene = "scene";
constexpr const char* kRootProps = "root_props";
constexpr const char* kLayers = "layers";
constexpr const char* kId = "id";
constexpr const char* kProps = "props";
constexpr const char* kHandlers = "handlers";
constexpr const char* kType = "type";
constexpr const char* kEnabled = "enabled";
constexpr const char* kPenetration = "penetration";
constexpr const char* kHandler = "handler";
constexpr const char* kFile = "file";
constexpr const char* kLabel = "label";
constexpr const char* kCall = "call";
constexpr const char* kParams = "params";
constexpr const char* kFilterParams = "filter_params";
constexpr const char* kAudio = "audio";
constexpr const char* kBgm = "bgm";
constexpr const char* kSe = "se";
constexpr const char* kVoice = "voice";
constexpr const char* kLoopPlay = "loop_play";
constexpr const char* kGain = "gain";
constexpr const char* kPan = "pan";
constexpr const char* kSkippable = "skippable";

/// 版本门（**未来跨版本策略的唯一分派点**）：新于本引擎 → 抛
/// FormatError（拒绝读，不猜）；旧于本引擎 → 按缺省值读下去（decode 的字段循环
/// 对缺字段一律用结构体默认值，从不迁移）。存档定义阶段：
/// 格式未冻结 ⇒ 现在不做跨版本兼容，只保留这一点作为未来加版本分派的位置；
/// 任何 SaveData 布局改动必须同步 bump kSaveFormatVersion（runtime_save.h）。
void check_version(const Reader& r) {
    if (r.version() > uint32_t(kSaveFormatVersion)) {
        throw FormatError("save format version " + std::to_string(r.version()) +
                          " newer than supported " +
                          std::to_string(kSaveFormatVersion));
    }
}

// ---------------------------------------------------------------------------
// runtime::Value <-> binary tags (nil/bool/int/float64/string only; the raw
// tags exist at the stream level for future binary carriers).
// ---------------------------------------------------------------------------
void write_value(Writer& w, const oa::runtime::Value& v) {
    switch (v.kind) {
        case oa::runtime::ValueKind::Int:
            w.i64(v.int_val);
            break;
        case oa::runtime::ValueKind::Float:
            w.f64(v.float_val);
            break;
        case oa::runtime::ValueKind::String:
            w.str(v.str_val);
            break;
        case oa::runtime::ValueKind::Bool:
            w.boolean(v.bool_val);
            break;
        case oa::runtime::ValueKind::Null:
        default:
            w.nil();
            break;
    }
}

oa::runtime::Value read_value(Reader& r) {
    const uint8_t t = r.peek_tag();
    if (t == oa::util::kTagNil) {
        (void)r.u8();
        return oa::runtime::Value::make_null();
    }
    if (t == oa::util::kTagTrue) {
        (void)r.u8();
        return oa::runtime::Value::make_bool(true);
    }
    if (t == oa::util::kTagFalse) {
        (void)r.u8();
        return oa::runtime::Value::make_bool(false);
    }
    if (Reader::is_int_tag(t)) return oa::runtime::Value::make_int(r.i64());
    if ((t >= 0xa0 && t <= 0xbf) || t == oa::util::kTagStr8 ||
        t == oa::util::kTagStr16 || t == oa::util::kTagStr32) {
        return oa::runtime::Value::make_string(r.str());
    }
    if (t == oa::util::kTagFloat32 || t == oa::util::kTagFloat64) {
        return oa::runtime::Value::make_float(r.f64());
    }
    throw FormatError("unsupported tag in variable value position");
}

// ---------------------------------------------------------------------------
// String maps (layer props / root props / handler params).
// ---------------------------------------------------------------------------
void write_str_map(Writer& w, const std::map<std::string, std::string>& m) {
    w.map_begin(m.size());
    for (const auto& [k, v] : m) {
        w.str(k);
        w.str(v);
    }
}

std::map<std::string, std::string> read_str_map(Reader& r) {
    std::map<std::string, std::string> out;
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string k = r.str();
        out[k] = r.str();
    }
    return out;
}

// ---------------------------------------------------------------------------
// Audio snapshot.
// ---------------------------------------------------------------------------
void write_channel(Writer& w, const AudioChannelSnap& c) {
    w.map_begin(6);
    w.str(kId);
    w.str(c.id);
    w.str(kFile);
    w.str(c.file);
    w.str(kLoopPlay);
    w.boolean(c.loop_play);
    w.str(kGain);
    w.i64(c.gain);
    w.str(kPan);
    w.i64(c.pan);
    w.str(kSkippable);
    w.boolean(c.skippable);
}

AudioChannelSnap read_channel(Reader& r) {
    AudioChannelSnap c;
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string key = r.str();
        if (key == kId) {
            c.id = r.str();
        } else if (key == kFile) {
            c.file = r.str();
        } else if (key == kLoopPlay) {
            c.loop_play = r.boolean();
        } else if (key == kGain) {
            const int64_t v = r.i64();
            if (v < std::numeric_limits<int32_t>::min() ||
                v > std::numeric_limits<int32_t>::max())
                throw FormatError("channel gain out of int32 range");
            c.gain = int32_t(v);
        } else if (key == kPan) {
            const int64_t v = r.i64();
            if (v < std::numeric_limits<int32_t>::min() ||
                v > std::numeric_limits<int32_t>::max())
                throw FormatError("channel pan out of int32 range");
            c.pan = int32_t(v);
        } else if (key == kSkippable) {
            c.skippable = r.boolean();
        } else {
            r.skip_value();
        }
    }
    return c;
}

void write_channels(Writer& w, const char* key,
                    const std::vector<AudioChannelSnap>& v) {
    w.str(key);
    w.array_begin(v.size());
    for (const auto& c : v) write_channel(w, c);
}

std::vector<AudioChannelSnap> read_channels(Reader& r) {
    std::vector<AudioChannelSnap> out;
    const size_t n = r.array_items();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(read_channel(r));
    return out;
}

void write_audio(Writer& w, const AudioSnap& a) {
    w.map_begin((a.bgm ? 1 : 0) + 2);
    if (a.bgm) {
        w.str(kBgm);
        write_channel(w, *a.bgm);
    }
    write_channels(w, kSe, a.se);
    write_channels(w, kVoice, a.voice);
}

AudioSnap read_audio(Reader& r) {
    AudioSnap a;
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string key = r.str();
        if (key == kBgm) {
            a.bgm = read_channel(r);
        } else if (key == kSe) {
            a.se = read_channels(r);
        } else if (key == kVoice) {
            a.voice = read_channels(r);
        } else {
            r.skip_value();
        }
    }
    return a;
}

// ---------------------------------------------------------------------------
// Layer event handlers + layer snapshot.
// ---------------------------------------------------------------------------
void write_handler(Writer& w, const LayerEventHandlerSnap& h) {
    w.map_begin(9);
    w.str(kType);
    w.str(h.type);
    w.str(kEnabled);
    w.boolean(h.enabled);
    w.str(kPenetration);
    w.boolean(h.penetration);
    w.str(kHandler);
    w.str(h.handler);
    w.str(kFile);
    w.str(h.file);
    w.str(kLabel);
    w.str(h.label);
    w.str(kCall);
    w.boolean(h.call);
    w.str(kParams);
    write_str_map(w, h.params);
    w.str(kFilterParams);
    write_str_map(w, h.filter_params);
}

LayerEventHandlerSnap read_handler(Reader& r) {
    LayerEventHandlerSnap h;
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string key = r.str();
        if (key == kType) {
            h.type = r.str();
        } else if (key == kEnabled) {
            h.enabled = r.boolean();
        } else if (key == kPenetration) {
            h.penetration = r.boolean();
        } else if (key == kHandler) {
            h.handler = r.str();
        } else if (key == kFile) {
            h.file = r.str();
        } else if (key == kLabel) {
            h.label = r.str();
        } else if (key == kCall) {
            h.call = r.boolean();
        } else if (key == kParams) {
            h.params = read_str_map(r);
        } else if (key == kFilterParams) {
            h.filter_params = read_str_map(r);
        } else {
            r.skip_value();
        }
    }
    return h;
}

void write_layer(Writer& w, const LayerSnap& ls) {
    w.map_begin(ls.handlers.empty() ? 2 : 3);
    w.str(kId);
    w.str(ls.id);
    w.str(kProps);
    write_str_map(w, ls.props);
    if (!ls.handlers.empty()) {
        w.str(kHandlers);
        w.array_begin(ls.handlers.size());
        for (const auto& h : ls.handlers) write_handler(w, h);
    }
}

LayerSnap read_layer(Reader& r) {
    LayerSnap ls;
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string key = r.str();
        if (key == kId) {
            ls.id = r.str();
        } else if (key == kProps) {
            ls.props = read_str_map(r);
        } else if (key == kHandlers) {
            const size_t hn = r.array_items();
            ls.handlers.reserve(hn);
            for (size_t j = 0; j < hn; ++j) ls.handlers.push_back(read_handler(r));
        } else {
            r.skip_value();
        }
    }
    return ls;
}

} // namespace

std::string SaveData::encode() const {
    if (current_line > size_t(std::numeric_limits<int64_t>::max()) ||
        call_stack.size() > size_t(std::numeric_limits<int64_t>::max())) {
        throw FormatError("save position out of int64 range");
    }
    Writer w;
    w.map_begin(4 + (has_scene ? 1 : 0) + (has_audio ? 1 : 0));
    w.str(kLocalVars);
    w.map_begin(local_variables.size());
    for (const auto& [k, v] : local_variables) {
        w.str(k);
        write_value(w, v);
    }
    w.str(kCurrentScript);
    w.str(current_script);
    w.str(kCurrentLine);
    w.i64(int64_t(current_line));
    w.str(kCallStack);
    w.array_begin(call_stack.size());
    for (const auto& [script, line] : call_stack) {
        if (line > size_t(std::numeric_limits<int64_t>::max())) {
            throw FormatError("return line out of int64 range");
        }
        w.map_begin(2);
        w.str(kScript);
        w.str(script);
        w.str(kReturnLine);
        w.i64(int64_t(line));
    }
    if (has_scene) {
        w.str(kScene);
        w.map_begin(2);
        w.str(kRootProps);
        write_str_map(w, root_props);
        w.str(kLayers);
        w.array_begin(layers.size());
        for (const auto& ls : layers) write_layer(w, ls);
    }
    if (has_audio) {
        w.str(kAudio);
        write_audio(w, audio);
    }
    return w.data();
}

SaveData SaveData::decode(const std::string& bytes) {
    Reader r(bytes);
    check_version(r);
    SaveData d;
    d.version = int(r.version());
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string key = r.str();
        if (key == kLocalVars) {
            const size_t vn = r.map_entries();
            for (size_t j = 0; j < vn; ++j) {
                const std::string name = r.str();
                d.local_variables[name] = read_value(r);
            }
        } else if (key == kCurrentScript) {
            d.current_script = r.str();
        } else if (key == kCurrentLine) {
            const int64_t line = r.i64();
            if (line < 0) throw FormatError("negative current_line in save");
            d.current_line = size_t(line);
        } else if (key == kCallStack) {
            const size_t cn = r.array_items();
            d.call_stack.reserve(cn);
            for (size_t j = 0; j < cn; ++j) {
                std::string script;
                int64_t line = 0;
                const size_t fn = r.map_entries();
                for (size_t k = 0; k < fn; ++k) {
                    const std::string fkey = r.str();
                    if (fkey == kScript) {
                        script = r.str();
                    } else if (fkey == kReturnLine) {
                        line = r.i64();
                    } else {
                        r.skip_value();
                    }
                }
                if (line < 0) throw FormatError("negative return_line in save");
                d.call_stack.emplace_back(std::move(script), size_t(line));
            }
        } else if (key == kScene) {
            d.has_scene = true;
            const size_t sn = r.map_entries();
            for (size_t j = 0; j < sn; ++j) {
                const std::string skey = r.str();
                if (skey == kRootProps) {
                    d.root_props = read_str_map(r);
                } else if (skey == kLayers) {
                    const size_t ln = r.array_items();
                    d.layers.reserve(ln);
                    for (size_t k = 0; k < ln; ++k) d.layers.push_back(read_layer(r));
                } else {
                    r.skip_value();
                }
            }
        } else if (key == kAudio) {
            d.has_audio = true;
            d.audio = read_audio(r);
        } else {
            r.skip_value();
        }
    }
    r.expect_eof("save document");
    return d;
}

std::string encode_domain_map(const std::map<std::string, oa::runtime::Value>& vars) {
    Writer w;
    w.map_begin(vars.size());
    for (const auto& [k, v] : vars) {
        w.str(k);
        write_value(w, v);
    }
    return w.data();
}

std::map<std::string, oa::runtime::Value> decode_domain_map(const std::string& bytes) {
    Reader r(bytes);
    check_version(r);
    std::map<std::string, oa::runtime::Value> out;
    const size_t n = r.map_entries(); // throws when the root is not a map
    for (size_t i = 0; i < n; ++i) {
        const std::string k = r.str();
        out[k] = read_value(r);
    }
    r.expect_eof("domain map");
    return out;
}

} // namespace oa::runtime

// ---------------------------------------------------------------------------
// SaveStore implementations (moved from core/fs/store.cpp).
// ---------------------------------------------------------------------------

namespace oa::runtime {

DirSaveStore::DirSaveStore(std::string root) : root_(std::move(root)) {
    if (!root_.empty()) mount_ = std::make_unique<oa::fs::WritableMount>(root_);
}

bool DirSaveStore::write(const std::string& rel_path, const std::vector<uint8_t>& data) {
    return mount_ && mount_->write(rel_path, data);
}

std::optional<std::vector<uint8_t>> DirSaveStore::read(const std::string& rel_path) const {
    return mount_ ? mount_->read(rel_path) : std::nullopt;
}

bool DirSaveStore::remove(const std::string& rel_path) {
    return mount_ && mount_->remove(rel_path);
}

bool DirSaveStore::exists(const std::string& rel_path) const {
    return mount_ && mount_->exists(rel_path);
}

std::optional<std::array<int64_t, 6>> DirSaveStore::modification_time(
    const std::string& rel_path) const {
    return mount_ ? mount_->modification_time(rel_path) : std::nullopt;
}

} // namespace oa::runtime
