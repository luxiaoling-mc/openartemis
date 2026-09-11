// video domain — ONE translation unit.
//
// "Small interface, big file": the public contract is core/media/video.h
// (VideoEngine + the video channel/config/finish types); the decode backends
// live here. This file merges three parts:
//
//   the video channel engine, decode pump, mask partner,
//   movie-audio pump and self-driving clock;
//   the always-compiled Ogg/Theora decode source;
//   the optional FFmpeg backend (FfmpegSource +
//   FfmpegAudioSource), compiled only when CMake detects
//   FFmpeg; the WHOLE section keeps its original
//   `#if defined(OA_HAVE_FFMPEG)` gate, so a build without
//   FFmpeg still gets the Theora-only surface.
//
// The declarations of the backend classes live in the internal header
// core/media/media_internal.h
// (video.h itself keeps only the public contract and forward-declares the
// backend types it holds by pointer).
//
// Partitions: §1 video engine, §2 theora decode source, §3 ffmpeg decode
// sources (OA_HAVE_FFMPEG).
#include "core/media/video.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ogg/ogg.h>
#include <theora/theoradec.h>

#include "core/media/decode_pool.h"
#include "core/media/media_internal.h"

namespace oa::media {


// ---------------------------------------------------------------------------
// §1 video engine — VideoEngine channels/pump/mask.
// ---------------------------------------------------------------------------


// the `<stem>_m` mask-partner stream of a masked channel —
// its own decoder, plus the current frame state. Complete here (before the
// out-of-line DecodeState special members) so the destructor sees the full
// type; VideoSource is complete through media_internal.h above.
struct VideoEngine::DecodeState::MaskPartner {
    std::unique_ptr<VideoSource> src; // decoder over the `_m` asset
    int w = 0;                        // current mask frame dimensions
    int h = 0;
    bool frozen = false; // stream ended: hold the last frame, never advance
};

namespace {

/// logical sibling name under the `_m` convention — same
/// directory + stem + "_m" + same extension ("movie/sakura.ogv" ->
/// "movie/sakura_m.ogv"; extension-less names get "_m" appended). The probe
/// result goes back through the loader's magic-path/resolve face, so
/// pfs-pack entries and loose-directory files follow the same rule.
std::string mask_sibling_name(const std::string& file) {
    const size_t slash = file.find_last_of("/\\");
    const size_t dot = file.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < file.size() &&
        (slash == std::string::npos || dot > slash)) {
        return file.substr(0, dot) + "_m" + file.substr(dot);
    }
    return file + "_m";
}

/// per-pixel composite bake, the video-domain equivalent of
/// the engine's image-mask convention (RenderEngine::texture_for_masked:
/// out.a = file.a * mask-gray / 255, gray = the R channel of the grey mask
/// image; the file's RGB is drawn verbatim). For canvas-strip videos the
/// "file alpha" of the main picture is its luma-key alpha (the same
/// layer_video_key_alpha map the layer upload host applies),
/// and the `_m` partner modulates that visibility. The main RGB is never
/// touched: the mask only gates how much of the (luma-keyed) main picture
/// shows, and it never contributes color.
void bake_video_mask_alpha(uint8_t* rgba, int w, int h, const uint8_t* mask_rgba) {
    const size_t n = size_t(w) * size_t(h);
    for (size_t i = 0; i < n; ++i) {
        const size_t p = i * 4;
        const uint16_t a = layer_video_key_alpha(rgba_luma(&rgba[p]));
        rgba[p + 3] = uint8_t((a * uint16_t(mask_rgba[p])) / 255u);
    }
}

} // namespace

// Out-of-line so the guarded FfmpegAudioSource member is complete here
// (video.cpp includes media_internal.h, which declares it under the same
// OA_HAVE_FFMPEG guard) and no other TU
// ever instantiates DecodeState's destructor with an incomplete type.
VideoEngine::DecodeState::DecodeState() = default;
VideoEngine::DecodeState::~DecodeState() = default;
VideoEngine::DecodeState::DecodeState(DecodeState&&) noexcept = default;
VideoEngine::DecodeState& VideoEngine::DecodeState::operator=(DecodeState&&) noexcept =
    default;

namespace {
std::unique_ptr<VideoSource> open_any_source(const std::vector<uint8_t>& bytes);
} // namespace

void VideoEngine::cancel_pipe(DecodeState& ds) {
    if (!ds.pipe) return;
    {
        std::lock_guard<std::mutex> lk(ds.pipe->mu);
        ds.pipe->cancel = true;
    }
    ds.pipe->cv.notify_all();
    ds.pipe.reset();
}

/// Spawn one pool worker that decodes `file` from frame 1 onward into the
/// double-buffered pipe (frame 0 is decoded synchronously at start_decode).
/// The worker restarts loops exactly like the synchronous pump does, so the
/// delivered frame sequence is deterministic and identical to decoding on
/// the caller's thread.
void VideoEngine::start_pipe(DecodeState& ds, const std::string& file, bool loop_play) {
    if (!pool_ || pool_->threads() <= 0 || !loader_) return;
    auto pf = std::make_shared<DecodeState::Pipe>();
    pf->consumed = 1; // frame 0 already delivered by start_decode
    const auto loader_copy = loader_;
    const bool ok = pool_->submit_long([pf, loader_copy, file, loop_play] {
        auto bytes = loader_copy(file);
        std::unique_ptr<VideoSource> src =
            bytes ? open_any_source(*bytes) : nullptr;
        auto give_up = [&] {
            std::lock_guard<std::mutex> lk(pf->mu);
            pf->eof = true;
            pf->cv.notify_all();
        };
        if (!src) {
            give_up();
            return;
        }
        for (uint64_t seq = 1;; ++seq) {
            // One-frame lookahead: only write slot seq%2 once frame seq-2
            // has been delivered.
            {
                std::unique_lock<std::mutex> lk(pf->mu);
                pf->cv.wait(lk, [&] { return pf->cancel || pf->consumed + 1 >= seq; });
                if (pf->cancel) return;
            }
            if (!src->read_frame()) {
                // EOF: mirror the sync pump's restart rule.
                if (loop_play && src->seek_zero() && src->read_frame()) {
                    // restarted: fall through and deliver this frame as seq
                } else {
                    give_up();
                    return;
                }
            }
            std::lock_guard<std::mutex> lk(pf->mu);
            const size_t px = size_t(src->width()) * size_t(src->height()) * 4;
            const int slot = int(seq % 2);
            if (pf->buf[slot].size() != px) pf->buf[slot].resize(px);
            std::memcpy(pf->buf[slot].data(), src->rgba().data(), px);
            pf->w = src->width();
            pf->h = src->height();
            pf->produced = seq;
            pf->cv.notify_all();
        }
    });
    if (ok) ds.pipe = std::move(pf);
}

VideoEngine::~VideoEngine() {
    stop_driver();
    std::lock_guard<std::mutex> lk(engine_mutex_);
    for (auto& [id, ds] : decode_) cancel_pipe(ds);
}

namespace {
/// Open the first decodable whole-file source for `bytes`.
/// Decoder preference: the always-compiled Ogg/Theora backend first, then
/// the optional FFmpeg backend (any other libavformat container: ASF/WMV3
/// and beyond). Returns nullptr when no backend accepts the data.
std::unique_ptr<VideoSource> open_any_source(const std::vector<uint8_t>& bytes) {
    if (auto s = TheoraSource::open(bytes)) return s;
#if defined(OA_HAVE_FFMPEG) && OA_HAVE_FFMPEG
    if (std::getenv("OA_VIDEO_DEBUG")) {
        std::fprintf(stderr, "[video] not ogg/theora; trying ffmpeg backend (bytes=%zu)\n",
                     bytes.size());
    }
    if (auto s = FfmpegSource::open(bytes)) return s;
#endif
    return nullptr;
}
} // namespace

// -- self-driving clock -------------------------------------------------------
//
// ONE resident driver thread serves every video channel (overlay + layers):
// it is spawned lazily on the first play and lives until the engine is
// destroyed. While anything plays it advances positions by wall time and
// pumps due frames every ~1 ms; when idle it sleeps (10 ms poll) instead of
// being destroyed - play/stop never join the worker (joining from a caller
// holding engine_mutex_ would deadlock against the worker's own lock wait).

void VideoEngine::start_driver() {
    if (driver_thread_.joinable()) return; // resident: start exactly once
    driver_stop_.store(false);
    driver_last_tick_ = std::chrono::steady_clock::now();
    driver_thread_ = std::thread([this] { driver_loop(); });
}

/// Destructor-only stop: signals the resident worker and joins it. Never
/// called from play/stop paths (those only wake the worker).
void VideoEngine::stop_driver() {
    if (!driver_thread_.joinable()) return;
    driver_stop_.store(true);
    driver_cv_.notify_all();
    driver_thread_.join();
    driver_running_.store(false);
}

void VideoEngine::driver_loop() {
    driver_running_.store(true);
    // integer-ms truncation of every measured wall interval
    // systematically under-drives the clocks (a ~0.5 ms average per
    // iteration at sub-2 ms cadence cost the audio pump ~7 % of real time —
    // with the movie audio on its own device stream that deficit became
    // periodic underruns). Sub-millisecond wall time is carried across
    // iterations so the deltas feeding position_ms / audio_frac sum to the
    // exact elapsed wall time.
    uint64_t clock_carry_us = 0;
    while (!driver_stop_.load()) {
        std::unique_lock<std::mutex> lk(engine_mutex_);
        const bool any_playing =
            (state_.overlay_video && state_.overlay_video->playing) ||
            std::any_of(state_.video_layers.begin(), state_.video_layers.end(),
                        [](const auto& p) { return p.second.playing; });
        if (!any_playing) {
            // Idle: stay alive, wake on activity or stop. The tick base is
            // refreshed on every idle pass: wall time spent with nothing
            // playing must never be credited into a later channel's clocks
            // (the first active delta after a title gap used to
            // equal the WHOLE gap — the movie jumped ~16 s at once: its head
            // frames were skipped, the audio pump burst ~16 s into the SDL
            // movie stream (which paced it into a ~2.75 s standing backlog),
            // and that backlog kept playing after the movie EOF).
            driver_last_tick_ = std::chrono::steady_clock::now();
            driver_cv_.wait_for(lk, std::chrono::milliseconds(10));
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        const uint64_t elapsed_us = uint64_t(std::chrono::duration_cast<
            std::chrono::microseconds>(now - driver_last_tick_).count());
        driver_last_tick_ = now;
        clock_carry_us += elapsed_us;
        const uint64_t delta = clock_carry_us / 1000;
        clock_carry_us -= delta * 1000;
        if (delta > 0) {
            state_.clock_ms += delta;
            if (state_.overlay_video) {
                VideoChannel& ch = *state_.overlay_video;
                ch.position_ms += delta;
                if (DecodeState* ds = decode_of(kOverlayVideoId); ds) {
                    pump_channel(ch, *ds);
                    pump_channel_audio(ch, *ds, delta);
                } else if (ch.playing && !ch.loop_play) {
                    ch.playing = false;
                }
            }
            for (auto& [id, ch] : state_.video_layers) {
                (void)id;
                ch.position_ms += delta;
                if (DecodeState* ds = decode_of(id); ds) {
                    pump_channel(ch, *ds);
                    pump_channel_audio(ch, *ds, delta);
                } else if (ch.playing && !ch.loop_play) {
                    ch.playing = false;
                }
            }
        }
        driver_cv_.wait_for(lk, std::chrono::milliseconds(1));
    }
    driver_running_.store(false);
}

void VideoEngine::ensure_driver() {
    if (pool_ && pool_->threads() > 0 && !driver_thread_.joinable()) {
        start_driver();
    }
}

/// Play/stop side: the worker is resident, so this only wakes it - an idle
/// worker parks itself inside driver_loop (no join on this path).
void VideoEngine::shutdown_driver_if_idle() {
    driver_cv_.notify_all();
}
VideoEngine::State VideoEngine::state() const {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    return state_;
}

/// Start real decode for a channel; returns true when a decoder attached
/// (frame 0 decoded). On failure the caller keeps logical behavior (the
/// documented immediate-finish fallback for non-loop videos).
bool VideoEngine::start_decode(VideoChannel& channel, DecodeState& ds) {
    if (!loader_ || channel.file.empty()) {
        if (std::getenv("OA_VIDEO_DEBUG")) {
            std::fprintf(stderr, "[video] no loader wired (file='%s')\n",
                         channel.file.c_str());
        }
        return false;
    }
    const auto bytes = loader_(channel.file);
    if (!bytes || bytes->empty()) {
        if (std::getenv("OA_VIDEO_DEBUG")) {
            std::fprintf(stderr, "[video] loader returned no bytes for '%s'\n",
                         channel.file.c_str());
        }
        return false;
    }
    auto src = open_any_source(*bytes);
    if (!src || !src->read_frame()) {
        if (std::getenv("OA_VIDEO_DEBUG")) {
            std::fprintf(stderr, "[video] decode attach failed: loader bytes=%zu\n",
                         bytes->size());
        }
        return false;
    }
    const double fps = src->frame_rate();
    if (fps > 0.0) ds.frame_interval_ms = 1000.0 / fps;
    // Frame 0 is already current: the next frame becomes due one interval
    // after the channel clock starts at 0.
    ds.next_frame_ms = ds.frame_interval_ms;
    ds.width = src->width();
    ds.height = src->height();
    ds.rgba = src->rgba();
    ds.revision = 1;
    ds.main_seq = 1; // frame 0 consumed by the sync decoder
    ds.source = std::move(src);
    channel.decoded = true;
#if defined(OA_HAVE_FFMPEG) && OA_HAVE_FFMPEG
    // Movie audio: when the same bytes carry a decodable
    // audio stream, attach the companion source — the movie plays with its
    // container sound (its own demux/decoder, so the pipe worker's video
    // stream stays independent). Fails silently for video-only files.
    if (!ds.audio && bytes && !bytes->empty()) {
        auto audio = FfmpegAudioSource::open(*bytes);
        if (audio) {
            ds.audio = std::move(audio);
            channel.audio_on = true;
            if (std::getenv("OA_VIDEO_DEBUG")) {
                std::fprintf(stderr,
                             "[video] movie audio attached '%s' %ldHz %dch\n",
                             channel.file.c_str(), ds.audio->rate(),
                             ds.audio->channels());
            }
        }
    }
#endif
    // auto-detect the `<stem>_m` sibling and attach it as the
    // mask partner (frame 0 composite in place on success). A missing /
    // undecodable / size-mismatched sibling leaves the channel byte-identical
    // to the unmasked pipeline.
    attach_mask(channel, ds);
    // Optional decode-pool worker: decodes frame 1+ ahead (frame 0 is
    // current). Falls back to the sync decoder on lag/cancel. The worker
    // only ever decodes the MAIN stream; the mask partner is stepped by the
    // caller (step_mask) one frame per delivered main frame, so both
    // delivery paths stay phase-locked without touching the worker.
    start_pipe(ds, channel.file, channel.loop_play);
    if (std::getenv("OA_VIDEO_DEBUG")) {
        std::fprintf(stderr, "[video] source attached '%s' %dx%d @%.2ffps\n",
                     channel.file.c_str(), ds.width, ds.height,
                     ds.frame_interval_ms > 0.0 ? 1000.0 / ds.frame_interval_ms : 0.0);
    }
    return true;
}

// probe the `<stem>_m` sibling of the playing file through the
// loader's magic-path face and attach it as the mask partner. The mask must
// decode (same Theora/FFmpeg open chain as the main) and its frame
// dimensions must match the main picture — mirroring the image-mask
// convention, a missing / undecodable / size-mismatched mask is ignored and
// the channel behaves exactly as before. On success the frame-0 composite is
// baked right here, so the first video_frame of the channel already carries
// the masked picture.
bool VideoEngine::attach_mask(VideoChannel& channel, DecodeState& ds) {
    if (!loader_ || channel.file.empty()) return false;
    const std::string sibling = mask_sibling_name(channel.file);
    if (sibling == channel.file) return false;
    const auto bytes = loader_(sibling);
    if (!bytes || bytes->empty()) {
        if (std::getenv("OA_VIDEO_DEBUG")) {
            std::fprintf(stderr, "[video] no _m sibling for '%s' (probed '%s')\n",
                         channel.file.c_str(), sibling.c_str());
        }
        return false;
    }
    auto src = open_any_source(*bytes);
    if (!src || !src->read_frame()) {
        if (std::getenv("OA_VIDEO_DEBUG")) {
            std::fprintf(stderr, "[video] _m sibling '%s' not decodable; "
                                 "playing unmasked\n",
                         sibling.c_str());
        }
        return false;
    }
    if (src->width() != ds.width || src->height() != ds.height) {
        if (std::getenv("OA_VIDEO_DEBUG")) {
            std::fprintf(stderr, "[video] _m sibling '%s' dims %dx%d != main "
                                 "%dx%d; mask ignored\n",
                         sibling.c_str(), src->width(), src->height(), ds.width,
                         ds.height);
        }
        return false;
    }
    auto mp = std::make_unique<DecodeState::MaskPartner>();
    mp->src = std::move(src);
    mp->w = ds.width;
    mp->h = ds.height;
    ds.mask = std::move(mp);
    channel.mask_on = true;
    bake_video_mask_alpha(ds.rgba.data(), ds.width, ds.height,
                          ds.mask->src->rgba().data());
    if (std::getenv("OA_VIDEO_DEBUG")) {
        std::fprintf(stderr, "[video] mask partner attached '%s' + '%s' "
                             "%dx%d (composited)\n",
                     channel.file.c_str(), sibling.c_str(), ds.width, ds.height);
    }
    return true;
}

// one mask-partner step per DELIVERED main frame (index lock;
// the pair shares a phase by construction on equal-length files — the
// census pair is 301 == 301 frames). The mask restarts at its
// own EOF exactly when an equal-length main restarts (including the decode-
// pool worker's silent loop wraps, which land on the same element boundary);
// when the mask can no longer advance it freezes at its last frame instead
// of popping the picture back to the unmasked look. Then the composite alpha
// (main luma-key x mask gray) is baked into the current frame. Runs on the
// caller thread right after every frame delivery site (pipe fast path, sync
// pull, sync loop restart), never on the pipe worker.
void VideoEngine::step_mask(const VideoChannel& channel, DecodeState& ds) {
    DecodeState::MaskPartner* mp = ds.mask.get();
    if (!mp || !mp->src) return;
    if (mp->w != ds.width || mp->h != ds.height) {
        // A decoder changing picture size mid-stream would invalidate the
        // per-pixel pairing; drop the partner (never observed in the wild;
        // logged for diagnostics).
        if (std::getenv("OA_VIDEO_DEBUG")) {
            std::fprintf(stderr, "[video] channel '%s' mask dims %dx%d != main "
                                 "%dx%d; dropping mask\n",
                         channel.id.c_str(), mp->w, mp->h, ds.width, ds.height);
        }
        ds.mask.reset();
        return;
    }
    if (!mp->frozen) {
        bool ok = mp->src->read_frame();
        if (!ok && channel.loop_play && mp->src->seek_zero()) {
            ok = mp->src->read_frame(); // equal-length pair: wraps together
        }
        if (!ok) {
            mp->frozen = true;
            if (std::getenv("OA_VIDEO_DEBUG")) {
                std::fprintf(stderr, "[video] channel '%s' mask EOF; frozen at "
                                     "last frame\n",
                             channel.id.c_str());
            }
        } else if (mp->src->width() != ds.width || mp->src->height() != ds.height) {
            if (std::getenv("OA_VIDEO_DEBUG")) {
                std::fprintf(stderr, "[video] channel '%s' mask dims changed "
                                     "mid-stream; dropping mask\n",
                             channel.id.c_str());
            }
            ds.mask.reset();
            return;
        } else {
            mp->w = ds.width;
            mp->h = ds.height;
        }
    }
    // Composite in place: rgb untouched (the mask never colors the main),
    // alpha = luma-key(main) * mask-gray / 255.
    bake_video_mask_alpha(ds.rgba.data(), ds.width, ds.height,
                          mp->src->rgba().data());
}

void VideoEngine::play_overlay(const VideoConfig& config) {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    stop_overlay_unlocked();
    // the previous movie's buffered tail must not play under
    // the new one — deterministic flush at the start edge (the old >250 ms
    // push-gap heuristic could not see a restart whose next push came sooner).
    flush_movie_audio_locked(kOverlayVideoId);
    VideoChannel channel;
    channel.id = kOverlayVideoId;
    channel.file = config.file;
    channel.loop_play = config.loop_play;
    channel.skippable = config.skippable;
    channel.playing = true;
    channel.delay_margin_ms = config.delay_margin_ms;
    channel.audio_raw_gain = config.gain.value_or(1000); // movie audio row gain
    state_.overlay_video = std::move(channel);
    DecodeState ds;
    if (start_decode(*state_.overlay_video, ds)) {
        decode_[kOverlayVideoId] = std::move(ds);
        ensure_driver();
        return;
    }
    if (!config.loop_play) {
        VideoFinishEvent ev;
        ev.id.clear();
        ev.handler = state_.finish_handler;
        finish_queue_.push_back(std::move(ev));
    }
}

/// deterministic movie-stream flush at a lifecycle edge. The
/// movie stream is shared by every audio-bearing video channel, so the flush
/// only runs when nothing else is left feeding it: `except`
/// names the channel that is going away (or the one about to start).
void VideoEngine::flush_movie_audio_locked(const std::string& except) {
    if (!audio_flush_) return;
    bool other = false;
    if (state_.overlay_video && state_.overlay_video->playing &&
        state_.overlay_video->audio_on && kOverlayVideoId != except) {
        other = true;
    }
    for (const auto& [id, ch] : state_.video_layers) {
        if (ch.playing && ch.audio_on && id != except) other = true;
    }
    if (!other) audio_flush_();
}

bool VideoEngine::stop_overlay_unlocked() {
    if (state_.overlay_video) {
        state_.overlay_video.reset();
        const auto it = decode_.find(kOverlayVideoId);
        if (it != decode_.end()) {
            cancel_pipe(it->second);
            decode_.erase(it);
        }
        return true;
    }
    return false;
}

bool VideoEngine::stop_overlay() {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    const bool r = stop_overlay_unlocked();
    // the stream must not keep playing a stopped movie's tail.
    if (r) flush_movie_audio_locked(kOverlayVideoId);
    shutdown_driver_if_idle();
    return r;
}

bool VideoEngine::is_overlay_playing() const {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    return state_.overlay_video && state_.overlay_video->playing;
}

void VideoEngine::play_layer(const std::string& id, const VideoConfig& config) {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    // deterministic flush at the layer start edge (an existing
    // audio-bearing layer keeps the stream: the helper checks the others).
    flush_movie_audio_locked(id);
    VideoChannel channel;
    channel.id = id;
    channel.file = config.file;
    channel.loop_play = config.loop_play;
    channel.skippable = config.skippable;
    channel.playing = true;
    channel.delay_margin_ms = config.delay_margin_ms;
    channel.audio_raw_gain = config.gain.value_or(1000); // movie audio row gain
    state_.video_layers[id] = std::move(channel);
    DecodeState ds;
    if (start_decode(state_.video_layers[id], ds)) {
        decode_[id] = std::move(ds);
        ensure_driver();
        return;
    }
    if (!config.loop_play) {
        VideoFinishEvent ev;
        ev.id = id;
        const auto it = state_.layer_finish_handlers.find(id);
        if (it != state_.layer_finish_handlers.end()) {
            ev.handler = it->second;
        } else {
            ev.handler = state_.finish_handler;
        }
        finish_queue_.push_back(std::move(ev));
    }
}

bool VideoEngine::stop_layer_unlocked(const std::string& id) {
    const auto it = state_.video_layers.find(id);
    if (it == state_.video_layers.end()) return false;
    state_.video_layers.erase(it);
    const auto d = decode_.find(id);
    if (d != decode_.end()) {
        cancel_pipe(d->second);
        decode_.erase(d);
    }
    return true;
}

bool VideoEngine::stop_layer(const std::string& id) {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    const bool r = stop_layer_unlocked(id);
    // same deterministic tail flush as stop_overlay.
    if (r) flush_movie_audio_locked(id);
    shutdown_driver_if_idle();
    return r;
}

size_t VideoEngine::stop_layer_subtree(const std::string& prefix) {
    // Mirror the emote-player release rule (runtime.cpp LayerDelete): a
    // deleted layer subtree releases every layer-video channel whose id
    // equals the deleted id or sits below it. Without this an unbound
    // channel keeps decoding + mask-compositing forever (snll's
    // title petal channel pumped into the story after the title ui
    // group deletion; the recall-montage strip kept looping after its
    // cgdel). Overlay channels are not part of the scene tree.
    std::lock_guard<std::mutex> lk(engine_mutex_);
    size_t n = 0;
    for (auto it = state_.video_layers.begin(); it != state_.video_layers.end();) {
        const std::string& id = it->first;
        const bool inside =
            id == prefix || (id.size() > prefix.size() + 1 &&
                             id.compare(0, prefix.size(), prefix) == 0 &&
                             id[prefix.size()] == '.');
        if (!inside) {
            ++it;
            continue;
        }
        const auto d = decode_.find(id);
        if (d != decode_.end()) {
            cancel_pipe(d->second);
            decode_.erase(d);
        }
        it = state_.video_layers.erase(it);
        ++n;
    }
    // subtree deletion is a stop path too (the
    // title petal / montage strip channels stop here) — flush the movie
    // stream when no audio-bearing channel is left.
    if (n > 0) flush_movie_audio_locked(std::string());
    shutdown_driver_if_idle();
    return n;
}

bool VideoEngine::is_layer_playing(const std::string& id) const {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    const auto it = state_.video_layers.find(id);
    return it != state_.video_layers.end() && it->second.playing;
}

void VideoEngine::stop_all_videos() {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    shutdown_driver_if_idle();
    state_.overlay_video.reset();
    state_.video_layers.clear();
    for (auto& [id, ds] : decode_) cancel_pipe(ds);
    decode_.clear();
    // nothing is playing any more — no tail may survive.
    flush_movie_audio_locked(std::string());
}

void VideoEngine::set_finish_handler(const std::string& id, VideoFinishHandler handler) {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    if (id.empty()) {
        state_.finish_handler = std::move(handler);
    } else {
        state_.layer_finish_handlers[id] = std::move(handler);
    }
}

void VideoEngine::remove_finish_handler(const std::string& id) {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    if (id.empty()) {
        state_.finish_handler.reset();
    } else {
        state_.layer_finish_handlers.erase(id);
    }
}

void VideoEngine::queue_finish_locked(const std::string& id) {
    VideoFinishEvent ev;
    ev.id = id;
    if (!id.empty()) {
        const auto it = state_.layer_finish_handlers.find(id);
        if (it != state_.layer_finish_handlers.end()) {
            ev.handler = it->second;
        } else {
            ev.handler = state_.finish_handler;
        }
    } else {
        ev.handler = state_.finish_handler;
    }
    finish_queue_.push_back(std::move(ev));
}

void VideoEngine::queue_finish(const std::string& id) {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    queue_finish_locked(id);
}

/// Produce the movie audio due for `delta_ms` on one channel: pull the
/// frames the 44100 Hz output clock owes (media players' delta-clock idiom),
/// apply the [video] row gain * the movie volume bus, forward to the output
/// hook and update the channel audio counters. Runs on the same thread that
/// advances the channel clock (driver loop or headless advance), so pacing
/// follows the deterministic tick stream. The audio source has its own demux
/// on the same bytes: the decode-pool pipe worker's video stream never
/// contends with it, and looping audio restarts itself at its own EOF like
/// the video loop restarts at the picture EOF (with a
/// bounded per-iteration drift).
void VideoEngine::pump_channel_audio(VideoChannel& channel, DecodeState& ds,
                                     uint64_t delta_ms) {
    if (!channel.playing || !channel.audio_on || ds.audio_done) return;
    ds.audio_frac += double(delta_ms) * (44100.0 / 1000.0);
    constexpr long kMaxChunk = 8192; // one iteration caps at ~186 ms
    while (channel.playing && ds.audio_frac >= 1.0) {
        long want = long(ds.audio_frac);
        if (want > kMaxChunk) want = kMaxChunk;
        if (ds.audio_buf.size() < size_t(want) * 2) {
            ds.audio_buf.resize(size_t(want) * 2);
        }
        long got = 0;
#if defined(OA_HAVE_FFMPEG) && OA_HAVE_FFMPEG
        got = ds.audio->read(ds.audio_buf.data(), want);
#endif
        if (got <= 0) {
            // End of the audio stream: a looping movie restarts its own
            // audio like the video restarts; a non-loop movie plays out
            // silently to its video EOF (or the movie's video EOF cuts the
            // tail when the track outlives the picture).
            bool restarted = false;
#if defined(OA_HAVE_FFMPEG) && OA_HAVE_FFMPEG
            if (channel.loop_play && ds.audio->seek_zero()) restarted = true;
#endif
            if (!restarted) {
                ds.audio_done = true;
                return;
            }
            continue;
        }
        ds.audio_frac -= double(got);
        // Gain: [video] row gain (Artemis raw 0..1000) * movie volume bus.
        float g = channel.audio_raw_gain > 0 ? float(channel.audio_raw_gain) / 1000.0f
                                             : 0.0f;
        g *= movie_volume_.load(std::memory_order_relaxed);
        if (g != 1.0f) {
            for (long i = 0; i < got * 2; ++i) ds.audio_buf[size_t(i)] *= g;
        }
        constexpr float kAudible = 1e-4f; // ~ -80 dBFS audibility epsilon
        float peak = 0.0f;
        uint64_t active = 0;
        for (long i = 0; i < got; ++i) {
            const float l = ds.audio_buf[size_t(i) * 2];
            const float r = ds.audio_buf[size_t(i) * 2 + 1];
            const float a = std::fabs(l) > std::fabs(r) ? std::fabs(l) : std::fabs(r);
            if (a > kAudible) ++active;
            if (a > peak) peak = a;
        }
        channel.audio_frames += uint64_t(got);
        channel.audio_active_frames += active;
        if (peak > channel.audio_peak) channel.audio_peak = peak;
        if (audio_output_) audio_output_(ds.audio_buf.data(), size_t(got));
    }
}

void VideoEngine::pump_channel(VideoChannel& channel, DecodeState& ds) {
    if (!channel.playing || !ds.source) return;
    // Decode frames whose presentation time has passed (frame 0 is already
    // current when the channel started).
    while (channel.playing && double(channel.position_ms) >= ds.next_frame_ms) {
        const uint64_t want = ds.revision + 1; // next frame sequence number

        // --- decode-pool pipe fast path (worker decodes the same stream) ---
        if (ds.pipe) {
            auto pf = ds.pipe;
            std::unique_lock<std::mutex> lk(pf->mu);
            if (!pf->cancel) {
                // The worker is normally one frame ahead; wait only a
                // hairline for it, then fall back synchronously.
                pf->cv.wait_for(lk, std::chrono::milliseconds(2),
                                [&] { return pf->cancel || pf->eof || pf->produced >= want; });
                if (pf->produced >= want) {
                    const size_t px = size_t(pf->w) * size_t(pf->h) * 4;
                    if (ds.rgba.size() != px) ds.rgba.resize(px);
                    std::memcpy(ds.rgba.data(), pf->buf[want % 2].data(), px);
                    ds.width = pf->w;
                    ds.height = pf->h;
                    ++ds.revision;
                    pf->consumed = want;
                    pf->cv.notify_all();
                    lk.unlock(); // mask step decodes; never under the pipe lock
                    step_mask(channel, ds); // mask partner: one frame per delivered frame
                    ds.next_frame_ms += ds.frame_interval_ms;
                    continue;
                }
                if (pf->eof) {
                    // The worker reached the end (non-loop EOF or a loop
                    // whose restart failed): natural finish, same as the
                    // sync pump's give-up path.
                    if (std::getenv("OA_VIDEO_DEBUG")) {
                        std::fprintf(stderr,
                                     "[video] channel '%s' pipe EOF after "
                                     "%llu decoded frames (pos=%llums)\n",
                                     channel.id.c_str(),
                                     (unsigned long long)ds.revision,
                                     (unsigned long long)channel.position_ms);
                    }
                    lk.unlock();
                    cancel_pipe(ds); // retire; finish follows below
                    channel.playing = false;
                    queue_finish_locked(channel.id == kOverlayVideoId ? std::string()
                                                                       : channel.id);
                    return;
                }
            }
            lk.unlock();
            // Worker lagged / was cancelled: retire the pipe permanently and
            // decode the wanted frame synchronously (same byte stream).
            {
                std::lock_guard<std::mutex> g(pf->mu);
                pf->cancel = true;
                pf->cv.notify_all();
            }
            ds.pipe.reset();
        }

        // --- synchronous decode (authoritative; original semantics) -------
        // The sync decoder restarts at stream zero whenever it runs: discard
        // any frames the pipe already delivered before pulling the wanted
        // sequence element.
        while (ds.main_seq + 1 < want) {
            if (!ds.source->read_frame()) {
                // The sync decoder crossed EOF mid-discard (a pipe worker
                // that already restarted a loop can run past the file):
                // mirror the loop restart rule instead of giving up.
                if (channel.loop_play && ds.source->seek_zero() &&
                    ds.source->read_frame()) {
                    ++ds.main_seq;
                    continue;
                }
                channel.playing = false;
                queue_finish_locked(channel.id == kOverlayVideoId ? std::string()
                                                                   : channel.id);
                return;
            }
            ++ds.main_seq;
        }
        if (ds.source->read_frame()) {
            ds.width = ds.source->width();
            ds.height = ds.source->height();
            ds.rgba = ds.source->rgba();
            ++ds.main_seq;
            ++ds.revision;
            step_mask(channel, ds); // mask partner: one frame per delivered frame
            ds.next_frame_ms += ds.frame_interval_ms;
            continue;
        }
        // EOF (or stream error): loop restarts at the current position,
        // otherwise the channel finishes (the runtime polls the VideoEngine
        // completion event through finish_video).
        if (channel.loop_play && ds.source->seek_zero() && ds.source->read_frame()) {
            ds.width = ds.source->width();
            ds.height = ds.source->height();
            ds.rgba = ds.source->rgba();
            ++ds.main_seq;
            ++ds.revision;
            step_mask(channel, ds); // mask restart rides the main's wrap
            ds.next_frame_ms = double(channel.position_ms) + ds.frame_interval_ms;
            continue;
        }
        channel.playing = false;
        // EOF is the edge the old ">250 ms push gap" heuristic
        // could never see (no further push ever comes) — flush the stream's
        // tail deterministically so nothing keeps playing after the picture
        // (bounded push alone would leave up to kMovieBoundFrames behind).
        flush_movie_audio_locked(channel.id);
        if (std::getenv("OA_VIDEO_DEBUG")) {
            std::fprintf(stderr,
                         "[video] channel '%s' EOF after %llu decoded frames "
                         "(pos=%llums)\n",
                         channel.id.c_str(), (unsigned long long)ds.revision,
                         (unsigned long long)channel.position_ms);
        }
        queue_finish_locked(channel.id == kOverlayVideoId ? std::string() : channel.id);
        return;
    }
}

void VideoEngine::update(uint64_t delta_ms) {
    if (driver_running_.load()) return;
    std::lock_guard<std::mutex> lk(engine_mutex_);
    state_.clock_ms += delta_ms;
    if (state_.overlay_video) {
        VideoChannel& ch = *state_.overlay_video;
        ch.position_ms += delta_ms;
        if (DecodeState* ds = decode_of(kOverlayVideoId); ds) {
            pump_channel(ch, *ds);
            pump_channel_audio(ch, *ds, delta_ms);
        } else if (ch.playing && !ch.loop_play) {
            ch.playing = false;
        }
    }
    for (auto& [id, ch] : state_.video_layers) {
        (void)id;
        ch.position_ms += delta_ms;
        if (DecodeState* ds = decode_of(id); ds) {
            pump_channel(ch, *ds);
            pump_channel_audio(ch, *ds, delta_ms);
        } else if (ch.playing && !ch.loop_play) {
            ch.playing = false;
        }
    }
}

std::vector<VideoFinishEvent> VideoEngine::poll_finish_events() {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    std::vector<VideoFinishEvent> out;
    out.swap(finish_queue_);
    return out;
}

bool VideoEngine::video_frame(const std::string& id, int* w, int* h, const uint8_t** rgba,
                              uint64_t* revision) const {
    const std::string key = id.empty() ? kOverlayVideoId : id;
    std::lock_guard<std::mutex> lk(engine_mutex_);
    const DecodeState* ds = decode_of(key);
    if (!ds || ds->rgba.empty()) return false;
    if (w) *w = ds->width;
    if (h) *h = ds->height;
    auto& staging = frame_staging_[key];
    staging = ds->rgba;
    if (rgba) *rgba = staging.data();
    revision_staging_[key] = ds->revision;
    if (revision) *revision = ds->revision;
    return true;
}

uint64_t VideoEngine::frame_revision(const std::string& id) const {
    std::lock_guard<std::mutex> lk(engine_mutex_);
    const DecodeState* ds = decode_of(id.empty() ? kOverlayVideoId : id);
    return ds ? ds->revision : 0;
}

VideoEngine::DecodeState* VideoEngine::decode_of(const std::string& id) {
    const auto it = decode_.find(id);
    return it == decode_.end() ? nullptr : &it->second;
}

const VideoEngine::DecodeState* VideoEngine::decode_of(const std::string& id) const {
    const auto it = decode_.find(id);
    return it == decode_.end() ? nullptr : &it->second;
}
// ---------------------------------------------------------------------------
// §2 theora decode source — TheoraSource.
// ---------------------------------------------------------------------------


namespace {
/// BT.601 limited-range Y'CbCr -> RGB (>>8 scaled constants).
inline uint8_t clamp8(int v) {
    return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
}

/// Convert one decoded Y'CbCr frame into RGBA32 at picture dimensions.
/// `yuv` planes cover the full encoded frame; the displayed picture starts
/// at (pic_x, pic_y) and is w*h. Chroma is decimated by (cx, cy) shifts
/// according to the stream's pixel format.
void ycbcr_to_rgba(const th_ycbcr_buffer yuv, int pic_x, int pic_y, int w, int h,
                   int cx, int cy, std::vector<uint8_t>& rgba) {
    const unsigned char* Y = yuv[0].data;
    const unsigned char* U = yuv[1].data;
    const unsigned char* V = yuv[2].data;
    const int ys = yuv[0].stride;
    const int us = yuv[1].stride;
    const int vs = yuv[2].stride;
    for (int py = 0; py < h; ++py) {
        const int fy = pic_y + py; // full-frame row of this picture row
        uint8_t* out = rgba.data() + size_t(py) * size_t(w) * 4;
        const unsigned char* yr = Y + size_t(fy) * size_t(ys);
        const unsigned char* ur = U + size_t(fy >> cy) * size_t(us);
        const unsigned char* vr = V + size_t(fy >> cy) * size_t(vs);
        for (int px = 0; px < w; ++px) {
            const int fx = pic_x + px;
            const int yv = int(yr[fx]) - 16;
            const int uv = int(ur[fx >> cx]) - 128;
            const int vv = int(vr[fx >> cx]) - 128;
            out[px * 4 + 0] = clamp8((298 * yv + 409 * vv + 128) >> 8);
            out[px * 4 + 1] = clamp8((298 * yv - 100 * uv - 208 * vv + 128) >> 8);
            out[px * 4 + 2] = clamp8((298 * yv + 516 * uv + 128) >> 8);
            out[px * 4 + 3] = 255;
        }
    }
}
} // namespace

/// Decoder/demux state, kept out of the public header (opaque pimpl) so that
/// video.h consumers never see the ogg/theora C headers. RAII: its own
/// destructor releases every ogg/theora allocation, so replacing the pimpl
/// (seek_zero / re-open) can never leak the previous decoder.
struct TheoraSource::Impl {
    Impl() = default;
    ~Impl() {
        if (dec) th_decode_free(dec);
        if (setup) th_setup_free(setup);
        if (stream_inited) ogg_stream_clear(&stream);
        if (sync_inited) ogg_sync_clear(&sync);
        th_comment_clear(&comment);
        th_info_clear(&info);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ogg_sync_state sync{};      // byte demuxer
    ogg_stream_state stream{};  // the theora logical stream
    bool sync_inited = false;
    bool stream_inited = false;
    long our_serial = 0; // serialno of the theora logical stream

    th_info info{};
    th_comment comment{};
    th_setup_info* setup = nullptr; // caller-owned (th_setup_free)
    th_dec_ctx* dec = nullptr;
    int headers = 0;       // theora header packets consumed (0..3)
    bool stream_eos = false; // EOS page of the theora stream pagein'ed
    // Chroma decimation of the stream's pixel format (shift count: 0 = full,
    // 1 = subsampled by 2 in that direction).
    int chroma_x = 1;
    int chroma_y = 1;
};

TheoraSource::~TheoraSource() = default;

std::unique_ptr<TheoraSource> TheoraSource::open(std::vector<uint8_t> bytes) {
    if (bytes.size() < 27) return nullptr;
    const uint8_t* b = bytes.data();
    if (!(b[0] == 'O' && b[1] == 'g' && b[2] == 'g' && b[3] == 'S')) {
        return nullptr; // not an ogg container
    }
    auto src = std::unique_ptr<TheoraSource>(new TheoraSource());
    src->bytes_ = std::move(bytes);
    if (!src->init_stream()) return nullptr;
    return src;
}

bool TheoraSource::init_stream() {
    p_ = std::make_unique<Impl>();
    in_pos_ = 0;
    eos_ = false;
    th_info_init(&p_->info);
    th_comment_init(&p_->comment);
    if (ogg_sync_init(&p_->sync) != 0) return false;
    p_->sync_inited = true;

    // Scan BOS pages for the Theora logical stream (payload starts with
    // 0x80 'theora').
    bool identified = false;
    while (!identified) {
        if (in_pos_ >= bytes_.size()) return false; // no theora stream
        char* buf = ogg_sync_buffer(&p_->sync, 4096);
        if (!buf) return false;
        const size_t n = std::min<size_t>(4096, bytes_.size() - in_pos_);
        std::memcpy(buf, bytes_.data() + in_pos_, n);
        ogg_sync_wrote(&p_->sync, long(n));
        in_pos_ += n;

        ogg_page page;
        while (ogg_sync_pageout(&p_->sync, &page) > 0) {
            if (!ogg_page_bos(&page)) continue;
            if (page.body_len >= 7 && std::memcmp(page.body, "\x80theora", 7) == 0) {
                p_->our_serial = ogg_page_serialno(&page);
                if (ogg_stream_init(&p_->stream, p_->our_serial) != 0) return false;
                p_->stream_inited = true;
                ogg_stream_pagein(&p_->stream, &page);
                identified = true;
                break;
            }
        }
    }

    // Consume the three theora header packets (identification, comment,
    // setup) through th_decode_headerin.
    while (p_->headers < 3) {
        ogg_packet packet;
        const int r = ogg_stream_packetout(&p_->stream, &packet);
        if (r == 1) {
            if (th_decode_headerin(&p_->info, &p_->comment, &p_->setup, &packet) < 0) {
                return false;
            }
            ++p_->headers;
            continue;
        }
        if (r < 0) return false;
        if (!feed_stream_page()) return false; // truncated headers
    }

    p_->dec = th_decode_alloc(&p_->info, p_->setup);
    if (!p_->dec) return false;
    // The setup data stays caller-owned; the decoder copied what it needs.
    th_setup_free(p_->setup);
    p_->setup = nullptr;

    width_ = int(p_->info.pic_width);
    height_ = int(p_->info.pic_height);
    if (width_ <= 0 || height_ <= 0) return false;
    frame_rate_ = p_->info.fps_denominator > 0 && p_->info.fps_numerator > 0
                      ? double(p_->info.fps_numerator) / double(p_->info.fps_denominator)
                      : 0.0;
    switch (p_->info.pixel_fmt) {
        case TH_PF_420:
            p_->chroma_x = 1;
            p_->chroma_y = 1;
            break;
        case TH_PF_422:
            p_->chroma_x = 1;
            p_->chroma_y = 0;
            break;
        default: // TH_PF_444 and anything unknown: full chroma
            p_->chroma_x = 0;
            p_->chroma_y = 0;
            break;
    }
    rgba_.assign(size_t(width_) * size_t(height_) * 4, 0);
    return true;
}

bool TheoraSource::feed_stream_page() {
    for (;;) {
        if (in_pos_ >= bytes_.size()) {
            // All input fed: drain whatever complete pages remain buffered
            // (ogg_sync_pageout returns 0 once the buffer holds none).
            ogg_page page;
            bool fed = false;
            while (ogg_sync_pageout(&p_->sync, &page) > 0) {
                if (ogg_page_serialno(&page) == p_->our_serial) {
                    ogg_stream_pagein(&p_->stream, &page);
                    if (ogg_page_eos(&page)) p_->stream_eos = true;
                    fed = true;
                }
            }
            return fed;
        }
        char* buf = ogg_sync_buffer(&p_->sync, 4096);
        if (!buf) return false;
        const size_t n = std::min<size_t>(4096, bytes_.size() - in_pos_);
        std::memcpy(buf, bytes_.data() + in_pos_, n);
        ogg_sync_wrote(&p_->sync, long(n));
        in_pos_ += n;

        ogg_page page;
        while (ogg_sync_pageout(&p_->sync, &page) > 0) {
            if (ogg_page_serialno(&page) == p_->our_serial) {
                ogg_stream_pagein(&p_->stream, &page);
                if (ogg_page_eos(&page)) p_->stream_eos = true;
                return true;
            }
        }
    }
}

bool TheoraSource::seek_zero() {
    // Re-run the whole open path over the same bytes (loop playback).
    return init_stream();
}

bool TheoraSource::read_frame() {
    if (eos_ || !p_ || !p_->dec) return false;
    for (;;) {
        ogg_packet packet;
        const int r = ogg_stream_packetout(&p_->stream, &packet);
        if (r == 1) {
            ogg_int64_t granpos = -1;
            if (th_decode_packetin(p_->dec, &packet, &granpos) == 0) {
                th_ycbcr_buffer yuv;
                if (th_decode_ycbcr_out(p_->dec, yuv) == 0) {
                    ycbcr_to_rgba(yuv, int(p_->info.pic_x), int(p_->info.pic_y),
                                  width_, height_, p_->chroma_x, p_->chroma_y, rgba_);
                    return true;
                }
            }
            continue;
        }
        if (r < 0) {
            eos_ = true;
            return false;
        }
        // r == 0: no complete packet buffered. When the stream's EOS page was
        // already consumed there is nothing more to decode.
        if (p_->stream_eos) {
            eos_ = true;
            return false;
        }
        if (!feed_stream_page()) {
            eos_ = true;
            return false;
        }
    }
}
} // namespace oa::media



// ---------------------------------------------------------------------------
// §3 ffmpeg decode sources — FfmpegSource/FfmpegAudioSource (OA_HAVE_FFMPEG gate preserved verbatim).
// ---------------------------------------------------------------------------

#if defined(OA_HAVE_FFMPEG) && OA_HAVE_FFMPEG

// <algorithm>/<cmath>/<cstdio>/<cstdlib>/<cstring> are already included
// unconditionally at the top of this TU, so the ffmpeg section carries no
// duplicates of them (the preprocessed TU is
// unchanged, verified with the guard on and off).
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

namespace oa::media {

namespace {
/// RGBA32 output frame size (bytes) for a picture of w*h.
constexpr size_t rgba_bytes(int w, int h) { return size_t(w) * size_t(h) * 4; }

/// In-memory input backing the custom AVIOContext (whole asset bytes).
struct MemIo {
    const std::vector<uint8_t>* bytes = nullptr;
    size_t pos = 0;
};

int mem_read(void* opaque, uint8_t* buf, int size) {
    auto* io = static_cast<MemIo*>(opaque);
    const std::vector<uint8_t>& b = *io->bytes;
    if (io->pos >= b.size()) return 0; // EOF
    const size_t n = std::min<size_t>(size_t(size), b.size() - io->pos);
    std::memcpy(buf, b.data() + io->pos, n);
    io->pos += n;
    return int(n);
}

int64_t mem_seek(void* opaque, int64_t offset, int whence) {
    auto* io = static_cast<MemIo*>(opaque);
    const std::vector<uint8_t>& b = *io->bytes;
    if (whence == AVSEEK_SIZE) return int64_t(b.size());
    int64_t base = 0;
    if (whence == SEEK_CUR) {
        base = int64_t(io->pos);
    } else if (whence == SEEK_END) {
        base = int64_t(b.size());
    }
    const int64_t np = base + offset;
    if (np < 0 || size_t(np) > b.size()) return -1;
    io->pos = size_t(np);
    return np;
}

/// Best-effort frames-per-second from the stream's rate fields.
double stream_fps(const AVStream* st) {
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
        return double(st->avg_frame_rate.num) / double(st->avg_frame_rate.den);
    }
    if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0) {
        return double(st->r_frame_rate.num) / double(st->r_frame_rate.den);
    }
    if (st->time_base.num > 0 && st->time_base.den > 0 &&
        st->time_base.den != st->time_base.num) {
        return double(st->time_base.den) / double(st->time_base.num);
    }
    return 0.0;
}


/// Convert one decoded planar 8-bit YUV AVFrame into RGBA32 at the frame's
/// dimensions. Supports 8-bit fully-planar YUV with 0..2 chroma decimation
/// (yuv420p / yuv422p / yuv444p and JPEG-range variants); the WMV3/VC-1
/// decoders produce yuv420p. Returns false for any other layout (callers
/// treat the source as unplayable).
bool yuv_frame_to_rgba(const AVFrame* f, std::vector<uint8_t>& rgba, int* out_w,
                       int* out_h) {
    const int w = f->width;
    const int h = f->height;
    if (w <= 0 || h <= 0 || !f->data[0] || !f->data[1] || !f->data[2]) return false;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    if (!d) return false;
    if (d->nb_components != 3 || (d->flags & AV_PIX_FMT_FLAG_RGB) ||
        !(d->flags & AV_PIX_FMT_FLAG_PLANAR)) {
        return false;
    }
    if (d->comp[0].depth != 8 || d->comp[0].step != 1) return false;
    const int cx = d->log2_chroma_w;
    const int cy = d->log2_chroma_h;
    const bool full_range = f->color_range == AVCOL_RANGE_JPEG;

    rgba.assign(rgba_bytes(w, h), 0);
    const unsigned char* Y = f->data[0];
    const unsigned char* U = f->data[1];
    const unsigned char* V = f->data[2];
    const int ys = f->linesize[0];
    const int us = f->linesize[1];
    const int vs = f->linesize[2];
    for (int py = 0; py < h; ++py) {
        uint8_t* out = rgba.data() + size_t(py) * size_t(w) * 4;
        const unsigned char* yr = Y + size_t(py) * size_t(ys);
        const unsigned char* ur = U + size_t(py >> cy) * size_t(us);
        const unsigned char* vr = V + size_t(py >> cy) * size_t(vs);
        for (int px = 0; px < w; ++px) {
            const int yv = int(yr[px]);
            const int uv = int(ur[px >> cx]) - 128;
            const int vv = int(vr[px >> cx]) - 128;
            int r, g, b;
            if (full_range) {
                r = clamp8((256 * yv + 359 * vv + 128) >> 8);
                g = clamp8((256 * yv - 88 * uv - 183 * vv + 128) >> 8);
                b = clamp8((256 * yv + 454 * uv + 128) >> 8);
            } else {
                const int yy = yv - 16;
                r = clamp8((298 * yy + 409 * vv + 128) >> 8);
                g = clamp8((298 * yy - 100 * uv - 208 * vv + 128) >> 8);
                b = clamp8((298 * yy + 516 * uv + 128) >> 8);
            }
            out[px * 4 + 0] = uint8_t(r);
            out[px * 4 + 1] = uint8_t(g);
            out[px * 4 + 2] = uint8_t(b);
            out[px * 4 + 3] = 255;
        }
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return true;
}
} // namespace

/// AVIO/decoder state behind the pimpl (all pointers owned here).
struct FfmpegSource::Impl {
    ~Impl() {
        // CUSTOM_IO keeps fmt from freeing our AVIOContext; free it (and its
        // internal buffer) after the demuxer is gone.
        if (fmt) avformat_close_input(&fmt);
        if (avio) avio_context_free(&avio);
        if (dec) avcodec_free_context(&dec);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
    }

    MemIo io;

    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    AVIOContext* avio = nullptr;
    unsigned char* iobuf = nullptr;
    int vstream = -1;  // video stream index
    int width = 0;     // decoded picture dims (from codecpar / first frame)
    int height = 0;
    bool input_eof = false;  // av_read_frame hit EOF (flush packet sent)
    bool drained = false;    // decoder reported EOF / unrecoverable error
};

FfmpegSource::~FfmpegSource() = default;

std::unique_ptr<FfmpegSource> FfmpegSource::open(std::vector<uint8_t> bytes) {
    if (bytes.size() < 16) return nullptr; // no container can be that small
    auto src = std::unique_ptr<FfmpegSource>(new FfmpegSource());
    src->bytes_ = std::move(bytes);
    if (!src->init_stream()) return nullptr;
    return src;
}

bool FfmpegSource::init_stream() {
    p_ = std::make_unique<Impl>();
    Impl& S = *p_;
    // The engine logs through its own OA_VIDEO_DEBUG diagnostics; keep the
    // decoders' per-frame advisory chatter (e.g. wmv3 "Extra data" notices)
    // off stderr while real errors still surface.
    av_log_set_level(AV_LOG_ERROR);

    constexpr int kIoBufSize = 1 << 16;
    S.iobuf = static_cast<unsigned char*>(av_malloc(kIoBufSize));
    if (!S.iobuf) return false;
    S.io.bytes = &bytes_;
    S.io.pos = 0;
    S.avio = avio_alloc_context(S.iobuf, kIoBufSize, 0, &S.io, mem_read, nullptr,
                                mem_seek);
    if (!S.avio) {
        av_free(S.iobuf);
        S.iobuf = nullptr;
        return false;
    }

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        avio_context_free(&S.avio);
        return false;
    }
    fmt->pb = S.avio;
    // Caller-owned AVIO: avformat_close_input must not free our context
    // (we own avio and its buffer).
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    const int orc = avformat_open_input(&fmt, nullptr, nullptr, nullptr);
    if (orc < 0) {
        // avformat_open_input freed `fmt` on failure; our AVIO is untouched.
        avio_context_free(&S.avio);
        return false;
    }
    S.fmt = fmt;
    if (avformat_find_stream_info(S.fmt, nullptr) < 0) return false;

    const AVCodec* codec = nullptr;
    S.vstream = av_find_best_stream(S.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (S.vstream < 0 || !codec) return false;
    AVStream* st = S.fmt->streams[S.vstream];
    if (!st->codecpar) return false;

    S.dec = avcodec_alloc_context3(codec);
    if (!S.dec) return false;
    if (avcodec_parameters_to_context(S.dec, st->codecpar) < 0) return false;
    S.dec->thread_count = 1; // deterministic, in-order frames
    if (avcodec_open2(S.dec, codec, nullptr) < 0) return false;

    S.frame = av_frame_alloc();
    S.pkt = av_packet_alloc();
    if (!S.frame || !S.pkt) return false;

    S.width = S.dec->width;
    S.height = S.dec->height;
    if (S.width <= 0 || S.height <= 0) {
        if (st->codecpar->width > 0 && st->codecpar->height > 0) {
            S.width = st->codecpar->width;
            S.height = st->codecpar->height;
        } else {
            return false;
        }
    }
    width_ = S.width;
    height_ = S.height;
    rgba_.assign(rgba_bytes(width_, height_), 0);
    frame_rate_ = stream_fps(st);

    if (std::getenv("OA_VIDEO_DEBUG")) {
        std::fprintf(stderr,
                     "[video] ffmpeg source: format=%s codec=%s %dx%d @%.2ffps "
                     "vstream=%d bytes=%zu\n",
                     S.fmt->iformat ? S.fmt->iformat->name : "?",
                     codec->name, width_, height_, frame_rate_, S.vstream,
                     bytes_.size());
    }
    return true;
}

bool FfmpegSource::read_frame() {
    if (!p_ || p_->drained) return false;
    Impl& S = *p_;
    for (;;) {
        const int rr = avcodec_receive_frame(S.dec, S.frame);
        if (rr == 0) {
            // Convert this decoded frame into rgba_ (YUV -> RGBA32, same
            // BT.601 math as the theora backend).
            int fw = 0, fh = 0;
            if (!yuv_frame_to_rgba(S.frame, rgba_, &fw, &fh)) {
                S.drained = true;
                eos_ = true;
                return false; // unsupported pixel format: treat as unplayable
            }
            width_ = fw;
            height_ = fh;
            S.width = fw;
            S.height = fh;
            av_frame_unref(S.frame);
            return true;
        }
        if (rr == AVERROR_EOF) {
            S.drained = true;
            eos_ = true;
            return false;
        }
        if (rr != AVERROR(EAGAIN)) {
            S.drained = true;
            eos_ = true;
            return false; // stream error
        }
        // Decoder wants input.
        if (!S.input_eof) {
            const int ar = av_read_frame(S.fmt, S.pkt);
            if (ar == AVERROR_EOF) {
                S.input_eof = true;
                // Flush the decoder so buffered frames drain out.
                if (avcodec_send_packet(S.dec, nullptr) < 0) {
                    S.drained = true;
                    eos_ = true;
                    return false;
                }
                continue;
            }
            if (ar < 0) {
                S.drained = true;
                eos_ = true;
                return false;
            }
            if (S.pkt->stream_index == S.vstream) {
                if (avcodec_send_packet(S.dec, S.pkt) < 0) {
                    av_packet_unref(S.pkt);
                    S.drained = true;
                    eos_ = true;
                    return false;
                }
            }
            av_packet_unref(S.pkt);
            continue;
        }
        // Input flushed but the decoder still asks for more: drained.
        S.drained = true;
        eos_ = true;
        return false;
    }
}

bool FfmpegSource::seek_zero() {
    p_.reset();
    eos_ = false;
    return init_stream();
}

// ---------------------------------------------------------------------------
// FfmpegAudioSource: container audio companion — decodes the
// best audio stream of the same in-memory bytes and converts it to the
// engine output format (interleaved stereo f32 @ 44100 Hz). No swresample
// exists in the vcpkg feature set, so format/channel/rate conversion happens
// here in-engine: decoded AVFrames -> f32, channel-map to mono/stereo, then a
// streaming linear-interpolation resampler onto 44100 (the MediaPlayers
// interpolation idiom of audio.cpp §3, simplified to the movie-audio
// lifecycle).
// ---------------------------------------------------------------------------

namespace {

/// PCM layout descriptor for the sample formats the movie codecs output
/// (AAC/wmav2/vorbis/pcm -> fltp/s16*/s32*/u8…). Anything outside the table
/// makes the companion give up on that stream (treated as undecodable).
struct FmtSpec {
    int bytes = 0;   // bytes per sample
    bool planar = false;
    bool is_float = false;
    bool is_signed = true;
};

bool fmt_spec_for(AVSampleFormat f, FmtSpec* out) {
    if (!out) return false;
    switch (f) {
        case AV_SAMPLE_FMT_U8: *out = {1, false, false, false}; return true;
        case AV_SAMPLE_FMT_U8P: *out = {1, true, false, false}; return true;
        case AV_SAMPLE_FMT_S16: *out = {2, false, false, true}; return true;
        case AV_SAMPLE_FMT_S16P: *out = {2, true, false, true}; return true;
        case AV_SAMPLE_FMT_S32: *out = {4, false, false, true}; return true;
        case AV_SAMPLE_FMT_S32P: *out = {4, true, false, true}; return true;
        case AV_SAMPLE_FMT_S64: *out = {8, false, false, true}; return true;
        case AV_SAMPLE_FMT_S64P: *out = {8, true, false, true}; return true;
        case AV_SAMPLE_FMT_FLT: *out = {4, false, true, true}; return true;
        case AV_SAMPLE_FMT_FLTP: *out = {4, true, true, true}; return true;
        case AV_SAMPLE_FMT_DBL: *out = {8, false, true, true}; return true;
        case AV_SAMPLE_FMT_DBLP: *out = {8, true, true, true}; return true;
        default: return false;
    }
}

/// Sample c of decoded frame f at frame index i, mapped to float.
float pcm_sample(const AVFrame* f, const FmtSpec& s, int c, int i) {
    const uint8_t* p = f->data[s.planar ? c : 0];
    if (!p) return 0.0f;
    if (s.planar) {
        p += size_t(i) * size_t(s.bytes);
    } else {
        p += (size_t(i) * size_t(f->ch_layout.nb_channels) + size_t(c)) * size_t(s.bytes);
    }
    if (s.is_float) {
        if (s.bytes == 4) {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
        double v;
        std::memcpy(&v, p, 8);
        return float(v);
    }
    if (s.bytes == 1) {
        const int v = int(*p) - 128; // unsigned 8-bit
        return float(v) / 128.0f;
    }
    // Signed integers: / 2^(bits-1).
    const double scale = s.bytes == 2    ? 32768.0
                         : s.bytes == 4  ? 2147483648.0
                                         : 9223372036854775808.0;
    if (s.bytes == 2) {
        int16_t v;
        std::memcpy(&v, p, 2);
        return float(double(v) / scale);
    }
    if (s.bytes == 4) {
        int32_t v;
        std::memcpy(&v, p, 4);
        return float(double(v) / scale);
    }
    int64_t v;
    std::memcpy(&v, p, 8);
    return float(double(v) / scale);
}

} // namespace

struct FfmpegAudioSource::Impl {
    ~Impl() {
        if (fmt) avformat_close_input(&fmt);
        if (avio) avio_context_free(&avio);
        if (dec) avcodec_free_context(&dec);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
    }

    MemIo io;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    AVIOContext* avio = nullptr;
    unsigned char* iobuf = nullptr;
    int astream = -1;     // audio stream index
    bool input_eof = false; // av_read_frame hit EOF (flush sent)
    bool drained = false;   // decoder EOF / unrecoverable error

    // Converted-source pipeline ---------------------------------------------
    int src_ch = 0;        // decoded channel count
    long src_rate = 0;     // decoded sample rate
    std::vector<float> conv;  // scratch: one converted AVFrame (stereo)
    // Window of converted stereo frames at src_rate (only the tail that has
    // not been consumed by the resampler is kept).
    std::vector<float> win;
    size_t wstart = 0;     // logical source-frame index of win[0]
    double pos = 0.0;      // next output position, in logical source frames
};

FfmpegAudioSource::~FfmpegAudioSource() = default;

std::unique_ptr<FfmpegAudioSource> FfmpegAudioSource::open(std::vector<uint8_t> bytes) {
    if (bytes.size() < 16) return nullptr;
    auto src = std::unique_ptr<FfmpegAudioSource>(new FfmpegAudioSource());
    src->bytes_ = std::move(bytes);
    if (!src->init_stream()) return nullptr;
    return src;
}

bool FfmpegAudioSource::init_stream() {
    p_ = std::make_unique<Impl>();
    Impl& S = *p_;
    av_log_set_level(AV_LOG_ERROR);

    constexpr int kIoBufSize = 1 << 16;
    S.iobuf = static_cast<unsigned char*>(av_malloc(kIoBufSize));
    if (!S.iobuf) return false;
    S.io.bytes = &bytes_;
    S.io.pos = 0;
    S.avio = avio_alloc_context(S.iobuf, kIoBufSize, 0, &S.io, mem_read, nullptr,
                                mem_seek);
    if (!S.avio) {
        av_free(S.iobuf);
        S.iobuf = nullptr;
        return false;
    }

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        avio_context_free(&S.avio);
        return false;
    }
    fmt->pb = S.avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    const int orc = avformat_open_input(&fmt, nullptr, nullptr, nullptr);
    if (orc < 0) {
        avio_context_free(&S.avio);
        return false;
    }
    S.fmt = fmt;
    if (avformat_find_stream_info(S.fmt, nullptr) < 0) return false;

    const AVCodec* codec = nullptr;
    S.astream = av_find_best_stream(S.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (S.astream < 0 || !codec) return false;
    AVStream* st = S.fmt->streams[S.astream];
    if (!st->codecpar) return false;

    S.dec = avcodec_alloc_context3(codec);
    if (!S.dec) return false;
    if (avcodec_parameters_to_context(S.dec, st->codecpar) < 0) return false;
    S.dec->thread_count = 1; // deterministic, in-order frames
    if (avcodec_open2(S.dec, codec, nullptr) < 0) return false;

    S.frame = av_frame_alloc();
    S.pkt = av_packet_alloc();
    if (!S.frame || !S.pkt) return false;

    S.src_ch = S.dec->ch_layout.nb_channels;
    if (S.src_ch < 1) S.src_ch = 1;
    S.src_rate = S.dec->sample_rate;
    if (S.src_rate <= 0) return false;
    channels_ = S.src_ch;
    rate_ = S.src_rate;

    if (std::getenv("OA_VIDEO_DEBUG")) {
        std::fprintf(stderr,
                     "[video] movie audio source: format=%s codec=%s %ldHz %dch "
                     "astream=%d bytes=%zu\n",
                     S.fmt->iformat ? S.fmt->iformat->name : "?",
                     codec->name, long(S.src_rate), S.src_ch, S.astream,
                     bytes_.size());
    }
    return true;
}

/// Decode one AVFrame and append its converted stereo f32 samples (native
/// rate) to the window. False when the decoder is drained or the frame's
/// sample format is unsupported (source gives up, like an undecodable video).
bool FfmpegAudioSource::refill_frame() {
    Impl& S = *p_;
    for (;;) {
        if (S.drained) return false;
        const int rr = avcodec_receive_frame(S.dec, S.frame);
        if (rr == 0) {
            FmtSpec spec;
            if (!fmt_spec_for(AVSampleFormat(S.frame->format), &spec) ||
                !S.frame->data[0]) {
                S.drained = true;
                av_frame_unref(S.frame);
                return false;
            }
            const int n = S.frame->nb_samples;
            if (n <= 0) {
                av_frame_unref(S.frame);
                continue; // empty frame: keep decoding
            }
            const int ch = S.frame->ch_layout.nb_channels > 0
                               ? S.frame->ch_layout.nb_channels
                               : S.src_ch;
            const long nch = ch < 1 ? 1 : ch;
            if (S.conv.size() < size_t(n) * 2) S.conv.resize(size_t(n) * 2);
            // Channel map to stereo (MediaPlayers convention, audio.cpp §3:
            // 1 = duplicate, 2 = pass, >2 = average down to mono then
            // duplicate).
            if (nch == 1) {
                for (int i = 0; i < n; ++i) {
                    const float v = pcm_sample(S.frame, spec, 0, i);
                    S.conv[size_t(i) * 2] = v;
                    S.conv[size_t(i) * 2 + 1] = v;
                }
            } else if (nch == 2) {
                for (int i = 0; i < n; ++i) {
                    S.conv[size_t(i) * 2] = pcm_sample(S.frame, spec, 0, i);
                    S.conv[size_t(i) * 2 + 1] = pcm_sample(S.frame, spec, 1, i);
                }
            } else {
                for (int i = 0; i < n; ++i) {
                    float sum = 0.0f;
                    for (long c = 0; c < nch; ++c) {
                        sum += pcm_sample(S.frame, spec, int(c), i);
                    }
                    S.conv[size_t(i) * 2] = sum / float(nch);
                    S.conv[size_t(i) * 2 + 1] = sum / float(nch);
                }
            }
            S.win.insert(S.win.end(), S.conv.begin(), S.conv.begin() + size_t(n) * 2);
            av_frame_unref(S.frame);
            return true;
        }
        if (rr == AVERROR_EOF) {
            S.drained = true;
            return false;
        }
        if (rr != AVERROR(EAGAIN)) {
            S.drained = true;
            return false;
        }
        if (!S.input_eof) {
            const int ar = av_read_frame(S.fmt, S.pkt);
            if (ar == AVERROR_EOF) {
                S.input_eof = true;
                if (avcodec_send_packet(S.dec, nullptr) < 0) {
                    S.drained = true;
                    return false;
                }
                continue;
            }
            if (ar < 0) {
                S.drained = true;
                return false;
            }
            if (S.pkt->stream_index == S.astream) {
                if (avcodec_send_packet(S.dec, S.pkt) < 0) {
                    av_packet_unref(S.pkt);
                    S.drained = true;
                    return false;
                }
            }
            av_packet_unref(S.pkt);
            continue;
        }
        S.drained = true;
        return false;
    }
}

bool FfmpegAudioSource::seek_zero() {
    p_.reset();
    eos_ = false;
    return init_stream();
}

long FfmpegAudioSource::read(float* interleaved, long max_frames) {
    if (!p_ || max_frames <= 0) return 0;
    Impl& S = *p_;
    constexpr double kTargetRate = 44100.0;
    const double ratio = double(S.src_rate) / kTargetRate;
    long made = 0;
    while (made < max_frames) {
        const double pos = S.pos;
        const long idx = long(std::floor(pos));
        // The window must cover the frame at idx (and ideally idx+1 for the
        // interpolation); refill until it does or the stream is drained.
        while (S.wstart + S.win.size() / 2 <= size_t(idx + 1) && !S.drained) {
            if (!refill_frame()) break;
        }
        const size_t wend = S.wstart + S.win.size() / 2;
        if (size_t(idx) >= wend) break; // nothing left for this position
        // Source samples around idx (past the end = silence, the players'
        // zero-fill convention for the final partial frame).
        const float* base = S.win.data();
        const size_t rel = size_t(idx) - S.wstart;
        const float v0l = base[rel * 2];
        const float v0r = base[rel * 2 + 1];
        float v1l = 0.0f, v1r = 0.0f;
        if (rel + 1 < S.win.size() / 2) {
            v1l = base[(rel + 1) * 2];
            v1r = base[(rel + 1) * 2 + 1];
        }
        const float frac = float(pos - double(idx));
        const float l = v0l + (v1l - v0l) * frac;
        const float r = v0r + (v1r - v0r) * frac;
        interleaved[size_t(made) * 2] = l;
        interleaved[size_t(made) * 2 + 1] = r;
        S.pos = pos + ratio;
        ++made;
    }
    // Trim consumed prefix frames (positions only move forward). With a
    // downsampling ratio the output position can step past the window end
    // at EOF; never erase more frames than the window holds.
    const long keep = long(std::floor(S.pos));
    long drop = keep - long(S.wstart);
    const long have = long(S.win.size() / 2);
    if (drop > have) drop = have;
    if (drop > 0) {
        S.win.erase(S.win.begin(), S.win.begin() + size_t(drop) * 2);
        S.wstart += size_t(drop);
    }
    if (made == 0 && S.drained) eos_ = true;
    return made;
}

} // namespace oa::media

#endif // OA_HAVE_FFMPEG
