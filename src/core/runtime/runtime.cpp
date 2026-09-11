#include "core/runtime/runtime_internal.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "core/fs/project.h"
#include "core/render/content_role.h" // bound_image_size 键公式收敛

namespace oa::runtime {

namespace {
// virtual key codes used by Artemis scripts
constexpr int kKeyMouseLeft = 1;
constexpr int kKeyMouseRight = 2;
constexpr int kKeyEnter = 13;
constexpr int kKeySpace = 32;

// Script param -> int, `dflt` when the text is not a full integer (the one
// file-local copy for every domain in this TU; the former
// parse_i32 / parse_i32_dflt pair had identical bodies).
int parse_i32(const std::string& v, int dflt) {
    try {
        return std::stoi(v);
    } catch (...) {
        return dflt;
    }
}

// Layer-prop number text for the [lyprop] readback and the click-wait icon
// homing: integral values print without a decimal point, others through %g
// (the two byte-identical lambdas that each defined it are merged).
std::string format_prop_num(double v) {
    char buf[64];
    const double iv = std::floor(v);
    if (v == iv) {
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    } else {
        std::snprintf(buf, sizeof(buf), "%g", v);
    }
    return std::string(buf);
}

// Header-only still-image dimension sniffing (PNG IHDR, JPEG
// SOFn) for the get_layer_info natural-size fallback. Decoding whole frames
// just to answer a width query would be wasteful (and potentially huge for
// bg/fg textures); the reference engine's UI rows resolve their size from
// the bound image, and only these two formats exist in Artemis UI data.
bool sniff_image_dims(const std::vector<uint8_t>& b, double* w, double* h) {
    *w = 0;
    *h = 0;
    static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (b.size() >= 24 && std::memcmp(b.data(), kPngSig, 8) == 0 && b[12] == 'I' &&
        b[13] == 'H' && b[14] == 'D' && b[15] == 'R') {
        const uint32_t W = (uint32_t(b[16]) << 24) | (uint32_t(b[17]) << 16) |
                           (uint32_t(b[18]) << 8) | uint32_t(b[19]);
        const uint32_t H = (uint32_t(b[20]) << 24) | (uint32_t(b[21]) << 16) |
                           (uint32_t(b[22]) << 8) | uint32_t(b[23]);
        if (W != 0 && H != 0 && W <= 65536 && H <= 65536) {
            *w = double(W);
            *h = double(H);
            return true;
        }
        return false;
    }
    if (b.size() >= 4 && b[0] == 0xFF && b[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < b.size()) {
            if (b[i] != 0xFF) return false;
            const uint8_t m = b[i + 1];
            if (m == 0xD8 || m == 0x01) {  // SOI / TEM: no length
                i += 2;
                continue;
            }
            if (m >= 0xD0 && m <= 0xD7) {  // RSTn: no length
                i += 2;
                continue;
            }
            if (m == 0xD9 || m == 0xDA) return false;  // EOI / SOS: no SOF before
            if (i + 4 > b.size()) return false;
            const uint16_t len = (uint16_t(b[i + 2]) << 8) | uint16_t(b[i + 3]);
            if (len < 2) return false;
            // SOF0-SOF3 / SOF5-SOF7 / SOF9-SOF11 / SOF13-SOF15 (exclude the
            // non-frame tables DHT/DAC/DRI/APPn/COM)
            const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
            if (sof) {
                if (i + 9 > b.size()) return false;
                const uint16_t H = (uint16_t(b[i + 5]) << 8) | uint16_t(b[i + 6]);
                const uint16_t W = (uint16_t(b[i + 7]) << 8) | uint16_t(b[i + 8]);
                if (W != 0 && H != 0) {
                    *w = double(W);
                    *h = double(H);
                    return true;
                }
                return false;
            }
            i += 2 + size_t(len);
        }
    }
    return false;
}
} // namespace

// ===========================================================================
// 对外接口：GameRuntime 薄转发（"小接口，大文件"）
//     实现面（状态 + 逻辑）全在 runtime_internal.h 的 RuntimeState；本类只持有
//     s_ 并把每个公开方法转给它的同名实现。公开签名与 runtime.h 逐字一致，
//     因此 56 个调用方一行不改。
// ===========================================================================
GameRuntime::RuntimeState::RuntimeState(GameRuntime* rt,
                                       std::shared_ptr<const oa::fs::IFileSystem> fs)
    // 初始化列表照搬原 GameRuntime 构造函数；顺序 = 声明顺序。
    : rt_(rt),
      save_store_(std::make_shared<oa::runtime::NullSaveStore>()),
      media_players_(std::make_unique<oa::media::MediaPlayers>()),
      fs_(std::move(fs)) {}

GameRuntime::GameRuntime(std::shared_ptr<const oa::fs::IFileSystem> fs)
    : s_(std::make_unique<RuntimeState>(this, std::move(fs))) {}

GameRuntime::~GameRuntime() = default;

// ---- 公开方法：逐个转发 ----------------------------------------------------

void GameRuntime::open_project(std::string_view platform) { s_->open_project(platform); }
void GameRuntime::boot_project() { s_->boot_project(); }
void GameRuntime::restart_project() { s_->restart_project(); }
void GameRuntime::dispatch_tween_done(const std::vector<oa::render::TweenDone>& done) { s_->dispatch_tween_done(done); }
void GameRuntime::tick(uint64_t delta_ms, const FrameInput& input) { s_->tick(delta_ms, input); }
bool GameRuntime::exit_requested() const { return s_->exit_requested(); }
std::vector<oa::runtime::Event> GameRuntime::drain_events() { return s_->drain_events(); }
oa::runtime::Interpreter& GameRuntime::interpreter() { return s_->interpreter(); }
const oa::runtime::Interpreter& GameRuntime::interpreter() const { return s_->interpreter(); }
const std::shared_ptr<const oa::fs::IFileSystem>& GameRuntime::fs() const { return s_->fs(); }
const oa::render::Compositor& GameRuntime::scene() const { return s_->scene(); }
size_t GameRuntime::scene_layer_events() const { return s_->scene_layer_events(); }
size_t GameRuntime::scene_tween_events() const { return s_->scene_tween_events(); }
size_t GameRuntime::tween_completion_calls() const { return s_->tween_completion_calls(); }
bool GameRuntime::waiting_stop() const { return s_->waiting_stop(); }
const oa::runtime::WaitReason* GameRuntime::current_wait() const { return s_->current_wait(); }
void GameRuntime::apply_override(int key, int status) { s_->apply_override(key, status); }
TransitionStatus GameRuntime::transition() { return TransitionStatus(&s_->transition_); }
TransitionStatus GameRuntime::transition() const { return TransitionStatus(&s_->transition_); }

// ---- [trans] 只读投影 ------------------------------------------------------
// 转发到 render 内部语义机 oa::render::Transition 的同名只读查询（定义见
// runtime_internal.h -> core/render/render_internal.h）。state_ 恒非空：
// 只有 GameRuntime::transition() 能构造本类（构造私有 + friend GameRuntime）。
bool TransitionStatus::active() const { return state_ && state_->active(); }
bool TransitionStatus::is_in_progress(uint64_t clock_ms) const {
    return state_ && state_->is_in_progress(clock_ms);
}
double TransitionStatus::progress(uint64_t clock_ms) const {
    return state_ ? state_->progress(clock_ms) : 0.0;
}
int TransitionStatus::type() const { return state_ ? state_->type() : 0; }
const std::string& TransitionStatus::rule() const {
    static const std::string kNoRule;
    return state_ ? state_->rule() : kNoRule;
}
int TransitionStatus::vague() const { return state_ ? state_->vague() : 32; }
uint64_t GameRuntime::now_ms() const { return s_->now_ms(); }
void GameRuntime::transition_begin(const std::map<std::string, std::string>& params) { s_->transition_begin(params); }
void GameRuntime::set_transition_capture_callback(std::function<void()> cb) { s_->set_transition_capture_callback(std::move(cb)); }
void GameRuntime::mark_transition_captured() { s_->mark_transition_captured(); }
void GameRuntime::begin_all_delete(uint64_t time_ms) { s_->begin_all_delete(time_ms); }
double GameRuntime::all_delete_fade() const { return s_->all_delete_fade(); }
void GameRuntime::set_hit_providers(oa::render::Compositor::QuadSizeFn size, oa::render::Compositor::AlphaSamplerFn alpha, void* userdata) { s_->set_hit_providers(std::move(size), std::move(alpha), userdata); }
void GameRuntime::set_pointer_observer(std::function<void(const PointerDispatch&)> cb) { s_->set_pointer_observer(std::move(cb)); }
const std::set<std::string>& GameRuntime::hovered_layers() const { return s_->hovered_layers(); }
bool GameRuntime::is_hovered(const std::string& id) const { return s_->is_hovered(id); }
size_t GameRuntime::pointer_dispatch_count() const { return s_->pointer_dispatch_count(); }
void GameRuntime::set_pointer_warp_callback(std::function<void(int, int)> cb) { s_->set_pointer_warp_callback(std::move(cb)); }
size_t GameRuntime::pointer_warp_requests() const { return s_->pointer_warp_requests(); }
std::pair<int, int> GameRuntime::mouse_point() const { return s_->mouse_point(); }
bool GameRuntime::skip_active() const { return s_->skip_active(); }
bool GameRuntime::automode_active() const { return s_->automode_active(); }
bool GameRuntime::hide_active() const { return s_->hide_active(); }
bool GameRuntime::control_skip_effective() const { return s_->control_skip_effective(); }
bool GameRuntime::rclick_allowed() const { return s_->rclick_allowed(); }
void GameRuntime::force_pointer_drag(const std::string& layer_id) { s_->force_pointer_drag(layer_id); }
const oa::render::TextEngine& GameRuntime::text() const { return s_->text(); }
oa::render::TextEngine& GameRuntime::text() { return s_->text(); }
uint64_t GameRuntime::text_revision() const { return s_->text_revision(); }
size_t GameRuntime::text_events() const { return s_->text_events(); }
size_t GameRuntime::message_switch_events() const { return s_->message_switch_events(); }
bool GameRuntime::apply_text_event(const oa::runtime::Event& e) { return s_->apply_text_event(e); }
oa::media::AudioEngine& GameRuntime::audio() { return s_->audio(); }
const oa::media::AudioEngine& GameRuntime::audio() const { return s_->audio(); }
oa::media::VideoEngine& GameRuntime::video() { return s_->video(); }
const oa::media::VideoEngine& GameRuntime::video() const { return s_->video(); }
const std::map<std::string, GameRuntime::EmoteLayerState>& GameRuntime::emote_layers() const { return s_->emote_layers(); }
size_t GameRuntime::emote_layer_events() const { return s_->emote_layer_events(); }
oa::media::MediaPlayers& GameRuntime::media_players() { return s_->media_players(); }
void GameRuntime::set_media_loader(std::function<std::optional<std::vector<uint8_t>>(const std::string&)> loader) { s_->set_media_loader(std::move(loader)); }
size_t GameRuntime::media_events() const { return s_->media_events(); }
size_t GameRuntime::media_handler_dispatches() const { return s_->media_handler_dispatches(); }
void GameRuntime::enable_decode_pool(int threads) { s_->enable_decode_pool(threads); }
void GameRuntime::set_save_store(std::shared_ptr<oa::runtime::SaveStore> store) { s_->set_save_store(std::move(store)); }
const std::shared_ptr<oa::runtime::SaveStore>& GameRuntime::save_store() const { return s_->save_store(); }
void GameRuntime::set_frame_capture(CaptureFn fn) { s_->set_frame_capture(std::move(fn)); }
void GameRuntime::post_frame_capture() { s_->post_frame_capture(); }
void GameRuntime::sysload() { s_->sysload(); }
bool GameRuntime::syssave() { return s_->syssave(); }
bool GameRuntime::save_game_to(const std::string& file) { return s_->save_game_to(file); }
LoadResult GameRuntime::load_game(const std::string& file, int64_t trans_type) {
    return s_->load_game(file, trans_type);
}
bool GameRuntime::load_game_from(const std::string& file, int64_t trans_type) { return s_->load_game_from(file, trans_type); }
const std::string& GameRuntime::savepath() const { return s_->savepath(); }
bool GameRuntime::save_file_exists(const std::string& file) const { return s_->save_file_exists(file); }
void GameRuntime::apply_save_event(const oa::runtime::Event& e) { s_->apply_save_event(e); }
bool GameRuntime::apply_media_event(const oa::runtime::Event& e) { return s_->apply_media_event(e); }
void GameRuntime::advance_media_frame(uint64_t delta_ms) { s_->advance_media_frame(delta_ms); }
void GameRuntime::sound_finished(oa::media::SoundCategory cat, const std::string& id) { s_->sound_finished(cat, id); }
bool GameRuntime::media_se_wait_finished(const std::string& id, bool time_given, uint64_t time_ms) const { return s_->media_se_wait_finished(id, time_given, time_ms); }


std::pair<double, double> GameRuntime::RuntimeState::bound_image_size(const oa::render::Layer& l) {
    // Natural (texture) size of the image bound to `l`, header-sniffed once
    // and cached. Mirror of the RenderEngine resolution face: the decoded
    // cache key is the logical file name (path prefix + file) and the fs
    // candidates are raw / +.png / +.jpg (renderer.cpp resolve_image).
    // 键公式收敛为角色 texture_key
    // (与渲染/命中/采样面同源)。
    // 键升级为 TextureKey 且只探测 Asset 域 —— 宿主供帧层(视频/emote)的
    // 资源名不是文件,恒 {0,0}(与旧实现"用保留命名空间去 fs 探测、恒失败"
    // 的结果逐位一致,同时不再可能撞上同名的真实资源)。
    if (l.file.empty()) return {0, 0};
    const oa::render::TextureKey key = oa::render::content_role_of(l).texture_key(l);
    if (key.empty() || !key.is_asset()) return {0, 0};
    const auto hit = image_size_cache_.find(key);
    if (hit != image_size_cache_.end()) return hit->second;
    std::pair<double, double> dims = {0, 0};
    auto probe = [&](const std::string& logical) {
        if (!interpreter_) return false;
        const std::string resolved = interpreter_->resolve_magic_path(logical);
        for (const std::string& cand : {resolved, resolved + ".png", resolved + ".jpg"}) {
            const auto data = fs_->read(cand);
            if (!data) continue;
            double w = 0, h = 0;
            if (sniff_image_dims(*data, &w, &h)) {
                dims = {w, h};
                return true;
            }
        }
        return false;
    };
    probe(key.name);
    image_size_cache_[key] = dims;  // negative results cached too (stable)
    return dims;
}

void GameRuntime::RuntimeState::enable_decode_pool(int threads) {
    if (threads == 0) {
        if (decode_pool_) {
            if (media_players_) media_players_->set_decode_pool(nullptr);
            video_.set_decode_pool(nullptr);
            decode_pool_.reset();
        }
        return;
    }
    decode_pool_ = std::make_unique<oa::media::DecodePool>(threads);
    if (media_players_) media_players_->set_decode_pool(decode_pool_.get());
    video_.set_decode_pool(decode_pool_.get());
}

void GameRuntime::RuntimeState::open_project(std::string_view platform) {
    platform_ = std::string(platform);
    rt_->project_ = oa::fs::Project::open(*fs_, platform);
    oa::runtime::Interpreter::Config cfg;
    cfg.charset = rt_->project_.config.charset;
    cfg.platform = rt_->project_.config.platform;
    cfg.stage_width = rt_->project_.config.stage_width;
    cfg.stage_height = rt_->project_.config.stage_height;
    cfg.fps = rt_->project_.config.fps;
    cfg.env = rt_->project_.config.env;

    interpreter_ = std::make_unique<oa::runtime::Interpreter>(cfg);
    // Native [alldelete] handler. Registered as an engine tag so the
    // Lua tag filter cannot swallow it (FPM registers tags.alldelete but its
    // Lua-side lydel bookkeeping races [exit]/[reset]; the engine contract is
    // the whole-scene fade-out + clear). The runtime callback
    // (set_callback) receives the emitted custom event and runs the fade.
    interpreter_->register_engine_tag(
        "alldelete",
        [](oa::runtime::Interpreter&, const oa::runtime::Instruction& ins)
            -> std::optional<oa::runtime::Event> {
            return oa::runtime::Event::custom("alldelete", ins.params);
        });
    // Native "emotestatic" handler — the real e-table
    // createEmoteLayer enqueues lyc2 + emotestatic; the runtime decodes the
    // PSB and renders the static pose (GameRuntime::apply_emote_static).
    interpreter_->register_engine_tag(
        "emotestatic",
        [](oa::runtime::Interpreter&, const oa::runtime::Instruction& ins)
            -> std::optional<oa::runtime::Event> {
            return oa::runtime::Event::custom("emotestatic", ins.params);
        });
    // e-table EmoteLayer methods route into the runtime
    // (players may not exist yet — calls queue and replay after creation).
    interpreter_->lua_bridge().host().emote_method =
        [this](const std::string& id, const std::string& method,
               const std::string& sv, const std::string& sv2, double n1,
               double n2, double n3, double* out, std::string* sout) {
            return emote_method_dispatch(id, method, sv, sv2, n1, n2, n3, out,
                                         sout);
        };
    interpreter_->variables().platform = cfg.platform;
    // project-level seeding: savepath 与系统存档读取
    savepath_ = sanitize_savepath(rt_->project_.config.savepath);
    interpreter_->set_variable("s.savepath", oa::runtime::Value::make_string(savepath_));
    interpreter_->hooks().frame_number = [this] { return tick_count_; };
    interpreter_->hooks().file_loader = [this](const std::string& name)
        -> std::optional<std::vector<uint8_t>> {
        return fs_->read(interpreter_->resolve_magic_path(name));
    };
    // Slider readback: e:var system=get_layer_info dumps one scene
    // layer's effective props (typed left/top/width/height/alpha/visible/
    // clip + verbatim params; slider_dragX reads name.left after [lyprop]).
    interpreter_->hooks().layer_info = [this](const std::string& id)
        -> std::map<std::string, std::string> {
        std::map<std::string, std::string> out;
        const oa::render::Layer* l = scene_.find(id);
        if (!l) return out;
        for (const auto& [k, v] : l->props) out[k] = v;
        out["left"] = format_prop_num(l->left);
        out["top"] = format_prop_num(l->top);
        out["x"] = format_prop_num(l->left);
        out["y"] = format_prop_num(l->top);
        // Report the layer's EFFECTIVE width/height (hit-test size
        // precedence: explicit width/height > clip w/h). FPM's csvbtn3
        // "obj" layers (e.g. the tablet tb_mask overlay) are created as
        // clipped images without explicit width/height; the reference engine
        // reports their clip size here, and FPM geometry math depends on it
        // (getTabletPos p.w -> tab_left/right bx = p.w - b; a 0 here made the
        // touchbar containment toggles park the bar at x=b instead of
        // ±(w-b)).
        // The same precedence continues to the bound image's
        // natural (texture) size. Newer framework revisions (甜蜜女友3)
        // create the touchbar drag overlay as a plain full-stage image row
        // (mw/tablet/tb_mask: no explicit w/h, no clip — its width is the
        // png's 1920); with a 0 fallback the containment arithmetic degraded
        // to bx = -b and the bar parked at ±b (almost fully visible) instead
        // of ±(w-b), i.e. the touchbar could never fully collapse.
        std::pair<double, double> natural = {0, 0};
        if (l->width <= 0 && !l->has_clip) natural = bound_image_size(*l);
        out["width"] = format_prop_num(l->width > 0
                                           ? l->width
                                           : (l->has_clip ? l->clip_w : natural.first));
        out["height"] = format_prop_num(l->height > 0
                                            ? l->height
                                            : (l->has_clip ? l->clip_h : natural.second));
        out["alpha"] = format_prop_num(l->alpha * 255.0);
        out["visible"] = l->visible != 0.0 ? "1" : "0";
        if (l->has_clip) {
            out["clip"] = format_prop_num(l->clip_x) + "," + format_prop_num(l->clip_y) +
                          "," + format_prop_num(l->clip_w) + "," +
                          format_prop_num(l->clip_h);
            out["clip_x"] = format_prop_num(l->clip_x);
            out["clip_y"] = format_prop_num(l->clip_y);
            out["clip_w"] = format_prop_num(l->clip_w);
            out["clip_h"] = format_prop_num(l->clip_h);
        }
        return out;
    };
    // The decode host reads media assets through the same resolution:
    // the interpreter's magic-path table first, then the project filesystem.
    media_players_->set_loader([this](const std::string& logical)
                                   -> std::optional<std::vector<uint8_t>> {
        if (!interpreter_) return std::nullopt;
        return fs_->read(interpreter_->resolve_magic_path(logical));
    });
    // The video decoder reads whole asset files through the same face.
    // A missing/undecodable file keeps the logical immediate-finish fallback
    // inside VideoEngine, so project flows never hang on unavailable video.
    video_.set_loader([this](const std::string& logical)
                          -> std::optional<std::vector<uint8_t>> {
        if (!interpreter_) return std::nullopt;
        return fs_->read(interpreter_->resolve_magic_path(logical));
    });
    // Save-area aware asset hooks — the runtime serves both the
    // project filesystem (assets) and the save store (files under the logical
    // savepath) through one resolution face, matching the host file
    // reader. Hooks: e:file / e:isFileExists / var file_exist(save) /
    // file_update_time / get_sound_info.
    const auto read_resolved =
        [this](const std::string& resolved) -> std::optional<std::vector<uint8_t>> {
        const bool under_save = !savepath_.empty() &&
                                (resolved == savepath_ ||
                                 (resolved.size() > savepath_.size() &&
                                  resolved.rfind(savepath_ + "/", 0) == 0));
        if (under_save && save_store_) {
            const std::string rel = qualify_save_file(resolved);
            if (!rel.empty()) {
                if (auto b = save_store_->read(rel)) return b;
                return std::nullopt; // save-area names do not fall back to PFS
            }
        }
        return fs_->read(resolved);
    };
    interpreter_->hooks().resource_read = read_resolved;
    interpreter_->hooks().resource_exists = [read_resolved](const std::string& resolved) {
        return read_resolved(resolved).has_value();
    };
    interpreter_->hooks().save_file_exists = [this](const std::string& file) -> bool {
        const std::string rel = qualify_save_file(file);
        if (rel.empty() || !save_store_) return false;
        return save_store_->exists(rel);
    };
    interpreter_->hooks().file_mtime =
        [this](const std::string& file)
        -> std::optional<std::array<int64_t, 6>> {
        const std::string rel = qualify_save_file(file);
        if (rel.empty() || !save_store_) return std::nullopt;
        return save_store_->modification_time(rel);
    };
    interpreter_->hooks().sound_info = [this] { return sound_info_snapshot_for_hook(); };
    // e:var system=get_backlog_size/get_backlog_tags/get_message_tags
    // host query hooks → oa::render::TextEngine data surface. The interpreter
    // side of this bridge was previously unwired; here the runtime provides
    // the real data.
    interpreter_->hooks().backlog_size = [this]() -> size_t {
        return text_.backlog_size();
    };
    interpreter_->hooks().backlog_tags = [this](size_t page, bool allfont)
        -> std::optional<std::vector<std::string>> {
        return text_.backlog_tags(page, allfont);
    };
    interpreter_->hooks().message_tags = [this](const std::string& id, bool allfont)
        -> std::optional<std::vector<std::string>> {
        return text_.message_tags(id, allfont);
    };
    // Waits materialize only when the callback pauses the interpreter; every
    // event (waits included) is queued for the host renderer/audio bridges.
    // [trans] with type != 0 pauses the script (Stop{reason:"trans"} wait);
    // type 0 is an
    // instant switch that flows through without pausing.
    // Scene events (Layer*/lytween/tweenset) never reach the host stream
    // — the runtime applies them to its Compositor right here at dispatch.
    interpreter_->set_callback([this](const oa::runtime::Event& e) {
        // [alldelete time=] (FPM go_title/go_exit/suspend rows): engine-
        // native whole-scene fade-out + clear. Starts the fade here and parks
        // the script until it completes (Stop{id:"alldelete"}); time==0
        // clears instantly and keeps the script running. Without this tag the
        // engine-owned message/text overlay nodes outlived the Lua-side scene
        // clear and painted story text over the black transition page.
        if (e.tag == "alldelete") {
            uint64_t t = 0;
            if (const auto it = e.params.find("time"); it != e.params.end()) {
                try {
                    t = (uint64_t)std::stoull(it->second);
                } catch (...) {
                    t = 0;
                }
            }
            begin_all_delete(t);
            if (t > 0) return oa::runtime::CallbackResult::Pause;
            return oa::runtime::CallbackResult::Continue;
        }
        // e:createEmoteLayer static layer (lyc2 already
        // materialized the carrier chain). Decode + render + bind happen
        // here; the script never pauses for it.
        if (e.tag == "emotestatic") {
            apply_emote_static(e);
            return oa::runtime::CallbackResult::Continue;
        }
        // Media tags (audio/video) are consumed first at dispatch time
        // (dispatch order: apply_media_event before text/compositor).
        // A fullscreen [video] (no id) still
        // pauses the script — the wait machine maps that pause to a
        // Stop{reason:"video"} wait.
        if (apply_media_event(e)) {
            if (e.tag == "video" && e.id.empty()) {
                return oa::runtime::CallbackResult::Pause;
            }
            return oa::runtime::CallbackResult::Continue;
        }
        // Message-layer text domain consumes the text tags at dispatch
        // time (apply_text_event); they never pause.
        if (apply_text_event(e)) {
            return oa::runtime::CallbackResult::Continue;
        }
        if (apply_scene_event(e)) {
            // consumed by the scene; still never pauses the script (no scene
            // event is a wait), but keep the Wait_/Trans handling below shared
            // by falling through is not needed — scene events never pause.
            return oa::runtime::CallbackResult::Continue;
        }
        pending_events_.push_back(e);
        if (e.kind == oa::runtime::Event::Kind::Wait_) {
            return oa::runtime::CallbackResult::Pause;
        }
        if (e.kind == oa::runtime::Event::Kind::Trans) {
            int type = 1; // default (see the tags table)
            if (const auto it = e.params.find("type"); it != e.params.end()) {
                try {
                    type = std::stoi(it->second);
                } catch (...) {
                    type = 1;
                }
            }
            if (type != 0) return oa::runtime::CallbackResult::Pause;
        }
        return oa::runtime::CallbackResult::Continue;
    });
}

void GameRuntime::RuntimeState::boot_project()
{
    // Read previous system saves (saveg.dat/system.dat) into the g./s.
    // domains before boot — the Lua framework's dataloading reads them via
    // e:var.
    sysload();
    wire_lua_host();
    interpreter_->boot(rt_->project_.config.boot_script);
}

void GameRuntime::RuntimeState::reset_domains() {
    // Domain clears for engine reset / go-title:
    // media/audio/video, text and
    // scene, transition, hover/drag/pointer state, the inline-event marker,
    // the [lytween sync] latch and the parked wait. Engine control flags are
    // returned to their pre-boot defaults — the boot script re-applies its
    // keyconfig/hide/rclick/skip/automode tags anyway (FPM init.lua).
    audio_.stop_all_sounds();
    if (media_players_) media_players_->stop_all();
    video_.stop_all_videos();
    // 与读档清场对齐（同一份 [C] 清单两处实现）——全屏视频结束闩
    // 若跨重置存活，会让新场景的任意 Stop 停驻被一次陈旧 EOF 释放。
    video_finished_ = false;
    text_.clear_scene();
    scene_.clear_scene();
    drop_pending_tween_cancels(); // deferred timer rounds die with nodes
    emote_layers_.clear(); // static emote frames die with the scene
    transition_.clear();
    hovered_.clear();
    drag_layer_.clear();
    pending_sync_tween_layer_.reset();
    inline_event_frame_.reset();
    screenshot_.valid = false;
    capture_pending_ = false;
    alldelete_active_ = false;
    alldelete_start_ms_ = 0;
    alldelete_duration_ms_ = 0;
    wait_.reset();
    wait_remaining_ms_ = 0;
    exit_requested_ = false;
    skip_active_ = false;
    exskip_active_ = false; // e:debugSkip state dies with the domains
    skip_allowed_ = true;
    skip_enabled_ = false;
    automode_allowed_ = true;
    automode_ = false;
    auto_elapsed_ms_ = 0;
    control_skip_pressed_ = false;
    control_skip_blocked_ = false;
    last_control_skip_effective_ = false;
    rclick_allowed_ = false;
    rclick_file_.clear();
    hide_allowed_ = false;
    hide_active_ = false;
    hide_window_.clear();
    hide_snapshot_.clear();
    keymap_.clear();
    script_status_ = 0;
    active_wait_icon_.clear();
    overrides_.clear();
    prev_overrides_.clear();
    keys_down_.clear();
    down_edges_.clear();
    up_edges_.clear();
    decide_edge_ = false;
    mouse_prev_x_ = 0;
    mouse_prev_y_ = 0;
    mouse_inited_ = false;
    left_down_prev_ = false;
}

// ---------------------------------------------------------------------------
// [alldelete] (engine-native whole-scene fade-out + clear)
// ---------------------------------------------------------------------------

void GameRuntime::RuntimeState::begin_all_delete(uint64_t time_ms) {
    // FPM go_title/go_exit/suspend rows: "[alldelete time=1500]" fades every
    // scene layer (message-text overlay nodes included) to 0 on one global
    // opacity ramp, then clears the scene and the text engine — the black
    // transition page. time==0 clears instantly. The script pauses
    // (Stop{id:"alldelete"}) while the fade runs (see resolve_wait).
    alldelete_start_ms_ = now_ms_;
    alldelete_duration_ms_ = time_ms;
    alldelete_active_ = true;
    if (time_ms == 0) finish_all_delete();
}

double GameRuntime::RuntimeState::all_delete_fade() const {
    if (!alldelete_active_ || alldelete_duration_ms_ == 0) return 1.0;
    const uint64_t elapsed =
        now_ms_ >= alldelete_start_ms_ ? now_ms_ - alldelete_start_ms_ : 0;
    const double p = elapsed >= alldelete_duration_ms_
                         ? 1.0
                         : double(elapsed) / double(alldelete_duration_ms_);
    return 1.0 - p; // 1 -> 0
}

void GameRuntime::RuntimeState::finish_all_delete() {
    alldelete_active_ = false;
    alldelete_start_ms_ = 0;
    alldelete_duration_ms_ = 0;
    // Everything the [alldelete] flow wipes: scene layers (image + text
    // overlay nodes + handler registries + message bindings), the text
    // engine content, and the pointer state pointing into them.
    scene_.clear_scene();
    text_.clear_scene();
    drop_pending_tween_cancels(); // deferred timer rounds die with nodes
    hovered_.clear();
    drag_layer_.clear();
    pending_sync_tween_layer_.reset();
    screenshot_.valid = false;
    capture_pending_ = false;
    emote_layers_.clear(); // static emote frames die with the scene
}

void GameRuntime::RuntimeState::restart_project() {
    if (!interpreter_) return;
    reset_domains();
    // Reboot with a fresh interpreter (fresh Lua VM: the boot script's [lua]
    // init blocks re-run — reusing the VM would leave the framework globals
    // at their mid-game state). open_project re-wires every interpreter hook;
    // the host-side bridges (save store, hit providers, capture) survive.
    pending_events_.clear();
    open_project(platform_);
    boot_project();
}

void GameRuntime::RuntimeState::wire_lua_host() {
    auto& host = interpreter_->lua_bridge().host();
    auto* interp = interpreter_.get();
    host.now_ms = [this] { return now_ms_; };
    host.get_script_status = [this] { return script_status_; };
    host.set_script_status = [this](int s) { script_status_ = s; };
    // e:debugSkip (FPM 次の選択肢に進む) starts the engine-side
    // exskip fast-forward. index=99999 from FPM; no registered meaning yet
    // (logged under OA_EXSKIPDBG).
    host.debug_skip_start = [this](int64_t index) {
        start_debug_skip();
        if (std::getenv("OA_EXSKIPDBG"))
            std::fprintf(stderr, "[exskipdbg] e:debugSkip index=%lld\n",
                         (long long)index);
    };
    host.is_down = [this](int key) {
        if (const auto it = overrides_.find(key); it != overrides_.end()) {
            if (it->second & 4) return true;
        }
        return keys_down_.count(key) > 0;
    };
    host.is_down_edge = [this](int key) {
        if (const auto it = overrides_.find(key); it != overrides_.end()) {
            const int prev = prev_overrides_.count(key) ? prev_overrides_.at(key) : 0;
            if ((it->second & 8) && !(prev & 8)) return true;
        }
        // physical edges are delivered through down-edge set computed in tick
        return down_edges_.count(key) > 0;
    };
    host.is_up_edge = [this](int key) {
        if (const auto it = overrides_.find(key); it != overrides_.end()) {
            const int prev = prev_overrides_.count(key) ? prev_overrides_.at(key) : 0;
            if ((it->second & 16) && !(prev & 16)) return true;
        }
        return up_edges_.count(key) > 0;
    };
    host.is_push = [this](int key) {
        if (const auto it = overrides_.find(key); it != overrides_.end()) {
            if (it->second & 2) return true;
        }
        return keys_down_.count(key) > 0;
    };
    host.is_decide = [this](int key) {
        if (const auto it = overrides_.find(key); it != overrides_.end()) {
            const int prev = prev_overrides_.count(key) ? prev_overrides_.at(key) : 0;
            if ((it->second & 32) && !(prev & 32)) return true;
        }
        return false;
    };
    host.get_mouse_point = [this] {
        return std::pair(mouse_x_, mouse_y_);
    };
    host.override_key = [this](int key, int status) {
        apply_override(key, status);
    };
    (void)interp;
}

void GameRuntime::RuntimeState::apply_override(int key, int status) {
    if (key < 0) {
        // "all keys" semantic: not supported yet, ignore
        return;
    }
    if (status == 0) {
        overrides_.erase(key);
    } else {
        overrides_[key] = status;
    }
}

void GameRuntime::RuntimeState::tick(uint64_t delta_ms, const FrameInput& in) {
    if (!interpreter_) return;
    ++tick_count_;
    now_ms_ += delta_ms;
    // A captured transition may have reached its duration: auto-clear before
    // any wait resolution consults it (transition::clear_finished every
    // frame).
    transition_.clear_finished(now_ms_);
    // Drop the inline-event marker once its frame left the stack.
    refresh_inline_event_frame();
    // [alldelete]: the fade clock expires -> clear the scene + text.
    if (alldelete_active_ && now_ms_ >= alldelete_start_ms_ + alldelete_duration_ms_)
        finish_all_delete();
    // [mouse] warp emulation ([mouse] config tag; apply_mouse_config). A warp
    // request normally reaches the engine as fresh OS-motion feedback from the
    // host's SDL warp. Hosts that cannot warp the OS cursor (hidden window /
    // headless) never produce feedback: while no fresh raw pointer input
    // arrives, advance the engine pointer to the requested target through the
    // normal pointer chain instead, so hover/rollover state and
    // e:getMousePoint follow the request (FPM dialog mouse_autocursor, the
    // exit-confirm YES-button auto-cursor). The first raw input change hands
    // the pointer back to the host and clears the pending warp.
    FrameInput eff = in;
    const bool raw_static =
        eff.mouse_x == raw_mouse_x_ && eff.mouse_y == raw_mouse_y_;
    raw_mouse_x_ = eff.mouse_x;
    raw_mouse_y_ = eff.mouse_y;
    if (pointer_warp_pending_) {
        if (raw_static && !eff.left_click_edge && !eff.right_click_edge &&
            drag_layer_.empty()) {
            eff.mouse_x = pointer_warp_x_;
            eff.mouse_y = pointer_warp_y_;
        } else {
            pointer_warp_pending_ = false;
        }
    }
    mouse_x_ = eff.mouse_x;
    mouse_y_ = eff.mouse_y;

    // update physical state
    down_edges_.clear();
    up_edges_.clear();
    keys_down_ = in.keys_down;
    for (const int k : in.key_down_edges) down_edges_.insert(k);
    for (const int k : in.key_up_edges) up_edges_.insert(k);
    if (in.left_click_edge) down_edges_.insert(kKeyMouseLeft);
    if (!in.left_down) up_edges_.insert(kKeyMouseLeft);
    if (std::getenv("OA_NM_SELDBG") && in.left_click_edge)
        std::fprintf(stderr, "[seldbg] t=%llu left-edge\n",
                     (unsigned long long)now_ms_);

    // decide edge: override bit 32 freshly set this frame (scripted decide).
    // The snapshot is taken BEFORE the vsync hook so overrides injected by
    // Lua's onEnterFrame (the FPM dummy-click pattern: setexclick ->
    // e:overrideKey status=32) produce their decide edge in the SAME advance
    // (Lua runs inside the host frame before wait resolution).
    prev_overrides_ = overrides_;

    // Lua per-frame hook first (it rewrites keys/decide edges, clears flags)
    if (std::getenv("OA_NO_VSYNC"));
    else interpreter_->fire_event("onEnterFrame");
    // decide 边沿 = 相对上一帧快照新出现的位 32（帧内/测试直接注入均
    // 生效）。配合每帧末清空覆盖（见 tick 尾部注释），FPM exclick 的
    // status=0+32 同帧注入在下一帧构成新边沿；滞留位不会再吞掉后续 decide。
    decide_edge_ = false;
    for (const auto& [key, bits] : overrides_) {
        const int prev = prev_overrides_.count(key) ? prev_overrides_.at(key) : 0;
        if ((bits & 32) && !(prev & 32)) decide_edge_ = true;
    }

    // Control-skip hold (role-14 keys) + effective-skip recompute —
    // the wait machine consults skip_active_.
    update_control_skip_hold(keys_down_);
    skip_active_ = skip_active();

    // Pointer/layer dispatch chain (rollover/rollout hover set, click,
    // drag, global push). Runs before wait resolution; a left-down edge
    // claimed by a layer/drag/global handler swallows the default click so
    // waits/trans skip do not fire.
    const PointerConsumption pc = process_pointer_input(eff);

    // clicked = physical left-down not consumed by layer/drag/push (and not
    // swallowed by a hide-mode click recovery) || legacy Enter/Space role-0
    // approximation || a keyconfig role-0 key edge.
    bool clicked;
    if (in.left_click_edge) {
        const bool push_absorbs =
            pc.left_push_handled &&
            !(wait_ && wait_->kind == oa::runtime::WaitReason::Kind::Timed &&
              wait_->input == 1);
        clicked = !pc.input_swallowed && !pc.layer_handled && !pc.drag_handled &&
                  !push_absorbs;
    } else {
        clicked = down_edges_.count(kKeyEnter) > 0 || down_edges_.count(kKeySpace) > 0;
    }
    clicked = clicked || pc.role_advance;

    // script status 4 = forced stop (no user input): park the story line but
    // keep onEnterFrame ticking (already fired) and scene tweens advancing.
    if (script_status_ != 4) {
        // While the e:debugSkip fast-forward is active the Lua
        // flg.exskip half already suppresses the visuals, so the crossing
        // parks must burn INSIDE one tick (bounded), not one per frame: at
        // 60 fps a multi-chapter jump (thousands of [@]/estag parks) would
        // otherwise hold the black uimask (uimask_on) for a minute-plus —
        // the real-machine "screen goes black after 次の選択肢 confirm"
        // report. The burst ends the moment the fast-forward state clears
        // (the [stop 0="exskip"] boundary fires onDebugSkipOut inside
        // resolve_wait) or a park survives resolution (Stop-class waits are
        // never auto-released).
        const int kMaxExskipBurst = 4096;
        int burst = 0;
        for (;;) {
            const bool was_ff = exskip_active_;
            bool attempted_resolve = false;
            if (wait_) {
                // Queued tags run while parked under every wait kind — the
                // interpreter drains its queue in each wait state every frame,
                // [stop]/[trans] parks included. FPM title
                // interactions (btn_over lyprop/flip, the click estag/jump
                // chain) queue tags exactly while the title [stop] park holds:
                // without this
                // drain a hover never highlights and a click never leaves the
                // title.
                //
                // A CLICK wait armed BY A QUEUED TAG holds the
                // rest of that queue instead of draining it. Lua's
                // e:enqueueTag batch is an ordered continuation — the
                // family's exkeyin (system/adv/keyconfig.lua:595, reached
                // through tags.exkey) enqueues
                //   [chgmsg dummy][@][/chgmsg][calllua exkeyin_exit label=..]
                // where the trailing calllua belongs to the state AFTER the
                // [@] click wait: it is what restores/clears flg.keycode and
                // jumps to the label when the pressed key is a CANCEL key.
                // Draining it one frame after the park ran exkeyin_exit
                // immediately (flg.keycode=nil) and, with the family's
                // setonpush_calllua keying its dummy click off flg.keycode
                // (csv.advkey.list[flg.keycode][1]), every later click inside
                // the extra CG viewer became a no-op — the "click does not
                // switch the 差分" report.
                //
                // Scope is deliberately narrow: only click waits
                // ([@]/[wt]/[wt0] = Generic*) that were armed from the queue
                // hold it. Script-armed parks keep draining (FPM title
                // hover/click under the [stop] park), and
                // so do the queue-armed PACING parks the family relies on to
                // run behind them: [wait time=N]/[wait input=1 time=N]
                // (mouse_autocursor chains) and [stop]/[trans] bursts
                // (holding those stalled the FPM title
                // return).
                const bool queue_click_park =
                    interpreter_->last_wait_from_queue() &&
                    (wait_->kind == oa::runtime::WaitReason::Kind::Generic ||
                     wait_->kind == oa::runtime::WaitReason::Kind::Generic0);
                if (std::getenv("OA_DRAINSKIP") && queue_click_park &&
                    interpreter_->has_queued_tags()) {
                    std::string q;
                    for (const auto& ins : interpreter_->peek_tag_queue(4)) {
                        q += " [" + ins.tag;
                        for (const auto& [k, v] : ins.params)
                            q += " " + k + "=" + v;
                        q += "]";
                    }
                    std::fprintf(stderr,
                                 "[drainskip] hold kind=%d queued=%zu at "
                                 "%s:%zu%s\n",
                                 (int)wait_->kind,
                                 interpreter_->queued_tag_count(),
                                 interpreter_->current_script()
                                     ? interpreter_->current_script()->c_str()
                                     : "?",
                                 interpreter_->current_line(), q.c_str());
                }
                if (interpreter_->has_queued_tags() && !queue_click_park) {
                    drain_parked_queue();
                }
                if (wait_) {
                    attempted_resolve = true;
                    if (resolve_wait(delta_ms, in, clicked)) {
                        advance_wait();
                    }
                } else {
                    // a queued jump/call/return resumed the script (it
                    // clears the wait and runs the interpreter the same frame).
                    run_until_wait();
                }
            } else {
                // execute until the next wait or completion
                run_until_wait();
            }
            // The exskip burst keeps consuming parks while the
            // fast-forward is still active. A pass that resolved a wait (or
            // ran the interpreter and parked at a fresh wait) continues; a
            // park that SURVIVED a resolve attempt is sticky (Stop-class
            // waits are never auto-released) and hands control back to the
            // normal per-frame pacing.
            if (!was_ff || !exskip_active_) break;
            if (++burst >= kMaxExskipBurst) break;
            if (attempted_resolve && wait_) break;
        }
    }

    // 每帧末尾清空按键覆盖（InputSnapshot::clear_edges 的
    // key_overrides.clear，每帧由运行时调用）。FPM 的
    // exclick 走 e:overrideKey{status=0}+{key=124,status=32} 同帧注入；若覆盖
    // 跨帧滞留，decide 位 32 永远置位，边沿检测（与上一帧快照比较）会把之后
    // 所有点击的 decide 边沿吞掉——剧情第二下点击起全部失效（真机“点击不推
    // 进”）。每帧清空后下一次注入重新构成边沿。
    overrides_.clear();
    // Save-domain events ([save]/[load]/[syssave]/[autosave]/[takess]/
    // [savess]/[file]) are applied here, after the interpreter step/wait
    // block and before the scene/text/media frame advance — the
    // dispatch slot (advance_logic: interpreter → drain/dispatch →
    // compositor/text/media).
    dispatch_save_events();
    // Engine control events ([reset]/[gotitle]/[exit]) — after the
    // save domain so a flow's own [syssave] lands before [reset] reboots.
    dispatch_control_events();
    maybe_autosave_for_wait();

    // 逐字显示每帧推进（runtime → renderer.reveal_next）
    // "内容在场面"的每帧推进收敛为唯一逐面
    // 序列 advance_content_planes —— 相位保真(红线 R6):reveal →
    // scene 轨(tween 值写/settle/anime 帧)→ emote(运行时自动时钟)→
    // click-wait 图标 → 媒体帧末(video 状态/EOF→解绑/overlay file)。序列
    // 严格保持 tick 原调用顺序与位置(原五行原位收口),任何重排即行为漂移。
    advance_content_planes(delta_ms);
}

void GameRuntime::RuntimeState::arm_stop_wait(std::string id) {
    oa::runtime::WaitReason w;
    w.kind = oa::runtime::WaitReason::Kind::Stop;
    w.id = std::move(id);
    wait_ = std::move(w);
    wait_remaining_ms_ = 0;
}

void GameRuntime::RuntimeState::run_until_wait() {
    if (!interpreter_) return;
    const oa::runtime::ExecutionResult r = interpreter_->run();
    if (r == oa::runtime::ExecutionResult::Wait) {
        const oa::runtime::Event* ev = interpreter_->last_wait_event();
        // The script parked itself on a real wait: a [lytween sync=1] seen
        // earlier in this run does not arm (the sync wait is only set
        // when no wait reason was parked at dispatch time).
        pending_sync_tween_layer_.reset();
        if (ev && ev->kind == oa::runtime::Event::Kind::Wait_) {
            wait_ = ev->reason;
            wait_remaining_ms_ = 0;
            switch (ev->reason.kind) {
                case oa::runtime::WaitReason::Kind::Timed:
                    wait_remaining_ms_ = ev->reason.milliseconds;
                    break;
                default:
                    break;
            }
        } else if (ev && ev->kind == oa::runtime::Event::Kind::Trans) {
            // [trans] type!=0: script parks until the transition visually
            // finishes (Wait(Trans) -> Stop{reason:"trans"}).
            arm_stop_wait("trans");
        } else if (ev && ev->kind == oa::runtime::Event::Kind::LayerEventCmd &&
                   ev->tag == "video" && ev->id.empty()) {
            // Fullscreen [video]: Wait(Event::VideoPlay{id:None}) ->
            // Stop{reason:"video"}. Layer videos
            // never pause.
            arm_stop_wait("video");
        } else if (ev && ev->kind == oa::runtime::Event::Kind::Custom &&
                   ev->tag == "alldelete") {
            // [alldelete time>0] parks the script until the whole-scene
            // fade-out + clear finished (Stop{id:"alldelete"}).
            arm_stop_wait("alldelete");
        }
    } else {
        // Completed without parking: if the run saw a [lytween sync=1] whose
        // layer tweens are still running, block frame progression until they
        // finish (Event::LayerTween { sync: true } -> Stop{reason:"tween:<id>"}).
        if (pending_sync_tween_layer_) {
            arm_stop_wait("tween:" + *pending_sync_tween_layer_);
            pending_sync_tween_layer_.reset();
        }
    }
}

void GameRuntime::RuntimeState::drain_parked_queue() {
    if (!interpreter_) return;
    in_parked_drain_ = true;
    bool saw_call = false;
    bool saw_jump = false;
    bool paused = false;
    auto settle = [&] {
        saw_call |= interpreter_->queued_saw_call();
        saw_jump |= interpreter_->queued_saw_jump();
        settle_inline_event_frame(paused, interpreter_->has_queued_tags(), saw_call,
                                  saw_jump);
    };
    int guard = 0;
    while (interpreter_->has_queued_tags() && guard++ < 64) {
        const std::string* script_before = interpreter_->current_script();
        const size_t line_before = interpreter_->current_line();
        const size_t stack_before = interpreter_->call_stack().size();
        const oa::runtime::ExecutionResult r = interpreter_->run_queued();
        const bool moved = script_before != interpreter_->current_script() ||
                           line_before != interpreter_->current_line() ||
                           stack_before != interpreter_->call_stack().size();
        if (r == oa::runtime::ExecutionResult::Wait) {
            if (std::getenv("OA_DRAINSKIP"))
                std::fprintf(stderr, "[drainskip] paused remain=%zu at %s:%zu\n",
                             interpreter_->queued_tag_count(),
                             interpreter_->current_script()
                                 ? interpreter_->current_script()->c_str()
                                 : "?",
                             interpreter_->current_line());
            paused = true;
            const oa::runtime::Event* ev = interpreter_->last_wait_event();
            if (ev && ev->kind == oa::runtime::Event::Kind::Wait_) {
                wait_ = ev->reason;
                wait_remaining_ms_ = 0;
                if (ev->reason.kind == oa::runtime::WaitReason::Kind::Timed) {
                    wait_remaining_ms_ = ev->reason.milliseconds;
                }
            } else if (ev && ev->kind == oa::runtime::Event::Kind::Trans) {
                arm_stop_wait("trans");
            } else if (ev && ev->kind == oa::runtime::Event::Kind::LayerEventCmd &&
                       ev->tag == "video" && ev->id.empty()) {
                arm_stop_wait("video");
            } else if (ev && ev->kind == oa::runtime::Event::Kind::Custom &&
                       ev->tag == "alldelete") {
                // queued [alldelete time>0] pauses the parked drain the
                // same way (fade-out runs on the tick clock; the parked wait
                // above is replaced by the alldelete stop).
                arm_stop_wait("alldelete");
            }
            settle();
            in_parked_drain_ = false;
            return; // a queued wait now governs; the caller resolves it
        }
        if (moved) {
            // queued jump/call/return changed position: clear the parked wait.
            wait_.reset();
            settle();
            in_parked_drain_ = false;
            return;
        }
        // Continue-type tags drained without a pause or position change
        // (lyprop/lyc/... apply and keep draining).
        saw_call |= interpreter_->queued_saw_call();
        saw_jump |= interpreter_->queued_saw_jump();
    }
    settle();
    in_parked_drain_ = false;
}

void GameRuntime::RuntimeState::dispatch_control_events() {
    if (pending_events_.empty()) return;
    std::deque<oa::runtime::Event> keep;
    for (const auto& e : pending_events_) {
        if (e.kind == oa::runtime::Event::Kind::Exit) {
            // [exit] (FPM system/ui.asb *go_exit — the "结束游戏" flow):
            // raise the host quit request. It is raised both in the
            // interpreter callback and at dispatch; openartemis consumes it
            // here so hosts
            // only poll exit_requested.
            std::fprintf(stderr, "[runtime] exit requested\n");
            exit_requested_ = true;
            exskip_active_ = false; // quit paths never leave it set
            continue;
        }
        if (e.kind == oa::runtime::Event::Kind::Reset) {
            // [reset] (FPM system/ui.asb *go_title — the "返回标题" flow runs
            // [stop 0=exskip][alldelete][delImageStack][saving][syssave]
            // [reset]). User ruling (2026-09, 方案一, VM 保留): the engine
            // clears every domain and RE-RUNS THE BOOT SCRIPT HEAD with the
            // interpreter (Lua VM) KEPT. The
            // engine hardcodes NO label: the FPM boot chain (first.iet *top
            // -> init.lua system_starting decision) decides where to go — the
            // `systemreset` Lua flag (set by the go_title chain) survives in
            // the kept VM and the decision's logo-skip branch routes to
            // first.iet *title (and consumes the flag). Landing prototypes
            // that jumped straight to the *title label (or re-ran boot with
            // the flag unconsumed) looped: the new-game chain re-entered the
            // boot context with `systemreset` still set and re-emitted
            // [reset] endlessly. Because the interpreter is
            // NOT rebuilt, the host-installed interpreter hooks (render/font
            // metrics) and boot-time font tables survive — post-title-return
            // uihelp centering vars keep measuring normally. [lua] blocks do
            // not re-run (the
            // script and its executed-block cache are kept) and boot()
            // re-positions the stream at the script head, re-invoking the
            // already-defined Lua init functions.
            std::fprintf(stderr, "[runtime] engine reset (VM kept; boot re-run)\n");
            reset_domains();
            pending_events_.clear();
            try {
                interpreter_->boot(rt_->project_.config.boot_script);
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "[runtime] reset boot re-run failed (%s); "
                                     "full restart fallback\n",
                             ex.what());
                restart_project();
                return;
            }
            return;
        }
        if (e.kind == oa::runtime::Event::Kind::GoTitle) {
            // Raw [gotitle] instruction (builtin fallback). FPM facts: the
            // game's own [gotitle] tags (script.asb *fileend error path) are
            // consumed by the Lua tag filter (tags.gotitle, system.lua:225 —
            // sets systemreset=true and calls ui.asb *go_title -> [reset]),
            // so this event only fires for data WITHOUT that Lua handler.
            // Per the reset ruling the engine assumes NO label: clear the
            // domains and re-run the boot head with the VM kept — the data's
            // own boot chain decides where to go (same shape as [reset]).
            std::fprintf(stderr, "[runtime] gotitle (VM kept; boot re-run)\n");
            reset_domains();
            pending_events_.clear();
            try {
                interpreter_->boot(rt_->project_.config.boot_script);
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "[runtime] gotitle boot re-run failed (%s); "
                                     "full restart fallback\n",
                             ex.what());
                restart_project();
                return;
            }
            return;
        }
        keep.push_back(e);
    }
    pending_events_.swap(keep);
}

// ---------------------------------------------------------------------------
// Message-layer text domain:文本标签在派发点被归约进
// oa::render::TextEngine（print/chgmsg/rt/rp/font/ruby/scein-sceout/排版配置）。
// 已消费的事件不再进宿主事件流。chgmsg/pop 同步合成器消息层绑定（~ 路由）。
// ---------------------------------------------------------------------------

void GameRuntime::RuntimeState::ensure_active_message_node() {
    if (!interpreter_) return;
    const std::string active = text_.active_layer_id();
    if (active.empty()) return;
    const oa::render::MessageLayer* al = text_.layer(active);
    if (!al) return;
    const bool is_layered = al->layered;
    // 槽位显式化 —— layered=1 走宿主树节点;layered=0 走引擎消息槽
    // (创建序,id 不随消息 id 变)。create/ensure 语义收在 Compositor 内。
    const std::string scene_id = scene_.ensure_message_scene_node(active, is_layered);
    scene_.set_message_layer_binding(active, scene_id);
    scene_.revive_message_layer(active);
}

bool GameRuntime::RuntimeState::apply_text_event(const oa::runtime::Event& e) {
    using K = oa::runtime::Event::Kind;
    auto par = [&e](const char* k) -> std::string {
        const auto it = e.params.find(k);
        return it == e.params.end() ? std::string() : it->second;
    };
    switch (e.kind) {
        case K::ScenarioLine: {
            ++text_events_;
            text_.push_text(e.content);
            // every drawable message layer owns a scene node — content
            // pushed before any chgmsg (default/active layer) materializes it
            // lazily, so glyphs have exactly one paint point (their node's
            // slot) and no scene-wide text fallback pass is needed.
            ensure_active_message_node();
            return true;
        }
        case K::LineBreak: {
            // [rt]：omitblankline=1（缺省 true）时末行为空行则不另起
            const std::string ob = par("omitblankline");
            const bool omit = ob.empty() || parse_i32(ob, 1) != 0;
            text_.line_break(omit);
            return true;
        }
        case K::PageBreak: {
            std::optional<int> bl;
            const std::string bp = par("backlog");
            if (!bp.empty()) bl = parse_i32(bp, 0);
            text_.page_break(bl);
            return true;
        }
        case K::MessageLayerSwitch: {
            ++message_switch_events_;
            // 参数 id/stack/layered（id 缺省 → 引擎生成匿名 id）
            const std::string id = par("id");
            std::optional<std::string> mid;
            if (!id.empty()) mid = id;
            const std::string st = par("stack");
            const bool stack = st.empty() || parse_i32(st, 1) != 0;
            std::optional<int> layered;
            const std::string ly = par("layered");
            if (!ly.empty()) layered = parse_i32(ly, -1);
            text_.switch_layer(mid, stack, layered);
            // 绑定：活动消息层 id → 场景目标 + 默认消息层
            const std::string active = text_.active_layer_id();
            const oa::render::MessageLayer* al = text_.layer(active);
            const bool is_layered = al && al->layered;
            // 槽位显式化 —— layered=0 独立消息分配引擎消息槽(创建序),
            // layered=1 走宿主树节点(ensure_path 物化,父链位姿语义不变:
            // glyph 落点=ml.left/top+排版偏移,经节点世界变换合成)。
            const std::string scene_id = scene_.ensure_message_scene_node(active, is_layered);
            scene_.set_message_layer_binding(active, scene_id);
            scene_.revive_message_layer(active);
            scene_.set_default_message_layer(active);
            return true;
        }
        case K::MessageLayerPop: {
            text_.pop_layer();
            const std::string active = text_.active_layer_id();
            const oa::render::MessageLayer* al = text_.layer(active);
            if (al) {
                const bool is_layered = al->layered;
                const std::string scene_id =
                    scene_.ensure_message_scene_node(active, is_layered);
                scene_.set_message_layer_binding(active, scene_id);
                scene_.revive_message_layer(active);
            }
            scene_.set_default_message_layer(active);
            return true;
        }
        case K::TextConfig: {
            // font 族 / ruby / scein-sceout / 排版配置标签（ +
            // tags/ 文本面）
            if (e.tag == "font") {
                text_.apply_font(e.params);
                return true;
            }
            if (e.tag == "font_close" || e.tag == "/font") {
                text_.font_close();
                return true;
            }
            if (e.tag == "fontinit") {
                text_.font_init();
                return true;
            }
            if (e.tag == "fontdefault") {
                text_.font_default(e.params);
                return true;
            }
            if (e.tag == "ruby") {
                text_.ruby_start(par("text"));
                return true;
            }
            if (e.tag == "/ruby") {
                text_.ruby_end();
                return true;
            }
            if (e.tag == "scein") {
                text_.set_text_hidden(false);
                return true;
            }
            if (e.tag == "sceout") {
                text_.set_text_hidden(true);
                return true;
            }
            if (e.tag == "prohibit") {
                text_.set_prohibit(par("head"), par("foot"));
                return true;
            }
            if (e.tag == "wordparts") {
                text_.set_wordparts(par("parts"));
                return true;
            }
            if (e.tag == "indent") {
                text_.set_indent(par("pair"));
                return true;
            }
            if (e.tag == "scetween") {
                text_.apply_scetween(e.params);
                return true;
            }
            if (e.tag == "glyph") {
                text_.set_glyph_config(e.params);
                return true;
            }
            if (e.tag == "backlog") {
                text_.apply_backlog_config(e.params, par("clear") == "1");
                return true;
            }
            if (e.tag == "writebacklog") {
                const std::string m = par("mode");
                text_.set_backlog_write_mode(!m.empty() && parse_i32(m, 0) != 0);
                return true;
            }
            // rt2/tximg/txkey/txnc/link… 仍 no-op（后续域/队列）
            return false; // 交给宿主（诊断/后续域）
        }
        default:
            return false;
    }
}

bool GameRuntime::RuntimeState::apply_scene_event(const oa::runtime::Event& e) {
    using K = oa::runtime::Event::Kind;
    if (e.kind == K::LayerCreate) {
        scene_.create(e.id, e.params);
        ++scene_layer_events_;
        return true;
    }
    if (e.kind == K::LayerDelete) {
        scene_.remove(e.id);
        ++scene_layer_events_;
        // a deleted layer (or subtree) releases its emote
        // player (fgdel/lydel2 flows) — players whose id equals the deleted
        // id or sits below it are dropped (textures die with the scene too).
        for (auto it = emote_layers_.begin(); it != emote_layers_.end();) {
            const std::string& id = it->first;
            if (id == e.id || (id.size() > e.id.size() + 1 &&
                               id.compare(0, e.id.size(), e.id) == 0 &&
                               id[e.id.size()] == '.')) {
                it = emote_layers_.erase(it);
            } else {
                ++it;
            }
        }
        // The deleted subtree releases its layer-video
        // channels the same way. A layer video whose scene carrier is gone
        // has nothing to draw; letting the channel run would keep decoding
        // and mask-compositing frames forever (snll's title petal channel
        // kept pumping into the story after the title ui teardown, and the
        // recall-montage strip kept looping after its cgdel — both cost a
        // full-screen decode + mask bake per delivered frame).
        video_.stop_layer_subtree(e.id);
        return true;
    }
    if (e.kind == K::LayerSetProps) {
        // routed: id == "!" hits the root props, never creates a "!" node.
        if (e.id == "!") {
            scene_.set_root_props(e.params);
        } else {
            scene_.set_props(e.id, e.params);
        }
        ++scene_layer_events_;
        return true;
    }
    if (e.kind == K::LayerEventCmd) {
        // Animation tags live on the Compositor and are
        // part of the compositor boundary (compositor/TweenSet*).
        if (e.tag == "lyevent") {
            // layer-event handler registrations are applied to the scene
            // registry at dispatch time (FPM type-less handler
            // expansion). Host stream never sees them.
            scene_.apply_lyevent(e.id, e.params);
            ++scene_layer_events_;
            return true;
        }
        if (e.tag == "lytween") {
            if (std::getenv("OA_DEBUG_LYTW")) {
                std::string p;
                for (const auto& [k, v] : e.params)
                    if (k == "id" || k == "param" || k == "from" ||
                        k == "to" || k == "time" || k == "ease" ||
                        k == "delay" || k == "sync" || k == "delete" ||
                        k == "handler" || k == "yoyo" || k == "loop")
                        p += k + "=" + v + " ";
                std::printf("[ltw] + id=%s %s\n", e.id.c_str(), p.c_str());
            }
            scene_.apply_lytween(e.params, now_ms_);
            ++scene_tween_events_;
            // [lytween sync=1]: remember the target so the runtime can park the
            // script once this run finishes without its own wait
            // (Event::LayerTween { sync: true } -> Stop{reason:"tween:<id>"}).
            // Only the first request of a run
            // arms; sync requests drained while parked never arm (the parked
            // wait already exists).
            if (!in_parked_drain_) {
                int sync = 0;
                const auto s = e.params.find("sync");
                if (s != e.params.end()) {
                    try {
                        sync = std::stoi(s->second);
                    } catch (...) {
                        sync = 0;
                    }
                }
                if (sync != 0 && !pending_sync_tween_layer_) {
                    const auto id = e.params.find("id");
                    if (id != e.params.end() && !id->second.empty()) {
                        pending_sync_tween_layer_ = id->second;
                    }
                }
            }
            return true;
        }
        if (e.tag == "lytweendel") {
            if (std::getenv("OA_DEBUG_LYTW"))
                std::printf("[ltw] - del id=%s\n", e.id.c_str());
            // cancel deliveries deferred to the frame-end scene
            // advance (flush_pending_tween_cancels) — a cancelled
            // script-timer round fires its completion only while its layer
            // survived the rest of the frame's script work. Config page
            // switches call config_delsample() (lytweendel) before the
            // csvbtn3 [lydel2]+rebuild of the SAME script batch; delivering
            // here would reprint the sample preview onto the next page
            // (snll/NekoMiko sample bleed across every config page).
            std::vector<oa::render::TweenDone> done = scene_.apply_lytweendel(e.id);
            if (!done.empty()) {
                PendingTweenCancel pc;
                pc.id = e.id;
                pc.node = scene_.find(e.id);
                pc.done = std::move(done);
                pending_tween_cancels_.push_back(std::move(pc));
            }
            ++scene_tween_events_;
            return true;
        }
        if (e.tag == "anime") {
            // [anime] frame animation: init/add/end reduced to the
            // AnimeState on the Compositor.
            scene_.apply_anime(e.params, now_ms_);
            ++scene_tween_events_;
            return true;
        }
        if (e.tag == "tweenset") {
            scene_.tweenset_start();
            ++scene_tween_events_;
            return true;
        }
        if (e.tag == "/tweenset") {
            scene_.tweenset_end(now_ms_);
            ++scene_tween_events_;
            return true;
        }
        return false;
    }
    if (e.kind == K::ConfigEvent) {
        // global input registry ([setonpush] family)
        // and the legacy layer-event aliases
        // ([setonclick] etc = lyevent mode init/reset).
        static const std::map<std::string, std::string> kLayerSet = {
            {"setonclick", "click"},       {"setondrag", "drag"},
            {"setondragin", "dragin"},     {"setondragout", "dragout"},
            {"setonrollover", "rollover"}, {"setonrollout", "rollout"}};
        static const std::map<std::string, std::string> kLayerDel = {
            {"delonclick", "click"},       {"delondrag", "drag"},
            {"delondragin", "dragin"},     {"delondragout", "dragout"},
            {"delonrollover", "rollover"}, {"delonrollout", "rollout"}};
        if (const auto it = kLayerSet.find(e.tag); it != kLayerSet.end()) {
            scene_.apply_legacy_layer_event(e.id, it->second, false, e.params);
            ++scene_layer_events_;
            return true;
        }
        if (const auto it = kLayerDel.find(e.tag); it != kLayerDel.end()) {
            scene_.apply_legacy_layer_event(e.id, it->second, true, e.params);
            ++scene_layer_events_;
            return true;
        }
        static const std::map<std::string, std::string> kInputSet = {
            {"setonpush", "push"},         {"setonautomodein", "automodein"},
            {"setonautomodeout", "automodeout"}, {"setonbacklogin", "backlogin"},
            {"setonbacklogout", "backlogout"},   {"setoncommandskipin", "commandskipin"},
            {"setoncommandskipout", "commandskipout"},
            {"setoncontrolskipin", "controlskipin"},
            {"setoncontrolskipout", "controlskipout"}, {"setondirchg", "dirchg"},
            {"setonhidein", "hidein"},     {"setonhideout", "hideout"},
            {"setonwindowbutton", "windowbutton"}};
        static const std::map<std::string, std::string> kInputDel = {
            {"delonpush", "push"},         {"delonautomodein", "automodein"},
            {"delonautomodeout", "automodeout"}, {"delonbacklogin", "backlogin"},
            {"delonbacklogout", "backlogout"},   {"deloncommandskipin", "commandskipin"},
            {"deloncommandskipout", "commandskipout"},
            {"deloncontrolskipin", "controlskipin"},
            {"deloncontrolskipout", "controlskipout"}, {"delondirchg", "dirchg"},
            {"delonhidein", "hidein"},     {"delonhideout", "hideout"},
            {"delonwindowbutton", "windowbutton"}};
        if (const auto it = kInputSet.find(e.tag); it != kInputSet.end()) {
            std::map<std::string, std::string> raw = e.params;
            // setonwindowbutton indexes by "button" unless key was given.
            if (e.tag == "setonwindowbutton" && !raw.count("key")) {
                if (const auto b = raw.find("button"); b != raw.end()) raw["key"] = b->second;
            }
            scene_.set_input_handler(it->second, raw);
            return true;
        }
        if (const auto it = kInputDel.find(e.tag); it != kInputDel.end()) {
            std::string key;
            const auto k = e.params.find("key");
            if (k != e.params.end()) key = k->second;
            if (e.tag == "delonwindowbutton" && key.empty()) {
                const auto b = e.params.find("button");
                if (b != e.params.end()) key = b->second;
            }
            scene_.del_input_handler(it->second, key);
            return true;
        }
        // control domain: [keyconfig]/[rclick]/[hide]/[skip]/[automode]/
        // [exec] consumed by the runtime control state machine
        // (apply_control_config, below).
        if (apply_control_config(e)) return true;
        return false;
    }
    if (e.kind == K::Custom && e.tag == "lydrag") {
        // [lydrag]: force drag regardless of draggable/hover.
        // The scene event consumed; host never sees it.
        const auto it = e.params.find("id");
        force_pointer_drag(it == e.params.end() ? "" : it->second);
        return true;
    }
    return false;
}

void GameRuntime::RuntimeState::dispatch_tween_done(
    const std::vector<oa::render::TweenDone>& done) {
    // handler completions: [lytween handler="calllua" function=...] are
    // enqueued as engine tags:
    // the handler tag runs with its extra params (calllua resolves the Lua
    // function), and a file/label pair is enqueued as jump (call=false).
    // The tags execute on the next script run / parked drain. The same
    // path delivers completions of handler-tweens cancelled by [lytweendel]
    // (see Compositor::apply_lytweendel) so script-timer rounds end cleanly.
    for (const oa::render::TweenDone& d : done) {
        if (!d.handler.empty() || !d.handler_file.empty() || !d.handler_label.empty()) {
            ++tween_completion_calls_;
        }
        if (!d.handler.empty()) {
            interpreter_->enqueue_tag(d.handler, d.extra);
        }
        if (!d.handler_file.empty() || !d.handler_label.empty()) {
            std::map<std::string, std::string> jump;
            if (!d.handler_file.empty()) jump["file"] = d.handler_file;
            if (!d.handler_label.empty()) jump["label"] = d.handler_label;
            interpreter_->enqueue_tag("jump", std::move(jump));
        }
    }
}

void GameRuntime::RuntimeState::flush_pending_tween_cancels() {
    // deferred [lytweendel] handler completions (see the
    // lytweendel dispatch and PendingTweenCancel). Deliver only while the
    // tween's layer survived this frame's script work — the config
    // page-switch rebuild ([lydel2 500] inside csvbtn3) removes the sample
    // timer node (500.sample / 500.z.zz) right after config_delsample()
    // cancelled its round, so the round must end WITHOUT reprinting the
    // sample text onto the next page. Same-frame same-page cancels (FPM
    // r10e slider drag) keep the layer and deliver exactly as before.
    if (pending_tween_cancels_.empty()) return;
    std::vector<oa::render::TweenDone> out;
    for (auto& pc : pending_tween_cancels_) {
        const oa::render::Layer* now = scene_.find(pc.id);
        if (now && now == pc.node) {
            for (auto& d : pc.done) out.push_back(std::move(d));
        }
        // else: layer deleted/rebuilt before the delivery point — the round
        // simply ends silently (dropped).
    }
    pending_tween_cancels_.clear();
    dispatch_tween_done(out);
}

void GameRuntime::RuntimeState::drop_pending_tween_cancels() {
    pending_tween_cancels_.clear();
}

// "内容在场面"的每帧推进唯一逐面
// 更新入口。每面 = 现状时钟的既有实现方法(原位收口,方法体零改动);顺序与
// tick 相位文档(红线 R6)逐行对应,见各步注释。
// 不在此面的每帧更新:宿主上传泵(emote 画布/video 帧,main.cpp
// tick 后渲染前)、组烘焙(绘制时按需)、视频解码线程自驱时钟 —— 见文档。
void GameRuntime::RuntimeState::advance_content_planes(uint64_t delta_ms) {
    // 面 1 — 文本 reveal 时钟(text_.reveal_next;逐字 reveal_index)。
    text_.reveal_next(delta_ms);

    // 面 2 — 场景属性动画轨(advance_scene_frame = flush 上帧 tween 完成投递
    // → Compositor::advance_tweens:tween 值写/settle/delete_on_finish 级联 +
    // [anime] 帧推进直写;帧内已含活动帧写收口,见 layer.cpp)。
    advance_scene_frame();

    // 面 3 — emote 姿态时钟的运行时自动臂(advance_emote_players:每 tick
    // advance_ms;host_clock 层跳过自动推进只同步 revision —— 双时钟另一臂
    // em:progress 由游戏驱动,事件时点,不在本序列)。
    advance_emote_players(delta_ms);

    // 面 4 — [glyph] click-wait 图标(reveal 完成 → 显示边沿落在同帧)。
    advance_click_wait();

    // 面 5 — 媒体帧末(audio 淡出时钟/decode 泵/video 状态推进 → EOF
    // finish_video 解绑 → overlay set/clear file;帧末收尾,等待解析对上帧
    // 状态)。
    advance_media_frame(delta_ms);
}

void GameRuntime::RuntimeState::advance_scene_frame() {
    // handler-tween completions are delivered only while
    // the tween's layer survived the frames in between. flush first (the
    // previous frame's staged cancels/naturals), then stage this frame's
    // natural finishes for the next flush.
    flush_pending_tween_cancels();
    std::vector<oa::render::TweenDone> done = scene_.advance_tweens(now_ms_);
    if (!done.empty()) {
        // natural finishes of handler tweens are staged exactly
        // like [lytweendel] cancels instead of being dispatched immediately.
        // A round that ended naturally must not fire its completion
        // (config_sampletext -> reprint + rearm of the sample preview) into
        // a screen where the timer layer is already gone — the config page
        // switch rebuild deletes 500.sample / 500.z.zz inside csvbtn3, and a
        // completion drained after that would repaint the preview text onto
        // the new page and re-arm a fresh round there (snll config "切页时
        // 预览文字显示"). Same rule, same guarantee as the cancel path:
        // alive layer (normal page-2 rounds, FPM r10e drag) delivers one
        // frame later; dead/rebuilt layer ends the round silently.
        for (oa::render::TweenDone& d : done) {
            PendingTweenCancel pc;
            pc.id = d.id;
            pc.node = scene_.find(d.id);
            pc.done.push_back(std::move(d));
            pending_tween_cancels_.push_back(std::move(pc));
        }
    }
    // [lytween sync=1] wait release: the script stays parked until the target
    // layer's tweens are gone (or the layer is deleted); release clears the
    // wait without advancing the interpreter line — the script never parked on
    // the lytween instruction.
    if (wait_ && wait_->kind == oa::runtime::WaitReason::Kind::Stop &&
        wait_->id.rfind("tween:", 0) == 0) {
        const std::string layer = wait_->id.substr(6);
        if (scene_.layer_tweens_finished(layer)) {
            wait_.reset();
            wait_remaining_ms_ = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// transitions
// ---------------------------------------------------------------------------

void GameRuntime::RuntimeState::transition_begin(const std::map<std::string, std::string>& params) {
    auto get = [&params](const char* k, const char* dflt) -> std::string {
        const auto it = params.find(k);
        return it == params.end() ? std::string(dflt) : it->second;
    };
    int type = 1;
    std::optional<uint64_t> time;
    std::string rule = get("rule", "");
    std::optional<int> vague;
    int input = 1;
    try {
        type = std::stoi(get("type", "1"));
    } catch (...) {
        type = 1;
    }
    try {
        if (!get("time", "").empty()) time = uint64_t(std::stoll(get("time", "0")));
    } catch (...) {
        time.reset();
    }
    try {
        if (!get("vague", "").empty()) vague = std::stoi(get("vague", "32"));
    } catch (...) {
        vague.reset();
    }
    try {
        input = std::stoi(get("input", "1"));
    } catch (...) {
        input = 1;
    }
    transition_.start(type, time, std::move(rule), vague, input, now_ms_);
    if (transition_.needs_capture()) {
        if (transition_capture_cb_) {
            transition_capture_cb_(); // host snapshots the old frame
        } else {
            // headless host: treat the capture as completed immediately so the
            // transition still runs its full duration on the clock.
            transition_.mark_captured(now_ms_);
        }
    }
}

bool GameRuntime::RuntimeState::waiting_stop() const {
    return wait_ && wait_->kind == oa::runtime::WaitReason::Kind::Stop;
}

// ---------------------------------------------------------------------------
// FPM e:debugSkip exskip fast-forward (次の選択肢に進む / 高速
// スキップ). The Lua system half runs under flg.exskip (mainloop whitelist,
// estag{"exskip_stop"} boundaries, onDebugSkipOut=exskip_end); the engine
// half is only:
//   - exskip_active_: every non-Stop wait releases instantly while active
//     (the story fast-forwards page by page with zero input);
//   - [stop 0="exskip"] while active: the fast-forward ends HERE — the state
//     is cleared, Lua flg.exskip is marked 2 (the guard exskip_end requires;
//     the real engine's Lua tags.stop hook does this at the row, openartemis
//     executes these rows natively) and the registered onDebugSkipOut handler
//     (FPM init.lua: exskip_end) runs — it cleans up and quickjumps back to
//     the last backlog point, whose queued jump repositions the interpreter
//     (the boundary row itself then passes; without active exskip the row is
//     a pass-through, exactly the pre-existing rule).
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::start_debug_skip() {
    if (exskip_active_) return;
    exskip_active_ = true;
    if (std::getenv("OA_EXSKIPDBG"))
        std::fprintf(stderr, "[exskipdbg] fast-forward start\n");
}

void GameRuntime::RuntimeState::end_debug_skip() {
    if (!exskip_active_) return;
    exskip_active_ = false;
    reveal_all_text(); // no half-revealed page left behind at the boundary
    if (std::getenv("OA_EXSKIPDBG"))
        std::fprintf(stderr, "[exskipdbg] boundary [stop 0=exskip] -> onDebugSkipOut\n");
    if (interpreter_) {
        try {
            // Mirror the real engine's Lua tags.stop hook side effect (sets
            // flg.exskip=2 so exskip_end's guard passes); then fire the
            // registered handler (onDebugSkipOut -> exskip_end).
            interpreter_->lua_bridge().run_code(
                "if type(flg)=='table' and flg.exskip then flg.exskip = 2 end",
                "exskip-boundary-mark");
            interpreter_->fire_event("onDebugSkipOut");
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[exskipdbg] boundary handler error: %s\n", ex.what());
        }
    }
}

bool GameRuntime::RuntimeState::resolve_wait(uint64_t delta_ms, const FrameInput& in, bool clicked) {
    if (!wait_) return true;
    (void)in;
    // FPM e:debugSkip fast-forward — every non-Stop wait
    // releases instantly while the engine exskip state is active (the Lua
    // flg.exskip half already suppresses text/sound/emote rows, so waits are
    // the only thing left to release). Stop-class parks are reserved for the
    // boundary rule below. Independent of skip_allowed_/skip_enabled_: this
    // is not the [skip allow]+[exec skip] state machine and fires no
    // skipin/skipout events (FPM debugSkip has none).
    if (exskip_active_ && wait_->kind != oa::runtime::WaitReason::Kind::Stop) {
        return true;
    }
    switch (wait_->kind) {
        case oa::runtime::WaitReason::Kind::Timed: {
            const bool clickable = wait_->input == 1;
            const bool skip_pass = wait_->input == 2;
            // A scripted decide edge (e:overrideKey status=32, the FPM
            // "dummy click") counts like a physical click for input waits.
            if ((clickable && (clicked || decide_edge_)) || skip_active_ || skip_pass)
                return true;
            if (wait_remaining_ms_ <= delta_ms) return true;
            wait_remaining_ms_ -= delta_ms;
            return false;
        }
        case oa::runtime::WaitReason::Kind::Stop: {
            // [stop]: physical clicks do NOT release; only scripted decide
            // edges (overrideKey status=32) or media/transition completions.
            // A fullscreen-video completion resumes ANY Stop-class park.
            if (video_finished_) {
                video_finished_ = false;
                return true;
            }
            // [stop 0="exskip"]: FPM debugSkip (次の選択肢/高速スキップ)
            // boundary row (ui.asb *exskip_stop / *go_title, script.asb
            // *movie_play first rows):
            //   - exskip NOT active: pass-through — the pre-existing rule
            //     (title/movie flows never hold the script);
            //   - exskip active: the fast-forward ends here — end_debug_skip()
            //     clears the state, marks Lua flg.exskip=2 and fires
            //     onDebugSkipOut (exskip_end cleans up + quickjump-replays the
            //     last backlog point); then this row passes so the boundary
            //     flow (and the queued replay jump) continue normally.
            if (wait_->id == "exskip") {
                if (exskip_active_) end_debug_skip();
                return true;
            }
            // [alldelete]: the script resumes once the whole-scene
            // fade-out + clear finished (finish_all_delete runs inside tick
            // when the fade clock expires).
            if (wait_->id == "alldelete") return !alldelete_active_;
            // fullscreen [video] auto-park (Stop{reason:"video"})
            // on a SKIPPABLE channel (video skip != 0): a physical left click
            // or the right mouse button ends the movie immediately. The skip
            // queues the same finish EOF would (VideoEngine::queue_finish);
            // advance_media_frame dispatches it at the end of this tick, so
            // the Lua video-finish handlers and the video_finished_ wait
            // release run on the identical path as natural EOF (next tick).
            // Non-skippable videos (skip=0) are untouched; the click is a
            // per-frame edge, so it cannot double-advance the wait that
            // follows the movie.
            if (wait_->id == "video") {
                const auto vsnap = video_.state();
                const auto& fsv = vsnap.overlay_video;
                const bool phys_skip =
                    down_edges_.count(kKeyMouseLeft) ||
                    down_edges_.count(kKeyMouseRight);
                if (fsv && fsv->skippable && fsv->playing &&
                    (phys_skip || clicked)) {
                    video_.queue_finish(std::string());
                    return false;
                }
                // 视频停驻的硬防线:只要 overlay 还在播放(含解码起步/
                // 首帧未到),任何非 finish 路径都不得放行 —— EOF/跳过
                // 的 finish 会先停掉 overlay(playing=false),之后才由
                // video_finished_ 释放。避免 OP 类影片(经 Lua 入队的
                // [video])在 1 帧后被提前释放、剧情在影片播放中继续。
                if (fsv && fsv->playing) return false;
            }
            // Stop{reason:"trans"}: a physical click may skip the transition
            // when [trans input=] allows (0 deny / 1 allow / 2 skip-mode
            // only); the wait itself releases once the transition reports
            // finished.
            if (wait_->id == "trans") {
                if (clicked || skip_active_) transition_.skip_by_input(skip_active_);
                if (decide_edge_) return true;
                return !transition_.is_in_progress(now_ms_);
            }
            if (decide_edge_) return true;
            return false;
        }
        case oa::runtime::WaitReason::Kind::Generic:
        case oa::runtime::WaitReason::Kind::Generic0: {
            // decide edges advance input waits too.
            const bool input_advance = clicked || decide_edge_;
            // 点击等待且文本仍在逐字揭示 → 先整页揭示，不推进
            // （reveal_text_now 并 return false）
            if (input_advance && !text_.is_reveal_complete()) {
                if (std::getenv("OA_WAITDBG")) {
                    std::fprintf(stderr, "[waitdbg] generic reveal-blocked at %s:%zu\n",
                                 interpreter_->current_script()
                                     ? interpreter_->current_script()->c_str()
                                     : "?",
                                 interpreter_->current_line());
                    for (const auto& [lid, l] : text_.layers()) {
                        std::fprintf(stderr,
                                     "[waitdbg]   layer '%s' chars=%zu idx=%zu pend=%d "
                                     "hidden=%d scetween=%zu\n",
                                     lid.c_str(), l.char_count, l.reveal_index,
                                     (int)l.reveal_pending, (int)l.text_hidden,
                                     l.scetween.size());
                    }
                }
                text_.reveal_all();
                return false;
            }
            if (input_advance) return true;
            // skip (command/control) releases generic waits after a full reveal.
            if (skip_active_) {
                if (!text_.is_reveal_complete()) {
                    text_.reveal_all();
                    return false;
                }
                return true;
            }
            if (automode_) {
                // pacing interval comes from the script var
                // s.automodewait (FPM clickAutomode writes it at every click
                // park: automode_vowait for a voiced line under conf.autostop,
                // getASpeed() otherwise); 900 ms fallback when unset. The
                // voice gate holds the release while any [automode syncse]
                // channel still plays, so a voiced page turns exactly when
                // its voice ends (never mid-voice), and a voice shorter than
                // the interval still gets its minimum display time.
                auto_elapsed_ms_ += delta_ms;
                if (auto_elapsed_ms_ < automode_wait_ms()) return false;
                if (automode_sync_blocked()) return false;
                auto_elapsed_ms_ = 0;
                return true;
            } else {
                auto_elapsed_ms_ = 0;
            }
            return false;
        }
        case oa::runtime::WaitReason::Kind::KeyWait: {
            for (const auto& btn : wait_->buttons) {
                int code = 0;
                try {
                    code = std::stoi(btn);
                } catch (...) {
                    continue;
                }
                if (down_edges_.count(code)) return true;
            }
            if (clicked || decide_edge_) return true;
            return false;
        }
        case oa::runtime::WaitReason::Kind::Se: {
            // [wait se=ID (time=N)]: releases on skip or when the tracked
            // sound ended.
            if (skip_active_) return true;
            return media_se_wait_finished(wait_->id, wait_->time_given, wait_->milliseconds);
        }
        case oa::runtime::WaitReason::Kind::VideoLayer: {
            // [wait video=layerID]: releases on skip or when the layer video
            // stopped.
            if (skip_active_) return true;
            // explicit video waits on a skippable channel also
            // honor physical input (left click / right button) — queue the
            // layer finish, released through the same stop path next tick.
            const bool phys_skip =
                down_edges_.count(kKeyMouseLeft) ||
                down_edges_.count(kKeyMouseRight);
            if (clicked || phys_skip) {
                const auto vsnap = video_.state();
                const auto vit = vsnap.video_layers.find(wait_->id);
                if (vit != vsnap.video_layers.end() && vit->second.skippable &&
                    vit->second.playing) {
                    video_.queue_finish(wait_->id);
                    return false;
                }
            }
            return !video_.is_layer_playing(wait_->id);
        }
        case oa::runtime::WaitReason::Kind::ScenarioTween: {
            // [wait scenario=1|2]: mode 2 (hide) is instantaneous; mode 1
            // (show) waits for the per-glyph reveal.
            if (skip_active_) return true;
            if (wait_->mode != 1) return true;
            return text_.is_reveal_complete();
        }
    }
    return false;
}

void GameRuntime::RuntimeState::advance_wait() {
    // 读档/UI 会话残链可能把活动消息层留在 UI 层（500.pageno 等），而剧本
    // 翻页分派（script.asb 的 chgmsg/pop/rp 清页模式）假定点击停驻释放时
    // 活动层 == 当前剧情文本层——错层导致清页清错层、新文本叠旧页后且
    // reveal 冻结（NekoMiko 读档后"剧情推进但文字不动"）。
    // 在页面等待释放（一次翻页/一次 UI 交互的起点）时把活动层锚回内容层。
    const bool page_wait_release =
        wait_ && (wait_->kind == oa::runtime::WaitReason::Kind::Generic ||
                  wait_->kind == oa::runtime::WaitReason::Kind::Generic0);
    if (std::getenv("OA_WAITDBG")) {
        const oa::runtime::WaitReason::Kind k = wait_ ? wait_->kind
                                                      : (oa::runtime::WaitReason::Kind)255;
        std::fprintf(stderr,
                     "[waitdbg] resolve kind=%d -> advance pos=%s:%zu queue=%d\n",
                     (int)k, interpreter_->current_script()
                                 ? interpreter_->current_script()->c_str()
                                 : "?",
                     interpreter_->current_line(),
                     (int)interpreter_->has_queued_tags());
    }
    wait_.reset();
    wait_remaining_ms_ = 0;
    if (page_wait_release) text_.anchor_active_to_content();
    // If a Stop wait resolves while Lua queued a jump, the position change
    // happens inside the next run; next_line only moves past inline wait
    // instructions (queue-sourced waits no-op inside the interpreter).
    interpreter_->next_line();
}

// [glyph] click-wait icon per-frame driving.
// advance_click_wait/页末等待（[wt]/[wt0] 都是 Generic/
// Generic0），一律按行末处理（page_end=false，用 [glyph] 的 layer）。位置由
// TextEngine::click_wait_placement 计算（几何近似）；homing
// 时把 left/top 写回图标层，否则只切可见性。未配置图标图层 → placement 为
// nullopt → 不动作；advance_click_wait 为空时退出路径也不写场景。
void GameRuntime::RuntimeState::advance_click_wait() {
    const bool click_wait =
        wait_ && (wait_->kind == oa::runtime::WaitReason::Kind::Generic ||
                  wait_->kind == oa::runtime::WaitReason::Kind::Generic0);
    const bool show = click_wait && text_.is_reveal_complete();
    if (!show) {
        if (!active_wait_icon_.empty()) {
            // set_props 的语义是"缺失即物化"
            // (layer.cpp: `lyprop on a missing id materializes the node`)。
            // 读档清场已经清了 active_wait_icon_，这里再判一次存在性：图标层
            // 被别的路径删掉时，隐藏请求绝不允许凭空造出幽灵层。
            if (scene_.find(active_wait_icon_)) {
                scene_.set_props(active_wait_icon_, {{"visible", "0"}});
            }
            active_wait_icon_.clear();
        }
        return;
    }
    const auto placement = text_.click_wait_placement(false);
    if (!placement) return;
    const bool homing = text_.glyph_icon().homing;
    const oa::render::Layer* l = scene_.find(placement->layer_id);
    const bool already = active_wait_icon_ == placement->layer_id && l && l->visible &&
                         (!homing ||
                          (l->left == placement->left && l->top == placement->top));
    if (already) return;
    if (!active_wait_icon_.empty() && active_wait_icon_ != placement->layer_id &&
        scene_.find(active_wait_icon_)) {
        scene_.set_props(active_wait_icon_, {{"visible", "0"}});
    }
    std::map<std::string, std::string> p{{"visible", "1"}};
    if (homing) {
        p["left"] = format_prop_num(placement->left);
        p["top"] = format_prop_num(placement->top);
    }
    scene_.set_props(placement->layer_id, p);
    active_wait_icon_ = placement->layer_id;
}

// ---------------------------------------------------------------------------
// Pointer dispatch chain. Semantics mirror the
// reference engine's process_pointer_handlers with the
// openartemis host model: hit geometry/alpha come from injected providers and
// Lua dispatch is a synchronous call_function (calllua-equivalent), the same
// ABI the host app uses for btn_over/btn_click.
// ---------------------------------------------------------------------------

namespace {
bool is_mouse_button_key(int key) { return key >= 1 && key <= 3; }
bool key_has_drag_handler(const oa::render::Compositor& scene, const std::string& id) {
    for (const char* type : {"drag", "dragin", "dragout"}) {
        const auto* h = scene.find_event_handler(id, type);
        if (h && h->enabled) return true;
    }
    return false;
}
} // namespace

GameRuntime::RuntimeState::PointerConsumption GameRuntime::RuntimeState::process_pointer_input(const FrameInput& in) {
    PointerConsumption pc;
    if (!interpreter_) return pc;
    const double mx = double(in.mouse_x);
    const double my = double(in.mouse_y);
    const bool left_down_edge = in.left_click_edge;
    const bool left_up_edge = left_down_prev_ && !in.left_down;
    left_down_prev_ = in.left_down;
    const bool pointer_moved =
        !mouse_inited_ || in.mouse_x != mouse_prev_x_ || in.mouse_y != mouse_prev_y_;
    mouse_inited_ = true;
    mouse_prev_x_ = in.mouse_x;
    mouse_prev_y_ = in.mouse_y;

    // hide mode: a left click first recovers the message window and is
    // swallowed for the rest of the frame (hide click recovery, no
    // click/advance chain).
    if (hide_active_ && left_down_edge) {
        exit_hide_mode();
        pc.input_swallowed = true;
        return pc;
    }

    // --- hit test (geometry + clickablethreshold alpha) --------------------
    const auto hits = scene_.hit_test_all(mx, my, hit_size_, compositor_userdata_,alpha_sampler_);
    // layers that can receive events at all = hit-test interaction gate
    // (applied here at dispatch)
    std::vector<std::string> interactive;
    for (const std::string& id : hits)
        if (scene_.has_any_enabled_handler(id)) interactive.push_back(id);

    // event_dispatch_layers: top + penetration, reversed.
    auto dispatch_list = [&](const std::vector<std::string>& base,
                             const std::string& event_type) -> std::vector<std::string> {
        std::vector<std::string> out;
        for (size_t i = 0; i < base.size(); ++i) {
            const auto* h = scene_.find_event_handler(base[i], event_type);
            if (h && h->enabled && (i == 0 || h->penetration)) out.push_back(base[i]);
        }
        std::reverse(out.begin(), out.end());
        return out;
    };

    // --- rollover/rollout hover set diff -----------------------------------
    // hover membership is RECT-BASED, not occlusion-based. A
    // hovered layer keeps its over state (no rollout) while the pointer stays
    // inside its rect and the layer stays visible — a higher layer appearing
    // under the pointer (e.g. the floating dock's buttons sliding up into the
    // dockarea strip, ui/config mwdock) must not fire the lower layer's
    // rollout, or the FPM dock-area state machine oscillates open/close (the
    // "unpinned dock retracts on hover" defect). Rollout fires only when the
    // pointer leaves the rect, or the layer hides/vanishes. New overs still
    // dispatch topmost-first (plus penetration); retained members are not
    // re-overed when the occluder leaves.
    //
    // the whole over/out diff runs only on POINTER MOTION
    // (FPM dispatches hover events from pointer motion). On still-pointer
    // frames nothing fires — a layer vanishing under a still pointer (e.g.
    // NekoMiko's select exit deletes the choice rows while the queued exit
    // chain exittrans -> select_clicknext is in flight) must not run game
    // out-handlers: the rollout would run select_out, clearing scr.select.id,
    // and the queued clicknext then indexes v[nil] -> select.lua:484 crash.
    // Vanished members are still dropped from hovered_ so the set stays
    // honest; their events fire later if the pointer moves off them.
    std::set<std::string> prev_hovered = hovered_;
    if (pointer_moved) {
    std::set<std::string> hit_ids(hits.begin(), hits.end());
    const auto hover_list = dispatch_list(interactive, "rollover");
    std::set<std::string> new_hovered(hover_list.begin(), hover_list.end());
    std::set<std::string> next = hovered_;
    // (a) rollouts: hovered layers that left the pointer rect / disappeared
    std::vector<std::string> old_only;
    for (const std::string& id : hovered_) {
        if (new_hovered.count(id)) continue;
        const oa::render::Layer* l = scene_.find(id);
        const bool still_visible =
            l && l->visible && scene_.is_effectively_visible(id);
        if (hit_ids.count(id) && still_visible) continue; // retained
        old_only.push_back(id);
        next.erase(id);
    }
    std::sort(old_only.begin(), old_only.end());
    for (const std::string& old : old_only) {
        const auto* row = scene_.find_event_handler(old, "rollout");
        if (row) dispatch_layer_row(old, *row, "rollout", {},
                                    PointerDispatch::Kind::HoverOut);
    }
    // (b) overs: topmost-first layers not hovered yet
    for (const std::string& id : hover_list) { // bottom -> top
        if (hovered_.count(id)) continue;
        const auto* row = scene_.find_event_handler(id, "rollover");
        if (!row) continue;
        dispatch_layer_row(id, *row, "rollover", {}, PointerDispatch::Kind::HoverIn);
        next.insert(id);
    }
    prev_hovered = hovered_;
    hovered_ = std::move(next);
    // ---- debug probe: hit-order / hover oscillation ----
    if (std::getenv("OA_DEBUG_HOVQ")) {
        bool dock_zone = false;
        for (const std::string& id : hit_ids)
            if (id == "1.80.mw.-1" || id.find("1.80.mw.bt.dc.1.11") == 0) {
                dock_zone = true;
                break;
            }
        if (dock_zone) {
            int si = -1, bi = -1;
            for (size_t i = 0; i < hits.size(); ++i) {
                if (hits[i] == "1.80.mw.-1") si = int(i);
                if (hits[i].find("1.80.mw.bt.dc.1.11") == 0) bi = int(i);
            }
            std::string hl, hh, hovn;
            for (const auto& s : hover_list) { if (!hl.empty()) hl += ","; hl += s; }
            for (const auto& s : prev_hovered) { if (!hh.empty()) hh += ","; hh += s; }
            for (const auto& s : hovered_) { if (!hovn.empty()) hovn += ","; hovn += s; }
            std::printf("[ph] ms=%llu nhits=%zu strip_idx=%d btn_idx=%d front=%s "
                        "hl=[%s] prev=[%s] next=[%s] x=%d y=%d\n",
                        (unsigned long long)now_ms(), hits.size(), si, bi,
                        (hits.empty() ? std::string("(none)") : hits.front()).c_str(),
                        hl.c_str(), hh.c_str(), hovn.c_str(), in.mouse_x, in.mouse_y);
        }
    }
    } else {
        // still pointer: no script events; silently forget vanished layers
        // (their rollout fires on a later move if the script still cares).
        bool changed = false;
        for (auto it = hovered_.begin(); it != hovered_.end();) {
            if (!scene_.find(*it)) {
                it = hovered_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        (void)changed;
    }
    // --- click + drag start  ------------------------------
    if (left_down_edge) {
        const std::string top_hover = interactive.empty() ? std::string() : interactive.front();
        if (!top_hover.empty() && scene_.is_layer_draggable(top_hover) &&
            key_has_drag_handler(scene_, top_hover)) {
            pc.drag_handled = drag_begin(top_hover, false);
        }
        if (!pc.drag_handled) {
            for (const std::string& id : dispatch_list(interactive, "click")) {
                const auto* row = scene_.find_event_handler(id, "click");
                if (!row) continue;
                if (std::getenv("OA_NM_SELDBG")) {
                    const auto it = row->params.find("function");
                    std::fprintf(stderr, "[seldbg] t=%llu layer-click id=%s fn=%s\n",
                                 (unsigned long long)now_ms_, id.c_str(),
                                 it == row->params.end() ? "-"
                                                         : it->second.c_str());
                }
                pc.layer_handled |= dispatch_layer_row(
                    id, *row, "click", {{"click", "1"}}, PointerDispatch::Kind::Click);
            }
        }
    }
    if (in.left_down && pointer_moved && !drag_layer_.empty()) {
        drag_update(mx, my);
        pc.drag_handled = true;
    }
    if (left_up_edge) {
        if (!drag_layer_.empty()) {
            drag_end();
            pc.drag_handled = true;
        }
    }

    // --- global push handlers for every key-down edge  ----
    std::vector<int> key_edges(down_edges_.begin(), down_edges_.end());
    std::sort(key_edges.begin(), key_edges.end());
    for (const int key : key_edges) {
        // right button / ESC first walk the engine rclick chain (hide
        // recovery or the configured rclick script); only when unconsumed does
        // the same key reach the push row.
        if ((key == kKeyMouseRight || key == 27) && trigger_rclick()) continue;
        // keyconfig role edges (advance/hide/backlog/automode/skip).
        // Role-0 edges count as a
        // click substitute (pc.role_advance feeds the clicked formula).
        if (handle_role_key_edge(key)) pc.role_advance = true;
        const std::string key_string = std::to_string(key);
        const auto* row = scene_.get_input_handler("push", key_string);
        if (!row) continue;
        if (std::getenv("OA_NM_SELDBG") && key == kKeyMouseLeft) {
            const auto it = row->params.find("function");
            std::fprintf(stderr, "[seldbg] t=%llu push key1 fn=%s\n",
                         (unsigned long long)now_ms_,
                         it == row->params.end() ? "-" : it->second.c_str());
        }
        const std::string event_type = is_mouse_button_key(key) ? "click" : "key";
        const bool handled = dispatch_input_row(
            "setonpush", *row,
            {{"key", key_string}, {"type", event_type}}, PointerDispatch::Kind::Push);
        if (key == kKeyMouseLeft && handled) pc.left_push_handled = true;
    }
    return pc;
}

void GameRuntime::RuntimeState::notify_pointer(const PointerDispatch& d) {
    ++pointer_dispatch_count_;
    if (pointer_observer_) pointer_observer_(d);
}

bool GameRuntime::RuntimeState::dispatch_layer_row(
    const std::string& id, const oa::render::LayerEventHandler& row,
    const std::string& event_type,
    const std::vector<std::pair<std::string, std::string>>& runtime_params,
    PointerDispatch::Kind kind) {
    (void)event_type;
    if (!row.enabled) return false;
    // e:setEventFilter consult: 1 = script claims it, 2 = pretend failure.
    const auto verdict = interpreter_->lua_bridge().run_event_filter("lyevent", row.filter_params);
    if (verdict == 2) return false;
    if (verdict == 1) return true; // handled by the script, nothing to run
    // calllua-equivalent: fn = params["function"]; missing = silent no-op
    // (the [calllua] branch lives in runtime_iet.cpp).
    std::map<std::string, std::string> params = row.params;
    for (const auto& [k, v] : runtime_params) params[k] = v;
    const auto fn_it = params.find("function");
    bool handled = true;
    bool fn_queued = false; // this row's fn enqueued >=1 engine tag
    if (fn_it != params.end() && !fn_it->second.empty()) {
        PointerDispatch d;
        d.kind = kind;
        d.layer = id;
        d.function = fn_it->second;
        const auto key_it = params.find("key");
        d.key = key_it == params.end() ? std::string() : key_it->second;
        const size_t q0 = interpreter_->queued_tag_count();
        try {
            d.ran_lua = interpreter_->lua_bridge().call_function(fn_it->second, params);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[runtime] handler %s error: %s\n", fn_it->second.c_str(),
                         ex.what());
            d.ran_lua = false;
        }
        fn_queued = interpreter_->queued_tag_count() > q0;
        notify_pointer(d);
    }
    // a row with file/label queues a navigation tag after the handler
    // runs:
    // `call` pushes a return frame so the target's [return] lands back at the
    // origin ("inline" execution round trip); plain `jump` navigates
    // permanently. Runs on the next tag drain.
    if (!row.file.empty() || !row.label.empty()) {
        std::map<std::string, std::string> nav;
        if (!row.file.empty()) nav["file"] = row.file;
        if (!row.label.empty()) nav["label"] = row.label;
        interpreter_->enqueue_tag(row.call ? "call" : "jump", std::move(nav));
        handled = true;
    }
    // a pure Lua handler (no file/label navigation) may queue a helper
    // flow (fn.push/estag) that ends with a trailing [return]; arm the
    // inline-event marker so that return pops the marker instead of a real
    // parked frame. Attribution is strict:
    // arm only when THIS row's fn actually enqueued tags (queued_tag_count
    // delta) — tags queued by earlier rows of the same input frame (hover
    // chains, other push handlers) must not arm a marker that would then be
    // claimed by their navigation (stale-marker pattern).
    if (fn_it != params.end() && !fn_it->second.empty() && row.file.empty() &&
        row.label.empty() && fn_queued) {
        if (std::getenv("OA_SELTRACE")) {
            std::fprintf(stderr,
                         "[seltrace] ROW-FN layer=%s fn=%s kind=%d queued "
                         "->begin_marker\n",
                         id.c_str(), fn_it->second.c_str(), (int)kind);
        }
        begin_inline_event_frame();
    }
    return handled;
}

bool GameRuntime::RuntimeState::dispatch_input_row(
    const std::string& filter_name, const oa::render::InputHandler& row,
    const std::vector<std::pair<std::string, std::string>>& runtime_params,
    PointerDispatch::Kind kind) {
    const auto verdict = interpreter_->lua_bridge().run_event_filter(filter_name, row.filter_params);
    if (verdict == 2) return false;
    if (verdict == 1) return true;
    std::map<std::string, std::string> params = row.params;
    for (const auto& [k, v] : runtime_params) params[k] = v;
    const auto fn_it = params.find("function");
    bool handled = true;
    bool fn_queued = false; // this row's fn enqueued >=1 engine tag
    if (fn_it != params.end() && !fn_it->second.empty()) {
        PointerDispatch d;
        d.kind = kind;
        d.layer.clear();
        d.function = fn_it->second;
        const auto key_it = params.find("key");
        d.key = key_it == params.end() ? std::string() : key_it->second;
        const size_t q0 = interpreter_->queued_tag_count();
        try {
            d.ran_lua = interpreter_->lua_bridge().call_function(fn_it->second, params);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[runtime] handler %s error: %s\n", fn_it->second.c_str(),
                         ex.what());
            d.ran_lua = false;
        }
        fn_queued = interpreter_->queued_tag_count() > q0;
        notify_pointer(d);
    }
    // file/label rows: enqueue jump/call — same path as layer rows.
    if (!row.file.empty() || !row.label.empty()) {
        std::map<std::string, std::string> nav;
        if (!row.file.empty()) nav["file"] = row.file;
        if (!row.label.empty()) nav["label"] = row.label;
        interpreter_->enqueue_tag(row.call ? "call" : "jump", std::move(nav));
        handled = true;
    }
    if (fn_it != params.end() && !fn_it->second.empty() && row.file.empty() &&
        row.label.empty() && fn_queued) {
        if (std::getenv("OA_SELTRACE")) {
            std::fprintf(stderr,
                         "[seltrace] INPUT-FN filter=%s fn=%s key=%s kind=%d "
                         "queued ->begin_marker\n",
                         filter_name.c_str(), fn_it->second.c_str(),
                         params.count("key") ? params["key"].c_str() : "-",
                         (int)kind);
        }
        begin_inline_event_frame();
    }
    return handled;
}

// ---------------------------------------------------------------------------
// inline-event return frames (begin /
// helpers / settle): see runtime.h decl.
// ---------------------------------------------------------------------------

bool GameRuntime::RuntimeState::inline_event_marker_active() const {
    if (!inline_event_frame_ || !interpreter_) return false;
    const auto& cs = interpreter_->call_stack();
    const size_t idx = inline_event_frame_->stack.size();
    if (idx >= cs.size()) return false;
    return cs[idx].script == inline_event_frame_->script &&
           cs[idx].return_line == inline_event_frame_->line;
}

void GameRuntime::RuntimeState::refresh_inline_event_frame() {
    if (!inline_event_frame_) return;
    if (!inline_event_marker_active()) {
        // OA_SELTRACE: the synthetic marker frame left
        // the interpreter stack — something popped it (a UI popfunc [return]
        // rewound the stream to the recorded park). Print the drop with the
        // remaining stack so the consuming return is attributable.
        if (std::getenv("OA_SELTRACE")) {
            const InlineEventFrame rec = *inline_event_frame_;
            std::string cst;
            const auto& cs = interpreter_->call_stack();
            for (const auto& fr : cs)
                cst += fr.script + ":" + std::to_string(fr.return_line) + " ";
            std::fprintf(stderr,
                         "[seltrace] MARKER-STALE marker=%s:%zu claimed=%d "
                         "stackN=%zu stack[%s]\n",
                         rec.script.c_str(), rec.line, rec.claimed_by_jump ? 1 : 0,
                         cs.size(), cst.c_str());
        }
        inline_event_frame_.reset();
    }
}

void GameRuntime::RuntimeState::begin_inline_event_frame() {
    if (!interpreter_) return;
    refresh_inline_event_frame();
    // Exactly one marker may be live at any time:
    // an active marker either still protects its armed park (nested
    // dispatches at the same park reuse it) or protects an in-flight helper
    // flow (claimed by a jump, real frames above). A second synthetic frame
    // would stack markers unboundedly, so never arm on top of one. Stale
    // records are dropped before this point by refresh (marker frame left the
    // stack) and by settle (reclaim rules); whatever
    // remains is genuinely live and must not be duplicated.
    if (inline_event_frame_) {
        // stale-marker reclaim. A
        // marker armed at the game's select-[stop] park (system/script.asb
        // line 0) and later CLAIMED by the select-exit chain's jump survives
        // the story's navigation away from that park (select_exit ->
        // gotoScript next chapter never [return]s to the park): it stays on
        // the interpreter stack as a zombie frame pointing at a CONSUMED
        // park. The next UI session's popfunc01 [return] then pops that
        // zombie and rewinds the stream to the dead select park
        // (select.lua:482 crash: exittrans guard skips, unguarded clicknext
        // runs with scr.select=nil). A later inline handler at a DIFFERENT
        // park with nothing in flight above the marker (no helper frames, no
        // queued tags) proves the old park was abandoned — drop the zombie
        // frame and arm the new marker at the current park instead, making
        // the UI-session [return] land on the live park exactly as in a
        // select-free journey.
        bool dropped = false;
        if (inline_event_frame_->claimed_by_jump) {
            const InlineEventFrame rec = *inline_event_frame_;
            const std::string* cur = interpreter_->current_script();
            const bool rec_is_select_park =
                rec.script == "system/script.asb" && rec.line <= 1;
            const bool at_same_park =
                cur && *cur == rec.script &&
                interpreter_->current_line() == rec.line;
            const bool anchor_park = !wait_ ||
                wait_->kind == oa::runtime::WaitReason::Kind::Stop ||
                wait_->kind == oa::runtime::WaitReason::Kind::Generic ||
                wait_->kind == oa::runtime::WaitReason::Kind::Generic0 ||
                (wait_->kind == oa::runtime::WaitReason::Kind::Timed &&
                 wait_->input == 2);
            const auto& cs = interpreter_->call_stack();
            // nothing above the marker frame: no helper chain in flight
            // (note: the queue always holds THIS handler's fresh tags —
            // begin runs after fn_queued — so queue state is no signal)
            const bool nothing_above =
                cs.size() == rec.stack.size() + 1;
            if (rec_is_select_park && !at_same_park && anchor_park &&
                nothing_above) {
                // defensive: confirm the frame at the recorded index is the
                // marker before erasing (refresh already validated it)
                if (cs.size() > rec.stack.size() &&
                    cs[rec.stack.size()].script == rec.script &&
                    cs[rec.stack.size()].return_line == rec.line) {
                    std::vector<oa::runtime::CallFrame> cs2 = interpreter_->call_stack();
                    cs2.erase(cs2.begin() + rec.stack.size());
                    inline_event_frame_.reset();
                    interpreter_->set_call_stack(std::move(cs2));
                    dropped = true;
                    if (std::getenv("OA_SELTRACE")) {
                        std::string cst;
                        const auto& cs3 = interpreter_->call_stack();
                        for (const auto& fr : cs3)
                            cst += fr.script + ":" +
                                   std::to_string(fr.return_line) + " ";
                        std::fprintf(stderr,
                                     "[seltrace] MARKER-DROP zombie=%s:%zu "
                                     "newpark=%s:%zu stackN=%zu stack[%s]\n",
                                     rec.script.c_str(), rec.line,
                                     cur ? cur->c_str() : "?",
                                     interpreter_->current_line(), cs3.size(),
                                     cst.c_str());
                    }
                }
            }
        }
        if (!dropped) {
            // OA_SELTRACE: one marker is already live
            // — a second inline handler at the same park reuses it.
            if (std::getenv("OA_SELTRACE")) {
                const InlineEventFrame rec = *inline_event_frame_;
                std::string cst;
                const auto& cs = interpreter_->call_stack();
                for (const auto& fr : cs)
                    cst += fr.script + ":" + std::to_string(fr.return_line) + " ";
                const std::string* cur = interpreter_->current_script();
                std::fprintf(stderr,
                             "[seltrace] MARKER-SKIP live=%s:%zu claimed=%d "
                             "want=%s:%zu stackN=%zu stack[%s]\n",
                             rec.script.c_str(), rec.line,
                             rec.claimed_by_jump ? 1 : 0,
                             cur ? cur->c_str() : "?",
                             interpreter_->current_line(), cs.size(), cst.c_str());
            }
            return;
        }
        // zombie dropped: fall through and arm the marker at the current park
    }
    // Arm only at user-input anchor parks. The helper flows that need marker
    // protection (FPM fn.push/estag UI windows, title/btn navigation) run
    // from [stop]/Generic/Generic0 click parks. Transient timed-wait parks
    // ([wt]-style rows of an in-flight estag chain, media/transition rows)
    // already carry real call frames of their own chain; a synthetic frame
    // there shifts the chain's return balance and the framework's row
    // advance skips the batch [return] (real-fpm media_fpm_probe movie
    // crash). Timed input=2 (skip) parks are still anchors.
    if (wait_) {
        switch (wait_->kind) {
            case oa::runtime::WaitReason::Kind::Stop:
            case oa::runtime::WaitReason::Kind::Generic:
            case oa::runtime::WaitReason::Kind::Generic0:
                break;
            case oa::runtime::WaitReason::Kind::Timed:
                if (wait_->input == 2) break;
                return;
            default:
                return;
        }
    }
    const std::string* script = interpreter_->current_script();
    if (!script) return;
    const size_t line = interpreter_->current_line();
    std::vector<oa::runtime::CallFrame> stack = interpreter_->call_stack();
    std::vector<oa::runtime::CallFrame> with_marker = stack;
    with_marker.push_back(oa::runtime::CallFrame{*script, line});
    // OA_SELTRACE: the synthetic marker frame push —
    // invisible to the interpreter flush/inline probes (set_call_stack
    // rewrite). A marker {system/script.asb:0} is the 482 resurrection
    // frame candidate (popped later by the save-UI popfunc01 [return]).
    if (std::getenv("OA_SELTRACE")) {
        std::string cst;
        for (const auto& fr : stack)
            cst += fr.script + ":" + std::to_string(fr.return_line) + " ";
        std::string wdesc = "nowait";
        if (wait_) {
            switch (wait_->kind) {
                case oa::runtime::WaitReason::Kind::Stop: wdesc = "stop:" + wait_->id; break;
                case oa::runtime::WaitReason::Kind::Generic: wdesc = "generic"; break;
                case oa::runtime::WaitReason::Kind::Generic0: wdesc = "generic0"; break;
                case oa::runtime::WaitReason::Kind::Timed: wdesc = "timed"; break;
                default: wdesc = "other"; break;
            }
        }
        std::fprintf(stderr,
                     "[seltrace] MARKER-ARM marker=%s:%zu wait=%s stackN=%zu "
                     "stack[%s]\n",
                     script->c_str(), line, wdesc.c_str(), stack.size(),
                     cst.c_str());
    }
    // Rewrite ONLY the call stack (push the synthetic marker frame): the
    // recorded script/line IS the current position, so restore_position's
    // position write is a no-op while its two flag resets are not.
    // restore_position clears last_wait_from_queue_, and when the marker is
    // armed while the interpreter is parked on a QUEUE-sourced wait that loses
    // the "this wait came from the queue" bookkeeping. When the wait then
    // resolves, next_line() advances the script line instead of standing
    // still, the parked chain skips one row, its trailing [return] unwinds
    // past its caller and the page's [stop] is never armed: the UI page stays
    // on screen while the boot/game walks on beneath it and no click can
    // reach its buttons any more. set_call_stack is the documented
    // stack-only mutation the settle/teardown paths already use.
    interpreter_->set_call_stack(with_marker);
    inline_event_frame_ = InlineEventFrame{*script, line, std::move(stack), false};
}

void GameRuntime::RuntimeState::settle_inline_event_frame(bool paused, bool has_queued, bool saw_call,
                                            bool saw_jump) {
    // settle = full marker lifecycle. Stack mutations happen ONLY under the
    // same precise
    // conditions as the arm path and only after marker_active confirms the
    // marker
    // frame still sits at its recorded index — arbitrary restore/erase/drop
    // here corrupted queued media/estag flows (real-fpm media_fpm_probe movie
    // replay crash) because they removed frames a live return chain
    // still depended on; the conditions below never do that.
    if (!interpreter_ || !inline_event_frame_) return;
    if (!inline_event_marker_active()) {
        // The marker frame left the stack: the helper's own trailing
        // [return] popped it and the interpreter already resumed the parked
        // line. Bookkeeping only.
        inline_event_frame_.reset();
        return;
    }
    const InlineEventFrame rec = *inline_event_frame_;
    if (saw_call && !rec.claimed_by_jump) {
        // A queued [call] from this helper established a REAL return frame
        // above the synthetic marker: that frame now protects the parked
        // position, the marker is redundant. Remove only the marker and
        // leave the call frame intact. The interpreter
        // position is unchanged (it may be parked mid-flow inside the
        // callee); rewrite the stack only — restore_position would also
        // clear last_wait_from_queue_/arrived_by_jump_, corrupting the
        // parked wait's resolution (see the teardown note below).
        std::vector<oa::runtime::CallFrame> cs = interpreter_->call_stack();
        cs.erase(cs.begin() + rec.stack.size()); // index validated above
        inline_event_frame_.reset();
        interpreter_->set_call_stack(std::move(cs));
        return;
    }
    // Lua UI helpers commonly enqueue a jump to system/script.asb
    // (fn.push/popfuncNN, estag chains): the marker is then the helper's ONLY
    // way back to the park — claim it so later drains (hover lyprop, nested
    // waits) keep it alive across the helper's own waits.
    if (saw_jump) inline_event_frame_->claimed_by_jump = true;
    if (inline_event_frame_->claimed_by_jump || paused || has_queued) {
        // OA_SELTRACE: the marker is retained (claimed
        // by a helper jump / paused / tags behind). A marker retained while
        // the Lua story navigates away (select_exit -> chapter gotoScript)
        // stays on the interpreter stack as a stale {park} frame until a UI
        // popfunc [return] pops it — the 482 resurrection chain.
        if (std::getenv("OA_SELTRACE")) {
            const InlineEventFrame _rec = *inline_event_frame_;
            std::string cst;
            const auto& cs = interpreter_->call_stack();
            for (const auto& fr : cs)
                cst += fr.script + ":" + std::to_string(fr.return_line) + " ";
            std::fprintf(stderr,
                         "[seltrace] MARKER-KEEP marker=%s:%zu claimed=%d "
                         "paused=%d queued=%d stackN=%zu stack[%s]\n",
                         _rec.script.c_str(), _rec.line,
                         inline_event_frame_->claimed_by_jump ? 1 : 0,
                         paused ? 1 : 0, has_queued ? 1 : 0, cs.size(), cst.c_str());
        }
        return;
    }
    // The drain consumed the helper's tags without any pause or navigation
    // and nothing is queued behind: nothing will ever pop the marker (the
    // parked position did not move and no helper flow is running on it).
    // Tear it down and restore the pre-arm stack. The
    // drain was stationary, so the parked position already equals the
    // recorded park — set_call_stack (stack only) is the exact mutation:
    // restore_position would additionally reset the interpreter's
    // last_wait_from_queue_ (a parked queue-wait's next_line then skips
    // the wait row instead of resuming it) and arrived_by_jump_.
    inline_event_frame_.reset();
    if (std::getenv("OA_SELTRACE")) {
        std::string cst;
        const auto& cs = interpreter_->call_stack();
        for (const auto& fr : cs)
            cst += fr.script + ":" + std::to_string(fr.return_line) + " ";
        std::fprintf(stderr,
                     "[seltrace] MARKER-TEARDOWN marker=%s:%zu stackN=%zu "
                     "stack[%s] ->restoreN=%zu\n",
                     rec.script.c_str(), rec.line, cs.size(), cst.c_str(),
                     rec.stack.size());
    }
    interpreter_->set_call_stack(rec.stack);
}

void GameRuntime::RuntimeState::force_pointer_drag(const std::string& layer_id) {
    if (layer_id.empty() || !interpreter_) return;
    drag_begin(layer_id, true);
}

bool GameRuntime::RuntimeState::drag_begin(const std::string& layer_id, bool forced) {
    (void)forced; // forced = [lydrag]: already checked by the caller
    const auto offset = scene_.layer_offset(layer_id);
    if (!offset) return false;
    drag_layer_ = layer_id;
    drag_start_mouse_x_ = double(mouse_x_);
    drag_start_mouse_y_ = double(mouse_y_);
    drag_origin_left_ = offset->first;
    drag_origin_top_ = offset->second;
    const auto* row = scene_.find_event_handler(layer_id, "dragin");
    if (row)
        dispatch_layer_row(layer_id, *row, "dragin",
                           {{"drag", "1"}, {"id", layer_id}}, PointerDispatch::Kind::DragIn);
    return true;
}

void GameRuntime::RuntimeState::drag_update(double mx, double my) {
    if (drag_layer_.empty()) return;
    const double dx = mx - drag_start_mouse_x_;
    const double dy = my - drag_start_mouse_y_;
    double left = 0, top = 0;
    if (!scene_.drag_layer_to(drag_layer_, drag_origin_left_, drag_origin_top_, dx, dy, &left,
                              &top)) {
        drag_layer_.clear();
        return;
    }
    const auto* row = scene_.find_event_handler(drag_layer_, "drag");
    if (row)
        dispatch_layer_row(drag_layer_, *row, "drag",
                           {{"drag", "1"}, {"id", drag_layer_}},
                           PointerDispatch::Kind::DragMove);
}

void GameRuntime::RuntimeState::drag_end() {
    if (drag_layer_.empty()) return;
    const auto* row = scene_.find_event_handler(drag_layer_, "dragout");
    if (row)
        dispatch_layer_row(drag_layer_, *row, "dragout",
                           {{"drag", "0"}, {"id", drag_layer_}},
                           PointerDispatch::Kind::DragOut);
    drag_layer_.clear();
}

// ===========================================================================
// Control domain（原 runtime_control.cpp 并入本 TU；"小接口，大文件"）
//    ——输入/模式状态机：keyconfig 角色表、[rclick]/[hide]/
//    [skip]/[automode]/[exec]/[mouse] 配置标签、命令 skip 与 automode 状态机、
//    控制键 skip、隐藏模式。
// Condensed input/process state machine over the surfaces the FPM
// actually drives:
//   - keyconfig role table (0 = advance, 3/4 = hide in/out,
//     5/6 = backlog, 9-11 = automode, 12/13 = skip, 14 = control-skip,
//     15/16 = avoid placeholder) with default assignments;
//   - command skip ([skip allow] + [exec command=skip]) and automode
//     ([automode allow] + [exec command=automode]) state machines that fire
//     the registered mode input handlers (commandskipin/out, automodein/out);
//   - control-skip: role-14 key held (Ctrl default / csv.advkey.ctrl in FPM)
//     engages an effective skip while held; transitions fire controlskipin/out;
//   - [rclick allow/file] config + engine right-click chain for keys 2/27
//     (hide-mode recovery first, then the rclick script as a call/jump);
//   - [hide allow/window] config + hide mode (window layers hidden, a left
//     click recovers and swallows the click, hidein/hideout events).
// ===========================================================================
namespace {

// keyconfig role numbers (docs/tag/system/keyconfig.md / ROLE_*).
constexpr int kRoleAdvance = 0;
constexpr int kRoleHideIn = 3;
constexpr int kRoleHideOut = 4;
constexpr int kRoleBacklogIn = 5;
constexpr int kRoleBacklogOut = 6;
constexpr int kRoleAutomodeIn = 9;
constexpr int kRoleAutomodeOut = 10;
constexpr int kRoleSkipIn = 12;
constexpr int kRoleSkipOut = 13;
constexpr int kRoleControlSkip = 14;

} // namespace

// ---------------------------------------------------------------------------
// [keyconfig] role application
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::apply_keyconfig(const std::map<std::string, std::string>& params) {
    const auto role_it = params.find("role");
    if (role_it == params.end()) return;
    const int role = parse_i32(role_it->second, -1);
    if (role < 0) return;
    std::set<int> keys;
    const auto k_it = params.find("keys");
    if (k_it != params.end()) {
        std::string raw = k_it->second;
        // keys are a comma / whitespace separated list of virtual key codes;
        // "" clears the role.
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t comma = raw.find_first_of(", \t\r\n", pos);
            const std::string tok = raw.substr(pos, comma == std::string::npos
                                                     ? std::string::npos
                                                     : comma - pos);
            if (!tok.empty()) keys.insert(parse_i32(tok, -1));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        keys.erase(-1);
    }
    keymap_[role] = std::move(keys);
}

bool GameRuntime::RuntimeState::has_role(int role, int key) const {
    const auto it = keymap_.find(role);
    return it != keymap_.end() && it->second.count(key) > 0;
}

bool GameRuntime::RuntimeState::key_is_toggle(int key, int other_role) const {
    return keymap_.count(other_role) > 0 && keymap_.at(other_role).count(key) > 0;
}

/// keyconfig role edge processing. Returns whether the key acted as a
/// role-0 advance.
/// Roles are populated only by [keyconfig] tags (FPM configures every role
/// it uses at boot); no default assignment is assumed so the
/// previous behavior is preserved until a script opts in.
bool GameRuntime::RuntimeState::handle_role_key_edge(int key) {
    if (has_role(kRoleAdvance, key)) return true;
    if (has_role(kRoleHideIn, key)) {
        toggle_hide_mode();
    } else if (has_role(kRoleHideOut, key)) {
        if (!key_is_toggle(key, kRoleHideIn)) exit_hide_mode();
    } else if (has_role(kRoleBacklogIn, key)) {
        dispatch_mode_input("backlogin");
    } else if (has_role(kRoleBacklogOut, key)) {
        dispatch_mode_input("backlogout");
    } else if (has_role(kRoleAutomodeIn, key)) {
        if (key_is_toggle(key, kRoleAutomodeOut)) {
            set_automode_mode(!automode_active());
        } else {
            set_automode_mode(true);
        }
    } else if (has_role(kRoleAutomodeOut, key)) {
        if (!key_is_toggle(key, kRoleAutomodeIn)) {
            set_automode_mode(false);
        }
    } else if (has_role(kRoleSkipIn, key)) {
        if (key_is_toggle(key, kRoleSkipOut)) {
            set_skip_mode(!skip_active());
        } else {
            set_skip_mode(true);
        }
    } else if (has_role(kRoleSkipOut, key)) {
        if (!key_is_toggle(key, kRoleSkipIn)) {
            set_skip_mode(false);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// [rclick allow/file] + engine right-click chain
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::apply_rclick_config(const std::map<std::string, std::string>& params) {
    const auto allow_it = params.find("allow");
    if (allow_it != params.end()) rclick_allowed_ = parse_i32(allow_it->second, 1) != 0;
    const auto file_it = params.find("file");
    if (file_it != params.end() && !file_it->second.empty()) rclick_file_ = file_it->second;
}
// ---------------------------------------------------------------------------
// [mouse left top] — pointer-warp request ([mouse] config tag). The FPM
// confirm-dialog flow moves the REAL pointer onto the default (YES) button:
// dialog.lua yesno_active -> adv.lua mouse_autocursor issues 10 eased [mouse]
// tags, each landing one step closer to the button center, then btn_active2
// focuses the button. The engine pointer is always FrameInput-driven (SDL
// motion events); this only requests the host to warp the OS cursor
// (SDL_WarpMouseInWindow). SDL then synthesizes real mouse-motion feedback,
// so rollover/rollout/click dispatch keeps running on the normal chain.
// Before this handler existed the [mouse] tag was a parse-only no-op and the
// cursor stayed where the user last left it (exit-dialog mouse-autocursor
// alignment — user feedback).
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::apply_mouse_config(const std::map<std::string, std::string>& params) {
    // A live drag owns the pointer: never warp mid-drag (defensive; the FPM
    // dialog path disables drags first via sliderdrag_stat(0)).
    if (!drag_layer_.empty()) return;
    const auto left_it = params.find("left");
    const auto top_it = params.find("top");
    if (left_it == params.end() && top_it == params.end()) return;
    int x = mouse_x_;
    int y = mouse_y_;
    if (left_it != params.end()) x = parse_i32(left_it->second, x);
    if (top_it != params.end()) y = parse_i32(top_it->second, y);
    // Clamp into the stage (defensive: official callers only send button
    // centers / +/-1px refresh nudges).
    const int sw = rt_->project_.config.stage_width;
    const int sh = rt_->project_.config.stage_height;
    if (sw > 0) x = std::clamp(x, 0, sw - 1);
    if (sh > 0) y = std::clamp(y, 0, sh - 1);
    pointer_warp_x_ = x;
    pointer_warp_y_ = y;
    pointer_warp_pending_ = true;
    ++pointer_warp_requests_;
    if (pointer_warp_cb_) pointer_warp_cb_(x, y);
}

bool GameRuntime::RuntimeState::trigger_rclick() {
    if (!interpreter_) return false;
    // hide mode: a right click first recovers the message window (no menu).
    if (hide_active_) {
        exit_hide_mode();
        return true;
    }
    if (!rclick_allowed_) return false;
    const std::string file =
        rclick_file_.empty() ? std::string("rclick.iet") : rclick_file_;
    // Already inside the rclick script: jump to its "leave" label (its return
    // comes back to the pre-rclick position —  approximation).
    const std::string* cur = interpreter_->current_script();
    if (cur && *cur == file) {
        std::map<std::string, std::string> nav{{"file", file}, {"label", "leave"}};
        interpreter_->enqueue_tag("jump", std::move(nav));
    } else {
        // Enter the rclick script as a subroutine (call frame; a [return] in
        // the script resumes the story at the original position).
        std::map<std::string, std::string> nav{{"file", file}};
        interpreter_->enqueue_tag("call", std::move(nav));
    }
    return true;
}

// ---------------------------------------------------------------------------
// [hide allow/window] + hide mode
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::apply_hide_config(const std::map<std::string, std::string>& params) {
    const auto allow_it = params.find("allow");
    if (allow_it != params.end()) {
        const bool allow = parse_i32(allow_it->second, 1) != 0;
        if (!allow && hide_active_) exit_hide_mode();
        hide_allowed_ = allow;
    }
    const auto win_it = params.find("window");
    if (win_it != params.end()) {
        hide_window_.clear();
        std::string raw = win_it->second;
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t comma = raw.find(',', pos);
            std::string tok = raw.substr(pos, comma == std::string::npos
                                                 ? std::string::npos
                                                 : comma - pos);
            // trim spaces
            const auto b = tok.find_first_not_of(" \t\r\n");
            const auto en = tok.find_last_not_of(" \t\r\n");
            tok = (b == std::string::npos) ? "" : tok.substr(b, en - b + 1);
            if (!tok.empty()) hide_window_.push_back(tok);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
}

void GameRuntime::RuntimeState::apply_hide_visibility(bool hidden) {
    for (const std::string& wid : hide_window_) {
        const oa::render::Layer* l = scene_.find(wid);
        if (!l) continue;
        const bool visible_now = l->visible != 0.0;
        if (hidden) {
            if (!hide_snapshot_.count(wid)) hide_snapshot_[wid] = visible_now;
            if (visible_now) scene_.set_props(wid, {{"visible", "0"}});
        } else {
            const bool was_visible = hide_snapshot_.count(wid) ? hide_snapshot_[wid] : true;
            hide_snapshot_.erase(wid);
            if (was_visible && !visible_now) scene_.set_props(wid, {{"visible", "1"}});
        }
    }
}

void GameRuntime::RuntimeState::toggle_hide_mode() {
    if (hide_active_) {
        exit_hide_mode();
    } else {
        if (!hide_allowed_) return;
        hide_active_ = true;
        apply_hide_visibility(true);
        dispatch_mode_input("hidein");
    }
}

void GameRuntime::RuntimeState::exit_hide_mode() {
    if (!hide_active_) return;
    hide_active_ = false;
    apply_hide_visibility(false);
    dispatch_mode_input("hideout");
}

// ---------------------------------------------------------------------------
// skip / automode state machines
// (set_skip_mode/set_automode_mode, apply_skip_config/apply_automode_config)
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::reveal_all_text() {
    if (!text_.is_reveal_complete()) text_.reveal_all();
}

void GameRuntime::RuntimeState::dispatch_mode_input(const std::string& event_name) {
    // Mode handlers register under (event, "") via [setonautomodein] etc.
    // ( input_handlers; enqueue_mode_input_handler ).
    const auto* row = scene_.get_input_handler(event_name, "");
    if (!row) return;
    dispatch_input_row("seton" + event_name, *row, {{"type", event_name}},
                       PointerDispatch::Kind::Push);
}

void GameRuntime::RuntimeState::set_skip_mode(bool enabled) {
    if (enabled && !skip_allowed_) return;
    if (enabled && automode_active()) set_automode_mode(false);
    const bool was_active = skip_active();
    skip_enabled_ = enabled;
    const bool now_active = skip_active();
    if (was_active == now_active) return;
    auto_elapsed_ms_ = 0;
    if (!now_active) {
        reveal_all_text(); // reveal on skip exit so new text is not stuck
    }
    dispatch_mode_input(now_active ? "commandskipin" : "commandskipout");
}

void GameRuntime::RuntimeState::set_automode_mode(bool enabled) {
    if (enabled && !automode_allowed_) return;
    if (enabled && skip_active()) set_skip_mode(false);
    const bool was_active = automode_active();
    automode_ = enabled;
    if (was_active == automode_active()) return;
    auto_elapsed_ms_ = 0;
    // leaving automode drops the voice-sync gate — the Lua side
    // (autoskip.lua) also clears it with [automode syncse=""], this makes the
    // native exec path symmetric so a later native re-entry starts ungated.
    if (!automode_active()) automode_syncse_.clear();
    dispatch_mode_input(automode_active() ? "automodein" : "automodeout");
}

void GameRuntime::RuntimeState::update_control_skip_hold(const std::set<int>& keys_down) {
    // role 14 keys (Ctrl default; FPM csv.advkey.ctrl) act while held.
    const bool held = std::any_of(keys_down.begin(), keys_down.end(),
                                  [this](int k) { return has_role(kRoleControlSkip, k); });
    const bool was_effective = control_skip_effective();
    if (!held) control_skip_blocked_ = false;
    control_skip_pressed_ = held;
    const bool now_effective = control_skip_effective();
    if (was_effective == now_effective) return;
    auto_elapsed_ms_ = 0;
    if (!now_effective) reveal_all_text();
    dispatch_mode_input(now_effective ? "controlskipin" : "controlskipout");
}

// ---------------------------------------------------------------------------
// config tags ([skip]/[automode]/[keyconfig]/[rclick]/[hide]/[exec]/[backlog])
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::apply_skip_config(const std::map<std::string, std::string>& params) {
    const auto allow_it = params.find("allow");
    if (allow_it != params.end()) {
        const bool allow = parse_i32(allow_it->second, 1) != 0;
        if (!allow) {
            if (skip_active()) set_skip_mode(false);
            // a held role-14 key is blocked until it is released
            control_skip_blocked_ = control_skip_pressed_;
        }
        skip_allowed_ = allow;
    }
    // unread stop-to-skip ([skip unread]) is not ported.
}

void GameRuntime::RuntimeState::apply_automode_config(const std::map<std::string, std::string>& params) {
    const auto allow_it = params.find("allow");
    if (allow_it != params.end()) {
        const bool allow = parse_i32(allow_it->second, 1) != 0;
        if (!allow && automode_active()) set_automode_mode(false);
        automode_allowed_ = allow;
    }
    // syncse is the FPM-family voice-sync contract, NOT Lua-only
    // bookkeeping. At every automode click park the Lua layer queues
    // [automode syncse=<current voice ids>] (conf.autostop==1) and clears it
    // (syncse="") on page release / automode stop; the engine gates the auto
    // page flip on those channels (resolve_wait Generic/Generic0 +
    // automode_sync_blocked). stopbyclick/stopbystop stay Lua-side (the Lua
    // flow stops automode itself on input/stop rows).
    const auto sync_it = params.find("syncse");
    if (sync_it != params.end()) automode_syncse_ = sync_it->second;
}

void GameRuntime::RuntimeState::apply_exec_command(const std::map<std::string, std::string>& params) {
    const auto cmd_it = params.find("command");
    if (cmd_it == params.end()) return;
    const std::string& command = cmd_it->second;
    const auto mode_it = params.find("mode");
    const bool has_mode = mode_it != params.end();
    const int mode = has_mode ? parse_i32(mode_it->second, 1) : 0;
    if (command == "skip") {
        set_skip_mode(has_mode ? mode != 0 : !skip_enabled_);
    } else if (command == "automode") {
        set_automode_mode(has_mode ? mode != 0 : !automode_active());
    } else if (command == "rclick") {
        if (!trigger_rclick()) {
            // fall back to the push key=2 chain ( enqueue_exec_input)
            const auto* row = scene_.get_input_handler("push", "2");
            if (row) dispatch_input_row("setonpush", *row,
                                        {{"key", "2"}, {"type", "click"}},
                                        PointerDispatch::Kind::Push);
        }
    } else if (command == "hide") {
        toggle_hide_mode();
    } else if (command == "backlog") {
        dispatch_mode_input("backlogin");
    } else if (command == "backlogclose") {
        dispatch_mode_input("backlogout");
    } else {
        // exec commands without an engine mapping are a diagnostic no-op
        std::fprintf(stderr, "[runtime] exec command not mapped: %s\n", command.c_str());
    }
}

bool GameRuntime::RuntimeState::apply_control_config(const oa::runtime::Event& e) {
    if (e.kind != oa::runtime::Event::Kind::ConfigEvent) return false;
    if (e.tag == "keyconfig") {
        apply_keyconfig(e.params);
        return true;
    }
    if (e.tag == "rclick") {
        apply_rclick_config(e.params);
        return true;
    }
    if (e.tag == "hide") {
        apply_hide_config(e.params);
        return true;
    }
    if (e.tag == "skip") {
        apply_skip_config(e.params);
        return true;
    }
    if (e.tag == "automode") {
        apply_automode_config(e.params);
        return true;
    }
    if (e.tag == "mouse") {
        apply_mouse_config(e.params);
        return true;
    }
    if (e.tag == "exec") {
        apply_exec_command(e.params);
        return true;
    }
    return false;
}

} // namespace oa::runtime
