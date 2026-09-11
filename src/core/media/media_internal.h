#pragma once
// core/media INTERNAL declarations — not a public contract.
//
// "Small interface, big file": the public media headers keep only what their
// callers name (audio.h: AudioEngine + MediaPlayers; video.h: VideoEngine and
// its channel/config types; decode_pool.h; image.h). Everything the public
// headers do NOT need to name lives here — the decode-source seam and the
// backend classes, collected from the former core/media/{video_source,
// theora,vorbis,ffmpeg_source}.h headers plus the implementation-only part of
// the media players.
//
// Only the media implementation TUs include this header:
//   audio.cpp — AudioSink, sink_diag_after_push
//   video.cpp — VideoSource, TheoraSource, FfmpegSource/FfmpegAudioSource
// (VorbisSource is the one backend that stays declared in the public
// core/media/audio.h, see its banner.)
// (same shape as render/render_internal.h.)
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_timer.h>

struct OggVorbis_File;

namespace oa::media {

// ---------------------------------------------------------------------------
// §1 decode-source seam — whole-file video backends.
// ---------------------------------------------------------------------------
class VideoSource {
public:
    virtual ~VideoSource() = default;

    /// Displayed (picture) width of the decoded frames.
    virtual int width() const = 0;
    /// Displayed (picture) height of the decoded frames.
    virtual int height() const = 0;
    /// Nominal frame rate (frames per second; 0 when unknown).
    virtual double frame_rate() const = 0;
    /// True once the stream reported end-of-stream.
    virtual bool eos() const = 0;

    /// Decode the next frame (in stream order). False at EOF / stream error.
    virtual bool read_frame() = 0;
    /// RGBA32 view of the last decoded frame (width*height*4 bytes,
    /// alpha = 255; valid until the next read_frame/seek_zero).
    virtual const std::vector<uint8_t>& rgba() const = 0;

    /// Restart from the beginning (loop playback): re-initializes the demux
    /// and decoder state over the same bytes.
    virtual bool seek_zero() = 0;
};

// ---------------------------------------------------------------------------
// §2 theora decode source.
// ---------------------------------------------------------------------------
class TheoraSource : public VideoSource {
public:
    TheoraSource(const TheoraSource&) = delete;
    TheoraSource& operator=(const TheoraSource&) = delete;
    ~TheoraSource() override;

    /// Open an ogg/theora stream from `bytes` (validates the container and
    /// the theora headers). Returns nullptr when the data carries no
    /// decodable theora stream (callers then treat the channel as
    /// unplayable — the logical immediate-finish fallback).
    static std::unique_ptr<TheoraSource> open(std::vector<uint8_t> bytes);

    /// Displayed (picture) width of the decoded frames.
    int width() const override { return width_; }
    /// Displayed (picture) height of the decoded frames.
    int height() const override { return height_; }
    /// Nominal frame rate (fps_numerator / fps_denominator; 0 when unknown).
    double frame_rate() const override { return frame_rate_; }
    /// True once the stream reported end-of-stream.
    bool eos() const override { return eos_; }

    /// Decode the next frame (in stream order). False at EOF / stream error.
    bool read_frame() override;
    /// RGBA32 view of the last decoded frame (width*height*4 bytes,
    /// alpha = 255; valid until the next read_frame/seek_zero).
    const std::vector<uint8_t>& rgba() const override { return rgba_; }

    /// Restart from the beginning (loop playback): re-initializes the demux
    /// and decoder state over the same bytes.
    bool seek_zero() override;

private:
    TheoraSource() = default;
    /// Full demux + header/decoder init over bytes_ (open + seek_zero).
    bool init_stream();
    /// Feed input (or drain buffered pages at input end) until a page of the
    /// theora stream is pagein'ed. Returns false when nothing more can be
    /// fed (caller treats that as EOF / failure).
    bool feed_stream_page();

    struct Impl;
    std::unique_ptr<Impl> p_;

    std::vector<uint8_t> bytes_;
    size_t in_pos_ = 0; // read cursor into bytes_
    bool eos_ = false;
    int width_ = 0;  // display (picture) dimensions
    int height_ = 0;
    double frame_rate_ = 0.0;
    std::vector<uint8_t> rgba_;
};

// ---------------------------------------------------------------------------
// §3 optional ffmpeg decode sources (guard preserved).
// ---------------------------------------------------------------------------
#if defined(OA_HAVE_FFMPEG) && OA_HAVE_FFMPEG

/// container-audio companion of FfmpegSource (movie sound).
/// Opens the SAME bytes through its own demux and decodes the container's
/// best audio stream (AAC in the mp4 movies of rr/snll, wmav2 in the ASF
/// brand movies of NekoMiko, vorbis inside ogg — whatever libavcodec can
/// decode). PCM is converted in-engine (the vcpkg "ffmpeg" port has no
/// swresample: avcodec+avformat only) to the engine output format every
/// movie-audio consumer expects — interleaved stereo f32 at 44100 Hz
/// (MediaPlayers::kOutputRate), the format of the AudioSink device stream:
/// sample format -> f32, channel map (1 = duplicate, 2 = pass, >2 = average
/// to mono then duplicate, the media players' convention), and sample-rate
/// conversion by streaming linear interpolation (the MediaPlayers idiom,
/// audio.cpp §3). AAC
/// decoder priming (~2 frames of container start) is not trimmed. VideoEngine
/// attaches one such source per real decode
/// channel whose file carries audio; the theora-only build never sees this
/// class (compiled only when OA_HAVE_FFMPEG, same gate as FfmpegSource).
class FfmpegAudioSource {
public:
    FfmpegAudioSource(const FfmpegAudioSource&) = delete;
    FfmpegAudioSource& operator=(const FfmpegAudioSource&) = delete;
    ~FfmpegAudioSource();

    /// Open a demuxable audio stream from `bytes`. Returns nullptr when the
    /// data carries no decodable audio stream (video-only files, or any
    /// build without FFmpeg: callers then play the movie silently).
    static std::unique_ptr<FfmpegAudioSource> open(std::vector<uint8_t> bytes);

    /// Source sample rate (Hz) of the decoded audio stream.
    long rate() const { return rate_; }
    /// Source channel count of the decoded audio stream.
    int channels() const { return channels_; }
    /// True once the stream reported end-of-stream.
    bool eos() const { return eos_; }

    /// Decode + convert up to `max_frames` frames of INTERLEAVED stereo f32
    /// at 44100 Hz into `interleaved` (room for max_frames*2 floats).
    /// Returns frames written (0 = end of stream / error).
    long read(float* interleaved, long max_frames);

    /// Restart from the beginning (loop playback): tears the demux/decoder
    /// down and re-initializes over the same bytes.
    bool seek_zero();

private:
    FfmpegAudioSource() = default;
    bool init_stream();
    /// Decode one AVFrame and append its converted samples to the window.
    /// False when the decoder is drained or a frame is unsupported.
    bool refill_frame();

    struct Impl;
    std::unique_ptr<Impl> p_;

    std::vector<uint8_t> bytes_;
    bool eos_ = false;
    int rate_ = 0;
    int channels_ = 0;
};

class FfmpegSource : public VideoSource {
public:
    FfmpegSource(const FfmpegSource&) = delete;
    FfmpegSource& operator=(const FfmpegSource&) = delete;
    ~FfmpegSource() override;

    /// Open a demuxable video stream from `bytes` (probes the container,
    /// opens the first video stream's decoder). Returns nullptr when the
    /// data carries no decodable video stream (callers then keep the
    /// logical immediate-finish fallback).
    static std::unique_ptr<FfmpegSource> open(std::vector<uint8_t> bytes);

    /// Displayed (picture) width of the decoded frames.
    int width() const override { return width_; }
    /// Displayed (picture) height of the decoded frames.
    int height() const override { return height_; }
    /// Nominal frame rate (frames per second; 0 when unknown).
    double frame_rate() const override { return frame_rate_; }
    /// True once the stream reported end-of-stream.
    bool eos() const override { return eos_; }

    /// Decode the next frame (in stream order, demuxing on demand). False at
    /// EOF / stream error.
    bool read_frame() override;
    /// RGBA32 view of the last decoded frame (width*height*4 bytes,
    /// alpha = 255; valid until the next read_frame/seek_zero).
    const std::vector<uint8_t>& rgba() const override { return rgba_; }

    /// Restart from the beginning (loop playback): tears the demux/decoder
    /// down and re-initializes over the same bytes.
    bool seek_zero() override;

private:
    FfmpegSource() = default;
    /// Full demux + decoder init over bytes_ (open + seek_zero).
    bool init_stream();

    struct Impl;
    std::unique_ptr<Impl> p_;

    std::vector<uint8_t> bytes_;
    bool eos_ = false;
    int width_ = 0; // display (picture) dimensions
    int height_ = 0;
    double frame_rate_ = 0.0;
    std::vector<uint8_t> rgba_;
};

#endif // OA_HAVE_FFMPEG

// ---------------------------------------------------------------------------
// §4 audio output sink (implementation part of the playback host).
// ---------------------------------------------------------------------------
/// one-sample delivery watermark sampled after
/// every SDL_PutAudioStreamData into a device stream (`source` picks the
/// bound stream: 0 = channel mix, 1 = movie container audio). Implemented in
/// audio.cpp §3; a no-op unless the environment enables the diagnostics. The
/// host prints the collected summary through
/// MediaPlayers::print_audio_diag() at shutdown.
void sink_diag_after_push(int source, SDL_AudioStream* stream, size_t frames);


/// SDL post-mix hook body (defined in audio.cpp §3 — it also
/// feeds the OA_AUDIO_REC device capture). Counts the callbacks where a live
/// bound stream ended up empty, i.e. the device had to pad silence.
void SDLCALL sink_postmix_cb(void* ud, const SDL_AudioSpec* spec, float* buffer,
                             int buflen);

// ---------------------------------------------------------------------------
// audio output sink: pushes the engine's 44100 Hz stereo f32 mix into
// SDL3 audio device streams. Each
// INDEPENDENT audio source gets its own SDL_AudioStream, all bound to the
// same output device (krkrsdl3's multi-stream pattern) — the device mixes
// them, so one source's backlog/underrun can never delay or choke another.
// Source 0 = the tick's channel mix (BGM/SE/voice, fed once per frame),
// source 1 = the movie container audio (VideoEngine driver pump). When no
// audio device is available the host runs the engine silently (no sink
// attached) — state machine, decoding and completion events still run
// (headless/no-device degradation).
// ---------------------------------------------------------------------------
class AudioSink {
    /// Stereo interleaved float frames at 44100 Hz. Null sink = silent
    /// (decode still advances so EOF/loop behavior stays deterministic).
public:
    enum { kStreamMix = 0, kStreamMovie = 1, kStreamCount = 2 };
    /// hard latency bound per bound stream, in 44100 Hz stereo
    /// frames (= queued frames / 44.1 ms). The queued level of a device-bound
    /// stream IS the audible latency: the device plays the queue in order, so
    /// a producer that pushes one wall-clock-sized chunk per tick leaves the
    /// queue at most(max chunk) — a hitch of D ms pushes D ms at once while
    /// the device drains the (small) standing queue dry, and from then on the
    /// queue keeps that level forever (production and consumption are both
    /// 1x, so nothing ever drains it). That "latency ratchet" is the
    /// mechanism removed here: a standing backlog is dropped instead
    /// of being played late. 100 ms mix / 120 ms movie are the tolerance
    /// bands of the two consumers (channel mix: tick-paced; movie: A/V
    /// clock-locked, where the queued level IS the audible A/V offset).
    enum : size_t {
        kMixBoundFrames = 4410,   // 100 ms
        kMovieBoundFrames = 5292, // 120 ms
    };
    /// safety valve for the channel mix. The mix pacer in
    /// MediaPlayers::update already keeps the level at ~2 device periods; this
    /// only fires for a genuine multi-second backlog (an app freeze), where
    /// draining is no longer the better trade and an instant resync is.
    enum : size_t { kMixSafetyFrames = 66150 }; // 1.5 s
    ~AudioSink() { close(); }
    bool open() {
        SDL_AudioSpec spec;
        spec.format = SDL_AUDIO_F32;
        spec.channels = 2;
        spec.freq = 44100;
        device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
        if (!device_) return false;
        stream_[kStreamMix] = SDL_CreateAudioStream(&spec, &spec);
        stream_[kStreamMovie] = SDL_CreateAudioStream(&spec, &spec);
        if (!stream_[kStreamMix] || !stream_[kStreamMovie] ||
            !SDL_BindAudioStream(device_, stream_[kStreamMix]) ||
            !SDL_BindAudioStream(device_, stream_[kStreamMovie])) {
            close();
            return false;
        }
        SDL_ResumeAudioDevice(device_);
        // the device-side truth is only visible after the mix.
        // SDL calls this with the final device buffer (all bound streams
        // summed, a starved stream already padded with silence) — the engine's
        // one chance to count dropouts without hardware-level capture.
        SDL_SetAudioPostmixCallback(device_, &sink_postmix_cb, this);
        // remember the device period — the mix pacer keeps the
        // queue at a small multiple of it (the device pulls one period per
        // callback and pads a shortfall with silence).
        int frames = 0;
        SDL_AudioSpec got;
        if (SDL_GetAudioDeviceFormat(device_, &got, &frames) && frames > 0) {
            device_frames_ = size_t(frames);
        }
        return true;
    }
    /// Channel-mix feed (per-frame tick mix of BGM/SE/voice). Latency policy
    /// lives in the producer (MediaPlayers::update's pacer): the mix sources
    /// are continuous (BGM/SE/voice), so withholding production is seamless
    /// and there is nothing to drop. Only the multi-second backlog safety
    /// valve is enforced here.
    void push(const float* interleaved_stereo, size_t frames) {
        SDL_AudioStream* st = stream_[kStreamMix];
        if (!st || frames == 0) return;
        if (bound_ && queued_frames(kStreamMix) > kMixSafetyFrames) {
            SDL_ClearAudioStream(st);
            ++resyncs_[kStreamMix];
        }
        push_to(kStreamMix, interleaved_stereo, frames);
    }
    /// Movie container-audio feed (VideoEngine pump, any thread). The pump's
    /// frames are clock-locked to the picture's position_ms, so the queued
    /// level IS the audible A/V offset: the bound is HARD — a chunk that
    /// would push the level past the cap first drops the stale head (clear +
    /// keep the newest cap frames), which resyncs the sound to the current
    /// picture instead of carrying the offset to the end of the movie.
    void push_movie(const float* interleaved_stereo, size_t frames) {
        SDL_AudioStream* st = stream_[kStreamMovie];
        if (!st || frames == 0) return;
        if (bound_ && queued_frames(kStreamMovie) + frames > kMovieBoundFrames) {
            SDL_ClearAudioStream(st);
            ++resyncs_[kStreamMovie];
            if (frames > kMovieBoundFrames) { // keep the NEWEST frames only
                interleaved_stereo += (frames - kMovieBoundFrames) * 2;
                frames = kMovieBoundFrames;
            }
        }
        push_to(kStreamMovie, interleaved_stereo, frames);
    }
    /// Drop the movie stream's buffered tail (a stopped/skipped movie's
    /// leftovers must never play over the next sound).
    void clear_movie() {
        if (stream_[kStreamMovie]) SDL_ClearAudioStream(stream_[kStreamMovie]);
    }
    /// diagnostics/tests: frames currently queued on one bound
    /// stream (0 without a device). This is the audible latency in frames.
    size_t queued_frames(int source) const {
        SDL_AudioStream* st = stream_[source];
        if (!st) return 0;
        const int q = SDL_GetAudioStreamQueued(st); // bytes (SDL3: no framesize)
        return q > 0 ? size_t(q) / (2 * sizeof(float)) : 0;
    }
    /// how often the bound had to drop a standing backlog.
    uint64_t resyncs(int source) const {
        return (source >= 0 && source < kStreamCount) ? resyncs_[source] : 0;
    }
    /// negative control: disable the bound (ratcheting
    /// behaviour) so a test can prove it distinguishes fixed from unfixed.
    void set_bound_enabled(bool on) { bound_ = on; }
    /// signal-face capture: SDL's post-mix hook is installed
    /// unconditionally (cheap: two level queries per device callback) so the
    /// DEVICE-side truth — how often a bound stream ran dry and had to be
    /// padded with silence — is countable in any run, not just a capture run.
    /// `source`: 0 = channel mix, 1 = movie container audio. Only counted
    /// while that stream has been pushed to within the last 250 ms (an idle
    /// stream is empty by definition, not starved).
    uint64_t underruns(int source) const {
        return (source >= 0 && source < kStreamCount) ? underruns_[source] : 0;
    }
    uint64_t device_callbacks() const { return device_callbacks_; }
    void note_push(int source) {
        if (source >= 0 && source < kStreamCount) last_push_ms_[source] = SDL_GetTicks();
    }
    /// Body of the post-mix hook (audio.cpp): counts the drained-dry events.
    void note_device_pull(const SDL_AudioSpec* spec, int buflen);
    bool bound_enabled() const { return bound_; }
    /// Device period in frames (one callback's worth); 0 when unknown. The
    /// mix pacer keeps the queue at ~2 periods so the device never starves.
    size_t device_frames() const { return device_frames_; }
    void close() {
        for (int i = 0; i < kStreamCount; ++i) {
            if (stream_[i]) {
                SDL_UnbindAudioStream(stream_[i]);
                SDL_DestroyAudioStream(stream_[i]);
                stream_[i] = nullptr;
            }
        }
        if (device_) {
            SDL_CloseAudioDevice(device_);
            device_ = 0;
        }
    }
    bool ok() const { return device_ != 0; }

private:
    void push_to(int source, const float* interleaved_stereo, size_t frames) {
        SDL_AudioStream* st = stream_[source];
        if (!st || frames == 0) return;
        SDL_PutAudioStreamData(st, interleaved_stereo,
            (int)(frames * 2 * sizeof(float)));
        note_push(source);
        // delivery diagnostics (OA_AUDIO_DIAG) — sample the
        // stream water level after every put (defined in audio.cpp §3; no-op
        // unless the env var enables it).
        sink_diag_after_push(source, st, frames);
    }
    /// bounded push. Before appending, a STANDING backlog
    /// (queued > cap) is dropped, so the stream can never carry a permanent
    /// delay: the queue of a wall-clock-paced producer does not drain by
    /// itself (its push chunk size equals what the device consumed in the
    /// same interval), so one hitch is enough to delay every later sound for
    /// the rest of the session. The bound drops the STALE audio, never the
    /// frames being produced: the movie pump's frames belong to the current
    /// picture, so the newest frames are the correct ones to keep.
    void push_bounded(int source, const float* interleaved_stereo, size_t frames,
                      size_t cap) {
        SDL_AudioStream* st = stream_[source];
        if (!st || frames == 0) return;
        if (bound_ && queued_frames(source) > cap) {
            SDL_ClearAudioStream(st);
            ++resyncs_[source];
        }
        push_to(source, interleaved_stereo, frames);
    }
    SDL_AudioDeviceID device_ = 0;
    SDL_AudioStream* stream_[kStreamCount] = {};
    uint64_t resyncs_[kStreamCount] = {};
    size_t device_frames_ = 0; // one device callback's worth
    bool bound_ = true; // OA_AUDIO_NO_BOUND / set_bound_enabled(false)
    uint64_t underruns_[kStreamCount] = {};   // drained-dry count
    uint64_t device_callbacks_ = 0;           // post-mix callbacks
    uint64_t last_push_ms_[kStreamCount] = {}; // "stream is live"
};


} // namespace oa::media
