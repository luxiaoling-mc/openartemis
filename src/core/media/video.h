#pragma once
// video domain: the video state backend with REAL decode pumps.
// A channel decodes frames through the
// decode-source seam — Ogg/Theora via TheoraSource and, when built with
// FFmpeg (OA_HAVE_FFMPEG, vcpkg "ffmpeg"), ASF/WMV3 and any other
// libavformat container via FfmpegSource. Both backends and the seam itself
// are implementation details declared in the internal header
// core/media/media_internal.h: this public contract only
// forward-declares what it holds by pointer. Layer video frames are drawn
// through the layer's VideoFrame content binding (场景侧角色实例给出读取域键),
// fullscreen frames
// drawn by the host over the stage. When the file cannot be decoded
// (missing / undecodable), the channel falls back to the documented logical
// behavior (non-loop completes right after it starts) so real-game flows
// never hang.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace oa::media {

class DecodePool;
class VideoSource;      // decode-source seam (core/media/media_internal.h)
class FfmpegAudioSource; // movie-audio companion (media_internal.h, OA_HAVE_FFMPEG)

static constexpr const char* kOverlayVideoId = "@video_overlay";

/// Luma-key mapping for LAYER video frames (effect strips).
/// The upload host keys the decoded frame with this map and the runtime
/// reuses it when deciding whether a missing story-layer carrier may be
/// auto-materialized, so both sides always agree on what a
/// layer video will look like. Canvas blacks (luma <= 16, the compression
/// floor of these strips) become fully transparent; content is already
/// opaque by luma 64 so mid-grey effect strips (btjy weather loops — snow03
/// flake content luma ~16..190) draw as authored. The older 32..224 ramp
/// suppressed every mid-tone strip to near-zero alpha, which
/// made the story-start snowfall invisible.
inline uint8_t layer_video_key_alpha(uint8_t luma) {
    if (luma <= 16) return 0;
    if (luma >= 64) return 255;
    return uint8_t((uint32_t(luma - 16) * 255) / 48);
}

/// The luma used by the key above (Rec.601-style integer luma).
inline uint8_t rgba_luma(const uint8_t* p) {
    return uint8_t((uint32_t(p[0]) * 77 + uint32_t(p[1]) * 150 +
                    uint32_t(p[2]) * 29 + 128) >> 8);
}

/// [video] play config ( VideoConfig).
struct VideoConfig {
    std::string file;
    bool skippable = true;
    bool loop_play = false; // fullscreen only
    std::optional<int32_t> delay_margin_ms; // layer jump-frame threshold
    /// Movie-audio row gain, Artemis raw scale 0..1000 ([video vol=N]).
    /// Absent = full (1000). The framework's Lua layer usually
    /// folds the movie volume bus (conf.movie + master) into the script var
    /// s.videovol, which the runtime syncs into VideoEngine::movie_volume;
    /// this raw row gain is the per-event extra factor on top.
    std::optional<int32_t> gain;
};

/// Registered video-finish callback ([setonvideofinish]).
struct VideoFinishHandler {
    std::string file;
    std::string label;
    bool call = false;
    std::string handler;
    /// Registration instruction params (verbatim, function/id/... extras).
    std::map<std::string, std::string> params;
    bool any() const { return !file.empty() || !label.empty() || !handler.empty(); }
};

struct VideoFinishEvent {
    std::string id; // layer id; "" = fullscreen
    std::optional<VideoFinishHandler> handler;
};

struct VideoChannel {
    std::string id;
    std::string file;
    bool playing = false;
    bool loop_play = false;
    bool skippable = true;
    /// Frame-skip threshold kept verbatim from the play config (layer
    /// jump-frame semantics; the param is kept through the decode path).
    std::optional<int32_t> delay_margin_ms;
    uint64_t position_ms = 0;
    bool decoded = false; // true when a decoder is attached (real playback)
    /// the channel auto-detected a `<stem>_m.<ext>` mask
    /// partner next to the playing file (same directory + stem + "_m", same
    /// extension, resolved through the same loader/magic-path face). While
    /// set, frames handed out by video_frame carry the engine-level
    /// composite alpha (the luma-key of the main picture modulated by the
    /// mask gray) instead of the raw decoder alpha, so hosts must upload
    /// them verbatim (their host-side luma-key step would re-key a picture
    /// whose alpha is already final).
    bool mask_on = false;

    // -- movie audio (container audio of the playing file) ------------------
    /// True when the file carried a decodable audio stream and a movie-audio
    /// source is attached to the channel (audio plays with the picture).
    bool audio_on = false;
    /// [video] row gain, Artemis raw scale 0..1000 (default 1000).
    int32_t audio_raw_gain = 1000;
    /// Total interleaved stereo frames (44100 Hz) produced for this channel.
    uint64_t audio_frames = 0;
    /// Frames whose post-gain samples exceed the audibility epsilon (a
    /// non-silent stream keeps this close to audio_frames; diagnostics).
    uint64_t audio_active_frames = 0;
    /// Peak |sample| of everything produced (pre-clamp, post-gain).
    float audio_peak = 0.0f;
};

/// Video state backend with a Theora decode pump ( VideoStateBackend
/// + real decoder).
class VideoEngine {
public:
    VideoEngine() = default;
    ~VideoEngine();

    VideoEngine(const VideoEngine&) = delete;
    VideoEngine& operator=(const VideoEngine&) = delete;

    /// Asset bytes loader (wired by the runtime: magic-path resolution +
    /// project fs, same face as the audio players). Absent loader or a
    /// non-decodable file ⇒ logical immediate-finish fallback.
    using Loader = std::function<std::optional<std::vector<uint8_t>>(const std::string& file)>;
    void set_loader(Loader loader) { loader_ = std::move(loader); }

    // -- movie audio --------------------------------------------------------
    /// Output hook for the decoded movie audio (interleaved stereo f32,
    /// 44100 Hz). The hook receives already-gained frames and may be called
    /// from the video driver thread, so it must be thread-safe (hosts push
    /// into the same SDL_AudioStream the media players use). Null/unset =
    /// decode + counters still run, samples are dropped (headless/tests).
    using AudioOutput = std::function<void(const float* interleaved_stereo,
                                           size_t frames)>;
    void set_audio_output(AudioOutput out) { audio_output_ = std::move(out); }
    /// deterministic movie-stream flush hook. Unlike the
    /// ">250 ms push gap" heuristic, this fires at the
    /// deterministic lifecycle edges — before a movie starts pushing, and
    /// when the last audio-bearing channel reaches EOF or is stopped — so a
    /// finished movie can never leave a tail behind (EOF has no "next push"
    /// for the heuristic to see). Invoked with the engine lock held; the host
    /// wires it to MediaPlayers::flush_video_audio (a no-op without a device).
    using AudioFlush = std::function<void()>;
    void set_audio_flush(AudioFlush f) { audio_flush_ = std::move(f); }
    /// Movie volume bus (linear 0..1). The runtime syncs it from the script
    /// var s.videovol ([video] frameworks fold conf.movie + conf.master into
    /// it, raw 0..1000) — atomic so the driver thread reads it lock-free.
    void set_movie_volume(float v) {
        movie_volume_.store(v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v),
                            std::memory_order_relaxed);
    }
    float movie_volume() const { return movie_volume_.load(std::memory_order_relaxed); }

    /// Attach the decode pool (optional). When present, every real decode
    /// channel runs on one pool worker: the worker decodes the SAME
    /// deterministic frame sequence (including loop restarts) into a
    /// double-buffered pipe one frame ahead of the caller; the caller
    /// copies the frame its virtual clock asks for and falls back to its
    /// own synchronous decoder whenever the worker lags or is unavailable.
    /// Null = plain synchronous decode (headless/tests stay byte-identical).
    void set_decode_pool(DecodePool* pool) { pool_ = pool; }

    // -- fullscreen -----------------------------------------------------------
    void play_overlay(const VideoConfig& config);
    bool stop_overlay();
    bool is_overlay_playing() const;

    // -- layers ---------------------------------------------------------------
    void play_layer(const std::string& id, const VideoConfig& config);
    bool stop_layer(const std::string& id);
    /// Stop every layer channel whose id equals `prefix` or sits below it
    /// (scene subtree deletion: cgdel / ui-group teardown / whole-scene
    /// clears). A layer video whose scene carrier is gone has nothing to
    /// draw; letting it run would decode + mask-composite forever
    /// (the title petal channel kept pumping into the story,
    /// the recall-montage strip kept looping after its cgdel). Returns how
    /// many channels were stopped. Overlay channels are untouched.
    size_t stop_layer_subtree(const std::string& prefix);
    bool is_layer_playing(const std::string& id) const;

    // -- global ---------------------------------------------------------------
    void stop_all_videos();

    // -- finish handlers ------------------------------------------------------
    void set_finish_handler(const std::string& id,
                            VideoFinishHandler handler);
    void remove_finish_handler(const std::string& id);

    // -- frame loop -----------------------------------------------------------
    void update(uint64_t delta_ms);
    std::vector<VideoFinishEvent> poll_finish_events();

    struct State {
        std::optional<VideoChannel> overlay_video;
        std::map<std::string, VideoChannel> video_layers;
        std::optional<VideoFinishHandler> finish_handler;      // global/fullscreen
        std::map<std::string, VideoFinishHandler> layer_finish_handlers;
        uint64_t clock_ms = 0;
    };
    /// Thread-safe snapshot of the engine state (copy under mutex).
    State state() const;

    // 旧的 layer_texture_name 保留命名空间
    // 键工厂(`__video_layer__:<id>`)已删除 —— 层视频帧的读取域键由场景侧
    // 角色实例给出(VideoContent::frame_key(层 id),名 = 通道 id = 层 id),
    // 上传方(app)与读取方(渲染)同源单点;media 层不再认识任何层名拼写。
    // Luma-keying is decided by the upload caller (layer vs overlay), never by
    // sniffing the name.

    /// Current decoded RGBA frame of a channel (`id` "" = fullscreen).
    /// Returns false when the channel has no current frame (not playing /
    /// decode fallback / before the first frame). `revision` (optional)
    /// increments on every decoded frame — hosts compare it to detect new
    /// frames for texture upload.
    bool video_frame(const std::string& id, int* w, int* h, const uint8_t** rgba,
                     uint64_t* revision) const;

    /// Per-channel decoded-frame revision (0 when none).
    uint64_t frame_revision(const std::string& id) const;

    /// Queue the finish event natural EOF would produce (`id` "" =
    /// fullscreen). Public so hosts / input-skip can force-finish a
    /// skippable channel exactly like EOF: the runtime
    /// dispatch poll converts it into the finish handlers + wait release.
    void queue_finish(const std::string& id);

    /// Self-driving clock: a dedicated thread advances position_ms by wall
    /// time so video tick is independent of the main loop.
    void start_driver();
    void stop_driver();
    bool driver_active() const { return driver_running_.load(); }

private:
    void driver_loop();
    void ensure_driver();
    void shutdown_driver_if_idle();
    void queue_finish_locked(const std::string& id);
    bool stop_overlay_unlocked();
    bool stop_layer_unlocked(const std::string& id);
    struct DecodeState {
        // Out-of-line special members (video.cpp): DecodeState owns a
        // FfmpegAudioSource unique_ptr whose type is only complete in
        // video.cpp (OA_HAVE_FFMPEG), so implicit in-header destruction
        // would break every other TU that includes video.h.
        DecodeState();
        ~DecodeState();
        DecodeState(DecodeState&&) noexcept;
        DecodeState& operator=(DecodeState&&) noexcept;

        std::unique_ptr<VideoSource> source; // theora or ffmpeg backend
        double frame_interval_ms = 1000.0 / 30.0; // fps fallback 30
        double next_frame_ms = 0.0;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;
        uint64_t revision = 0;
        uint64_t main_seq = 0; // frames the sync decoder pulled (incl. 0)

        // Mask partner of a masked channel: the `<stem>_m`
        // video's own decoder, stepped exactly one frame per DELIVERED main
        // frame (index lock, caller thread) so the pair stays phase-locked
        // regardless of the delivery path (pipe worker / sync decoder) and
        // of loop restarts on equal-length files. Defined out-of-line in
        // video.cpp (unique_ptr keeps the incomplete type out of this TU).
        struct MaskPartner;
        std::unique_ptr<MaskPartner> mask;

        // Movie audio: the container's audio stream, decoded
        // on this (main/driver) thread in step with the channel clock. Only
        // present when OA_HAVE_FFMPEG and the file carried decodable audio.
#if defined(OA_HAVE_FFMPEG) && OA_HAVE_FFMPEG
        std::unique_ptr<FfmpegAudioSource> audio;
#endif
        /// Fractional 44100 Hz output-frame accumulator (media players'
        /// out_frame_frac_ pattern: every pump call produces the frames due
        /// for the delta that just elapsed).
        double audio_frac = 0.0;
        /// Audio source reached EOF (or its own loop restarted it already).
        bool audio_done = false;
        /// Output scratch (2 * chunk frames).
        std::vector<float> audio_buf;

        // Decode-pool pipe (null = plain synchronous decode) ----------------
        struct Pipe {
            std::mutex mu;
            std::condition_variable cv;
            std::vector<uint8_t> buf[2]; // double buffer, one frame lookahead
            int w = 0;
            int h = 0;
            uint64_t produced = 0; // last worker-decoded frame sequence (1-based)
            uint64_t consumed = 1; // frames already delivered (frame 0 exists)
            bool eof = false;      // worker gave up (non-loop end / restart fail)
            bool cancel = false;
        };
        std::shared_ptr<Pipe> pipe;
    };
    DecodeState* decode_of(const std::string& id);
    const DecodeState* decode_of(const std::string& id) const;
    /// Start real decode for a channel; returns true when a decoder attached
    /// (frame 0 decoded). On failure the caller keeps logical behavior.
    bool start_decode(VideoChannel& channel, DecodeState& ds);
    /// Probe the `<stem>_m` sibling of the channel's file and attach it as
    /// the mask partner. Returns true when a decodable mask
    /// with matching dimensions attached; frame 0 of the pair is composited
    /// before returning so the first video_frame already shows the masked
    /// picture. A missing / undecodable / size-mismatched sibling leaves the
    /// channel byte-identical to the unmasked pipeline.
    bool attach_mask(VideoChannel& channel, DecodeState& ds);
    /// Advance the mask partner by one frame (index lock with the frame the
    /// caller just delivered) and bake the composite alpha into the current
    /// main frame. No-op for unmasked channels.
    void step_mask(const VideoChannel& channel, DecodeState& ds);
    /// Advance one channel's decode by the wall position (finish events go
    /// through finish_queue_).
    void pump_channel(VideoChannel& channel, DecodeState& ds);
    /// Produce the movie audio due for `delta_ms` on one channel (decodes +
    /// converts the container track, applies gain, pushes to the output
    /// hook, updates the channel audio counters).
    void pump_channel_audio(VideoChannel& channel, DecodeState& ds,
                            uint64_t delta_ms);
    /// Retire a channel's pipe worker (sync decode takes over).
    void cancel_pipe(DecodeState& ds);
    /// Spawn the decode-pool pipe worker for a fresh decode state.
    void start_pipe(DecodeState& ds, const std::string& file, bool loop_play);
    /// deterministic movie-stream flush. The movie stream is
    /// shared by every audio-bearing video channel (the audio-bearing split), so
    /// the flush only runs when no OTHER playing channel still feeds it
    /// (except = the channel that is going away / the one about to start).
    /// Engine lock must be held.
    void flush_movie_audio_locked(const std::string& except);

    Loader loader_;
    DecodePool* pool_ = nullptr; // optional decode-pool worker host
    AudioOutput audio_output_;   // movie-audio sink
    AudioFlush audio_flush_;     // movie-stream flush
    std::atomic<float> movie_volume_{1.0f}; // movie volume bus (s.videovol)
    State state_;
    std::map<std::string, DecodeState> decode_;
    std::vector<VideoFinishEvent> finish_queue_;

    // Self-driving clock ---------------------------------------------------
    mutable std::mutex engine_mutex_;
    std::thread driver_thread_;
    std::atomic<bool> driver_stop_{false};
    std::atomic<bool> driver_running_{false};
    std::condition_variable driver_cv_;
    std::chrono::steady_clock::time_point driver_last_tick_;

    // Main-thread frame staging (driver never writes here).
    // video_frame() copies ds->rgba → staging under lock, then returns a
    // pointer to the staging buffer. Valid until the next video_frame() call
    // for the same channel id.
    mutable std::map<std::string, std::vector<uint8_t>> frame_staging_;
    mutable std::map<std::string, uint64_t> revision_staging_;
};

} // namespace oa::media
