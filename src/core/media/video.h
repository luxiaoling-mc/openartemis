#pragma once
// video domain: the video state backend with REAL decode pumps. A channel decodes frames through the decode-source seam — Ogg/Theora via Th...
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

namespace oa::render {
class HostTexture; // backend.h：与后端无关的可写纹理
}

namespace oa::media {

class DecodePool;
class VideoSource;      // decode-source seam (core/media/media_internal.h)
class FfmpegAudioSource; // movie-audio companion (media_internal.h, OA_HAVE_FFMPEG)

static constexpr const char* kOverlayVideoId = "@video_overlay";

/// Luma-key mapping for LAYER video frames (effect strips). The upload host keys the decoded frame with this map and the runtime reuses it w...
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
    using AudioOutput = std::function<void(const float* interleaved_stereo,
                                           size_t frames)>;
    void set_audio_output(AudioOutput out) { audio_output_ = std::move(out); }
    /// deterministic movie-stream flush hook. Unlike the
    using AudioFlush = std::function<void()>;
    void set_audio_flush(AudioFlush f) { audio_flush_ = std::move(f); }
    /// Movie volume bus (linear 0..1). The runtime syncs it from the script
    /// var s.videovol ([video] frameworks fold conf.movie + conf.master into
    /// it, raw 0..1000) — atomic so the driver thread reads it lock-free.
    void set_movie_volume(float v) {
        movie_volume_.store(v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v),
                            std::memory_order_relaxed);
    }

    /// 与后端无关的可写纹理（渲染层提供；源 update 后交给宿主挂到层上）。
    using TextureRef = std::shared_ptr<oa::render::HostTexture>;
    using TextureFactory = std::function<TextureRef(int w, int h)>;
    /// 纹理工厂（渲染器注入；只在源线程构造对象，不碰后端）。
    void set_texture_factory(TextureFactory f) { texture_factory_ = std::move(f); }
    /// 一帧解码完成：通道 + 已更新的纹理（宿主负责挂到层上）。
    using FramePush = std::function<void(const std::string& channel,
                                         const TextureRef& tex, int w, int h)>;
    void set_frame_push(FramePush fn) { frame_push_ = std::move(fn); }

    /// Attach the decode pool (optional). When present, every real decode
    void set_decode_pool(DecodePool* pool) { pool_ = pool; }

    // -- fullscreen -----------------------------------------------------------
    void play_overlay(const VideoConfig& config);
    bool stop_overlay();
    bool is_overlay_playing() const;

    // -- layers ---------------------------------------------------------------
    void play_layer(const std::string& id, const VideoConfig& config);
    bool stop_layer(const std::string& id);
    /// Stop every layer channel whose id equals `prefix` or sits below it
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
    FramePush frame_push_;
    TextureFactory texture_factory_;
    std::map<std::string, uint64_t> pushed_rev_;
    std::map<std::string, TextureRef> textures_; // 每通道一块可写纹理

    /// 收集有新版解码帧的通道（调用方持 engine_mutex_；只读状态）。
    void collect_fresh_locked(std::vector<std::pair<std::string, bool>>* fresh) const;
    /// 把新帧推给渲染推送面（锁外调用；video_frame 内部会再次取锁）。
    void push_frames(const std::vector<std::pair<std::string, bool>>& fresh);
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
            // Mask-compositing worker: the pipe worker steps the `_m` mask
            std::unique_ptr<VideoSource> returned_main;
            std::shared_ptr<VideoSource> returned_mask;
            bool returned_mask_frozen = false;
            bool exited = false;
        };
        std::shared_ptr<Pipe> pipe;
    };
    DecodeState* decode_of(const std::string& id);
    const DecodeState* decode_of(const std::string& id) const;
    /// Start real decode for a channel; returns true when a decoder attached
    /// (frame 0 decoded). On failure the caller keeps logical behavior.
    bool start_decode(VideoChannel& channel, DecodeState& ds);
    /// Probe the `<stem>_m` sibling of the channel's file and attach it as
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
    void start_pipe(DecodeState& ds, const std::string& file, bool loop_play,
                    const std::shared_ptr<VideoSource>& mask);
    /// Drop the per-channel frame staging (the detached copy video_frame
    /// hands out). Called when a channel goes away: the staging of a
    /// 1080p channel is 8.3 MB and used to stay allocated for the rest of
    /// the process once the channel stopped.
    void release_channel_frames(const std::string& key);
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
