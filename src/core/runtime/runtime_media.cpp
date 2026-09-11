// Media domain glue.
//
// The interpreter already emits every media tag as a typed event with its
// params preserved verbatim (audio tags land as ConfigEvent, [video] as
// LayerEventCmd, seton/delonvideofinish as Custom). At dispatch time the
// runtime reduces those events into oa::media::AudioEngine / VideoEngine,
// exactly like the interpreter reduces other events into its own state
// backends. Consumed media events never reach the host event stream.
//
// Parameter parsing follows the interpreter tag-handler shapes
// (audio/video handler parameter names preserved verbatim):
// file names stay verbatim strings, numeric fields are raw-parse with the
// documented defaults, loop/skippable treat "0" as off and anything else
// (default) as on.
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

/// The `param` + parse_i64_raw pair every media tag used to spell out inline
/// (`parse_i64_raw(param(e, "time") ? *param(e, "time") : "", 0)`): the
/// missing key falls back to `dflt`, which is what the old inline defaults
/// ("0" with dflt 0, "" with dflt 0, "1" with dflt 1) all resolved to.
int64_t param_i64(const oa::runtime::Event& e, const char* key, int64_t dflt) {
    const std::string* v = param(e, key);
    return parse_i64_raw(v ? *v : std::string(), dflt);
}

} // namespace

bool GameRuntime::RuntimeState::media_se_wait_finished(const std::string& id, bool time_given,
                                         uint64_t time_ms) const {
    // Channel lookup covers both the SE and voice maps (the same lookup the
    // interpreter's se_wait_finished uses).
    const auto* state = &audio_.state();
    const oa::media::SoundChannel* channel = nullptr;
    const auto it = state->se_channels.find(id);
    if (it != state->se_channels.end()) {
        channel = &it->second;
    } else {
        const auto jt = state->voice_channels.find(id);
        if (jt != state->voice_channels.end()) channel = &jt->second;
    }
    if (!channel || !channel->playing) return true;
    if (time_given) {
        // time=N counts from the SE's play-start instant (channel clock).
        return state->clock_ms >= channel->started_at_ms + time_ms;
    }
    // No time: hold until the channel actually stops (EOF via the decoder /
    // notify path). The state backend cannot end it by itself (host media
    // semantics); no "release immediately" fallback exists because
    // openartemis always carries a decoder host.
    return false;
}

uint64_t GameRuntime::RuntimeState::automode_wait_ms() const {
    // FPM clickAutomode (adv/mainloop.lua) writes s.automodewait before every
    // click park while automode runs: init.automode_vowait (200, voiced line
    // under conf.autostop==1) or getASpeed() = (100 - conf.aspeed) * 60 + 300
    // (list_windows.tbl automode_speed={300,60}); the engine falls back to the
    // historical 900 ms default when a game never sets it.
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
    // [automode syncse="1,2,..."]: comma-separated sound ids captured by the
    // Lua layer from the channels playing on the current page (voice bus or
    // SE bus — ids ride either map, same lookup as media_se_wait_finished).
    // Any listed channel still playing blocks the auto page flip.
    if (automode_syncse_.empty()) return false;
    std::string_view rest = automode_syncse_;
    for (;;) {
        const size_t comma = rest.find(',');
        std::string_view tok = rest.substr(0, comma);
        // tolerate stray spaces around commas
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.remove_prefix(1);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
            tok.remove_suffix(1);
        if (!tok.empty() && !media_se_wait_finished(std::string(tok), false, 0))
            return true;
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    return false;
}

oa::runtime::InterpreterHooks::SoundInfoSnap
GameRuntime::RuntimeState::sound_info_snapshot_for_hook() const {
    // sound_info snapshot:
    // BGM slot + every SE-bus channel (sorted by id, SE list order stable);
    // voice_channels are not surfaced (FPM voices ride the SE bus).
    using Snap = oa::runtime::InterpreterHooks::SoundInfoSnap;
    using Ch = oa::runtime::InterpreterHooks::SoundChannelSnap;
    Snap out;
    const auto& st = audio_.state();
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
    if (bgm && bgm != last_bgm_volume_) {
        last_bgm_volume_ = bgm;
        audio_.set_bgm_volume(*bgm);
    }
    const auto se = read_volume("s.sevol");
    if (se && se != last_se_volume_) {
        last_se_volume_ = se;
        audio_.set_se_volume(*se);
    }
    // The movie volume bus. The [video] frameworks (movie.lua)
    // fold conf.movie (or conf.bgm) + conf.master into the script var
    // s.videovol (raw 0..1000, volume_count in the game's conf.lua) right
    // before every movie event, so the same bridge the bgm/se buses use
    // reaches the VideoEngine movie volume. Games that never compute the var
    // (older frameworks, e.g. NekoMiko's movie.lua) leave the bus at 1.0 —
    // full volume, the movies' historical behavior there.
    const auto movie = read_volume("s.videovol");
    if (movie) video_.set_movie_volume(*movie);
}

void GameRuntime::RuntimeState::set_bgm_loop_file(const std::string& file, bool loop_play) {
    // A-B loop segment convention (see ab_loop_file + play_bgm).
    if (!loop_play) return;
    if (auto loop_file = oa::media::ab_loop_file(file)) {
        if (audio_.state().bgm_channel) {
            audio_.state().bgm_channel->loop_file = std::move(loop_file);
        }
    }
}

void GameRuntime::RuntimeState::dispatch_media_handler(const oa::media::SoundFinishHandler& h) {
    if (!interpreter_) return;
    ++media_handler_dispatches_;
    if (!h.handler.empty()) {
        // Inline engine tag (calllua etc). Pass the registration params so
        // calllua's function= name and per-use extras survive (the same
        // convention the interpreter uses for its lytween handler tags).
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
    ++media_handler_dispatches_;
    oa::media::SoundFinishHandler s;
    s.file = h.file;
    s.label = h.label;
    s.call = h.call;
    s.handler = h.handler;
    s.params = h.params;
    dispatch_media_handler(s);
}

/// Playback-end report for one sound channel (EOF via the decode pump, or a
/// host/sink stop): ends the channel and dispatches its finish handler.
void GameRuntime::RuntimeState::sound_finished(oa::media::SoundCategory cat,
                                 const std::string& id) {
    finish_sound(cat == oa::media::SoundCategory::Bgm, id);
}

void GameRuntime::RuntimeState::finish_sound(bool bgm, const std::string& id) {
    // Fetch the registered completion handler before stopping the channel.
    std::optional<oa::media::SoundFinishHandler> handler;
    if (bgm) {
        handler = audio_.state().bgm_finish_handler;
        audio_.stop_bgm(0);
    } else {
        const auto it = audio_.state().se_finish_handlers.find(id);
        if (it != audio_.state().se_finish_handlers.end()) handler = it->second;
        audio_.stop_se(id, 0); // removes the channel from se + voice maps
    }
    // Discard queued fallback completions from the internal state machine
    // (the end-of-play report is authoritative).
    (void)audio_.poll_finish_events();
    if (handler && handler->any()) dispatch_media_handler(*handler);
}

/// Video finish (layer or fullscreen; polled from VideoEngine completion
/// events plus the [vstop] paths).
void GameRuntime::RuntimeState::finish_video(const std::string& id) {
    std::optional<oa::media::VideoFinishHandler> handler;
    const auto snap = video_.state();
    if (id.empty()) {
        handler = snap.finish_handler;
        video_.stop_overlay();
    } else {
        const auto it = snap.layer_finish_handlers.find(id);
        if (it != snap.layer_finish_handlers.end()) {
            handler = it->second;
        } else {
            handler = snap.finish_handler; // per-layer, global fallback
        }
        video_.stop_layer(id);
        // Unbind the host-decoded layer texture (内容来源状态解绑 —— 取代旧的
        // clear_layer_file_if_matches(保留命名空间串),只在层仍绑着视频帧时
        // 生效)。
        scene_.unbind_video_layer(id);
    }
    (void)video_.poll_finish_events();
    if (id.empty()) {
        video_finished_ = true; // fullscreen completion resumes Stop-class waits
    }
    if (handler && handler->any()) dispatch_media_handler(*handler);
}

void GameRuntime::RuntimeState::advance_media_frame(uint64_t delta_ms) {
    if (!interpreter_ || !media_players_) return;
    sync_system_audio_volumes();
    audio_.set_skipping(skip_active_);
    // Fade clock first: fade-out completions become engine events below.
    audio_.update(delta_ms);
    // Decode + EOF handling for every playing channel; natural (EOF)
    // completions stop their channels and dispatch handlers.
    media_players_->update(
        delta_ms, audio_,
        [this](oa::media::SoundCategory cat, const std::string& id) {
            sound_finished(cat, id);
        });
    // Fade-out stop completions (channels already removed by the engine).
    for (const auto& ev : audio_.poll_finish_events()) {
        if (ev.handler && ev.handler->any()) dispatch_media_handler(*ev.handler);
    }
    // Video logic backend: non-loop channels "complete" right after they
    // start (no decoder yet; see the VideoStateBackend semantics).
    video_.update(delta_ms);
    for (const auto& ev : video_.poll_finish_events()) {
        finish_video(ev.id);
    }
    // Unified video compositing: the overlay plays through a structural
    // topmost Compositor slot (Compositor::set_overlay_file) bound to the
    // host-uploaded overlay texture; no compare_ids tricks, no host blit.
    if (video_.is_overlay_playing()) {
        scene_.set_overlay_file();
    } else {
        scene_.clear_overlay_file();
    }
}

bool GameRuntime::RuntimeState::apply_media_event(const oa::runtime::Event& e) {
    // ---- [video] family ----------------------------------------------------
    if (e.tag == "video") {
        const std::string file = param(e, "file") ? *param(e, "file") : std::string();
        const bool loop_play = flag_on(param(e, "loop") ? *param(e, "loop") : "0", false);
        // skip: 0 = never skippable; default 1 = click-skip; 2 = menu-only
        // skip (treated like 1 for now; video skip=2 is still a TODO).
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
        // [video vol=N] — Artemis raw scale 0..1000, the movie
        // audio's per-event gain (movie.lua movie_playfile forwards the
        // [movie] row's vol). Most games leave it out and rely on the
        // s.videovol volume bus instead (synced above).
        if (const std::string* v = param(e, "vol")) {
            const int64_t g = parse_i64_raw(*v, 1000);
            if (g >= 0) cfg.gain = int32_t(g > 1000 ? 1000 : g);
        }
        if (e.id.empty()) {
            video_.play_overlay(cfg);
        } else {
            video_.play_layer(e.id, cfg);
            // Bind the layer to its video channel
            // (内容来源状态绑定,取代旧的保留命名空间 file 串).
            // The host uploads decoded frames under the layer's VideoFrame
            // read key (VideoContent::frame_key) while the channel plays;
            // finish_video/layer removal unbind.
            //
            // In-story effect-strip videos (weather loops etc.)
            // never author their carrier leaf — image.lua's movie branch only
            // lyprops the ancestor chain — so a strict bind makes the whole
            // family silently invisible (btjy story-start snow03.ogv). When
            // the video is a LOOP whose decoded frame-0 content is a sparse
            // dark-canvas overlay (keyed-alpha coverage <= 1/3, judged with
            // the same oa::media::layer_video_key_alpha map the upload host
            // applies), the missing carrier may be auto-materialized: the
            // strip draws at its default full-stage geometry, which is what
            // the effect-strip data intends. Dense strips (snll's
            // recall-montage near-white noise loop) and
            // one-shot wipes/cut-ins keep the strict existing-carrier rule.
            //
            // A loop with a `_m` mask partner (mask_on) is
            // judged on the COMPOSITED veil instead — its frame alpha is
            // final (luma-key(main) × mask gray), so the
            // frame-0 alpha coverage is what the strip will actually draw.
            // snll's recall-montage noise pair (ノイズa: near-white main +
            // sparse gray-grain mask) measures ~9.6% alpha>32 coverage on
            // frame 0 (runtime gate log) — a sparse veil, i.e. the "fog
            // flash" the data intends over the montage (the SOLO near-white
            // loop stays refused; with the mask partner the
            // whiteout can never happen because alpha rides the mask
            // grains).
            bool allow_auto = false;
            if (loop_play) {
                const auto vsnap = video_.state();
                const auto vit = vsnap.video_layers.find(e.id);
                const bool mask_on =
                    vit != vsnap.video_layers.end() && vit->second.mask_on;
                int w = 0, h = 0;
                const uint8_t* rgba = nullptr;
                uint64_t rev = 0;
                if (video_.video_frame(e.id, &w, &h, &rgba, &rev) && rgba &&
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
                    if (std::getenv("OA_VIDEO_DEBUG")) {
                        std::fprintf(stderr,
                                     "[video] story strip gate '%s' loop=1 "
                                     "mask=%d %s-coverage=%.2f%% -> %s\n",
                                     e.id.c_str(), mask_on ? 1 : 0,
                                     mask_on ? "veil-alpha" : "keyed",
                                     100.0 * double(content) / double(n),
                                     allow_auto ? "materialize" : "refuse");
                    }
                }
            }
            const bool bound = scene_.bind_video_layer(e.id, allow_auto);
            if (!bound && loop_play) {
                // A LOOP whose carrier stayed missing is
                // refused by the materialization gates above, and nothing
                // later binds it (the framework never authors the movie
                // leaf). Keeping the channel would decode + mask-composite
                // its frames forever with zero pixels on screen (snll's
                // recall-montage near-white noise strip).
                // Stop it here; a later [video] row for the same id simply
                // re-plays it. One-shot (non-loop) channels keep running
                // unbound: their EOF drives [wait video=...] waits (the
                // montage's シャッター夜_縦 wipe does exactly this).
                video_.stop_layer(e.id);
                if (std::getenv("OA_VIDEO_DEBUG")) {
                    std::fprintf(stderr,
                                 "[video] unbound loop '%s' stopped "
                                 "(no carrier ever binds it)\n",
                                 e.id.c_str());
                }
            }
        }
        ++media_events_;
        return true;
    }
    if (e.tag == "setonvideofinish" || e.tag == "delonvideofinish") {
        const std::string id = param(e, "id") ? *param(e, "id") : std::string();
        if (e.tag == "delonvideofinish") {
            video_.remove_finish_handler(id);
            ++media_events_;
            return true;
        }
        oa::media::VideoFinishHandler h;
        h.file = param(e, "file") ? *param(e, "file") : std::string();
        h.label = param(e, "label") ? *param(e, "label") : std::string();
        h.call = flag_on(param(e, "call") ? *param(e, "call") : "0", false);
        h.handler = param(e, "handler") ? *param(e, "handler") : std::string();
        h.params = e.params; // verbatim extras (function= etc.)
        video_.set_finish_handler(id, std::move(h));
        ++media_events_;
        return true;
    }

    // ---- [splay]/[sstop]/[sfade]/[span]/[sxfade] (BGM) --------------------
    // OA_AUDIO_DEBUG traces the BGM control tags (the game's
    // media/bgm.lua emits splay/sfade/span/sxfade/sstop sequences; the exact
    // order and the time= argument decide whether a fade-in survives).
    static const bool audio_dbg = std::getenv("OA_AUDIO_DEBUG") != nullptr;
    if (audio_dbg && (e.tag == "splay" || e.tag == "sstop" || e.tag == "sfade" ||
                      e.tag == "sfadein" || e.tag == "sfadeout" || e.tag == "span" ||
                      e.tag == "sxfade")) {
        std::string kv;
        for (const auto& [k, v] : e.params) kv += " " + k + "=" + v;
        const auto& st = audio_.state();
        std::fprintf(stderr, "[audiotag] t=%llu %s%s | ch raw_gain=%d cur=%.4f fade=%s\n",
                     (unsigned long long)st.clock_ms, e.tag.c_str(), kv.c_str(),
                     st.bgm_channel ? st.bgm_channel->raw_gain : -1,
                     st.bgm_channel ? double(st.bgm_channel->current_gain) : 0.0,
                     (st.bgm_channel && st.bgm_channel->fade) ? "Y" : "-");
    }
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
        audio_.play_bgm(*fp, cfg);
        set_bgm_loop_file(*fp, cfg.loop_play);
        ++media_events_;
        return true;
    }
    if (e.tag == "sstop") {
        const uint64_t time =
            uint64_t(param_i64(e, "time", 0));
        audio_.stop_bgm(time);
        ++media_events_;
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
        audio_.fade_bgm_gain(int32_t(gain), time);
        ++media_events_;
        return true;
    }
    if (e.tag == "span") {
        const int64_t pan = param_i64(e, "pan", 0);
        const uint64_t time =
            uint64_t(param_i64(e, "time", 0));
        audio_.pan_bgm(int32_t(pan), time);
        ++media_events_;
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
        audio_.crossfade_bgm(*fp, cfg);
        set_bgm_loop_file(*fp, cfg.loop_play);
        ++media_events_;
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
            voice_serial_ = voice_serial_ + 1;
            id = "voice:" + std::to_string(voice_serial_);
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
            audio_.play_voice(id, *fp, cfg);
        } else {
            audio_.play_se(id, *fp, cfg);
        }
        ++media_events_;
        return true;
    }
    if (e.tag == "sestop") {
        const std::string id = param(e, "id") ? *param(e, "id") : std::string();
        const uint64_t time =
            uint64_t(param_i64(e, "time", 0));
        audio_.stop_se(id, time);
        ++media_events_;
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
        audio_.fade_se_gain(id, int32_t(gain), time);
        ++media_events_;
        return true;
    }
    if (e.tag == "sepan") {
        const std::string id = param(e, "id") ? *param(e, "id") : std::string();
        const int64_t pan = param_i64(e, "pan", 0);
        const uint64_t time =
            uint64_t(param_i64(e, "time", 0));
        audio_.pan_se(id, int32_t(pan), time);
        ++media_events_;
        return true;
    }
    if (e.tag == "/voice") {
        ++media_events_; // backlog replay link marker; no audio op
        return true;
    }

    // ---- [setonsoundfinish]/[delonsoundfinish] ----------------------------
    if (e.tag == "setonsoundfinish" || e.tag == "delonsoundfinish") {
        const std::string id = param(e, "id") ? *param(e, "id") : std::string();
        if (e.tag == "delonsoundfinish") {
            audio_.remove_sound_finish_handler(id); // "" = BGM
            ++media_events_;
            return true;
        }
        oa::media::SoundFinishHandler h;
        h.file = param(e, "file") ? *param(e, "file") : std::string();
        h.label = param(e, "label") ? *param(e, "label") : std::string();
        h.call = flag_on(param(e, "call") ? *param(e, "call") : "0", false);
        h.handler = param(e, "handler") ? *param(e, "handler") : std::string();
        h.params = e.params; // verbatim extras (function= id fl md ct ...)
        audio_.set_sound_finish_handler(id, std::move(h)); // "" = BGM
        ++media_events_;
        return true;
    }

    // ---- [allsoundstop] ----------------------------------------------------
    if (e.tag == "allsoundstop") {
        audio_.stop_all_sounds();
        if (media_players_) media_players_->stop_all();
        ++media_events_;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// E-mote layers. e:createEmoteLayer enqueues the
// native "emotestatic" tag (lyc2 materializes the carrier chain first); here
// we decode the first PSB file through the project loader and create the
// EmotePlayer (static pose rendered at the requested canvas size, using the
// R1 fit mapping). Player clocks advance every runtime tick (idle breathing /
// foreground expression timelines) and hosts upload each new revision under
// the layer's EmoteCanvas read key (EmoteContent::canvas_key). Lua method
// calls that
// arrive before the layer materializes are queued and replayed here.
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::apply_emote_static(const oa::runtime::Event& e) {
    const auto idIt = e.params.find("id");
    if (idIt == e.params.end() || idIt->second.empty()) return;
    const std::string& id = idIt->second;
    const auto filesIt = e.params.find("files");
    if (filesIt == e.params.end() || filesIt->second.empty()) return;
    // e:createEmoteLayer joins the file list with \x1f (paths may contain
    // backslashes on win archives; only the first file is used).
    const std::string first = filesIt->second.substr(0, filesIt->second.find('\x1f'));
    if (first.empty()) return;

    int w = 1920, h = 1620;
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
        if (std::getenv("OA_EMOTE_DEBUG"))
            std::fprintf(stderr, "[emote] %s: cannot read '%s'\n", id.c_str(), first.c_str());
        return;
    }
    auto player = std::make_shared<oa::emote::EmotePlayer>();
    std::string err;
    if (!player->load(*bytes, w, h, &err)) {
        if (std::getenv("OA_EMOTE_DEBUG"))
            std::fprintf(stderr, "[emote] %s: psb load failed: %s\n", id.c_str(),
                         err.c_str());
        return;
    }
    EmoteLayerState st;
    st.width = player->width();   // actual render buffer (may be half res)
    st.height = player->height();
    st.file = first;
    st.revision = player->revision();
    st.player = std::move(player);
    emote_layers_[id] = std::move(st);
    ++emote_layer_events_;
    // size the carrier (the Lua side lyprops the parent chain) and bind the
    // emote canvas so the scene draws the figure (内容来源状态绑定,取代旧的
    // 保留命名空间 file 串)。
    std::map<std::string, std::string> props;
    props["width"] = std::to_string(w);
    props["height"] = std::to_string(h);
    scene_.set_props(id, std::move(props));
    scene_.bind_emote_layer(id);
    if (std::getenv("OA_EMOTE_DEBUG"))
        std::fprintf(stderr, "[emote] static layer id='%s' %dx%d from '%s'\n",
                     id.c_str(), w, h, first.c_str());
    // replay any method calls the Lua chain made right after createEmoteLayer
    const auto pend = pending_emote_methods_.find(id);
    if (pend != pending_emote_methods_.end()) {
        if (std::getenv("OA_EMOTE_DEBUG"))
            std::fprintf(stderr, "[emote] %s: replay %zu queued methods\n",
                         id.c_str(), pend->second.size());
        for (const auto& m : pend->second) {
            double out = 0;
            std::string sout;
            emote_method_dispatch(id, m.method, m.s, m.s2, m.n1, m.n2, m.n3, &out,
                                  &sout);
        }
        pending_emote_methods_.erase(pend);
    }
}

void GameRuntime::RuntimeState::advance_emote_players(uint64_t delta_ms) {
    if (delta_ms == 0 || emote_layers_.empty()) return;
    for (auto& [id, st] : emote_layers_) {
        if (!st.player) continue;
        // Players whose clock the game drives itself
        // (em:progress every vsync — 甜蜜女友3-style frameworks) must not be
        // auto-advanced as well or their timelines run at double speed; the
        // state revision still tracks the player so host upload pumps see
        // every new pose (the app's emote_pump_frames keys on st.revision).
        if (st.host_clock) {
            const uint64_t rev = st.player->revision();
            if (rev != st.revision) {
                st.revision = rev;
                if (std::getenv("OA_EMOTE_DEBUG"))
                    std::fprintf(stderr, "[emote] %s: host-clock pose revision "
                                         "-> %llu\n",
                                 id.c_str(), (unsigned long long)rev);
            }
            continue;
        }
        const uint64_t before = st.player->revision();
        st.player->advance_ms(delta_ms);
        if (st.player->revision() != before) {
            st.revision = st.player->revision();
            if (std::getenv("OA_EMOTE_DEBUG"))
                std::fprintf(stderr, "[emote] %s: pose revision -> %llu\n", id.c_str(),
                             (unsigned long long)st.revision);
        }
    }
}

bool GameRuntime::RuntimeState::emote_method_dispatch(const std::string& id, const std::string& method,
                                        const std::string& s, const std::string& s2,
                                        double n1, double n2, double n3, double* out,
                                        std::string* sout) {
    auto it = emote_layers_.find(id);
    if (it == emote_layers_.end() || !it->second.player) {
        // Not materialized yet (methods run in the same Lua chunk as
        // e:createEmoteLayer): queue and replay after apply_emote_static.
        PendingEmoteMethod m;
        m.method = method;
        m.s = s;
        m.s2 = s2;
        m.n1 = n1;
        m.n2 = n2;
        m.n3 = n3;
        auto& q = pending_emote_methods_[id];
        if (q.size() < 128) q.push_back(std::move(m));
        if (out) *out = 0;
        if (sout) sout->clear();
        return true;
    }
    oa::emote::EmotePlayer& p = *it->second.player;
    // 甜蜜女友3 e-mote.lua: the game drives the emote clock
    // itself via em:progress(elapsed_60fps_frames) once per vsync
    // (ex.progress at fg-creation + emote.vsync for every live layer). From
    // the first progress call the layer is host-clock-driven and exempt from
    // the runtime auto-advance (double-speed guard, advance_emote_players).
    if (method == "progress") {
        it->second.host_clock = true;
        p.progress(n1);
        return true;
    }
    if (method == "isTimelinePlaying") {
        // 甜蜜女友3 ex.stop queries the named timeline before
        // stopping it (e-mote.lua:1200); playing = active in either slot.
        // The foreground is a parallel LIST — query membership, not
        // just the top entry.
        if (out) *out = p.is_timeline_playing(s) ? 1.0 : 0.0;
        return true;
    }
    if (method == "playTimeline") {
        // playTimeline(label, flags) — the official SDK's second
        // argument (TIMELINE_PLAY_PARALLEL=1). 甜蜜女友3 passes flags=1 on
        // its emote.action gesture path (e-mote.lua:1183 → tg3 手势轨), which
        // must run IN PARALLEL with the expression/voice track instead of
        // replacing it. The Lua bridge already hands us the second argument
        // as n1 (string-first method family, runtime_lua.cpp).
        p.play_timeline(s, int(n1));
    } else if (method == "fadeInTimeline") {
        p.fade_in_timeline(s);
    } else if (method == "stopTimeline") {
        // stop the named timeline when it plays (fg match =
        // pass — internal end, pose kept; idle match = stop — the delta
        // layer ends). 甜蜜女友3 ex.stop relies on the idle match stopping
        // 待機 loops (待機停止 / emreset flows, e-mote.lua:1201).
        // Per-entry stop (a plain pass() would end unrelated
        // parallel foreground entries).
        p.stop_timeline(s);
    } else if (method == "fadeOutTimeline") {
        // 甜蜜女友3 e-mote.lua:309: the named timeline fades
        // out (always-animation switch with a fade > 0). Fade/ratio math is
        // not implemented (no official SDK basis), so the
        // structural part is: end that slot like stopTimeline does (fg ->
        // pass, idle -> stop), leaving the persistent pose untouched.
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
        // KukkoroDays emote.lua:128/144 — the pose save/restore loop bound.
        // The framework's loop is inclusive (`for i = 0, countVariables()`),
        // so getVariableLabelAt must stay callable one past the end.
        if (out) *out = double(p.variable_count());
        return true;
    } else if (method == "getVariableLabelAt") {
        // KukkoroDays emote.lua:129/145 — the i-th label of the file's
        // variable domain (metadata.variableList order); "" out of range.
        if (sout) *sout = p.variable_label_at(int(n1));
        return true;
    } else if (method == "setVariableDiff") {
        // SetVariableDiff(srcLabel, dstLabel, value) — KukkoroDays
        // emote.lua:186/202/213 and vsync:368/376 adjust face_cheek /
        // face_tears by an offset the game derived from the observed value
        // (n1 = value; the dst label is the second string). The srcLabel has
        // no engine-visible role on these paths (research/127 待裁定).
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
