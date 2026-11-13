// Media domain: reduce media events into AudioEngine / VideoEngine.
// Filenames stay verbatim; "0" = off, anything else = on for flags.
#include "core/runtime/runtime_internal.h"

#include <algorithm>

#include "core/emote/emote_file.h"
#include <cstdint>
#include <map>

namespace oa::runtime {

namespace {

int64_t parse_i64_raw(const std::string& v, int64_t dflt) {
    if (v.empty()) return dflt;
    try {
        size_t used = 0;
        const long long r = std::stoll(v, &used);
        return used == v.size() ? r : dflt;
    } catch (...) {
        return dflt;
    }
}

bool flag_on(const std::string& v, bool dflt) {
    return parse_i64_raw(v, dflt ? 1 : 0) != 0;
}

const std::string* param(const oa::runtime::Event& e, const char* key) {
    const auto it = e.params.find(key);
    return it == e.params.end() ? nullptr : &it->second;
}

/// Event integer param with a default for a missing/invalid key.
int64_t param_i64(const oa::runtime::Event& e, const char* key, int64_t dflt) {
    const std::string* v = param(e, key);
    return parse_i64_raw(v ? *v : std::string(), dflt);
}

} // namespace

bool GameRuntime::RuntimeState::se_wait_finished(oa::runtime::Wait& w) {
    // SE and voice ids share the channel namespace. The deadline lives on the
    // park itself: an abandoned wait can no longer leak a stale timer into a
    // later [wait se=] on the same id.
    const auto& state = media_.audio_.state();
    const oa::media::SoundChannel* channel = nullptr;
    const auto it = state.se_channels.find(w.wake_id);
    if (it != state.se_channels.end()) {
        channel = &it->second;
    } else {
        const auto jt = state.voice_channels.find(w.wake_id);
        if (jt != state.voice_channels.end()) channel = &jt->second;
    }
    // A non-playing channel still carries its play-start instant.
    if (channel && w.se_time_given)
        return state.clock_ms >= channel->started_at_ms + w.se_time_ms;
    if (!channel || !channel->playing) {
        // No live sound: release at the requested offset via a park-local
        // deadline (never a runtime-global map).
        if (!w.se_time_given) return true;
        if (w.se_deadline_ms == 0) {
            w.se_deadline_ms = state.clock_ms + w.se_time_ms;
            return w.se_time_ms == 0;
        }
        return state.clock_ms >= w.se_deadline_ms;
    }
    if (w.se_time_given) {
        // time=N counts from the SE's play-start instant (channel clock).
        return state.clock_ms >= channel->started_at_ms + w.se_time_ms;
    }
    // No time: hold until the channel stops (decoder EOF/notify).
    return false;
}

bool GameRuntime::RuntimeState::se_is_playing(const std::string& id) const {
    const auto& state = media_.audio_.state();
    if (const auto it = state.se_channels.find(id); it != state.se_channels.end())
        return it->second.playing;
    if (const auto jt = state.voice_channels.find(id); jt != state.voice_channels.end())
        return jt->second.playing;
    return false;
}

uint64_t GameRuntime::RuntimeState::automode_wait_ms() const {
    // s.automodewait (FPM clickAutomode), 900 ms fallback.
    if (interpreter_) {
        const auto v = interpreter_->variables().get("s.automodewait");
        if (v) {
            const auto i = v->as_int();
            if (i && *i > 0) return uint64_t(*i);
        }
    }
    return 900;
}

bool GameRuntime::RuntimeState::automode_sync_blocked() const {
    // [automode syncse]: any listed channel still playing blocks the flip.
    if (control_.automode_syncse_.empty()) return false;
    std::string_view rest = control_.automode_syncse_;
    for (;;) {
        const size_t comma = rest.find(',');
        std::string_view tok = rest.substr(0, comma);
        // tolerate stray spaces around commas
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.remove_prefix(1);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
            tok.remove_suffix(1);
        if (!tok.empty() && se_is_playing(std::string(tok))) return true;
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    return false;
}

oa::runtime::InterpreterHooks::SoundInfoSnap
GameRuntime::RuntimeState::sound_info_snapshot_for_hook() const {
    // get_sound_info: BGM + SE bus, sorted by id.
    using Snap = oa::runtime::InterpreterHooks::SoundInfoSnap;
    using Ch = oa::runtime::InterpreterHooks::SoundChannelSnap;
    Snap out;
    const auto& st = media_.audio_.state();
    const auto to_ch = [](const oa::media::SoundChannel& c) {
        Ch ch;
        ch.id = c.id;
        ch.playing = c.playing;
        ch.gain = c.raw_gain;
        ch.pan = c.raw_pan;
        return ch;
    };
    if (st.bgm_channel) out.bgm = to_ch(*st.bgm_channel);
    for (const auto& [id, c] : st.se_channels) {
        (void)id;
        out.se.push_back(to_ch(c));
    }
    std::sort(out.se.begin(), out.se.end(),
              [](const Ch& a, const Ch& b) { return a.id < b.id; });
    return out;
}

void GameRuntime::RuntimeState::sync_system_audio_volumes() {
    if (!interpreter_) return;
    const auto& vars = interpreter_->variables();
    auto read_volume = [&vars](const char* key) -> std::optional<float> {
        const auto v = vars.get(key);
        if (!v) return std::nullopt;
        const auto i = v->as_int();
        if (!i) return std::nullopt;
        float f = float(*i) / 1000.0f;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        return f;
    };
    // Only push changes down to the engine.
    const auto bgm = read_volume("s.bgmvol");
    if (bgm && bgm != media_.last_bgm_volume_) {
        media_.last_bgm_volume_ = bgm;
        media_.audio_.set_bgm_volume(*bgm);
    }
    const auto se = read_volume("s.sevol");
    if (se && se != media_.last_se_volume_) {
        media_.last_se_volume_ = se;
        media_.audio_.set_se_volume(*se);
    }
    // s.videovol → movie bus (absent = full volume).
    const auto movie = read_volume("s.videovol");
    if (movie) media_.video_.set_movie_volume(*movie);
}

void GameRuntime::RuntimeState::set_bgm_loop_file(const std::string& file, bool loop_play) {
    // A-B loop segment convention (see ab_loop_file + play_bgm).
    if (!loop_play) return;
    if (auto loop_file = oa::media::ab_loop_file(file)) {
        if (media_.audio_.state().bgm_channel) {
            media_.audio_.state().bgm_channel->loop_file = std::move(loop_file);
        }
    }
}

void GameRuntime::RuntimeState::dispatch_media_handler(const oa::media::SoundFinishHandler& h) {
    if (!interpreter_) return;
    ++media_.media_handler_dispatches_;
    if (!h.handler.empty()) {
        // Inline engine tag; registration params carry function=/extras.
        interpreter_->enqueue_tag(h.handler, h.params);
    }
    if (!h.file.empty() || !h.label.empty()) {
        std::map<std::string, std::string> jump;
        if (!h.file.empty()) jump["file"] = h.file;
        if (!h.label.empty()) jump["label"] = h.label;
        interpreter_->enqueue_tag(h.call ? "call" : "jump", std::move(jump));
    }
}

void GameRuntime::RuntimeState::dispatch_media_handler(const oa::media::VideoFinishHandler& h) {
    ++media_.media_handler_dispatches_;
    oa::media::SoundFinishHandler s;
    s.file = h.file;
    s.label = h.label;
    s.call = h.call;
    s.handler = h.handler;
    s.params = h.params;
    dispatch_media_handler(s);
}

/// Playback ended: stop the channel and dispatch its finish handler.
void GameRuntime::RuntimeState::sound_finished(oa::media::SoundCategory cat,
                                 const std::string& id) {
    finish_sound(cat == oa::media::SoundCategory::Bgm, id);
}

void GameRuntime::RuntimeState::finish_sound(bool bgm, const std::string& id) {
    // Fetch the handler before stopping the channel.
    std::optional<oa::media::SoundFinishHandler> handler;
    if (bgm) {
        handler = media_.audio_.state().bgm_finish_handler;
        media_.audio_.stop_bgm(0);
    } else {
        const auto it = media_.audio_.state().se_finish_handlers.find(id);
        if (it != media_.audio_.state().se_finish_handlers.end()) handler = it->second;
        media_.audio_.stop_se(id, 0); // removes the channel from se + voice maps
    }
    // The end-of-play report is authoritative; drop queued fallbacks.
    (void)media_.audio_.poll_finish_events();
    if (handler && handler->any()) dispatch_media_handler(*handler);
}

/// Video EOF/stop: stop the channel and dispatch its handler.
void GameRuntime::RuntimeState::finish_video(const std::string& id) {
    std::optional<oa::media::VideoFinishHandler> handler;
    const auto snap = media_.video_.state();
    if (id.empty()) {
        handler = snap.finish_handler;
        media_.video_.stop_overlay();
    } else {
        const auto it = snap.layer_finish_handlers.find(id);
        if (it != snap.layer_finish_handlers.end()) {
            handler = it->second;
        } else {
            handler = snap.finish_handler; // per-layer, global fallback
        }
        media_.video_.stop_layer(id);
        // Unbind the layer's video-frame texture when it still carries one.
        scene().unbind_video_layer(id);
    }
    (void)media_.video_.poll_finish_events();
    if (handler && handler->any()) dispatch_media_handler(*handler);
}

void GameRuntime::RuntimeState::advance_media_frame(uint64_t delta_ms) {
    // 视频源线程 update 好的纹理在这里（主线程）挂到层上；场景数据只有
    // 主线程写，渲染线程下一帧 flush 上传。
    {
        std::map<std::string, oa::render::HostTextureRef> take;
        {
            std::lock_guard<std::mutex> lk(host_tex_mu_);
            if (!pending_host_tex_.empty()) take.swap(pending_host_tex_);
        }
        if (!take.empty()) {
            for (auto& [id, tex] : take) {
                if (!tex) continue;
                const int w = tex->width();
                const int h = tex->height();
                renderer_.scene().set_host_texture(id, std::move(tex), w, h);
            }
            renderer_.request_repaint(); // 像素更新：本帧重绘（不重建计划）
        }
    }
    if (!interpreter_ || !media_.media_players_) return;
    sync_system_audio_volumes();
    media_.audio_.set_skipping(control_.skip_active_);
    // Fade clock first: completions surface below.
    media_.audio_.update(delta_ms);
    // Decode/EOF for playing channels; natural completions dispatch handlers.
    media_.media_players_->update(
        delta_ms, media_.audio_,
        [this](oa::media::SoundCategory cat, const std::string& id) {
            sound_finished(cat, id);
        });
    // Fade-out completions (channels already removed).
    for (const auto& ev : media_.audio_.poll_finish_events()) {
        if (ev.handler && ev.handler->any()) dispatch_media_handler(*ev.handler);
    }
    // Video logic backend update.
    media_.video_.update(delta_ms);
    for (const auto& ev : media_.video_.poll_finish_events()) {
        finish_video(ev.id);
    }
    // Overlay video draws through the topmost compositor slot.
    if (media_.video_.is_overlay_playing()) {
        scene().set_overlay_file();
    } else {
        scene().clear_overlay_file();
    }
}

bool GameRuntime::RuntimeState::apply_media_event(const oa::runtime::Event& e) {
    // ---- [video] family ----------------------------------------------------
    if (e.tag == "video") {
        renderer_.request_repaint();
        const std::string file = param(e, "file") ? *param(e, "file") : std::string();
        const bool loop_play = flag_on(param(e, "loop") ? *param(e, "loop") : "0", false);
        // skip: 0 never, 1 click-skip, 2 currently treated as 1.
        const int64_t skip = param_i64(e, "skip", 1);
        // delaymargin >= 0 → layer jump-frame threshold.
        std::optional<int32_t> delay;
        if (const std::string* v = param(e, "delaymargin")) {
            const int64_t d = parse_i64_raw(*v, -1);
            if (d >= 0) delay = int32_t(d);
        }
        oa::media::VideoConfig cfg;
        cfg.file = file;
        cfg.skippable = skip != 0;
        cfg.loop_play = loop_play;
        cfg.delay_margin_ms = delay;
        // [video vol=N]: per-event movie gain (raw 0..1000).
        if (const std::string* v = param(e, "vol")) {
            const int64_t g = parse_i64_raw(*v, 1000);
            if (g >= 0) cfg.gain = int32_t(g > 1000 ? 1000 : g);
        }
        if (e.id.empty()) {
            media_.video_.play_overlay(cfg);
        } else {
            media_.video_.play_layer(e.id, cfg);
            // Bind the layer to its VideoFrame texture. A loop whose frame-0
            // keyed-alpha coverage is sparse (<= 1/3, mask-composited alpha
            // when a _m partner exists) may auto-materialize a missing
            // effect-strip carrier; dense strips and one-shots stay strict.
            bool allow_auto = false;
            if (loop_play) {
                const auto vsnap = media_.video_.state();
                const auto vit = vsnap.video_layers.find(e.id);
                const bool mask_on =
                    vit != vsnap.video_layers.end() && vit->second.mask_on;
                int w = 0, h = 0;
                const uint8_t* rgba = nullptr;
                uint64_t rev = 0;
                if (media_.video_.video_frame(e.id, &w, &h, &rgba, &rev) && rgba &&
                    w > 0 && h > 0) {
                    const size_t n = size_t(w) * size_t(h);
                    size_t content = 0;
                    for (size_t i = 0; i < n; ++i) {
                        const bool lit =
                            mask_on ? rgba[i * 4 + 3] > 32
                                    : oa::media::layer_video_key_alpha(
                                          oa::media::rgba_luma(&rgba[i * 4])) > 32;
                        if (lit) ++content;
                    }
                    allow_auto = double(content) / double(n) <= 0.35;
                }
            }
            const bool bound = scene().bind_video_layer(e.id, allow_auto);
            if (!bound && loop_play) {
                // Unbound loop: stop it (no carrier = zero pixels forever).
                // One-shots keep running unbound so EOF drives their waits.
                media_.video_.stop_layer(e.id);
            }
        }
        ++media_.media_events_;
        return true;
    }
    if (e.tag == "setonvideofinish" || e.tag == "delonvideofinish") {
        const std::string id = param(e, "id") ? *param(e, "id") : std::string();
        if (e.tag == "delonvideofinish") {
            media_.video_.remove_finish_handler(id);
            ++media_.media_events_;
            return true;
        }
        oa::media::VideoFinishHandler h;
        h.file = param(e, "file") ? *param(e, "file") : std::string();
        h.label = param(e, "label") ? *param(e, "label") : std::string();
        h.call = flag_on(param(e, "call") ? *param(e, "call") : "0", false);
        h.handler = param(e, "handler") ? *param(e, "handler") : std::string();
        h.params = e.params; // verbatim extras (function= etc.)
        media_.video_.set_finish_handler(id, std::move(h));
        ++media_.media_events_;
        return true;
    }

    // ---- [splay]/[sstop]/[sfade]/[span]/[sxfade] (BGM) --------------------
    if (e.tag == "splay") {
        const std::string* fp = param(e, "file");
        if (!fp || fp->empty()) return true; // nothing to play
        oa::media::BgmConfig cfg;
        cfg.loop_play = flag_on(param(e, "loop") ? *param(e, "loop") : "1", true);
        if (const std::string* v = param(e, "gain")) cfg.gain = int32_t(parse_i64_raw(*v, 0));
        if (const std::string* v = param(e, "pan")) cfg.pan = int32_t(parse_i64_raw(*v, 0));
        if (const std::string* v = param(e, "time"))
            cfg.fade_in_ms = uint64_t(parse_i64_raw(*v, 0));
        // buffer: playback buffering hint (-1 = memory); ignored by the sink.
        media_.audio_.play_bgm(*fp, cfg);
        set_bgm_loop_file(*fp, cfg.loop_play);
        ++media_.media_events_;
        return true;
    }
    if (e.tag == "sstop") {
        const uint64_t time =
            uint64_t(param_i64(e, "time", 0));
        media_.audio_.stop_bgm(time);
        ++media_.media_events_;
        return true;
    }
    if (e.tag == "sfade" || e.tag == "sfadein" || e.tag == "sfadeout") {
        int64_t gain = 0;
        if (e.tag == "sfadein") {
            gain = 1000;
        } else if (e.tag == "sfadeout") {
            gain = 0;
        } else {
            gain = param_i64(e, "gain", 0);
        }
        const uint64_t time =
            uint64_t(param_i64(e, "time", 0));
        media_.audio_.fade_bgm_gain(int32_t(gain), time);
        ++media_.media_events_;
        return true;
    }
    if (e.tag == "span") {
        const int64_t pan = param_i64(e, "pan", 0);
        const uint64_t time =
            uint64_t(param_i64(e, "time", 0));
        media_.audio_.pan_bgm(int32_t(pan), time);
        ++media_.media_events_;
        return true;
    }
    if (e.tag == "sxfade") {
        const std::string* fp = param(e, "file");
        if (!fp || fp->empty()) return true;
        oa::media::BgmConfig cfg;
        cfg.loop_play = flag_on(param(e, "loop") ? *param(e, "loop") : "1", true);
        if (const std::string* v = param(e, "gain")) cfg.gain = int32_t(parse_i64_raw(*v, 0));
        if (const std::string* v = param(e, "pan")) cfg.pan = int32_t(parse_i64_raw(*v, 0));
        cfg.fade_in_ms =
            uint64_t(param_i64(e, "time", 0));
        media_.audio_.crossfade_bgm(*fp, cfg);
        set_bgm_loop_file(*fp, cfg.loop_play);
        ++media_.media_events_;
        return true;
    }

    // ---- [seplay]/[sestop]/[sefade]/[sepan] (SE) --------------------------
    if (e.tag == "seplay" || e.tag == "voice") {
        const std::string* fp = param(e, "file");
        if (!fp || fp->empty()) return true;
        const bool voice = e.tag == "voice";
        const std::string raw_id = param(e, "id") ? *param(e, "id") : std::string();
        std::string id;
        if (voice && raw_id.empty()) {
            // Auto voice id (the default voice:{serial} shape).
            media_.voice_serial_ = media_.voice_serial_ + 1;
            id = "voice:" + std::to_string(media_.voice_serial_);
        } else {
            id = raw_id;
        }
        oa::media::SeConfig cfg;
        cfg.loop_play = flag_on(param(e, "loop") ? *param(e, "loop") : "0", false);
        if (const std::string* v = param(e, "gain")) cfg.gain = int32_t(parse_i64_raw(*v, 0));
        if (const std::string* v = param(e, "pan")) cfg.pan = int32_t(parse_i64_raw(*v, 0));
        if (const std::string* v = param(e, "time"))
            cfg.fade_in_ms = uint64_t(parse_i64_raw(*v, 0));
        cfg.skippable = flag_on(param(e, "skippable") ? *param(e, "skippable") : "0", false);
        if (voice) {
            media_.audio_.play_voice(id, *fp, cfg);
        } else {
            media_.audio_.play_se(id, *fp, cfg);
        }
        ++media_.media_events_;
        return true;
    }
    if (e.tag == "sestop") {
        const std::string id = param(e, "id") ? *param(e, "id") : std::string();
        const uint64_t time =
            uint64_t(param_i64(e, "time", 0));
        media_.audio_.stop_se(id, time);
        ++media_.media_events_;
        return true;
    }
    if (e.tag == "sefade" || e.tag == "sefadein" || e.tag == "sefadeout") {
        const std::string id = param(e, "id") ? *param(e, "id") : std::string();
        int64_t gain = 0;
        if (e.tag == "sefadein") {
            gain = 1000;
        } else if (e.tag == "sefadeout") {
            gain = 0;
        } else {
            gain = param_i64(e, "gain", 0);
        }
        const uint64_t time =
            uint64_t(param_i64(e, "time", 0));
        media_.audio_.fade_se_gain(id, int32_t(gain), time);
        ++media_.media_events_;
        return true;
    }
    if (e.tag == "sepan") {
        const std::string id = param(e, "id") ? *param(e, "id") : std::string();
        const int64_t pan = param_i64(e, "pan", 0);
        const uint64_t time =
            uint64_t(param_i64(e, "time", 0));
        media_.audio_.pan_se(id, int32_t(pan), time);
        ++media_.media_events_;
        return true;
    }
    if (e.tag == "/voice") {
        ++media_.media_events_; // backlog replay link marker; no audio op
        return true;
    }

    // ---- [setonsoundfinish]/[delonsoundfinish] ----------------------------
    if (e.tag == "setonsoundfinish" || e.tag == "delonsoundfinish") {
        const std::string id = param(e, "id") ? *param(e, "id") : std::string();
        if (e.tag == "delonsoundfinish") {
            media_.audio_.remove_sound_finish_handler(id); // "" = BGM
            ++media_.media_events_;
            return true;
        }
        oa::media::SoundFinishHandler h;
        h.file = param(e, "file") ? *param(e, "file") : std::string();
        h.label = param(e, "label") ? *param(e, "label") : std::string();
        h.call = flag_on(param(e, "call") ? *param(e, "call") : "0", false);
        h.handler = param(e, "handler") ? *param(e, "handler") : std::string();
        h.params = e.params; // verbatim extras (function= id fl md ct ...)
        media_.audio_.set_sound_finish_handler(id, std::move(h)); // "" = BGM
        ++media_.media_events_;
        return true;
    }

    // ---- [allsoundstop] ----------------------------------------------------
    if (e.tag == "allsoundstop") {
        media_.audio_.stop_all_sounds();
        if (media_.media_players_) media_.media_players_->stop_all();
        ++media_.media_events_;
        return true;
    }
    return false;
}

// E-mote static layer: create the player, render its pose, bind the canvas.
void GameRuntime::RuntimeState::apply_emote_static(const oa::runtime::Event& e) {
    const auto idIt = e.params.find("id");
    if (idIt == e.params.end() || idIt->second.empty()) return;
    const std::string& id = idIt->second;
    renderer_.request_repaint();
    const auto filesIt = e.params.find("files");
    if (filesIt == e.params.end() || filesIt->second.empty()) return;
    // e:createEmoteLayer joins the file list with \x1f (paths may contain
    // backslashes on win archives; only the first file is used).
    const std::string first = filesIt->second.substr(0, filesIt->second.find('\x1f'));
    if (first.empty()) return;

    // Default canvas = project stage (a taller request shifts the figure down).
    int w = 1920, h = 1080;
    if (interpreter_) {
        const auto& cfg = interpreter_->config();
        if (cfg.stage_width > 0) w = cfg.stage_width;
        if (cfg.stage_height > 0) h = cfg.stage_height;
    }
    auto pick_int = [&](const char* key, int dflt) {
        const auto it = e.params.find(key);
        if (it == e.params.end()) return dflt;
        try {
            const int v = std::stoi(it->second);
            return (v >= 64 && v <= 4096) ? v : dflt;
        } catch (...) {
            return dflt;
        }
    };
    w = pick_int("width", w);
    h = pick_int("height", h);

    auto bytes = interpreter_ ? fs_->read(interpreter_->resolve_magic_path(first))
                              : std::nullopt;
    if (!bytes) {
        return;
    }
    auto player = std::make_shared<oa::emote::EmotePlayer>();
    // emote 源自己推：每次产出新帧（CPU 帧或 GPU 姿态部件）直接进渲染槽。
    const bool gpu_pose = oa::emote::emote_default_external_pose() &&
                          renderer_.renderer_ok();
    player->set_external_pose(gpu_pose);
    player->set_frame_push(
        [this, id](int fw, int fh, const uint8_t* px) {
            auto* layer = renderer_.scene().find(id);
            auto tex = layer ? layer->texture : nullptr;
            if (!tex || tex->width() != fw || tex->height() != fh)
                tex = renderer_.create_host_texture(fw, fh);
            if (!tex) return false;
            tex->update(fw, fh, px);
            const bool ok = renderer_.scene().set_host_texture(id, tex, fw, fh);
            if (ok) renderer_.request_repaint();
            return ok;
        },
        [this, id](const oa::emote::EmoteFile& f,
                   const std::vector<oa::emote::EmoteDrawPart>& parts, int fw,
                   int fh) {
            auto* layer = renderer_.scene().find(id);
            auto tex = layer ? layer->texture : nullptr;
            if (!tex || tex->width() != fw || tex->height() != fh)
                tex = renderer_.create_host_texture(fw, fh);
            if (!tex) return false;
            if (!renderer_.compose_canvas(id, tex, f, parts, fw, fh)) return false;
            const bool ok = renderer_.scene().set_host_texture(id, tex, fw, fh);
            if (ok) renderer_.request_repaint();
            return ok;
        });
    std::string err;
    if (!player->load(*bytes, w, h, &err)) {
        return;
    }
    // Same-id rebuild: carry the view over (avoid the 0.6/0 default reset).
    if (const auto it = media_.emote_layers_.find(id); it != media_.emote_layers_.end() && it->second.player) {
        player->copy_view_from(*it->second.player);
    } else if (const auto vit = media_.emote_view_cache_.find(id);
               vit != media_.emote_view_cache_.end()) {
        // Rebuild from the cached view (view calls ran on the old player).
        player->set_view_state(vit->second);
    }
    EmoteLayerState st;
    st.width = player->width();   // actual render buffer (may be half res)
    st.height = player->height();
    st.file = first;
    st.revision = player->revision();
    st.player = std::move(player);
    media_.emote_layers_[id] = std::move(st);
    ++media_.emote_layer_events_;
    // Size the carrier and bind the EmoteCanvas texture.
    std::map<std::string, std::string> props;
    props["width"] = std::to_string(w);
    props["height"] = std::to_string(h);
    scene().set_props(id, std::move(props));
    scene().bind_emote_layer(id);
    // Replay method calls queued before materialization.
    const auto pend = media_.pending_emote_methods_.find(id);
    if (pend != media_.pending_emote_methods_.end()) {
        for (const auto& m : pend->second) {
            double out = 0;
            std::string sout;
            emote_method_dispatch(id, m.method, m.s, m.s2, m.n1, m.n2, m.n3, &out,
                                  &sout);
        }
        media_.pending_emote_methods_.erase(pend);
    }
}

void GameRuntime::RuntimeState::advance_emote_players(uint64_t delta_ms) {
    if (delta_ms == 0 || media_.emote_layers_.empty()) return;
    for (auto& [id, st] : media_.emote_layers_) {
        if (!st.player) continue;
        // Host-clock layers must not auto-advance (double-speed guard).
        if (st.host_clock) {
            const uint64_t rev = st.player->revision();
            if (rev != st.revision) {
                st.revision = rev;
            }
            continue;
        }
        const uint64_t before = st.player->revision();
        st.player->advance_ms(delta_ms);
        if (st.player->revision() != before) {
            st.revision = st.player->revision();
        }
    }
}

bool GameRuntime::RuntimeState::emote_method_dispatch(const std::string& id, const std::string& method,
                                        const std::string& s, const std::string& s2,
                                        double n1, double n2, double n3, double* out,
                                        std::string* sout) {
    auto it = media_.emote_layers_.find(id);
    if (it == media_.emote_layers_.end() || !it->second.player) {
        // Not materialized yet: queue and replay after apply_emote_static.
        PendingEmoteMethod m;
        m.method = method;
        m.s = s;
        m.s2 = s2;
        m.n1 = n1;
        m.n2 = n2;
        m.n3 = n3;
        auto& q = media_.pending_emote_methods_[id];
        if (q.size() < 128) q.push_back(std::move(m));
        if (out) *out = 0;
        if (sout) sout->clear();
        return true;
    }
    oa::emote::EmotePlayer& p = *it->second.player;
    // First progress call switches the layer to host-clock mode.
    if (method == "progress") {
        it->second.host_clock = true;
        p.progress(n1);
        return true;
    }
    if (method == "isTimelinePlaying") {
        // Query membership in either slot (foreground is a parallel list).
        if (out) *out = p.is_timeline_playing(s) ? 1.0 : 0.0;
        return true;
    }
    if (method == "playTimeline") {
        // flags (n1): 1 = append in parallel, otherwise replace.
        p.play_timeline(s, int(n1));
    } else if (method == "fadeInTimeline") {
        p.fade_in_timeline(s);
    } else if (method == "stopTimeline") {
        // Per-entry stop (fg: pass-like, idle: delta ends).
        p.stop_timeline(s);
    } else if (method == "fadeOutTimeline") {
        // Fade math is not implemented; end the slot structurally.
        p.stop_timeline(s);
    } else if (method == "stop") {
        p.stop(); // freeze at the current pose, drop both timeline slots
    } else if (method == "pass") {
        p.pass();
    } else if (method == "step") {
        p.step();
    } else if (method == "skip") {
        p.skip();
    } else if (method == "setVariable") {
        p.set_variable(s, n1);
    } else if (method == "getVariable") {
        if (out) *out = p.get_variable(s);
        return true;
    } else if (method == "countVariables") {
        // Inclusive framework loop bound (one-past query stays valid).
        if (out) *out = double(p.variable_count());
        return true;
    } else if (method == "getVariableLabelAt") {
        // i-th variable label (metadata.variableList order).
        if (sout) *sout = p.variable_label_at(int(n1));
        return true;
    } else if (method == "setVariableDiff") {
        // setVariableDiff: dst = s2, value = n1.
        p.set_variable_diff(s2, n1);
    } else if (method == "getTimelinePlaying") {
        if (out) *out = p.foreground_timeline().empty() ? 0.0 : 1.0;
        return true;
    } else if (method == "setScale") {
        p.set_scale(n1, n2, n3);
    } else if (method == "setCoord") {
        p.set_coord(n1, n2);
    } else if (method == "setZoom") {
        p.set_scale(n1, n2, n3);
    }
    return true;
}

} // namespace oa::runtime
