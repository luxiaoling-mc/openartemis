#pragma once
// audio domain — public contract.
//
// "Small interface, big file": this is the ONE audio header. It carries the
// sound logic state machine (AudioEngine + its config/state structs)
// AND the playback host (MediaPlayers); the merged
// implementation lives in core/media/audio.cpp (§1 state machine, §2 vorbis
// decode source, §3 playback host). The implementation-only declarations of
// the playback host (AudioSink, sink_diag_after_push) moved to the
// internal header core/media/media_internal.h, so this header no longer needs
// the SDL audio or the libvorbis C headers at all. VorbisSource itself stays
// declared here: MediaPlayers::Player owns one through std::unique_ptr and the
// implicit-instantiation chain (make_unique<MediaPlayers> -> map default ctor
// -> node destruction) needs the complete type in every TU that instantiates
// MediaPlayers; internalising it would require out-of-line special members on
// Player.
struct OggVorbis_File;

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace oa::media {

class AudioSink;   // device-stream sink owned by MediaPlayers (media_internal.h)
class DecodePool;  // optional decode-worker host (core/media/decode_pool.h)

// ---------------------------------------------------------------------------
// §1 sound logic state machine: SoundCategory/FadeState/
// SoundChannel/BgmConfig/SeConfig/SoundFinishHandler/SoundFinishEvent,
// AudioEngine and ab_loop_file.
// ---------------------------------------------------------------------------

/// Sound category: decides its volume bus and finish-handler lookup.
enum class SoundCategory { Bgm, Se, Voice };

/// Linear-gain/pan transition state ( FadeState).
struct FadeState {
    float target_gain = 0.0f;
    float target_pan = 0.0f;
    float from_gain = 0.0f;
    float from_pan = 0.0f;
    uint64_t start_ms = 0;
    uint64_t duration_ms = 0;
    bool stop_on_complete = false; // fade-out used to stop the channel

    /// Interpolated value at `now_ms`; finished when elapsed >= duration.
    void current_value(uint64_t now_ms, float* gain, float* pan, bool* finished) const {
        const uint64_t elapsed = now_ms > start_ms ? now_ms - start_ms : 0;
        if (duration_ms == 0 || elapsed >= duration_ms) {
            *gain = target_gain;
            *pan = target_pan;
            *finished = true;
            return;
        }
        const float t = float(elapsed) / float(duration_ms);
        *gain = from_gain + (target_gain - from_gain) * t;
        *pan = from_pan + (target_pan - from_pan) * t;
        *finished = false;
    }
};

/// One playing channel ( SoundChannel).
struct SoundChannel {
    std::string id;
    std::string file;
    SoundCategory category = SoundCategory::Se;
    bool playing = false;
    bool loop_play = false;
    bool skippable = false;
    int32_t raw_gain = 1000; // Artemis raw scale 0-1000
    int32_t raw_pan = 0;     // Artemis raw scale -1000..1000
    float current_gain = 1.0f;
    float current_pan = 0.0f;
    std::optional<FadeState> fade;
    /// A-B loop second segment (splay foo_a.ogg -> foo_b.ogg). Only the BGM
    /// channel uses it: play file once, then loop loop_file forever.
    std::optional<std::string> loop_file;
    /// Channel start time on the audio subsystem clock; [wait se=ID time=N]
    /// counts N from this instant ( started_at_ms).
    uint64_t started_at_ms = 0;

    static float gain_to_linear(int32_t raw) {
        return raw > 0 ? float(raw) / 1000.0f : 0.0f;
    }
    static float pan_to_linear(int32_t raw) {
        if (raw < -1000) raw = -1000;
        if (raw > 1000) raw = 1000;
        return float(raw) / 1000.0f;
    }
};

/// [splay]/[sxfade] config ( BgmConfig; loop defaults on).
struct BgmConfig {
    bool loop_play = true;
    std::optional<int32_t> gain;
    std::optional<int32_t> pan;
    uint64_t fade_in_ms = 0;
    std::optional<int32_t> buffer_size; // kept for shape; playback ignores
};

/// [seplay]/[voice] config ( SeConfig; loop defaults off).
struct SeConfig {
    bool loop_play = false;
    std::optional<int32_t> gain;
    std::optional<int32_t> pan;
    uint64_t fade_in_ms = 0;
    std::optional<int32_t> buffer_size;
    bool skippable = false; // skipped while is_skipping (SE only)
};

/// Registered sound-finish callback ([setonsoundfinish]); id-less = BGM.
struct SoundFinishHandler {
    std::string file;
    std::string label;
    bool call = false; // call stack vs jump when file/label used
    std::string handler; // inline engine tag to run (e.g. "calllua")
    /// Registration instruction params (kept verbatim: function/id/... extras).
    std::map<std::string, std::string> params;
    bool any() const { return !file.empty() || !label.empty() || !handler.empty(); }
};

struct SoundFinishEvent {
    std::string id; // "" for the BGM channel ( None)
    SoundCategory category = SoundCategory::Se;
    std::optional<SoundFinishHandler> handler;
};

/// Sound logic backend ( AudioState + AudioStateBackend).
class AudioEngine {
public:
    AudioEngine() = default;

    void reset();

    // -- BGM (single channel) -------------------------------------------------
    void play_bgm(const std::string& file, const BgmConfig& config);
    bool stop_bgm(uint64_t fade_time_ms);
    void crossfade_bgm(const std::string& file, const BgmConfig& config);
    void fade_bgm_gain(int32_t target_gain_raw, uint64_t time_ms);
    void pan_bgm(int32_t target_pan_raw, uint64_t time_ms);

    // -- SE (id-keyed; id space shared with voice for control ops) -----------
    void play_se(const std::string& id, const std::string& file, const SeConfig& config);
    bool stop_se(const std::string& id, uint64_t fade_time_ms);
    void fade_se_gain(const std::string& id, int32_t target_gain_raw, uint64_t time_ms);
    void pan_se(const std::string& id, int32_t target_pan_raw, uint64_t time_ms);

    // -- Voice ----------------------------------------------------------------
    void play_voice(const std::string& id, const std::string& file, const SeConfig& config);

    // -- Global ---------------------------------------------------------------
    void stop_all_sounds();
    void set_master_volume(float volume);
    void set_bgm_volume(float volume);
    void set_se_volume(float volume);
    void set_skipping(bool skipping);
    bool is_se_playing(const std::string& id) const;
    bool is_bgm_playing() const;
    bool is_sound_playing(const std::string& id) const; // se or voice channel

    // -- Finish handlers ------------------------------------------------------
    void set_sound_finish_handler(const std::string& id /* "" = BGM */,
                                  SoundFinishHandler handler);
    void remove_sound_finish_handler(const std::string& id /* "" = BGM */);

    // -- Frame loop -----------------------------------------------------------
    /// Update the subsystem clock and process fade progress.
    void update(uint64_t delta_ms);
    /// Drain queued completion events (fade-out stops). Natural end-of-file
    /// completions are NOT modeled here: the playback host reports them.
    std::vector<SoundFinishEvent> poll_finish_events();

    // -- State ----------------------------------------------------------------
    struct State {
        std::optional<SoundChannel> bgm_channel;
        std::map<std::string, SoundChannel> se_channels;
        std::map<std::string, SoundChannel> voice_channels;
        float master_volume = 1.0f;
        float bgm_volume = 1.0f;
        float se_volume = 1.0f;
        float voice_volume = 1.0f;
        std::optional<SoundFinishHandler> bgm_finish_handler;
        std::map<std::string, SoundFinishHandler> se_finish_handlers;
        uint64_t clock_ms = 0;
        bool is_skipping = false;
    };
    const State& state() const { return state_; }
    State& state() { return state_; }

private:
    /// Queue one channel's fade-out-stop completion into pending_finish_.
    void queue_channel_finish(const SoundChannel& channel);
    State state_;
    std::vector<SoundFinishEvent> pending_finish_;
};

/// A-B loop segment naming ([splay] foo_a.ogg -> foo_b.ogg; splay.md).
std::optional<std::string> ab_loop_file(const std::string& file);

// ---------------------------------------------------------------------------
// §2 vorbis decode source: per-channel streaming decode.
// ---------------------------------------------------------------------------
// Ogg Vorbis decode source over an in-memory file image, using libvorbisfile
// (Vorbis::vorbisfile) with memory callbacks. The engine reads whole asset
// files through its virtual filesystem (PFS), so no OS file I/O happens here.
// Decode is planar float at the file's native rate/channels; resampling and
// mixing live in oa::media::MediaPlayers.
class VorbisSource {
public:
    ~VorbisSource();

    /// Open an ogg/vorbis stream from `bytes`. Returns nullptr when the data
    /// is not a decodable vorbis stream (the caller then treats the channel
    /// as unplayable).
    static std::unique_ptr<VorbisSource> open(std::vector<uint8_t> bytes);

    int channels() const { return channels_; }
    long rate() const { return rate_; }

    /// Decode up to `max_frames` frames of planar float samples. planar must
    /// have room for channels*max_frames floats: channel c occupies
    /// planar[c*max_frames + f]. Returns frames decoded (0 = EOF, <0 = error).
    long read(float* planar, long max_frames);

    /// Seek back to the first PCM frame (loop restart).
    bool seek_zero();

private:
    VorbisSource() = default;
    OggVorbis_File* vf_ = nullptr;
    std::vector<uint8_t> bytes_; // keeps the backing memory alive
    int channels_ = 0;
    long rate_ = 0;
};

// ---------------------------------------------------------------------------
// §3 playback host: MediaPlayers turns AudioEngine channels
// into actual audio through streaming vorbis players + the SDL device sink.
// ---------------------------------------------------------------------------
class MediaPlayers {
public:
    static constexpr int kOutputRate = 44100;

    // base
    bool init();
    void release();
    ~MediaPlayers(); // cancels prefetch workers before the players die

    /// Resolve + read a whole media asset (logical name, magic paths already
    /// applied by the caller's loader). Return nullopt when missing/unreadable.
    void set_loader(std::function<std::optional<std::vector<uint8_t>>(const std::string&)> l) {
        loader_ = std::move(l);
    }

    /// Attach the decode pool (optional). When present, active channels
    /// decode ahead on pool workers (byte-identical source frames; the
    /// mixer still runs on the caller's thread and falls back to the
    /// synchronous path whenever the worker lags). Null = plain
    /// synchronous decode (headless/tests stay byte-identical).
    void set_decode_pool(DecodePool* pool) { pool_ = pool; }

    /// Natural completion of a non-loop channel: (category, channel id; the
    /// BGM id is "").
    using FinishFn = std::function<void(SoundCategory, const std::string&)>;

    /// Drive every currently-playing channel for delta_ms (call once per tick,
    /// after AudioEngine::update).
    void update(uint64_t delta_ms, AudioEngine& engine, const FinishFn& on_finish);

    /// External audio feed into the SAME device stream the channel mixer
    /// uses — the movie-audio path (VideoEngine pushes its
    /// decoded container sound here). SDL_AudioStream serializes internally,
    /// so the video driver thread may call this while the tick thread mixes
    /// channels. Frames are already gained/clamped by the producer; a null
    /// sink (headless) drops them silently.
    void push_video_audio(const float* interleaved_stereo, size_t frames);

    /// dump the OA_AUDIO_DIAG delivery summary (no-op when the
    /// diagnostics never ran). `label` names the dump in the output.
    void print_audio_diag(const char* label) const;

    /// frames queued on one bound device stream (0 without a
    /// device). The queued level IS the audible latency — the device plays
    /// the queue in order, so a standing queue is a standing delay. `source`:
    /// 0 = channel mix (BGM/SE/voice), 1 = movie container audio.
    size_t audio_level_frames(int source) const;

    /// device callbacks where a LIVE bound stream was found
    /// empty after the device pulled — the shortfall was padded with silence,
    /// i.e. a hole in the middle of the sound (the measured form of the "every
    /// BGM has a little noise" report). Counted on the device side through
    /// SDL's post-mix hook; `source` as in audio_level_frames.
    uint64_t device_underruns(int source) const;
    /// device callbacks (post-mix invocations) this run.
    uint64_t device_callbacks() const;

    /// deterministic movie-stream flush (movie start, EOF and
    /// every stop path call it through the VideoEngine hook). Drops the
    /// buffered tail so a finished/stopped movie can never keep playing over
    /// the next scene, independent of any timing heuristic.
    void flush_video_audio();

    /// negative control (tests/probes): turn the bounded push
    /// off to reproduce the pre-fix ratcheting behaviour in-process. Default
    /// bounded; `OA_AUDIO_NO_BOUND=1` does the same from the environment.
    void set_audio_bound_enabled(bool on);

    /// queued level the mix pacer aims at (one device
    /// period + the producer's own tick chunk, floored at 2 periods and
    /// capped at 4; 0 without a device). Diagnostics/tests only.
    size_t mix_target_frames() const;
    /// Ticks whose mix frames were withheld because the stream still carried
    /// more than the target (the latency-drain counter).
    uint64_t paced_out_ticks() const { return paced_out_ticks_; }
    /// catch-up frames produced on top of the wall-clock chunk
    /// to restore the level to the target (and the ticks that did it).
    uint64_t refill_frames() const { return refill_frames_; }
    uint64_t refill_ticks() const { return refill_ticks_; }
    /// negative control (`OA_AUDIO_NO_REFILL=1`): restore the
    /// one-sided pacer (withhold only, level parks where it lands) so
    /// a run can measure the dropouts the refill removes.
    void set_mix_refill_enabled(bool on);
    bool mix_refill_enabled() const { return mix_refill_enabled_; }

    /// Tear down every player (all-sound-stop / reset paths).
    void stop_all();

    /// Number of active decode players (diagnostics/tests).
    size_t active_players() const { return players_.size(); }

    // One decoding channel (implementation detail; definition needed by the
    // runtime dtor through the unique_ptr member).
    struct Player {
        // Creation signature (recreate the player when any of these change).
        SoundCategory category = SoundCategory::Se;
        std::string id; // channel id; empty for BGM
        std::string file; // logical file name being decoded
        bool loop_play = false;
        std::optional<std::string> loop_file;
        uint64_t started_at_ms = 0;

        // Decode state -----------------------------------------------------------
        std::unique_ptr<VorbisSource> src;
        bool unplayable = false;   // file missing / not a decodable vorbis stream
        bool silent_forever = false; // loop source gave up (B segment missing…)
        bool in_b_segment = false; // A-B loop currently in the B segment
        bool reported = false;     // natural completion already reported
        long avail_ch = 1;         // channels fed to the interpolator (1 or 2)
        long src_ch = 1;           // real source channels
        double rate = 44100.0;

        // Streaming resampler state ---------------------------------------------
        double p = 0.0; // next output position, in logical source frames
        std::vector<float> dq; // pulled frames, interleaved (avail_ch each)
        size_t dq_start = 0;   // logical index of dq.front
        size_t zero_from = (size_t)-1; // first logical index that is silence
        std::vector<float> planar;   // decode scratch (src_ch * kChunkFrames)
        size_t planar_frames = 0;
        size_t planar_off = 0;
        uint64_t pull_seq = 0; // frames the sync decoder produced (incl. discards)

        // Decode-pool prefetch (null = pure synchronous decode) --------------
        struct Prefetch {
            std::mutex mu;
            std::condition_variable cv;
            std::vector<float> ring; // interleaved avail_ch floats, cap frames
            size_t cap = 0;          // ring capacity in frames
            size_t produced = 0;     // total frames the worker decoded
            size_t consumed = 0;     // frames the mixer already took
            bool eof = false;        // worker reached end of the file
            bool cancel = false;     // player replaced/stopped: exit the worker
        };
        std::shared_ptr<Prefetch> prefetch;
    };

private:
    std::map<std::string, std::unique_ptr<Player>> players_;
    std::function<std::optional<std::vector<uint8_t>>(const std::string&)> loader_;
    DecodePool* pool_ = nullptr; // optional decode-pool worker host
    AudioSink* sink_ = nullptr;
    double out_frame_frac_ = 0.0; // sub-frame accumulator of the output clock
    uint64_t paced_out_ticks_ = 0; // latency-drain ticks
    // two-sided pacer state.
    size_t last_tick_frames_ = 0;       // producer cadence (the tick's chunk)
    uint64_t refill_frames_ = 0;        // catch-up frames produced
    uint64_t refill_ticks_ = 0;         // ticks that produced a catch-up
    bool mix_refill_enabled_ = true;    // OA_AUDIO_NO_REFILL=1: pre-125 control
};

} // namespace oa::media
