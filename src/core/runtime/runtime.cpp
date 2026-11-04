#include "core/runtime/runtime_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "core/emote/emote_player.h"
#include "core/fs/physfs_fs.h"
#include "core/fs/project.h"
#include "core/render/renderer.h"
#include "core/runtime/runtime.h" // App (host contract)
#include "core/runtime/runtime_save.h"
#include "core/version.h"

namespace oa::runtime {

namespace {
// virtual key codes used by Artemis scripts
constexpr int kKeyMouseLeft = 1;
/// Shared budget for the OA_STORYDBG lines (arm + release, first N).
int g_storydbg = 0;
int g_storydbg_arm = 0;
constexpr int kKeyMouseRight = 2;
constexpr int kKeyEnter = 13;
// Short name for the renderer's frame-dirty tags (renderer.h Note).
using RNote = oa::render::RenderEngine::Note;
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

// GameRuntime public forwarding.
//     实现面（状态 + 逻辑）全在 runtime_internal.h 的 RuntimeState；本类只持有
//     s_ 并把每个公开方法转给它的同名实现。公开签名与 runtime.h 逐字一致，
//     因此 56 个调用方一行不改。
// ===========================================================================
GameRuntime::RuntimeState::RuntimeState(GameRuntime* rt,
                                       std::shared_ptr<const oa::fs::IFileSystem> fs)
    // 初始化列表照搬原 GameRuntime 构造函数；顺序 = 声明顺序。
    : rt_(rt),
      fs_(std::move(fs)),
      renderer_(fs_.get(), static_cast<oa::render::RenderHost*>(rt), false)
{
    save_.save_store_ = std::make_shared<oa::runtime::NullSaveStore>();
    media_.media_players_ = std::make_unique<oa::media::MediaPlayers>();
    set_hit_providers(oa::render::RenderEngine::hit_size,
                      oa::render::RenderEngine::alpha_sampler, &renderer_);
    set_pointer_warp_callback([this](int sx, int sy) {
        renderer_.warp_mouse_to_stage(float(sx), float(sy));
    });
    set_frame_capture([this](uint32_t* w, uint32_t* h) {
        return renderer_.capture_frame(w, h);
    });
}

GameRuntime::RuntimeState::~RuntimeState()
{
    renderer_.release_all();
    // Stop the video driver and cancel decode pipes BEFORE the audio sink and
    // the decode pool are torn down: the driver thread calls the movie-audio
    // hook and the pool workers, so releasing either first is a use-after-free.
    media_.video_.stop_all_videos();
    media_.video_.stop_driver();
    media_.video_.set_audio_output(nullptr);
    media_.video_.set_audio_flush(nullptr);
    media_.media_players_->print_audio_diag("run");
    media_.media_players_->release();
}

bool GameRuntime::RuntimeState::attach_window(void* native_window,
                                              const std::string& backend_kind)
{
    if (!renderer_.create_renderer(native_window, backend_kind)) return false;
    oa::emote::emote_set_default_external_pose(true);
    media_.media_players_->init();
    media_.video_.set_audio_output([mp = media_.media_players_.get()](const float* d, size_t n) {
        mp->push_video_audio(d, n);
    });
    media_.video_.set_audio_flush([mp = media_.media_players_.get()]() {
        mp->flush_video_audio();
    });
    // 视频源自己解码并 update 自己的纹理；纹理挂到层上（driver 线程入队，
    // 主线程落场景，render 只负责画层）。
    media_.video_.set_texture_factory(
        [this](int w, int h) { return renderer_.create_host_texture(w, h); });
    media_.video_.set_frame_push(
        [this](const std::string& channel, const oa::media::TextureRef& tex,
               int, int) {
            const std::string id = channel.empty()
                                       ? std::string(oa::render::kOverlayNodeId)
                                       : channel;
            queue_host_texture(id, tex);
        });
    return true;
}

std::string GameRuntime::RuntimeState::resolve_magic_path(
    const std::string& name) const
{
    return interpreter_ ? interpreter_->resolve_magic_path(name) : name;
}

std::optional<std::string> GameRuntime::RuntimeState::host_variable(
    const std::string& key) const
{
    if (!interpreter_) return std::nullopt;
    const auto v = interpreter_->variables().get(key);
    if (!v) return std::nullopt;
    return v->to_string();
}

std::optional<std::vector<uint8_t>> GameRuntime::RuntimeState::read_save_file(
    const std::string& rel_path) const
{
    const std::string& sp = save_.savepath_;
    if (sp.empty() || rel_path.empty()) return std::nullopt;
    if (!(rel_path == sp ||
          (sp.size() < rel_path.size() && rel_path.rfind(sp + "/", 0) == 0)))
        return std::nullopt;
    if (!save_.save_store_) return std::nullopt;
    return save_.save_store_->read(rel_path);
}

void GameRuntime::RuntimeState::bind_text_metrics(
    std::function<std::tuple<double, double, double>()> fn)
{
    if (!interpreter_ || !fn) return;
    interpreter_->hooks().message_layer_metrics = std::move(fn);
}

GameRuntime::GameRuntime(std::shared_ptr<const oa::fs::IFileSystem> fs)
    : s_(std::make_unique<RuntimeState>(this, std::move(fs))) {}

GameRuntime::~GameRuntime() = default;

// ---- 公开方法：逐个转发 ----------------------------------------------------

void GameRuntime::open_project(std::string_view platform) { s_->open_project(platform); }
void GameRuntime::boot_project() { s_->boot_project(); }
void GameRuntime::tick(uint64_t delta_ms, const FrameInput& input) { s_->tick(delta_ms, input); }
void GameRuntime::tick(uint64_t delta_ms) { s_->tick_input(delta_ms); }
void GameRuntime::key_event(int vk, bool down) { s_->key_event(vk, down); }
void GameRuntime::mouse_move(float window_x, float window_y) { s_->mouse_move(window_x, window_y); }
void GameRuntime::mouse_button(int vk, bool down) { s_->mouse_button(vk, down); }
void GameRuntime::mouse_wheel(int direction) { s_->mouse_wheel(direction); }
bool GameRuntime::exit_requested() const { return s_->exit_requested(); }
std::vector<oa::runtime::Event> GameRuntime::drain_events() { return s_->drain_events(); }
oa::runtime::Interpreter& GameRuntime::interpreter() { return s_->interpreter(); }
const oa::runtime::Interpreter& GameRuntime::interpreter() const { return s_->interpreter(); }
const std::shared_ptr<const oa::fs::IFileSystem>& GameRuntime::fs() const { return s_->fs(); }
bool GameRuntime::attach_window(void* native_window, const std::string& renderer_kind) {
    return s_->attach_window(native_window, renderer_kind);
}
std::string GameRuntime::resolve_magic_path(const std::string& name) const {
    return s_->resolve_magic_path(name);
}
std::optional<std::string> GameRuntime::host_variable(const std::string& key) const {
    return s_->host_variable(key);
}
std::optional<std::vector<uint8_t>> GameRuntime::read_save_file(
    const std::string& rel_path) const {
    return s_->read_save_file(rel_path);
}
void GameRuntime::bind_text_metrics(
    std::function<std::tuple<double, double, double>()> fn) {
    s_->bind_text_metrics(std::move(fn));
}
int GameRuntime::stage_width() const { return s_->stage_width(); }
int GameRuntime::stage_height() const { return s_->stage_height(); }
bool GameRuntime::vsync_active() const { return s_->vsync_active(); }
bool GameRuntime::frame_presented() const { return s_->frame_presented(); }
void GameRuntime::request_repaint() { s_->request_repaint(); }
std::optional<std::vector<uint8_t>> GameRuntime::capture_frame(uint32_t* w,
                                                              uint32_t* h) {
    return s_->capture_frame(w, h);
}
oa::render::Compositor& GameRuntime::scene() { return s_->scene(); }
const oa::render::Compositor& GameRuntime::scene() const { return s_->scene(); }
size_t GameRuntime::scene_layer_events() const { return s_->scene_layer_events(); }
size_t GameRuntime::scene_tween_events() const { return s_->scene_tween_events(); }
size_t GameRuntime::tween_completion_calls() const { return s_->tween_completion_calls(); }
bool GameRuntime::waiting_stop() const { return s_->waiting_stop(); }
const oa::runtime::WaitReason* GameRuntime::current_wait() const { return s_->current_wait(); }
void GameRuntime::apply_override(int key, int status) { s_->apply_override(key, status); }
TransitionStatus GameRuntime::transition() { return TransitionStatus(&s_->transition()); }
TransitionStatus GameRuntime::transition() const { return TransitionStatus(&s_->transition()); }

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
oa::render::Transition& GameRuntime::transition_state() { return s_->transition_state(); }
void GameRuntime::set_transition_capture_callback(std::function<void()> cb) { s_->set_transition_capture_callback(std::move(cb)); }
void GameRuntime::mark_transition_captured() { s_->mark_transition_captured(); }
void GameRuntime::begin_all_delete(uint64_t time_ms) { s_->begin_all_delete(time_ms); }
double GameRuntime::all_delete_fade() const { return s_->all_delete_fade(); }
void GameRuntime::set_hit_providers(oa::render::Compositor::QuadSizeFn size, oa::render::Compositor::AlphaSamplerFn alpha, void* userdata) { s_->set_hit_providers(std::move(size), std::move(alpha), userdata); }
void GameRuntime::set_pointer_observer(std::function<void(const PointerDispatch&)> cb) { s_->set_pointer_observer(std::move(cb)); }
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
const oa::render::TextEngine& GameRuntime::text() const { return s_->text(); }
oa::render::TextEngine& GameRuntime::text() { return s_->text(); }
size_t GameRuntime::text_events() const { return s_->text_events(); }
size_t GameRuntime::message_switch_events() const { return s_->message_switch_events(); }
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
void GameRuntime::set_frame_capture(CaptureFn fn) { s_->set_frame_capture(std::move(fn)); }
void GameRuntime::post_frame_capture() { s_->post_frame_capture(); }
bool GameRuntime::syssave() { return s_->syssave(); }
bool GameRuntime::save_game_to(const std::string& file) { return s_->save_game_to(file); }
LoadResult GameRuntime::load_game(const std::string& file, int64_t trans_type) {
    return s_->load_game(file, trans_type);
}
bool GameRuntime::load_game_from(const std::string& file, int64_t trans_type) { return s_->load_game_from(file, trans_type); }
const std::string& GameRuntime::savepath() const { return s_->savepath(); }
void GameRuntime::apply_save_event(const oa::runtime::Event& e) { s_->apply_save_event(e); }
bool GameRuntime::apply_media_event(const oa::runtime::Event& e) { return s_->apply_media_event(e); }
oa::runtime::CallbackResult GameRuntime::apply_event(const oa::runtime::Event& e) {
    return s_->dispatch_interpreter_event(e);
}
void GameRuntime::sound_finished(oa::media::SoundCategory cat, const std::string& id) { s_->sound_finished(cat, id); }

std::pair<double, double> GameRuntime::RuntimeState::bound_image_size(const oa::render::Layer& l) {
    // Cached header-sniffed natural size; Asset textures only.
    if (l.file.empty()) return {0, 0};
    const oa::render::TextureKey key = oa::render::content_texture_key(l);
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
        if (media_.decode_pool_) {
            if (media_.media_players_) media_.media_players_->set_decode_pool(nullptr);
            media_.video_.set_decode_pool(nullptr);
            media_.decode_pool_.reset();
        }
        return;
    }
    media_.decode_pool_ = std::make_unique<oa::media::DecodePool>(threads);
    if (media_.media_players_) media_.media_players_->set_decode_pool(media_.decode_pool_.get());
    media_.video_.set_decode_pool(media_.decode_pool_.get());
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
    renderer_.bind_runtime_hooks();
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
    save_.savepath_ = sanitize_savepath(rt_->project_.config.savepath);
    interpreter_->set_variable("s.savepath", oa::runtime::Value::make_string(save_.savepath_));
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
        const oa::render::Layer* l = scene().find(id);
        if (!l) return out;
        for (const auto& [k, v] : l->props) out[k] = v;
        out["left"] = format_prop_num(l->left);
        out["top"] = format_prop_num(l->top);
        out["x"] = format_prop_num(l->left);
        out["y"] = format_prop_num(l->top);
        // Effective size: explicit width/height > clip > natural texture.
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
    // (MediaPlayers::set_loader also probes the ".ogg"/".wav" containers for
    // extension-less sound names — the Ethornell-derived projects hand the
    // sound tags a bare base name.)
    media_.media_players_->set_loader([this](const std::string& logical)
                                   -> std::optional<std::vector<uint8_t>> {
        if (!interpreter_) return std::nullopt;
        return fs_->read(interpreter_->resolve_magic_path(logical));
    });
    // The video decoder reads whole asset files through the same face.
    // A missing/undecodable file keeps the logical immediate-finish fallback
    // inside VideoEngine, so project flows never hang on unavailable video.
    media_.video_.set_loader([this](const std::string& logical)
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
        const bool under_save = !save_.savepath_.empty() &&
                                (resolved == save_.savepath_ ||
                                 (resolved.size() > save_.savepath_.size() &&
                                  resolved.rfind(save_.savepath_ + "/", 0) == 0));
        if (under_save && save_.save_store_) {
            const std::string rel = qualify_save_file(resolved);
            if (!rel.empty()) {
                if (auto b = save_.save_store_->read(rel)) return b;
                return std::nullopt; // save-area names do not fall back to PFS
            }
        }
        return fs_->read(resolved);
    };
    interpreter_->hooks().resource_read = read_resolved;
    interpreter_->hooks().resource_exists = [read_resolved](const std::string& resolved) {
        return read_resolved(resolved).has_value();
    };
    interpreter_->hooks().immediate_tags = false;
    // FPM's estag calls are fall-through label chains that enqueue the whole
    // UI state; holding the queue behind them collapses the entrance into one
    // burst (see InterpreterHooks::queue_call_hold). Non-FPM keeps the hold.
    interpreter_->hooks().queue_call_hold = false;
    interpreter_->hooks().save_root = [this]() -> std::string {
        if (auto dir = std::dynamic_pointer_cast<oa::runtime::DirSaveStore>(save_.save_store_)) {
            return dir->root();
        }
        return std::string();
    };
    interpreter_->hooks().save_file_exists = [this](const std::string& file) -> bool {
        const std::string rel = qualify_save_file(file);
        if (rel.empty() || !save_.save_store_) return false;
        return save_.save_store_->exists(rel);
    };
    interpreter_->hooks().file_mtime =
        [this](const std::string& file)
        -> std::optional<std::array<int64_t, 6>> {
        const std::string rel = qualify_save_file(file);
        if (rel.empty() || !save_.save_store_) return std::nullopt;
        return save_.save_store_->modification_time(rel);
    };
    interpreter_->hooks().sound_info = [this] { return sound_info_snapshot_for_hook(); };
    // e:var system=get_backlog_size/get_backlog_tags/get_message_tags
    // host query hooks → oa::render::TextEngine data surface. The interpreter
    // side of this bridge was previously unwired; here the runtime provides
    // the real data.
    interpreter_->hooks().backlog_size = [this]() -> size_t {
        return text().backlog_size();
    };
    interpreter_->hooks().backlog_tags = [this](size_t page, bool allfont)
        -> std::optional<std::vector<std::string>> {
        return text().backlog_tags(page, allfont);
    };
    interpreter_->hooks().message_tags = [this](const std::string& id, bool allfont)
        -> std::optional<std::vector<std::string>> {
        return text().message_tags(id, allfont);
    };
    // Every interpreter event is routed by domain in one place; waits only
    // reach the host stream (and pause) through that dispatcher.
    interpreter_->set_callback([this](const oa::runtime::Event& e) {
        return dispatch_interpreter_event(e);
    });
}

// --------------------------------------------------------------------------- Interpreter event dispatch — the fixed domain order is: engin...
oa::runtime::CallbackResult GameRuntime::RuntimeState::dispatch_interpreter_event(
    const oa::runtime::Event& e)
{
    oa::runtime::CallbackResult result = oa::runtime::CallbackResult::Continue;
    if (dispatch_engine_event(e, &result)) return result;
    if (dispatch_media_event(e, &result)) return result;
    if (apply_text_event(e)) return oa::runtime::CallbackResult::Continue;
    if (apply_scene_event(e)) return oa::runtime::CallbackResult::Continue;
    if (apply_control_event(e)) return oa::runtime::CallbackResult::Continue;

    pending_events_.push_back(e);
    return host_event_result(e);
}

bool GameRuntime::RuntimeState::dispatch_engine_event(
    const oa::runtime::Event& e, oa::runtime::CallbackResult* out)
{
    if (e.tag != "alldelete") return false;
    // Whole-scene fade-out + clear; time==0 clears instantly and continues.
    uint64_t t = 0;
    if (const auto it = e.params.find("time"); it != e.params.end()) {
        try {
            t = (uint64_t)std::stoull(it->second);
        } catch (...) {
            t = 0;
        }
    }
    begin_all_delete(t);
    *out = t > 0 ? oa::runtime::CallbackResult::Pause
                 : oa::runtime::CallbackResult::Continue;
    return true;
}

bool GameRuntime::RuntimeState::dispatch_media_event(
    const oa::runtime::Event& e, oa::runtime::CallbackResult* out)
{
    // e:createEmoteLayer static layer (lyc2 already materialized the carrier
    // chain); decode + render + bind, never pauses.
    if (e.tag == "emotestatic") {
        apply_emote_static(e);
        *out = oa::runtime::CallbackResult::Continue;
        return true;
    }
    if (!apply_media_event(e)) return false;
    // A fullscreen [video] (no id) parks the script (Stop{reason:"video"}).
    *out = (e.tag == "video" && e.id.empty())
               ? oa::runtime::CallbackResult::Pause
               : oa::runtime::CallbackResult::Continue;
    return true;
}

oa::runtime::CallbackResult GameRuntime::RuntimeState::host_event_result(
    const oa::runtime::Event& e) const
{
    using K = oa::runtime::Event::Kind;
    if (e.kind == K::Wait_) return oa::runtime::CallbackResult::Pause;
    // [reset] hands the engine back to its boot script: continuing would run
    // the rows after [reset] against the cleared variable store.
    if (e.kind == K::Reset) return oa::runtime::CallbackResult::Pause;
    if (e.kind == K::Trans) {
        int type = 1;
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
}

void GameRuntime::RuntimeState::boot_project()
{
    // Load system saves before boot (Lua dataloading reads g./s.).
    sysload();
    wire_lua_host();
    interpreter_->boot(rt_->project_.config.boot_script);
}

void GameRuntime::RuntimeState::reset_domains() {
    // Clear media/text/scene/transition/pointer/wait; boot re-applies config.
    media_.audio_.stop_all_sounds();
    if (media_.media_players_) media_.media_players_->stop_all();
    media_.video_.stop_all_videos();
    // Aligned with reset_ephemeral_on_load: stale video EOF must not leak.
    media_.video_finished_ = false;
    text().clear_scene();
    scene().clear_scene();
    drop_pending_tween_cancels(); // deferred timer rounds die with nodes
    media_.emote_layers_.clear(); // static emote frames die with the scene
    media_.emote_view_cache_.clear(); // ... and so does a recorded view
    transition().clear();
    pointer_.hovered_.clear();
    pointer_.drag_layer_.clear();
    pending_sync_tween_layer_.reset();
    inline_event_frame_.reset();
    save_.screenshot_.valid = false;
    save_.capture_pending_ = false;
    renderer_.stop_all_delete();
    wait_.reset();
    wait_remaining_ms_ = 0;
    exit_requested_ = false;
    control_.skip_active_ = false;
    control_.exskip_active_ = false; // e:debugSkip state dies with the domains
    control_.skip_allowed_ = true;
    control_.skip_enabled_ = false;
    control_.automode_allowed_ = true;
    control_.automode_ = false;
    control_.auto_elapsed_ms_ = 0;
    control_.control_skip_pressed_ = false;
    control_.control_skip_blocked_ = false;
    control_.last_control_skip_effective_ = false;
    control_.rclick_allowed_ = false;
    control_.rclick_file_.clear();
    control_.hide_allowed_ = false;
    control_.hide_active_ = false;
    control_.hide_window_.clear();
    control_.hide_snapshot_.clear();
    control_.keymap_.clear();
    control_.script_status_ = 0;
    control_.active_wait_icon_.clear();
    input_state_.overrides_.clear();
    input_state_.prev_overrides_.clear();
    input_state_.keys_down_.clear();
    input_state_.down_edges_.clear();
    input_state_.up_edges_.clear();
    input_state_.decide_edge_ = false;
    pointer_.mouse_prev_x_ = 0;
    pointer_.mouse_prev_y_ = 0;
    pointer_.mouse_inited_ = false;
    pointer_.left_down_prev_ = false;
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
    renderer_.begin_all_delete(time_ms, now_ms_);
    if (time_ms == 0) finish_all_delete();
}

double GameRuntime::RuntimeState::all_delete_fade() const {
    return renderer_.all_delete_fade(now_ms_);
}

void GameRuntime::RuntimeState::finish_all_delete() {
    renderer_.stop_all_delete();
    // Everything the [alldelete] flow wipes: scene layers (image + text
    // overlay nodes + handler registries + message bindings), the text
    // engine content, and the pointer state pointing into them.
    scene().clear_scene();
    text().clear_scene();
    drop_pending_tween_cancels(); // deferred timer rounds die with nodes
    pointer_.hovered_.clear();
    pointer_.drag_layer_.clear();
    pending_sync_tween_layer_.reset();
    save_.screenshot_.valid = false;
    save_.capture_pending_ = false;
    media_.emote_layers_.clear(); // static emote frames die with the scene
    media_.emote_view_cache_.clear(); // ... and so does a recorded view
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
    host.get_script_status = [this] { return control_.script_status_; };
    host.set_script_status = [this](int s) { control_.script_status_ = s; };
    host.debug_skip_start = [this](int64_t index) {
        (void)index;
        start_debug_skip();
    };
    host.is_down = [this](int key) {
        if (const auto it = input_state_.overrides_.find(key); it != input_state_.overrides_.end()) {
            if (it->second & 4) return true;
        }
        return input_state_.keys_down_.count(key) > 0;
    };
    host.is_down_edge = [this](int key) {
        if (const auto it = input_state_.overrides_.find(key); it != input_state_.overrides_.end()) {
            const int prev = input_state_.prev_overrides_.count(key) ? input_state_.prev_overrides_.at(key) : 0;
            if ((it->second & 8) && !(prev & 8)) return true;
        }
        // physical edges are delivered through down-edge set computed in tick
        return input_state_.down_edges_.count(key) > 0;
    };
    host.is_up_edge = [this](int key) {
        if (const auto it = input_state_.overrides_.find(key); it != input_state_.overrides_.end()) {
            const int prev = input_state_.prev_overrides_.count(key) ? input_state_.prev_overrides_.at(key) : 0;
            if ((it->second & 16) && !(prev & 16)) return true;
        }
        return input_state_.up_edges_.count(key) > 0;
    };
    host.is_push = [this](int key) {
        if (const auto it = input_state_.overrides_.find(key); it != input_state_.overrides_.end()) {
            if (it->second & 2) return true;
        }
        return input_state_.keys_down_.count(key) > 0;
    };
    host.is_decide = [this](int key) {
        if (const auto it = input_state_.overrides_.find(key); it != input_state_.overrides_.end()) {
            const int prev = input_state_.prev_overrides_.count(key) ? input_state_.prev_overrides_.at(key) : 0;
            if ((it->second & 32) && !(prev & 32)) return true;
        }
        return false;
    };
    host.get_mouse_point = [this] {
        return std::pair(input_state_.mouse_x_, input_state_.mouse_y_);
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
        input_state_.overrides_.erase(key);
    } else {
        input_state_.overrides_[key] = status;
    }
}

void GameRuntime::RuntimeState::tick(uint64_t delta_ms, const FrameInput& in) {
    if (!interpreter_) return;
    ++tick_count_;
    now_ms_ += delta_ms;
    // A captured transition may have reached its duration: auto-clear before
    // any wait resolution consults it (transition::clear_finished every
    // frame).
    transition().clear_finished(now_ms_);
    // Drop the inline-event marker once its frame left the stack.
    refresh_inline_event_frame();
    // [alldelete]: the render fade clock expired -> clear the scene + text.
    if (renderer_.all_delete_expired(now_ms_)) finish_all_delete();
    // Cursor-warp emulation when the host cannot warp (hidden/headless).
    FrameInput eff = in;
    const bool raw_static =
        eff.mouse_x == pointer_.raw_mouse_x_ && eff.mouse_y == pointer_.raw_mouse_y_;
    pointer_.raw_mouse_x_ = eff.mouse_x;
    pointer_.raw_mouse_y_ = eff.mouse_y;
    if (pointer_.pointer_warp_pending_) {
        if (raw_static && !eff.left_click_edge && !eff.right_click_edge &&
            pointer_.drag_layer_.empty()) {
            eff.mouse_x = pointer_.pointer_warp_x_;
            eff.mouse_y = pointer_.pointer_warp_y_;
        } else {
            pointer_.pointer_warp_pending_ = false;
        }
    }
    input_state_.mouse_x_ = eff.mouse_x;
    input_state_.mouse_y_ = eff.mouse_y;

    // update physical state
    input_state_.down_edges_.clear();
    input_state_.up_edges_.clear();
    input_state_.keys_down_ = in.keys_down;
    for (const int k : in.key_down_edges) input_state_.down_edges_.insert(k);
    for (const int k : in.key_up_edges) input_state_.up_edges_.insert(k);
    if (in.left_click_edge) input_state_.down_edges_.insert(kKeyMouseLeft);
    if (!in.left_down) input_state_.up_edges_.insert(kKeyMouseLeft);
    // decide edge: override bit 32 freshly set this frame (scripted decide).
    // The snapshot is taken BEFORE the vsync hook so overrides injected by
    // Lua's onEnterFrame (the FPM dummy-click pattern: setexclick ->
    // e:overrideKey status=32) produce their decide edge in the SAME advance
    // (Lua runs inside the host frame before wait resolution).
    input_state_.prev_overrides_ = input_state_.overrides_;

    // Lua per-frame hook first (it rewrites keys/decide edges, clears flags)
    interpreter_->fire_event("onEnterFrame");
    // decide 边沿 = 相对上一帧快照新出现的位 32（帧内/测试直接注入均
    // 生效）。配合每帧末清空覆盖（见 tick 尾部注释），FPM exclick 的
    // status=0+32 同帧注入在下一帧构成新边沿；滞留位不会再吞掉后续 decide。
    input_state_.decide_edge_ = false;
    for (const auto& [key, bits] : input_state_.overrides_) {
        const int prev = input_state_.prev_overrides_.count(key) ? input_state_.prev_overrides_.at(key) : 0;
        if ((bits & 32) && !(prev & 32)) input_state_.decide_edge_ = true;
    }

    // Control-skip hold (role-14 keys) + effective-skip recompute —
    // the wait machine consults control_.skip_active_.
    update_control_skip_hold(input_state_.keys_down_);
    control_.skip_active_ = skip_active();

    // Pointer chain runs before wait resolution; claimed clicks are swallowed.
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
        clicked = input_state_.down_edges_.count(kKeyEnter) > 0 || input_state_.down_edges_.count(kKeySpace) > 0;
    }
    clicked = clicked || pc.role_advance;
    // NB: only fresh PRESSES count — the host injects a mouse-left up-edge on
    // every frame the button is not held, which would disable the guard always.
    input_this_tick_ = clicked || input_state_.decide_edge_ || !input_state_.down_edges_.empty();

    // script status 4 = forced stop (no user input): park the story line but
    // keep onEnterFrame ticking (already fired) and scene tweens advancing.
    if (control_.script_status_ != 4) {
        // Exskip: burn crossing parks in one bounded burst.
        const int kMaxExskipBurst = 4096;
        int burst = 0;
        for (;;) {
            const bool was_ff = control_.exskip_active_;
            bool attempted_resolve = false;
            if (wait_) {
                // Queue-armed click parks hold the rest of the queue.
                const bool queue_click_park =
                    interpreter_->last_wait_from_queue() &&
                    (wait_->kind == oa::runtime::WaitReason::Kind::Generic ||
                     wait_->kind == oa::runtime::WaitReason::Kind::Generic0);
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
            if (!was_ff || !control_.exskip_active_) break;
            if (++burst >= kMaxExskipBurst) break;
            if (attempted_resolve && wait_) break;
        }
    }

    // Overrides are frame-local: a sticky decide bit 32 would swallow every
    // later click edge.
    input_state_.overrides_.clear();
    // Host-stream drain (save then control) runs before the content advance.
    dispatch_pending_events();
    maybe_autosave_for_wait();

    // Content-plane order is phase-critical (R6): text reveal → scene tweens
    // → emote clock → click-wait icon → media frame end. Do not reorder.
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
        // A real wait wins over a [lytween sync=1] seen earlier in this run.
        pending_sync_tween_layer_.reset();
        if (ev && ev->kind == oa::runtime::Event::Kind::Wait_) {
            wait_ = ev->reason;
            wait_armed_this_tick_ = true;
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
    } else if (r == oa::runtime::ExecutionResult::BudgetExhausted) {
        // Polling VM: resume next frame; do not clear the sync latch or arm a wait.
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
    // Clock-driven parks (Timed / [trans] / fullscreen [video] / [alldelete])
    // hold the pre-enqueued batch: a queued park would replace the running
    // park and lose its remaining time (test2: the 2500 ms logo wait).
    // Interactive parks ([@]/[stop]/[exkey]) keep draining for title input.
    bool hold_drain = false;
    if (wait_) {
        // A fullscreen [video] park owns the click (the click is the skip
        // input), so the queue holds until the skip/EOF finish.
        // Draining first let queued [wait]/[trans] replace the movie park.
        const bool video_park =
            wait_->kind == oa::runtime::WaitReason::Kind::Stop && wait_->id == "video";
        if (video_park) {
            const auto v = media_.video_.state();
            hold_drain = v.overlay_video && v.overlay_video->playing;
        } else if (!input_this_tick_) {
            if (wait_->kind == oa::runtime::WaitReason::Kind::Timed) {
                // Long pacing waits (the splash logo's 2500 ms) must not be
                // replaced by the pre-enqueued rows; short ones (0..300 ms) are
                // the framework's own animation steps — holding those would turn a
                // UI entrance into stop-motion.
                hold_drain = wait_remaining_ms_ > 300;
            } else if (wait_->kind == oa::runtime::WaitReason::Kind::Stop) {
                // Only while the park is genuinely running on the clock. An
                // armed-but-dead park (transition the host never captured)
                // keeps the old drain-through behaviour so the script can move
                // on.
                if (wait_->id == "trans") {
                    hold_drain = transition().is_in_progress(now_ms_);
                } else if (wait_->id == "alldelete") {
                    hold_drain = renderer_.all_delete_active();
                }
            }
        }
    }
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
        if (hold_drain) break; // keep the batch for after the clock-driven park
        const std::string* script_before = interpreter_->current_script();
        const size_t line_before = interpreter_->current_line();
        const size_t stack_before = interpreter_->call_stack().size();
        const oa::runtime::ExecutionResult r = interpreter_->run_queued();
        const bool moved = script_before != interpreter_->current_script() ||
                           line_before != interpreter_->current_line() ||
                           stack_before != interpreter_->call_stack().size();
        if (r == oa::runtime::ExecutionResult::Wait) {
            paused = true;
            const oa::runtime::Event* ev = interpreter_->last_wait_event();
            if (ev && ev->kind == oa::runtime::Event::Kind::Wait_) {
                wait_ = ev->reason;
                wait_armed_this_tick_ = true;
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

void GameRuntime::RuntimeState::dispatch_pending_events() {
    // Fixed frame-level order: a flow's own [syssave] must land before the
    // [reset] that reboots it.
    dispatch_save_events();
    dispatch_control_events();
}

void GameRuntime::RuntimeState::reboot_boot_keep_vm(const char* reason) {
    // [reset]/[gotitle]: clear every domain and re-run the boot head with the
    // Lua VM kept. No label is hardcoded — the data's own boot chain decides
    // where to go (FPM keeps `systemreset` in the VM for that decision).
    std::fprintf(stderr, "[runtime] %s (VM kept; boot re-run)\n", reason);
    reset_domains();
    pending_events_.clear();
    try {
        interpreter_->boot(rt_->project_.config.boot_script);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[runtime] %s boot re-run failed (%s); "
                             "full restart fallback\n", reason, ex.what());
        restart_project();
    }
}

void GameRuntime::RuntimeState::dispatch_control_events() {
    if (pending_events_.empty()) return;
    std::deque<oa::runtime::Event> keep;
    for (const auto& e : pending_events_) {
        if (e.kind == oa::runtime::Event::Kind::Exit) {
            // Host quit request; hosts only poll exit_requested.
            std::fprintf(stderr, "[runtime] exit requested\n");
            exit_requested_ = true;
            control_.exskip_active_ = false; // quit paths never leave it set
            continue;
        }
        // [reset] is the FPM go-title flow; raw [gotitle] only fires for data
        // without the Lua tag handler. Both use the same VM-kept reboot.
        if (e.kind == oa::runtime::Event::Kind::Reset) {
            reboot_boot_keep_vm("engine reset");
            return;
        }
        if (e.kind == oa::runtime::Event::Kind::GoTitle) {
            reboot_boot_keep_vm("gotitle");
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
    const std::string active = text().active_layer_id();
    if (active.empty()) return;
    const oa::render::MessageLayer* al = text().layer(active);
    if (!al) return;
    const bool is_layered = al->layered;
    // 槽位显式化 —— layered=1 走宿主树节点;layered=0 走引擎消息槽
    // (创建序,id 不随消息 id 变)。create/ensure 语义收在 Compositor 内。
    const std::string scene_id = scene().ensure_message_scene_node(active, is_layered);
    scene().set_message_layer_binding(active, scene_id);
    scene().revive_message_layer(active);
}

bool GameRuntime::RuntimeState::apply_text_event(const oa::runtime::Event& e) {
    using K = oa::runtime::Event::Kind;
    auto par = [&e](const char* k) -> std::string {
        const auto it = e.params.find(k);
        return it == e.params.end() ? std::string() : it->second;
    };
    switch (e.kind) {
        case K::ScenarioLine: {
            renderer_.note(RNote::Text);
            text().push_text(e.content);
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
            text().line_break(omit);
            return true;
        }
        case K::PageBreak: {
            std::optional<int> bl;
            const std::string bp = par("backlog");
            if (!bp.empty()) bl = parse_i32(bp, 0);
            text().page_break(bl);
            return true;
        }
        case K::MessageLayerSwitch: {
            renderer_.note(RNote::MessageSwitch);
            // 参数 id/stack/layered（id 缺省 → 引擎生成匿名 id）
            const std::string id = par("id");
            std::optional<std::string> mid;
            if (!id.empty()) mid = id;
            const std::string st = par("stack");
            const bool stack = st.empty() || parse_i32(st, 1) != 0;
            std::optional<int> layered;
            const std::string ly = par("layered");
            if (!ly.empty()) layered = parse_i32(ly, -1);
            text().switch_layer(mid, stack, layered);
            // 绑定：活动消息层 id → 场景目标 + 默认消息层
            const std::string active = text().active_layer_id();
            const oa::render::MessageLayer* al = text().layer(active);
            const bool is_layered = al && al->layered;
            // 槽位显式化 —— layered=0 独立消息分配引擎消息槽(创建序),
            // layered=1 走宿主树节点(ensure_path 物化,父链位姿语义不变:
            // glyph 落点=ml.left/top+排版偏移,经节点世界变换合成)。
            const std::string scene_id = scene().ensure_message_scene_node(active, is_layered);
            scene().set_message_layer_binding(active, scene_id);
            scene().revive_message_layer(active);
            scene().set_default_message_layer(active);
            // Publish s.current_message_layer for framework guards.
            if (interpreter_)
                interpreter_->set_variable("s.current_message_layer",
                                           oa::runtime::Value::make_string(active));
            return true;
        }
        case K::MessageLayerPop: {
            text().pop_layer();
            const std::string active = text().active_layer_id();
            const oa::render::MessageLayer* al = text().layer(active);
            if (al) {
                const bool is_layered = al->layered;
                const std::string scene_id =
                    scene().ensure_message_scene_node(active, is_layered);
                scene().set_message_layer_binding(active, scene_id);
                scene().revive_message_layer(active);
            }
            scene().set_default_message_layer(active);
            if (interpreter_)
                interpreter_->set_variable("s.current_message_layer",
                                           oa::runtime::Value::make_string(active));
            return true;
        }
        case K::TextConfig: {
            // font 族 / ruby / scein-sceout / 排版配置标签（ +
            // tags/ 文本面）
            if (e.tag == "font") {
                text().apply_font(e.params);
                return true;
            }
            if (e.tag == "font_close" || e.tag == "/font") {
                text().font_close();
                return true;
            }
            if (e.tag == "fontinit") {
                text().font_init();
                return true;
            }
            if (e.tag == "fontdefault") {
                text().font_default(e.params);
                return true;
            }
            if (e.tag == "ruby") {
                text().ruby_start(par("text"));
                return true;
            }
            if (e.tag == "/ruby") {
                text().ruby_end();
                return true;
            }
            if (e.tag == "scein") {
                text().set_text_hidden(false);
                return true;
            }
            if (e.tag == "sceout") {
                text().set_text_hidden(true);
                return true;
            }
            if (e.tag == "prohibit") {
                text().set_prohibit(par("head"), par("foot"));
                return true;
            }
            if (e.tag == "wordparts") {
                text().set_wordparts(par("parts"));
                return true;
            }
            if (e.tag == "indent") {
                text().set_indent(par("pair"));
                return true;
            }
            if (e.tag == "scetween") {
                text().apply_scetween(e.params);
                return true;
            }
            if (e.tag == "glyph") {
                text().set_glyph_config(e.params);
                return true;
            }
            if (e.tag == "backlog") {
                text().apply_backlog_config(e.params, par("clear") == "1");
                return true;
            }
            if (e.tag == "writebacklog") {
                const std::string m = par("mode");
                text().set_backlog_write_mode(!m.empty() && parse_i32(m, 0) != 0);
                return true;
            }
            // rt2/tximg/txkey/txnc/link… 仍 no-op（后续域/队列）
            return false; // 交给宿主（诊断/后续域）
        }
        default:
            return false;
    }
}

void GameRuntime::RuntimeState::apply_lyc2_csv(std::map<std::string, std::string>* props) {
    const auto fit = props->find("file");
    if (fit == props->end() || fit->second.empty()) return;
    // An explicit geometry on the tag wins (the framework sometimes overrides
    // a csv-placed layer inline).
    if (props->count("left") || props->count("top") || props->count("width") ||
        props->count("height"))
        return;
    if (!interpreter_ || !interpreter_->hooks().resource_read) return;
    const std::string csv = fit->second + ".csv";
    std::optional<std::vector<uint8_t>> bytes;
    if (auto b = interpreter_->hooks().resource_read(interpreter_->resolve_magic_path(csv)))
        bytes = std::move(b);
    else if (csv.rfind("_data/", 0) != 0 && csv[0] != ':')
        bytes = interpreter_->hooks().resource_read("_data/" + csv);
    if (!bytes) return;
    const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    // Two shapes in the wild: positional "left,top" (the mw plates) and
    // key=value rows (`left = 290`). Both are merged as layer props; comment
    // rows and blanks are skipped.
    std::vector<std::string> nums;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find_first_of("\r\n", pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty() || (line.size() > 1 && line[0] == '/' && line[1] == '/')) continue;
        if (const size_t eq = line.find('='); eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
            if (const size_t sp = val.find_first_of(" \t"); sp != std::string::npos)
                val.resize(sp); // drop a trailing // comment
            if (!key.empty() && !val.empty()) (*props)[key] = val;
            continue;
        }
        size_t p = 0;
        while (p <= line.size()) {
            size_t c = line.find(',', p);
            if (c == std::string::npos) c = line.size();
            std::string tok = line.substr(p, c - p);
            while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(0, 1);
            while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
            if (!tok.empty()) nums.push_back(tok);
            if (c >= line.size()) break;
            p = c + 1;
        }
    }
    static const char* kOrder[] = {"left", "top", "width", "height", "xscale",
                                   "yscale", "alpha", "rotate"};
    for (size_t i = 0; i < nums.size() && i < sizeof(kOrder) / sizeof(kOrder[0]); ++i)
        if (!props->count(kOrder[i])) (*props)[kOrder[i]] = nums[i];
}

bool GameRuntime::RuntimeState::apply_scene_event(const oa::runtime::Event& e) {
    using K = oa::runtime::Event::Kind;
    if (e.kind == K::LayerCreate) {
        std::map<std::string, std::string> props = e.params;
        if (e.tag == "lyc2") apply_lyc2_csv(&props);
        scene().create(e.id, props);
        renderer_.note(RNote::Layer);
        return true;
    }
    if (e.kind == K::LayerDelete) {
        scene().remove(e.id);
        renderer_.note(RNote::Layer);
        // a deleted layer (or subtree) releases its emote
        // player (fgdel/lydel2 flows) — players whose id equals the deleted
        // id or sits below it are dropped (textures die with the scene too).
        for (auto it = media_.emote_layers_.begin(); it != media_.emote_layers_.end();) {
            const std::string& id = it->first;
            if (id == e.id || (id.size() > e.id.size() + 1 &&
                               id.compare(0, e.id.size(), e.id) == 0 &&
                               id[e.id.size()] == '.')) {
                if (it->second.player) {
                    media_.emote_view_cache_[id] = it->second.player->view_state();
                    // Park the player for the immediate same-beat rebuild.
                    if (media_.parked_emote_.size() >= 8) media_.parked_emote_.erase(media_.parked_emote_.begin());
                    // Key by PSB file: scene-id rebuilds keep the expression.
                    media_.parked_emote_[it->second.file] = std::move(it->second);
                }
                it = media_.emote_layers_.erase(it);
            } else {
                ++it;
            }
        }
        // A deleted subtree releases its layer-video channels.
        media_.video_.stop_layer_subtree(e.id);
        return true;
    }
    if (e.kind == K::LayerSetProps) {
        // routed: id == "!" hits the root props, never creates a "!" node.
        if (e.id == "!") {
            scene().set_root_props(e.params);
        } else {
            scene().set_props(e.id, e.params);
        }
        renderer_.note(RNote::Layer);
        return true;
    }
    if (e.kind == K::LayerEventCmd) {
        // Animation tags live on the Compositor and are
        // part of the compositor boundary (compositor/TweenSet*).
        if (e.tag == "lyevent") {
            // layer-event handler registrations are applied to the scene
            // registry at dispatch time (FPM type-less handler
            // expansion). Host stream never sees them.
            scene().apply_lyevent(e.id, e.params);
            renderer_.note(RNote::Layer);
            return true;
        }
        if (e.tag == "lytween") {
            scene().apply_lytween(e.params, now_ms_);
            renderer_.note(RNote::Tween);
            // [lytween sync=1]: first target of a run arms a tween park.
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
            // Defer cancelled completions to the frame-end flush.
            std::vector<oa::render::TweenDone> done = scene().apply_lytweendel(e.id);
            if (!done.empty()) {
                PendingTweenCancel pc;
                pc.id = e.id;
                pc.node = scene().find(e.id);
                pc.done = std::move(done);
                pending_tween_cancels_.push_back(std::move(pc));
            }
            renderer_.note(RNote::Tween);
            return true;
        }
        if (e.tag == "anime") {
            // [anime] frame animation: init/add/end reduced to the
            // AnimeState on the Compositor.
            scene().apply_anime(e.params, now_ms_);
            renderer_.note(RNote::Tween);
            return true;
        }
        if (e.tag == "tweenset") {
            scene().tweenset_start();
            renderer_.note(RNote::Tween);
            return true;
        }
        if (e.tag == "/tweenset") {
            scene().tweenset_end(now_ms_);
            renderer_.note(RNote::Tween);
            return true;
        }
        return false;
    }
    return false;
}

// Control/config domain: legacy rows → input registry → config → [lydrag].
bool GameRuntime::RuntimeState::apply_control_event(const oa::runtime::Event& e) {
    using K = oa::runtime::Event::Kind;
    if (e.kind == K::ConfigEvent) {
        static const std::map<std::string, std::string> kLayerSet = {
            {"setonclick", "click"},       {"setondrag", "drag"},
            {"setondragin", "dragin"},     {"setondragout", "dragout"},
            {"setonrollover", "rollover"}, {"setonrollout", "rollout"}};
        static const std::map<std::string, std::string> kLayerDel = {
            {"delonclick", "click"},       {"delondrag", "drag"},
            {"delondragin", "dragin"},     {"delondragout", "dragout"},
            {"delonrollover", "rollover"}, {"delonrollout", "rollout"}};
        if (const auto it = kLayerSet.find(e.tag); it != kLayerSet.end()) {
            scene().apply_legacy_layer_event(e.id, it->second, false, e.params);
            renderer_.note(RNote::Layer);
            return true;
        }
        if (const auto it = kLayerDel.find(e.tag); it != kLayerDel.end()) {
            scene().apply_legacy_layer_event(e.id, it->second, true, e.params);
            renderer_.note(RNote::Layer);
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
            scene().set_input_handler(it->second, raw);
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
            scene().del_input_handler(it->second, key);
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
    // Enqueue tween completion handlers as engine tags (handler + jump).
    for (const oa::render::TweenDone& d : done) {
        if (!d.handler.empty() || !d.handler_file.empty() || !d.handler_label.empty()) {
            renderer_.note(RNote::TweenComplete);
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
    // Deferred [lytweendel] completions: deliver only while the layer
    // survived this frame (same-frame cancels deliver; rebuilt/deleted
    // layers end the round silently).
    if (pending_tween_cancels_.empty()) return;
    std::vector<oa::render::TweenDone> out;
    for (auto& pc : pending_tween_cancels_) {
        const oa::render::Layer* now = scene().find(pc.id);
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

// Runtime-side frame advance (host upload pumps / group bakes stay outside).
// R6 order: text reveal → scene tweens/anime → emote clock → click-wait icon
// → media frame end. Reordering changes animation phase.
void GameRuntime::RuntimeState::advance_content_planes(uint64_t delta_ms) {
    text().reveal_next(delta_ms);
    advance_scene_frame();
    advance_emote_players(delta_ms); // host_clock layers only sync revisions
    advance_click_wait();
    advance_media_frame(delta_ms);
}

void GameRuntime::RuntimeState::advance_scene_frame() {
    // Deliver staged completions only while the tween's layer survived the
    // frames in between; then stage this frame's natural finishes for the
    // next flush (a config page rebuild must not reprint the old preview).
    flush_pending_tween_cancels();
    std::vector<oa::render::TweenDone> done = scene().advance_tweens(now_ms_);
    if (!done.empty()) {
        for (oa::render::TweenDone& d : done) {
            PendingTweenCancel pc;
            pc.id = d.id;
            pc.node = scene().find(d.id);
            pc.done.push_back(std::move(d));
            pending_tween_cancels_.push_back(std::move(pc));
        }
    }
    // [lytween sync=1] release: clears the wait without advancing the line
    // (the script never parked on the instruction itself).
    if (wait_ && wait_->kind == oa::runtime::WaitReason::Kind::Stop &&
        wait_->id.rfind("tween:", 0) == 0) {
        const std::string layer = wait_->id.substr(6);
        if (scene().layer_tweens_finished(layer)) {
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
    transition().start(type, time, std::move(rule), vague, input, now_ms_);
    renderer_.request_repaint();
    if (transition().needs_capture()) {
        if (transition_capture_cb_) {
            transition_capture_cb_(); // test host snapshots the old frame
        } else {
            // Renderer owns the transition capture; without a backend the
            // capture is treated as complete (headless duration still runs).
            renderer_.capture_transition_source();
            transition().mark_captured(now_ms_);
        }
    }
}

bool GameRuntime::RuntimeState::waiting_stop() const {
    return wait_ && wait_->kind == oa::runtime::WaitReason::Kind::Stop;
}

// Debug-skip fast-forward: non-Stop waits release instantly; the
// [stop 0="exskip"] row ends the fast-forward. Details: docs/RUNTIME_NOTES.md.
void GameRuntime::RuntimeState::start_debug_skip() {
    if (control_.exskip_active_) return;
    control_.exskip_active_ = true;
}

void GameRuntime::RuntimeState::end_debug_skip() {
    if (!control_.exskip_active_) return;
    control_.exskip_active_ = false;
    reveal_all_text(); // no half-revealed page left behind at the boundary
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
    // A wait armed this tick resolves next tick (Artemis yields ≥1 frame).
    const bool armed_now = wait_armed_this_tick_;
    wait_armed_this_tick_ = false;
    // Exskip releases every non-Stop wait instantly.
    if (control_.exskip_active_ && wait_->kind != oa::runtime::WaitReason::Kind::Stop) {
        return true;
    }
    switch (wait_->kind) {
        case oa::runtime::WaitReason::Kind::Timed: {
            if (armed_now) return false;
            const bool clickable = wait_->input == 1;
            // input=2: only skip mode passes early; decide edge counts as click.
            if ((clickable && (clicked || input_state_.decide_edge_)) || control_.skip_active_) {
                return true;
            }
            if (wait_remaining_ms_ <= delta_ms) {
                return true;
            }
            wait_remaining_ms_ -= delta_ms;
            return false;
        }
        case oa::runtime::WaitReason::Kind::Stop: {
            // [stop]: physical clicks do NOT release; only scripted decide
            // edges (overrideKey status=32) or media/transition completions.
            // A fullscreen-video completion resumes ANY Stop-class park.
            if (media_.video_finished_) {
                media_.video_finished_ = false;
                return true;
            }
            // [stop 0="exskip"]: pass-through; ends the fast-forward if active.
            if (wait_->id == "exskip") {
                if (control_.exskip_active_) end_debug_skip();
                return true;
            }
            // [alldelete]: the script resumes once the whole-scene
            // fade-out + clear finished (finish_all_delete runs inside tick
            // when the fade clock expires).
            if (wait_->id == "alldelete") return !renderer_.all_delete_active();
            // Skippable fullscreen video: click/right-click queues EOF.
            if (wait_->id == "video") {
                const auto vsnap = media_.video_.state();
                const auto& fsv = vsnap.overlay_video;
                const bool phys_skip =
                    input_state_.down_edges_.count(kKeyMouseLeft) ||
                    input_state_.down_edges_.count(kKeyMouseRight);
                if (fsv && fsv->skippable && fsv->playing &&
                    (phys_skip || clicked)) {
                    media_.video_.queue_finish(std::string());
                    return false;
                }
                // While the overlay is still playing, never release the park.
                if (fsv && fsv->playing) return false;
            }
            // [trans]: input rules may skip; release when the transition ends.
            if (wait_->id == "trans") {
                if (clicked || control_.skip_active_) transition().skip_by_input(control_.skip_active_);
                if (input_state_.decide_edge_) return true;
                return !transition().is_in_progress(now_ms_);
            }
            if (input_state_.decide_edge_) return true;
            return false;
        }
        case oa::runtime::WaitReason::Kind::Generic:
        case oa::runtime::WaitReason::Kind::Generic0: {
            // decide edges advance input waits, except plain [wt].
            const bool input_advance =
                clicked || (input_state_.decide_edge_ && wait_->id != "wt");
            // 点击等待且文本仍在逐字揭示 → 先整页揭示，不推进
            // （reveal_text_now 并 return false）
            if (input_advance && !text().is_reveal_complete()) {
                text().reveal_all();
                return false;
            }
            if (input_advance) {
                return true;
            }
            // skip (command/control) releases generic waits after a full reveal.
            if (control_.skip_active_) {
                if (!text().is_reveal_complete()) {
                    text().reveal_all();
                    return false;
                }
                return true;
            }
            if (control_.automode_) {
                // s.automodewait pacing; a live syncse voice holds the flip.
                control_.auto_elapsed_ms_ += delta_ms;
                if (control_.auto_elapsed_ms_ < automode_wait_ms()) return false;
                if (automode_sync_blocked()) return false;
                control_.auto_elapsed_ms_ = 0;
                return true;
            } else {
                control_.auto_elapsed_ms_ = 0;
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
                if (input_state_.down_edges_.count(code)) return true;
            }
            if (clicked || input_state_.decide_edge_) return true;
            return false;
        }
        case oa::runtime::WaitReason::Kind::Se: {
            // [wait se=ID (time=N)]: releases on skip or when the tracked
            // sound ended.
            if (control_.skip_active_) return true;
            return media_se_wait_finished(wait_->id, wait_->time_given, wait_->milliseconds);
        }
        case oa::runtime::WaitReason::Kind::VideoLayer: {
            // [wait video=layerID]: releases on skip or when the layer video
            // stopped.
            if (control_.skip_active_) return true;
            // explicit video waits on a skippable channel also
            // honor physical input (left click / right button) — queue the
            // layer finish, released through the same stop path next tick.
            const bool phys_skip =
                input_state_.down_edges_.count(kKeyMouseLeft) ||
                input_state_.down_edges_.count(kKeyMouseRight);
            if (clicked || phys_skip) {
                const auto vsnap = media_.video_.state();
                const auto vit = vsnap.video_layers.find(wait_->id);
                if (vit != vsnap.video_layers.end() && vit->second.skippable &&
                    vit->second.playing) {
                    media_.video_.queue_finish(wait_->id);
                    return false;
                }
            }
            return !media_.video_.is_layer_playing(wait_->id);
        }
        case oa::runtime::WaitReason::Kind::ScenarioTween: {
            // [wait scenario=1|2]: mode 2 (hide) is instantaneous; mode 1
            // (show) waits for the per-glyph reveal.
            if (control_.skip_active_) return true;
            if (wait_->mode != 1) return true;
            return text().is_reveal_complete();
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
    wait_.reset();
    wait_remaining_ms_ = 0;
    if (page_wait_release) text().anchor_active_to_content();
    // If a Stop wait resolves while Lua queued a jump, the position change
    // happens inside the next run; next_line only moves past inline wait
    // instructions (queue-sourced waits no-op inside the interpreter).
    interpreter_->next_line();
}

// [glyph] click-wait icon: show/move while a click wait is fully revealed.
void GameRuntime::RuntimeState::advance_click_wait() {
    const bool click_wait =
        wait_ && (wait_->kind == oa::runtime::WaitReason::Kind::Generic ||
                  wait_->kind == oa::runtime::WaitReason::Kind::Generic0);
    const bool show = click_wait && text().is_reveal_complete();
    if (!show) {
        if (!control_.active_wait_icon_.empty()) {
            // find guard: set_props would materialize a missing icon layer.
            if (scene().find(control_.active_wait_icon_)) {
                scene().set_props(control_.active_wait_icon_, {{"visible", "0"}});
            }
            control_.active_wait_icon_.clear();
        }
        return;
    }
    const auto placement = text().click_wait_placement(false);
    if (!placement) return;
    const bool homing = text().glyph_icon().homing;
    const oa::render::Layer* l = scene().find(placement->layer_id);
    const bool already = control_.active_wait_icon_ == placement->layer_id && l && l->visible &&
                         (!homing ||
                          (l->left == placement->left && l->top == placement->top));
    if (already) return;
    if (!control_.active_wait_icon_.empty() && control_.active_wait_icon_ != placement->layer_id &&
        scene().find(control_.active_wait_icon_)) {
        scene().set_props(control_.active_wait_icon_, {{"visible", "0"}});
    }
    std::map<std::string, std::string> p{{"visible", "1"}};
    if (homing) {
        p["left"] = format_prop_num(placement->left);
        p["top"] = format_prop_num(placement->top);
    }
    scene().set_props(placement->layer_id, p);
    control_.active_wait_icon_ = placement->layer_id;
}

// Pointer chain: hit test → hover diff → click/drag/push (docs/RUNTIME_NOTES.md).

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
    const bool left_up_edge = pointer_.left_down_prev_ && !in.left_down;
    pointer_.left_down_prev_ = in.left_down;
    const bool pointer_moved =
        !pointer_.mouse_inited_ || in.mouse_x != pointer_.mouse_prev_x_ || in.mouse_y != pointer_.mouse_prev_y_;
    pointer_.mouse_inited_ = true;
    pointer_.mouse_prev_x_ = in.mouse_x;
    pointer_.mouse_prev_y_ = in.mouse_y;

    // hide mode: a left click first recovers the message window and is
    // swallowed for the rest of the frame (hide click recovery, no
    // click/advance chain).
    if (control_.hide_active_ && left_down_edge) {
        exit_hide_mode();
        pc.input_swallowed = true;
        return pc;
    }

    // --- hit test (geometry + clickablethreshold alpha) --------------------
    const auto hits = scene().hit_test_all(mx, my, pointer_.hit_size_, pointer_.compositor_userdata_,pointer_.alpha_sampler_);
    // layers that can receive events at all = hit-test interaction gate
    // (applied here at dispatch)
    std::vector<std::string> interactive;
    for (const std::string& id : hits)
        if (scene().has_any_enabled_handler(id)) interactive.push_back(id);

    // event_dispatch_layers: top + penetration, reversed.
    auto dispatch_list = [&](const std::vector<std::string>& base,
                             const std::string& event_type) -> std::vector<std::string> {
        std::vector<std::string> out;
        for (size_t i = 0; i < base.size(); ++i) {
            const auto* h = scene().find_event_handler(base[i], event_type);
            if (h && h->enabled && (i == 0 || h->penetration)) out.push_back(base[i]);
        }
        std::reverse(out.begin(), out.end());
        return out;
    };

    // Hover is rect-based; the diff runs only on pointer motion.
    // Details: docs/RUNTIME_NOTES.md.
    std::set<std::string> prev_hovered = pointer_.hovered_;
    if (pointer_moved) {
    std::set<std::string> hit_ids(hits.begin(), hits.end());
    const auto hover_list = dispatch_list(interactive, "rollover");
    std::set<std::string> new_hovered(hover_list.begin(), hover_list.end());
    std::set<std::string> next = pointer_.hovered_;
    // (a) rollouts: hovered layers that left the pointer rect / disappeared
    std::vector<std::string> old_only;
    for (const std::string& id : pointer_.hovered_) {
        if (new_hovered.count(id)) continue;
        const oa::render::Layer* l = scene().find(id);
        const bool still_visible =
            l && l->visible && scene().is_effectively_visible(id);
        if (hit_ids.count(id) && still_visible) continue; // retained
        old_only.push_back(id);
        next.erase(id);
    }
    std::sort(old_only.begin(), old_only.end());
    for (const std::string& old : old_only) {
        const auto* row = scene().find_event_handler(old, "rollout");
        if (row) dispatch_layer_row(old, *row, "rollout", {},
                                    PointerDispatch::Kind::HoverOut);
    }
    // (b) overs: topmost-first layers not hovered yet
    for (const std::string& id : hover_list) { // bottom -> top
        if (pointer_.hovered_.count(id)) continue;
        const auto* row = scene().find_event_handler(id, "rollover");
        if (!row) continue;
        dispatch_layer_row(id, *row, "rollover", {}, PointerDispatch::Kind::HoverIn);
        next.insert(id);
    }
    prev_hovered = pointer_.hovered_;
    pointer_.hovered_ = std::move(next);
    } else {
        // still pointer: no script events; silently forget vanished layers
        // (their rollout fires on a later move if the script still cares).
        bool changed = false;
        for (auto it = pointer_.hovered_.begin(); it != pointer_.hovered_.end();) {
            if (!scene().find(*it)) {
                it = pointer_.hovered_.erase(it);
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
        if (!top_hover.empty() && scene().is_layer_draggable(top_hover) &&
            key_has_drag_handler(scene(), top_hover)) {
            pc.drag_handled = drag_begin(top_hover, false);
        }
        if (!pc.drag_handled) {
            for (const std::string& id : dispatch_list(interactive, "click")) {
                const auto* row = scene().find_event_handler(id, "click");
                if (!row) continue;
                pc.layer_handled |= dispatch_layer_row(
                    id, *row, "click", {{"click", "1"}}, PointerDispatch::Kind::Click);
            }
        }
    }
    if (in.left_down && pointer_moved && !pointer_.drag_layer_.empty()) {
        drag_update(mx, my);
        pc.drag_handled = true;
    }
    if (left_up_edge) {
        if (!pointer_.drag_layer_.empty()) {
            drag_end();
            pc.drag_handled = true;
        }
    }

    // --- global push handlers for every key-down edge  ----
    std::vector<int> key_edges(input_state_.down_edges_.begin(), input_state_.down_edges_.end());
    std::sort(key_edges.begin(), key_edges.end());
    for (const int key : key_edges) {
        bool cancel_park = false;
        // right button / ESC first walk the engine rclick chain (hide
        // recovery or the configured rclick script); only when unconsumed does
        // the same key reach the push row.
        if (key == kKeyMouseRight || key == 27) {
            if (trigger_rclick()) continue;
            // Cancel key satisfies a queue-armed [exkey] click park.
            cancel_park = wait_ &&
                          (wait_->kind == oa::runtime::WaitReason::Kind::Generic ||
                           wait_->kind == oa::runtime::WaitReason::Kind::Generic0) &&
                          interpreter_ && interpreter_->last_wait_from_queue();
        }
        // keyconfig role edges (advance/hide/backlog/automode/skip).
        // Role-0 edges count as a
        // click substitute (pc.role_advance feeds the clicked formula).
        if (handle_role_key_edge(key)) pc.role_advance = true;
        const std::string key_string = std::to_string(key);
        const auto* row = scene().get_input_handler("push", key_string);
        if (!row) {
            // No push row (project registers no key handler for this key):
            // the cancel press still has to release the exkey park.
            if (cancel_park) pc.role_advance = true;
            continue;
        }
        const std::string event_type = is_mouse_button_key(key) ? "click" : "key";
        const bool handled = dispatch_input_row(
            "setonpush", *row,
            {{"key", key_string}, {"type", event_type}}, PointerDispatch::Kind::Push);
        if (key == kKeyMouseLeft && handled) pc.left_push_handled = true;
        // Cancel press also runs the push row so flg.keycode is recorded.
        if (cancel_park) pc.role_advance = true;
    }
    return pc;
}

void GameRuntime::RuntimeState::notify_pointer(const PointerDispatch& d) {
    ++pointer_.pointer_dispatch_count_;
    renderer_.request_repaint();
    if (pointer_.pointer_observer_) pointer_.pointer_observer_(d);
}

bool GameRuntime::RuntimeState::dispatch_layer_row(
    const std::string& id, oa::render::LayerEventHandler row,
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
    // handler="var": apply the row's [var name data] assignment (iMel buttons).
    if (row.handler == "var") {
        const auto name_it = params.find("name");
        if (name_it != params.end() && !name_it->second.empty()) {
            std::map<std::string, std::string> var_params;
            var_params["name"] = name_it->second;
            const auto data_it = params.find("data");
            var_params["data"] = data_it == params.end() ? std::string() : data_it->second;
            interpreter_->apply_var(var_params);
            PointerDispatch d;
            d.kind = kind;
            d.layer = id;
            d.function = "var:" + name_it->second;
            notify_pointer(d);
        }
    }
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
    // Queue navigation after the handler; call pushes a return frame.
    if (!row.file.empty() || !row.label.empty()) {
        // Protect the caller chain for this click's tag drain (see
        // Interpreter::set_return_floor).
        interpreter_->set_return_floor(interpreter_->call_stack().size());
        std::map<std::string, std::string> nav;
        if (!row.file.empty()) nav["file"] = row.file;
        if (!row.label.empty()) nav["label"] = row.label;
        interpreter_->enqueue_tag(row.call ? "call" : "jump", std::move(nav));
        handled = true;
    }
    // Arm the inline marker only when THIS row enqueued tags.
    if (fn_it != params.end() && !fn_it->second.empty() && row.file.empty() &&
        row.label.empty() && fn_queued) {
        begin_inline_event_frame();
    }
    return handled;
}

bool GameRuntime::RuntimeState::dispatch_input_row(
    const std::string& filter_name, oa::render::InputHandler row,
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
        inline_event_frame_.reset();
    }
}

void GameRuntime::RuntimeState::begin_inline_event_frame() {
    if (!interpreter_) return;
    refresh_inline_event_frame();
    // Single marker: never arm on top of a live one.
    if (inline_event_frame_) {
        // Reclaim a zombie select-park marker before arming a new one.
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
                // Confirm the recorded frame is still the marker.
                if (cs.size() > rec.stack.size() &&
                    cs[rec.stack.size()].script == rec.script &&
                    cs[rec.stack.size()].return_line == rec.line) {
                    std::vector<oa::runtime::CallFrame> cs2 = interpreter_->call_stack();
                    cs2.erase(cs2.begin() + rec.stack.size());
                    inline_event_frame_.reset();
                    interpreter_->set_call_stack(std::move(cs2));
                    dropped = true;
                }
            }
        }
        if (!dropped) {
            return;
        }
        // zombie dropped: fall through and arm the marker at the current park
    }
    // Arm only at input anchor parks (Stop/Generic/Generic0/Timed input=2).
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
    // Stack-only mutation: restore_position would clear queue-wait flags.
    interpreter_->set_call_stack(with_marker);
    inline_event_frame_ = InlineEventFrame{*script, line, std::move(stack), false};
}

void GameRuntime::RuntimeState::settle_inline_event_frame(bool paused, bool has_queued, bool saw_call,
                                            bool saw_jump) {
    // Settle mutates only when the marker is still at its recorded index.
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
        // A real [call] frame protects the park; drop only the marker.
        std::vector<oa::runtime::CallFrame> cs = interpreter_->call_stack();
        cs.erase(cs.begin() + rec.stack.size()); // index validated above
        inline_event_frame_.reset();
        interpreter_->set_call_stack(std::move(cs));
        return;
    }
    // A jump makes the marker the helper's only way back; claim it.
    if (saw_jump) inline_event_frame_->claimed_by_jump = true;
    if (inline_event_frame_->claimed_by_jump || paused || has_queued) {
        return;
    }
    // Nothing can pop the marker: tear it down and restore the pre-arm stack.
    inline_event_frame_.reset();
    interpreter_->set_call_stack(rec.stack);
}

void GameRuntime::RuntimeState::force_pointer_drag(const std::string& layer_id) {
    if (layer_id.empty() || !interpreter_) return;
    drag_begin(layer_id, true);
}

bool GameRuntime::RuntimeState::drag_begin(const std::string& layer_id, bool forced) {
    (void)forced; // forced = [lydrag]: already checked by the caller
    const auto offset = scene().layer_offset(layer_id);
    if (!offset) return false;
    pointer_.drag_layer_ = layer_id;
    pointer_.drag_start_mouse_x_ = double(input_state_.mouse_x_);
    pointer_.drag_start_mouse_y_ = double(input_state_.mouse_y_);
    pointer_.drag_origin_left_ = offset->first;
    pointer_.drag_origin_top_ = offset->second;
    const auto* row = scene().find_event_handler(layer_id, "dragin");
    if (row)
        dispatch_layer_row(layer_id, *row, "dragin",
                           {{"drag", "1"}, {"id", layer_id}}, PointerDispatch::Kind::DragIn);
    return true;
}

void GameRuntime::RuntimeState::drag_update(double mx, double my) {
    if (pointer_.drag_layer_.empty()) return;
    const double dx = mx - pointer_.drag_start_mouse_x_;
    const double dy = my - pointer_.drag_start_mouse_y_;
    double left = 0, top = 0;
    if (!scene().drag_layer_to(pointer_.drag_layer_, pointer_.drag_origin_left_, pointer_.drag_origin_top_, dx, dy, &left,
                              &top)) {
        pointer_.drag_layer_.clear();
        return;
    }
    const auto* row = scene().find_event_handler(pointer_.drag_layer_, "drag");
    if (row)
        dispatch_layer_row(pointer_.drag_layer_, *row, "drag",
                           {{"drag", "1"}, {"id", pointer_.drag_layer_}},
                           PointerDispatch::Kind::DragMove);
}

void GameRuntime::RuntimeState::drag_end() {
    if (pointer_.drag_layer_.empty()) return;
    const auto* row = scene().find_event_handler(pointer_.drag_layer_, "dragout");
    if (row)
        dispatch_layer_row(pointer_.drag_layer_, *row, "dragout",
                           {{"drag", "0"}, {"id", pointer_.drag_layer_}},
                           PointerDispatch::Kind::DragOut);
    pointer_.drag_layer_.clear();
}

// Control domain: keyconfig roles, skip/automode, control-skip, rclick, hide.
// Details: docs/RUNTIME_NOTES.md.
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
    control_.keymap_[role] = std::move(keys);
}

bool GameRuntime::RuntimeState::has_role(int role, int key) const {
    const auto it = control_.keymap_.find(role);
    return it != control_.keymap_.end() && it->second.count(key) > 0;
}

bool GameRuntime::RuntimeState::key_is_toggle(int key, int other_role) const {
    return control_.keymap_.count(other_role) > 0 && control_.keymap_.at(other_role).count(key) > 0;
}

/// Role edge; true when the key acted as role-0 advance.
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
    if (allow_it != params.end()) control_.rclick_allowed_ = parse_i32(allow_it->second, 1) != 0;
    const auto file_it = params.find("file");
    if (file_it != params.end() && !file_it->second.empty()) control_.rclick_file_ = file_it->second;
}
// [mouse left top] — request an OS cursor warp (host hook).
void GameRuntime::RuntimeState::apply_mouse_config(const std::map<std::string, std::string>& params) {
    // A live drag owns the pointer: never warp mid-drag (defensive; the FPM
    // dialog path disables drags first via sliderdrag_stat(0)).
    if (!pointer_.drag_layer_.empty()) return;
    const auto left_it = params.find("left");
    const auto top_it = params.find("top");
    if (left_it == params.end() && top_it == params.end()) return;
    int x = input_state_.mouse_x_;
    int y = input_state_.mouse_y_;
    if (left_it != params.end()) x = parse_i32(left_it->second, x);
    if (top_it != params.end()) y = parse_i32(top_it->second, y);
    // Clamp into the stage (defensive: official callers only send button
    // centers / +/-1px refresh nudges).
    const int sw = rt_->project_.config.stage_width;
    const int sh = rt_->project_.config.stage_height;
    if (sw > 0) x = std::clamp(x, 0, sw - 1);
    if (sh > 0) y = std::clamp(y, 0, sh - 1);
    pointer_.pointer_warp_x_ = x;
    pointer_.pointer_warp_y_ = y;
    pointer_.pointer_warp_pending_ = true;
    ++pointer_.pointer_warp_requests_;
    if (pointer_.pointer_warp_cb_) pointer_.pointer_warp_cb_(x, y);
}

bool GameRuntime::RuntimeState::trigger_rclick() {
    if (!interpreter_) return false;
    // hide mode: a right click first recovers the message window (no menu).
    if (control_.hide_active_) {
        exit_hide_mode();
        return true;
    }
    if (!control_.rclick_allowed_) return false;
    const std::string file =
        control_.rclick_file_.empty() ? std::string("rclick.iet") : control_.rclick_file_;
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
        if (!allow && control_.hide_active_) exit_hide_mode();
        control_.hide_allowed_ = allow;
    }
    const auto win_it = params.find("window");
    if (win_it != params.end()) {
        control_.hide_window_.clear();
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
            if (!tok.empty()) control_.hide_window_.push_back(tok);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
}

void GameRuntime::RuntimeState::apply_hide_visibility(bool hidden) {
    for (const std::string& wid : control_.hide_window_) {
        const oa::render::Layer* l = scene().find(wid);
        if (!l) continue;
        const bool visible_now = l->visible != 0.0;
        if (hidden) {
            if (!control_.hide_snapshot_.count(wid)) control_.hide_snapshot_[wid] = visible_now;
            if (visible_now) scene().set_props(wid, {{"visible", "0"}});
        } else {
            const bool was_visible = control_.hide_snapshot_.count(wid) ? control_.hide_snapshot_[wid] : true;
            control_.hide_snapshot_.erase(wid);
            if (was_visible && !visible_now) scene().set_props(wid, {{"visible", "1"}});
        }
    }
}

void GameRuntime::RuntimeState::toggle_hide_mode() {
    if (control_.hide_active_) {
        exit_hide_mode();
    } else {
        if (!control_.hide_allowed_) return;
        control_.hide_active_ = true;
        apply_hide_visibility(true);
        dispatch_mode_input("hidein");
    }
}

void GameRuntime::RuntimeState::exit_hide_mode() {
    if (!control_.hide_active_) return;
    control_.hide_active_ = false;
    apply_hide_visibility(false);
    dispatch_mode_input("hideout");
}

// ---------------------------------------------------------------------------
// skip / automode state machines
// (set_skip_mode/set_automode_mode, apply_skip_config/apply_automode_config)
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::reveal_all_text() {
    if (!text().is_reveal_complete()) text().reveal_all();
}

void GameRuntime::RuntimeState::dispatch_mode_input(const std::string& event_name) {
    // Mode handlers register under (event, "") via [setonautomodein] etc.
    // ( input_handlers; enqueue_mode_input_handler ).
    const auto* row = scene().get_input_handler(event_name, "");
    if (!row) return;
    dispatch_input_row("seton" + event_name, *row, {{"type", event_name}},
                       PointerDispatch::Kind::Push);
}

void GameRuntime::RuntimeState::set_skip_mode(bool enabled) {
    if (enabled && !control_.skip_allowed_) return;
    if (enabled && automode_active()) set_automode_mode(false);
    const bool was_active = skip_active();
    control_.skip_enabled_ = enabled;
    const bool now_active = skip_active();
    if (was_active == now_active) return;
    control_.auto_elapsed_ms_ = 0;
    if (!now_active) {
        reveal_all_text(); // reveal on skip exit so new text is not stuck
    }
    dispatch_mode_input(now_active ? "commandskipin" : "commandskipout");
}

void GameRuntime::RuntimeState::set_automode_mode(bool enabled) {
    if (enabled && !control_.automode_allowed_) return;
    if (enabled && skip_active()) set_skip_mode(false);
    const bool was_active = automode_active();
    control_.automode_ = enabled;
    if (was_active == automode_active()) return;
    control_.auto_elapsed_ms_ = 0;
    // leaving automode drops the voice-sync gate — the Lua side
    // (autoskip.lua) also clears it with [automode syncse=""], this makes the
    // native exec path symmetric so a later native re-entry starts ungated.
    if (!automode_active()) control_.automode_syncse_.clear();
    dispatch_mode_input(automode_active() ? "automodein" : "automodeout");
}

void GameRuntime::RuntimeState::update_control_skip_hold(const std::set<int>& keys_down) {
    // role 14 keys (Ctrl default; FPM csv.advkey.ctrl) act while held.
    const bool held = std::any_of(keys_down.begin(), keys_down.end(),
                                  [this](int k) { return has_role(kRoleControlSkip, k); });
    const bool was_effective = control_skip_effective();
    if (!held) control_.control_skip_blocked_ = false;
    control_.control_skip_pressed_ = held;
    const bool now_effective = control_skip_effective();
    if (was_effective == now_effective) return;
    control_.auto_elapsed_ms_ = 0;
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
            control_.control_skip_blocked_ = control_.control_skip_pressed_;
        }
        control_.skip_allowed_ = allow;
    }
    // unread stop-to-skip ([skip unread]) is not ported.
}

void GameRuntime::RuntimeState::apply_automode_config(const std::map<std::string, std::string>& params) {
    const auto allow_it = params.find("allow");
    if (allow_it != params.end()) {
        const bool allow = parse_i32(allow_it->second, 1) != 0;
        if (!allow && automode_active()) set_automode_mode(false);
        control_.automode_allowed_ = allow;
    }
    // syncse: automode flip waits for these voice ids to stop.
    const auto sync_it = params.find("syncse");
    if (sync_it != params.end()) control_.automode_syncse_ = sync_it->second;
}

void GameRuntime::RuntimeState::apply_exec_command(const std::map<std::string, std::string>& params) {
    const auto cmd_it = params.find("command");
    if (cmd_it == params.end()) return;
    const std::string& command = cmd_it->second;
    const auto mode_it = params.find("mode");
    const bool has_mode = mode_it != params.end();
    const int mode = has_mode ? parse_i32(mode_it->second, 1) : 0;
    if (command == "skip") {
        set_skip_mode(has_mode ? mode != 0 : !control_.skip_enabled_);
    } else if (command == "automode") {
        set_automode_mode(has_mode ? mode != 0 : !automode_active());
    } else if (command == "rclick") {
        if (!trigger_rclick()) {
            // fall back to the push key=2 chain ( enqueue_exec_input)
            const auto* row = scene().get_input_handler("push", "2");
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

// ---------------------------------------------------------------------------
// Host input (SDL conversion already done in main.cpp)
// ---------------------------------------------------------------------------
namespace {

void input_begin_frame(HostInput& in)
{
    in.frame.keys_down = in.kbd_down;
    for (const int mk : in.mouse_held) in.frame.keys_down.insert(mk);
    in.frame.left_down = in.left_down;
    in.frame.left_click_edge = in.left_pressed;
    in.left_pressed = false;
    in.frame.keys_down.insert(in.frame.key_down_edges.begin(),
                              in.frame.key_down_edges.end());

    for (auto it = in.wheel_hold_frames.begin(); it != in.wheel_hold_frames.end();) {
        const int vk = it->first;
        if (it->second > 0) {
            in.frame.keys_down.insert(vk);
            --it->second;
            if (it->second == 0) in.frame.key_up_edges.push_back(vk);
            ++it;
        } else {
            it = in.wheel_hold_frames.erase(it);
        }
    }
}

void input_end_frame(HostInput& in)
{
    in.frame.key_down_edges.clear();
    in.frame.key_up_edges.clear();
}

} // namespace

void GameRuntime::RuntimeState::key_event(int vk, bool down)
{
    if (vk <= 0) return;
    if (down) {
        if (input_.kbd_down.insert(vk).second)
            input_.frame.key_down_edges.push_back(vk);
    } else if (input_.kbd_down.erase(vk) > 0) {
        input_.frame.key_up_edges.push_back(vk);
    }
}

void GameRuntime::RuntimeState::mouse_move(float window_x, float window_y)
{
    float x = window_x, y = window_y;
    if (renderer_.get_renderer_coordinates(window_x, window_y, &x, &y)) {
        input_.frame.mouse_x = int(x);
        input_.frame.mouse_y = int(y);
    } else {
        input_.frame.mouse_x = int(window_x);
        input_.frame.mouse_y = int(window_y);
    }
}

void GameRuntime::RuntimeState::mouse_button(int vk, bool down)
{
    if (vk == 1) {
        input_.left_down = down;
        if (down) input_.left_pressed = true;
    } else if (vk == 2 || vk == 3) {
        if (down) {
            if (input_.mouse_held.insert(vk).second)
                input_.frame.key_down_edges.push_back(vk);
        } else if (input_.mouse_held.erase(vk) > 0) {
            input_.frame.key_up_edges.push_back(vk);
        }
    }
}

void GameRuntime::RuntimeState::mouse_wheel(int direction)
{
    if (direction == 0) return;
    const int vk = direction > 0 ? 136 : 137;
    input_.frame.key_down_edges.push_back(vk);
    input_.wheel_hold_frames[vk] = 2;
}

void GameRuntime::RuntimeState::finish_frame()
{
    // Host path only: apply the events this tick queued, then render. Tests
    // using tick(delta, FrameInput) drain manually and never reach this.
    while (!pending_events_.empty()) {
        const Event e = pending_events_.front();
        pending_events_.pop_front();
        renderer_.process_event(e);
    }
    if (renderer_.renderer_ok())
        renderer_.render_frame(now_ms_, all_delete_fade());
}

void GameRuntime::RuntimeState::tick_input(uint64_t delta_ms)
{
    input_begin_frame(input_);
    tick(delta_ms, input_.frame);
    finish_frame();
    input_end_frame(input_);
}

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------
struct App::Impl {
    using Clock = std::chrono::steady_clock;

    AppConfig cfg;
    std::shared_ptr<GameRuntime> rt;

    bool shutdown_done = false;
    Clock::time_point last_tick{};
    uint64_t frames = 0;
    bool quit = false;

    bool boot();
    AppResult tick();
    void reset_clock();
    void shutdown();
    void dump_frame();
    void dump_state();
};

namespace {

const char* wait_kind_str(oa::runtime::WaitReason::Kind k)
{
    using K = oa::runtime::WaitReason::Kind;
    switch (k) {
        case K::Generic: return "generic";
        case K::Generic0: return "wt0";
        case K::Timed: return "timed";
        case K::Stop: return "stop";
        case K::Se: return "se";
        case K::VideoLayer: return "video";
        case K::ScenarioTween: return "scenario-tween";
        case K::KeyWait: return "key";
    }
    return "?";
}

std::string wait_desc(const oa::runtime::WaitReason* w)
{
    if (!w) return "none";
    std::string s = wait_kind_str(w->kind);
    if (!w->id.empty()) s += "('" + w->id + "')";
    return s;
}

bool write_ppm(const std::string& path, uint32_t w, uint32_t h,
               const std::vector<uint8_t>& rgba)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            std::fputc(p[0], f);
            std::fputc(p[1], f);
            std::fputc(p[2], f);
        }
    }
    std::fclose(f);
    return true;
}

} // namespace

void App::Impl::dump_frame()
{
    if (cfg.dump.empty() || !rt) return;
    uint32_t w = 0, h = 0;
    auto rgba = rt->capture_frame(&w, &h);
    if (!rgba || w == 0 || h == 0) {
        std::fprintf(stderr, "[app] dump failed: no frame (%s)\n",
                     cfg.dump.c_str());
        return;
    }
    if (write_ppm(cfg.dump, w, h, *rgba))
        std::printf("[app] dump %s (%ux%u)\n", cfg.dump.c_str(), w, h);
    else
        std::fprintf(stderr, "[app] dump failed: cannot write %s\n",
                     cfg.dump.c_str());
}

void App::Impl::dump_state()
{
    if (!rt) return;

    if (const char* dump = std::getenv("OA_SCENEDUMP")) {
        const std::string prefix(dump);
        for (const oa::render::Layer* l : rt->scene().draw_order()) {
            if (!prefix.empty() && l->id.rfind(prefix, 0) != 0) continue;
            std::printf("[scene] %-16s left=%.0f top=%.0f w=%.0f h=%.0f "
                        "vis=%.0f alpha=%.3f eff=%d file=%s\n",
                        l->id.c_str(), l->left, l->top, l->width, l->height,
                        l->visible ? 1.0 : 0.0, l->alpha,
                        rt->scene().is_effectively_visible(l->id) ? 1 : 0,
                        l->file.c_str());
        }
    }
    if (std::getenv("OA_TXTDUMP")) {
        for (const auto& [id, ml] : rt->text().layers()) {
            std::string raw;
            for (const auto& u : ml.page)
                raw += u.kind == oa::render::PageUnit::Kind::Newline
                           ? std::string("\\n")
                           : u.data;
            const oa::render::Layer* node = rt->scene().find(id);
            std::fprintf(stderr,
                         "[txt] %-24s chars=%zu reveal=%zu pend=%d hidden=%d "
                         "positioned=%d page=%zu alpha=%.2f vis=%d eff=%d "
                         "box=(%.0f,%.0f %.0fx%.0f) size=%.0f align=%s "
                         "sample='%.40s'\n",
                         id.c_str(), ml.char_count, size_t(ml.reveal_index),
                         ml.reveal_pending ? 1 : 0, ml.text_hidden ? 1 : 0,
                         ml.positioned ? 1 : 0, ml.page.size(),
                         node ? node->alpha : -1.0,
                         node ? (node->visible ? 1 : 0) : -1,
                         node ? (rt->scene().is_effectively_visible(id) ? 1 : 0)
                              : -1,
                         ml.left, ml.top, ml.width, ml.height, ml.font.size(),
                         ml.font.get_or("align", "-").c_str(), raw.c_str());
        }
    }
    if (const char* vd = std::getenv("OA_VARDUMP")) {
        const std::string prefix(vd);
        const auto& vars = rt->interpreter().variables();
        auto dump_map = [&](const auto& map) {
            for (const auto& [k, v] : map) {
                if (!prefix.empty() && k.rfind(prefix, 0) != 0) continue;
                std::fprintf(stderr, "[var] %-28s = %s\n", k.c_str(),
                             v.to_string().c_str());
            }
        };
        dump_map(vars.local);
        dump_map(vars.temp);
        dump_map(vars.global);
        dump_map(vars.system);
    }

    size_t handler_layers = 0;
    for (const oa::render::Layer* l : rt->scene().draw_order())
        if (!l->event_handlers.empty()) ++handler_layers;

    std::printf("[app] headless end frames=%llu layers=%zu layer_ev=%zu "
                "text_ev=%zu switch_ev=%zu msg_lines=%zu handlers=%zu "
                "wait=%s media_ev=%zu audio_players=%zu bgm=%d se=%zu voice=%zu\n",
                (unsigned long long)frames, rt->scene().size(),
                rt->scene_layer_events(), rt->text_events(),
                rt->message_switch_events(),
                rt->text().visible_content_layers().size(), handler_layers,
                wait_desc(rt->current_wait()).c_str(), rt->media_events(),
                rt->media_players().active_players(),
                rt->audio().is_bgm_playing() ? 1 : 0,
                rt->audio().state().se_channels.size(),
                rt->audio().state().voice_channels.size());
    std::fflush(stdout);
}

bool App::Impl::boot()
{
    oa::log_build_info();

    const std::string& path = cfg.project;
    std::error_code ec;
    const bool is_dir = std::filesystem::is_directory(path, ec);
    std::shared_ptr<const oa::fs::IFileSystem> fs;
    try {
        fs = std::make_shared<oa::fs::PhysFileSystem>(path);
        if (is_dir) {
            std::printf("[app] folder project: %s\n", path.c_str());
        } else if (std::strcmp(fs->kind(), "pfs") == 0) {
            std::printf("[app] pfs project: %s\n", path.c_str());
        } else {
            const std::string side =
                std::filesystem::path(path).parent_path().string();
            std::printf("[app] pfs project: %s (sidecar dir layer: %s)\n",
                        path.c_str(), side.c_str());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cannot open project source: %s\n", e.what());
        return false;
    }

    rt = std::make_shared<GameRuntime>(fs);
    rt->enable_decode_pool();

    if (!cfg.save_root.empty()) {
        auto store = std::make_shared<oa::runtime::DirSaveStore>(cfg.save_root);
        std::error_code sec;
        std::filesystem::create_directories(std::filesystem::path(cfg.save_root), sec);
        rt->set_save_store(store);
        std::printf("[app] save store root: %s\n",
                    std::filesystem::absolute(cfg.save_root).string().c_str());
    }

    try {
        rt->open_project(cfg.platform);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "project open failed: %s\n", e.what());
        return false;
    }
    try {
        rt->boot_project();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "project boot failed: %s\n", e.what());
        return false;
    }
    rt->audio().set_master_volume(cfg.mute ? 0.0f : 1.0f);
    std::printf("[app] project booted\n");
    return true;
}

AppResult App::Impl::tick()
{
    if (!rt) return AppResult::Error;

    const Clock::time_point frame_start = Clock::now();
    uint64_t delta = 0;
    if (cfg.fixed_delta_ms > 0)
        delta = uint64_t(cfg.fixed_delta_ms);
    else if (cfg.headless)
        delta = 16;
    else
        delta = std::chrono::duration_cast<std::chrono::milliseconds>(
            frame_start - last_tick).count();
    last_tick = frame_start;
    try {
        rt->tick(delta > 0 ? delta : 1);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[app] tick error: %s\n", e.what());
        return AppResult::Error;
    }
    if (rt->exit_requested()) quit = true;

    ++frames;
    if (cfg.frames_target > 0 && frames >= cfg.frames_target) {
        quit = true;
        if (cfg.headless) dump_state();
        dump_frame();
    }
    if (cfg.headless) return quit ? AppResult::Exit : AppResult::Continue;

    const int cap = cfg.fps > 0
                        ? cfg.fps
                        : (rt->vsync_active() && rt->frame_presented()
                               ? rt->project().config.fps
                               : 0);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - frame_start).count();
    if (cap > 0) {
        const long want = 1000 / cap;
        if (elapsed < want)
            std::this_thread::sleep_for(std::chrono::milliseconds(want - elapsed));
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    return quit ? AppResult::Exit : AppResult::Continue;
}

void App::Impl::reset_clock()
{
    last_tick = Clock::now();
}

void App::Impl::shutdown()
{
    if (shutdown_done) return;
    shutdown_done = true;
    std::printf("[app] bye (frames=%llu)\n",
                static_cast<unsigned long long>(frames));
    std::fflush(stdout);
    rt.reset();
}

App::App(AppConfig config) : impl_(std::make_unique<Impl>())
{
    impl_->cfg = std::move(config);
    impl_->reset_clock();
}

App::~App()
{
    impl_->shutdown();
}

bool App::boot()
{
    return impl_->boot();
}

bool App::attach_window(void* native_window)
{
    if (!impl_->rt || !impl_->rt->attach_window(native_window, impl_->cfg.renderer))
        return false;
    impl_->reset_clock();
    return true;
}

int App::stage_width() const
{
    return impl_->rt ? impl_->rt->stage_width() : 0;
}

int App::stage_height() const
{
    return impl_->rt ? impl_->rt->stage_height() : 0;
}

AppResult App::run_headless()
{
    AppResult r = AppResult::Continue;
    while (r == AppResult::Continue) r = impl_->tick();
    return r;
}

void App::reset_clock()
{
    impl_->reset_clock();
}

void App::request_repaint()
{
    if (impl_->rt) impl_->rt->request_repaint();
}

void App::key_event(int vk, bool down)
{
    if (impl_->rt) impl_->rt->key_event(vk, down);
}

void App::mouse_move(float window_x, float window_y)
{
    if (impl_->rt) impl_->rt->mouse_move(window_x, window_y);
}

void App::mouse_button(int vk, bool down)
{
    if (impl_->rt) impl_->rt->mouse_button(vk, down);
}

void App::mouse_wheel(int direction)
{
    if (impl_->rt) impl_->rt->mouse_wheel(direction);
}

AppResult App::tick()
{
    return impl_->tick();
}

} // namespace oa::runtime
