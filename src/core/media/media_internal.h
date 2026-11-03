#pragma once
// core/media INTERNAL declarations — not a public contract. "Small interface, big file": the public media headers keep only what their call...
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

/// container-audio companion of FfmpegSource (movie sound). Opens the SAME bytes through its own demux and decodes the container's best audi...
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

// --------------------------------------------------------------------------- §4 audio output sink (implementation part of the playback host).
void sink_diag_after_push(int source, SDL_AudioStream* stream, size_t frames);


/// SDL post-mix hook body (defined in audio.cpp §3 — it also
/// feeds the OA_AUDIO_REC device capture). Counts the callbacks where a live
/// bound stream ended up empty, i.e. the device had to pad silence.
void SDLCALL sink_postmix_cb(void* ud, const SDL_AudioSpec* spec, float* buffer,
                             int buflen);

// --------------------------------------------------------------------------- audio output sink: pushes the engine's 44100 Hz stereo f32 mi...
class AudioSink {
    /// Stereo interleaved float frames at 44100 Hz. Null sink = silent
    /// (decode still advances so EOF/loop behavior stays deterministic).
public:
    enum { kStreamMix = 0, kStreamMovie = 1, kStreamCount = 2 };
    /// hard latency bound per bound stream, in 44100 Hz stereo
    enum : size_t {
        kMixBoundFrames = 4410,   // 100 ms
        kMovieBoundFrames = 5292, // 120 ms
    };
    /// safety valve for the channel mix. The mix pacer in
    /// MediaPlayers::update already keeps the level at the jitter cushion
    /// below; this only fires for a genuine multi-second backlog (an app
    /// freeze), where draining is no longer the better trade and an instant
    /// resync is.
    enum : size_t { kMixSafetyFrames = 66150 }; // 1.5 s
    /// Jitter cushion the mix pacer holds ON TOP of the producer's own
    enum : size_t { kMixJitterFrames = 44100 * 160 / 1000 }; // 160 ms
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
