// audio domain — ONE translation unit.
//
// "Small interface, big file": the public contract is core/media/audio.h
// (AudioEngine + MediaPlayers); the decode backend and the playback host live
// here. This file merges three parts:
//
//   the sound logic state machine (AudioEngine, ab_loop_file);
//   the Ogg Vorbis decode source (VorbisSource);
//   the playback host (AudioSink device streams + MediaPlayers).
//
// The former core/media/players.h / vorbis.h declarations split by audience:
// the contract (VorbisSource included — it must be a
// complete type in every TU that instantiates MediaPlayers) into audio.h, the
// implementation-only types (AudioSink, sink_diag_after_push) into the internal
// header core/media/media_internal.h.
//
// Partitions: §1 sound logic state machine, §2 vorbis decode source,
// §3 playback host (players + SDL output sink).
#include "core/media/audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <thread>
#include <vector>

#include <vorbis/vorbisfile.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

#include "core/media/decode_pool.h"
#include "core/media/media_internal.h"

namespace oa::media {


// ---------------------------------------------------------------------------
// §1 sound logic state machine — AudioEngine.
// ---------------------------------------------------------------------------


std::optional<std::string> ab_loop_file(const std::string& file) {
    // Only look for the extension on the last path segment: dots inside
    // directories must not count.
    const size_t dot = file.rfind('.');
    const size_t last_slash = file.find_last_of("/\\");
    if (dot == std::string::npos || (last_slash != std::string::npos && dot < last_slash)) {
        // No extension: the whole name may still end in _a (magic paths).
        if (file.size() >= 2 && file.compare(file.size() - 2, 2, "_a") == 0) {
            std::string base(file, 0, file.size() - 2);
            return base + "_b";
        }
        return std::nullopt;
    }
    const std::string stem(file, 0, dot);
    const std::string ext(file, dot);
    if (stem.size() >= 2 && stem.compare(stem.size() - 2, 2, "_a") == 0) {
        std::string base(stem, 0, stem.size() - 2);
        return base + "_b" + ext;
    }
    return std::nullopt;
}

namespace {

/// Shared channel constructor: raw gain/pan conversion + optional fade-in.
/// Raw gain/pan conversion shared by every channel (fade-in applied).
SoundChannel build_channel(const std::string& id, const std::string& file,
                           SoundCategory category, bool loop_play, bool skippable,
                           const std::optional<int32_t>& gain,
                           const std::optional<int32_t>& pan, uint64_t fade_in_ms,
                           uint64_t clock_ms) {
    SoundChannel channel;
    channel.id = id;
    channel.file = file;
    channel.category = category;
    channel.playing = true;
    channel.loop_play = loop_play;
    channel.skippable = skippable;
    channel.started_at_ms = clock_ms;
    if (gain) {
        channel.raw_gain = *gain;
        channel.current_gain = SoundChannel::gain_to_linear(*gain);
    }
    if (pan) {
        channel.raw_pan = *pan;
        channel.current_pan = SoundChannel::pan_to_linear(*pan);
    }
    if (fade_in_ms > 0) {
        const float target_gain = channel.current_gain;
        FadeState fade;
        fade.target_gain = target_gain;
        fade.target_pan = channel.current_pan;
        fade.from_gain = 0.0f;
        fade.from_pan = channel.current_pan;
        fade.start_ms = clock_ms;
        fade.duration_ms = fade_in_ms;
        fade.stop_on_complete = false;
        channel.current_gain = 0.0f;
        channel.fade = std::move(fade);
    }
    return channel;
}

void apply_channel_fade_out(SoundChannel* channel, uint64_t time_ms, uint64_t clock_ms) {
    if (time_ms == 0) {
        channel->playing = false;
        channel->current_gain = 1.0f;
        channel->fade.reset();
        return;
    }
    FadeState fade;
    fade.target_gain = 0.0f;
    fade.target_pan = channel->current_pan;
    fade.from_gain = channel->current_gain;
    fade.from_pan = channel->current_pan;
    fade.start_ms = clock_ms;
    fade.duration_ms = time_ms;
    fade.stop_on_complete = true;
    channel->fade = std::move(fade);
}

void apply_channel_gain_fade(SoundChannel* channel, int32_t target_gain_raw, uint64_t time_ms,
                             uint64_t clock_ms) {
    const float target = SoundChannel::gain_to_linear(target_gain_raw);
    channel->raw_gain = target_gain_raw;
    if (time_ms == 0) {
        channel->current_gain = target;
        // an INSTANT gain change must not cancel an in-flight
        // fade that is still moving the PAN — the game's bgm_play() emits
        // [splay]/[sxfade] (fade-in) immediately followed by [span] for the
        // same channel, and the pair must not lose the fade-in. A pending
        // stop fade (stop_on_complete) keeps its historical "cancelled by an
        // instant gain change" behavior.
        if (channel->fade && !channel->fade->stop_on_complete) {
            channel->fade->from_gain = target;
            channel->fade->target_gain = target;
        } else {
            channel->fade.reset();
        }
        return;
    }
    FadeState fade;
    fade.target_gain = target;
    fade.target_pan = channel->current_pan;
    fade.from_gain = channel->current_gain;
    fade.from_pan = channel->current_pan;
    fade.start_ms = clock_ms;
    fade.duration_ms = time_ms;
    fade.stop_on_complete = false;
    channel->fade = std::move(fade);
}

void apply_channel_pan_fade(SoundChannel* channel, int32_t target_pan_raw, uint64_t time_ms,
                            uint64_t clock_ms) {
    const float target = SoundChannel::pan_to_linear(target_pan_raw);
    channel->raw_pan = target_pan_raw;
    if (time_ms == 0) {
        channel->current_pan = target;
        // symmetric to apply_channel_gain_fade — the instant
        // pan of the [splay] + [span] pair must leave a running fade-in
        // alive instead of dropping it (which left current_gain pinned at
        // the fade-in's 0.0 start while raw_gain stayed at the requested
        // value: a decoded but inaudible BGM channel).
        if (channel->fade && !channel->fade->stop_on_complete) {
            channel->fade->from_pan = target;
            channel->fade->target_pan = target;
        } else {
            channel->fade.reset();
        }
        return;
    }
    FadeState fade;
    fade.target_gain = channel->current_gain;
    fade.target_pan = target;
    fade.from_gain = channel->current_gain;
    fade.from_pan = channel->current_pan;
    fade.start_ms = clock_ms;
    fade.duration_ms = time_ms;
    fade.stop_on_complete = false;
    channel->fade = std::move(fade);
}

/// Advance one channel's fade; returns true when the fade completed with
/// stop_on_complete (the caller removes the channel and emits the finish).
bool advance_channel_fade(SoundChannel* channel, uint64_t now_ms) {
    if (!channel->fade) return false;
    const FadeState& fade = *channel->fade;
    float gain = 0.0f, pan = 0.0f;
    bool finished = false;
    fade.current_value(now_ms, &gain, &pan, &finished);
    channel->current_gain = gain;
    channel->current_pan = pan;
    if (finished) {
        const bool stop = fade.stop_on_complete;
        channel->fade.reset();
        if (stop) channel->playing = false;
        return stop;
    }
    return false;
}

} // namespace

void AudioEngine::reset() {
    state_ = State{};
    pending_finish_.clear();
}

void AudioEngine::queue_channel_finish(const SoundChannel& channel) {
    std::optional<SoundFinishHandler> handler;
    switch (channel.category) {
        case SoundCategory::Bgm:
            handler = state_.bgm_finish_handler;
            break;
        case SoundCategory::Se: {
            const auto it = state_.se_finish_handlers.find(channel.id);
            if (it != state_.se_finish_handlers.end()) handler = it->second;
            break;
        }
        case SoundCategory::Voice:
            break; // voices never carry a finish handler of their own
    }
    SoundFinishEvent ev;
    ev.id = channel.category == SoundCategory::Bgm ? std::string() : channel.id;
    ev.category = channel.category;
    ev.handler = std::move(handler);
    pending_finish_.push_back(std::move(ev));
}

// ---------------------------------------------------------------------------
// BGM
// ---------------------------------------------------------------------------

void AudioEngine::play_bgm(const std::string& file, const BgmConfig& config) {
    state_.bgm_channel.reset(); // replaced, not finished ( comment)
    state_.bgm_channel = build_channel("bgm", file, SoundCategory::Bgm, config.loop_play,
                                       false, config.gain, config.pan, config.fade_in_ms,
                                       state_.clock_ms);
}

bool AudioEngine::stop_bgm(uint64_t fade_time_ms) {
    if (!state_.bgm_channel) return false;
    SoundChannel& channel = *state_.bgm_channel;
    if (!channel.playing) return false;
    apply_channel_fade_out(&channel, fade_time_ms, state_.clock_ms);
    if (fade_time_ms == 0) state_.bgm_channel.reset();
    return true;
}

void AudioEngine::crossfade_bgm(const std::string& file, const BgmConfig& config) {
    // Fade the old BGM out (state backend limitation: the channel slot is then
    // replaced; real overlap is the host sink's job,  ).
    if (state_.bgm_channel && state_.bgm_channel->playing) {
        apply_channel_fade_out(&*state_.bgm_channel, config.fade_in_ms, state_.clock_ms);
    }
    state_.bgm_channel = build_channel("bgm", file, SoundCategory::Bgm, config.loop_play,
                                       false, config.gain, config.pan, config.fade_in_ms,
                                       state_.clock_ms);
}

void AudioEngine::fade_bgm_gain(int32_t target_gain_raw, uint64_t time_ms) {
    if (state_.bgm_channel && state_.bgm_channel->playing) {
        apply_channel_gain_fade(&*state_.bgm_channel, target_gain_raw, time_ms,
                                state_.clock_ms);
    }
}

void AudioEngine::pan_bgm(int32_t target_pan_raw, uint64_t time_ms) {
    if (state_.bgm_channel && state_.bgm_channel->playing) {
        apply_channel_pan_fade(&*state_.bgm_channel, target_pan_raw, time_ms, state_.clock_ms);
    }
}

// ---------------------------------------------------------------------------
// SE
// ---------------------------------------------------------------------------

void AudioEngine::play_se(const std::string& id, const std::string& file,
                          const SeConfig& config) {
    // skippable SEs do not play while skipping.
    if (config.skippable && state_.is_skipping) return;
    state_.se_channels.erase(id); // same-id new play replaces the old
    SoundChannel channel = build_channel(id, file, SoundCategory::Se, config.loop_play,
                                         config.skippable, config.gain, config.pan,
                                         config.fade_in_ms, state_.clock_ms);
    state_.se_channels.emplace(id, std::move(channel));
}

bool AudioEngine::stop_se(const std::string& id, uint64_t fade_time_ms) {
    bool stopped = false;
    for (auto* map : {&state_.se_channels, &state_.voice_channels}) {
        auto it = map->find(id);
        if (it == map->end() || !it->second.playing) continue;
        apply_channel_fade_out(&it->second, fade_time_ms, state_.clock_ms);
        if (fade_time_ms == 0) map->erase(it);
        stopped = true;
    }
    return stopped;
}

void AudioEngine::fade_se_gain(const std::string& id, int32_t target_gain_raw, uint64_t time_ms) {
    for (auto* map : {&state_.se_channels, &state_.voice_channels}) {
        auto it = map->find(id);
        if (it != map->end() && it->second.playing) {
            apply_channel_gain_fade(&it->second, target_gain_raw, time_ms, state_.clock_ms);
        }
    }
}

void AudioEngine::pan_se(const std::string& id, int32_t target_pan_raw, uint64_t time_ms) {
    for (auto* map : {&state_.se_channels, &state_.voice_channels}) {
        auto it = map->find(id);
        if (it != map->end() && it->second.playing) {
            apply_channel_pan_fade(&it->second, target_pan_raw, time_ms, state_.clock_ms);
        }
    }
}

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------

void AudioEngine::play_voice(const std::string& id, const std::string& file,
                             const SeConfig& config) {
    state_.voice_channels.erase(id);
    SoundChannel channel = build_channel(id, file, SoundCategory::Voice, config.loop_play,
                                         config.skippable, config.gain, config.pan,
                                         config.fade_in_ms, state_.clock_ms);
    state_.voice_channels.emplace(id, std::move(channel));
}

// ---------------------------------------------------------------------------
// Global
// ---------------------------------------------------------------------------

void AudioEngine::stop_all_sounds() {
    state_.bgm_channel.reset();
    state_.se_channels.clear();
    state_.voice_channels.clear();
}

void AudioEngine::set_master_volume(float volume) {
    state_.master_volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
}
void AudioEngine::set_bgm_volume(float volume) {
    state_.bgm_volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
}
void AudioEngine::set_se_volume(float volume) {
    state_.se_volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
}

void AudioEngine::set_skipping(bool skipping) { state_.is_skipping = skipping; }

bool AudioEngine::is_se_playing(const std::string& id) const {
    const auto it = state_.se_channels.find(id);
    return it != state_.se_channels.end() && it->second.playing;
}

bool AudioEngine::is_bgm_playing() const {
    return state_.bgm_channel && state_.bgm_channel->playing;
}

bool AudioEngine::is_sound_playing(const std::string& id) const {
    const auto it = state_.se_channels.find(id);
    if (it != state_.se_channels.end() && it->second.playing) return true;
    const auto jt = state_.voice_channels.find(id);
    return jt != state_.voice_channels.end() && jt->second.playing;
}

void AudioEngine::set_sound_finish_handler(const std::string& id, SoundFinishHandler handler) {
    if (id.empty()) {
        state_.bgm_finish_handler = std::move(handler);
    } else {
        state_.se_finish_handlers[id] = std::move(handler);
    }
}

void AudioEngine::remove_sound_finish_handler(const std::string& id) {
    if (id.empty()) {
        state_.bgm_finish_handler.reset();
    } else {
        state_.se_finish_handlers.erase(id);
    }
}

void AudioEngine::update(uint64_t delta_ms) {
    state_.clock_ms += delta_ms;
    const uint64_t now = state_.clock_ms;

    if (state_.bgm_channel && advance_channel_fade(&*state_.bgm_channel, now)) {
        std::optional<SoundChannel> stopped = std::move(state_.bgm_channel);
        state_.bgm_channel.reset();
        if (stopped) queue_channel_finish(*stopped);
    }

    std::vector<std::string> finished_se;
    for (auto& [id, channel] : state_.se_channels) {
        (void)id;
        if (advance_channel_fade(&channel, now)) finished_se.push_back(channel.id);
    }
    for (const std::string& id : finished_se) {
        auto it = state_.se_channels.find(id);
        if (it == state_.se_channels.end()) continue;
        SoundChannel channel = std::move(it->second);
        state_.se_channels.erase(it);
        queue_channel_finish(channel);
    }

    std::vector<std::string> finished_voice;
    for (auto& [id, channel] : state_.voice_channels) {
        (void)id;
        if (advance_channel_fade(&channel, now)) finished_voice.push_back(channel.id);
    }
    for (const std::string& id : finished_voice) {
        auto it = state_.voice_channels.find(id);
        if (it == state_.voice_channels.end()) continue;
        SoundChannel channel = std::move(it->second);
        state_.voice_channels.erase(it);
        queue_channel_finish(channel);
    }
}

std::vector<SoundFinishEvent> AudioEngine::poll_finish_events() {
    std::vector<SoundFinishEvent> out;
    out.swap(pending_finish_);
    return out;
}
// ---------------------------------------------------------------------------
// §2 vorbis decode source — VorbisSource.
// ---------------------------------------------------------------------------


namespace {
// Memory-backed vorbisfile callbacks (ov_callbacks read/seek/close/tell).
struct MemImage {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

size_t mem_read_cb(void* ptr, size_t size, size_t nmemb, void* datasource) {
    auto* img = static_cast<MemImage*>(datasource);
    const size_t want = size * nmemb;
    const size_t avail = img->size - img->pos;
    const size_t n = want < avail ? want : avail;
    if (n > 0) {
        std::memcpy(ptr, img->data + img->pos, n);
        img->pos += n;
    }
    return n;
}

int mem_seek_cb(void* datasource, ogg_int64_t offset, int whence) {
    auto* img = static_cast<MemImage*>(datasource);
    ogg_int64_t base = 0;
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = ogg_int64_t(img->pos);
    } else if (whence == SEEK_END) {
        base = ogg_int64_t(img->size);
    } else {
        return -1;
    }
    const ogg_int64_t target = base + offset;
    if (target < 0 || target > ogg_int64_t(img->size)) return -1;
    img->pos = size_t(target);
    return 0;
}

long mem_tell_cb(void* datasource) {
    auto* img = static_cast<MemImage*>(datasource);
    return long(img->pos);
}

int mem_close_cb(void* datasource) {
    // vorbisfile calls the close callback from ov_clear; the MemImage
    // datasource is engine-owned and must be released here (ASan/LSan
    // surfaced it as a per-open leak).
    delete static_cast<MemImage*>(datasource);
    return 0;
}

} // namespace

VorbisSource::~VorbisSource() {
    if (vf_) ov_clear(vf_);
    delete vf_;
}

std::unique_ptr<VorbisSource> VorbisSource::open(std::vector<uint8_t> bytes) {
    if (bytes.size() < 4) return nullptr;
    // Ogg container magic: "OggS"
    if (!(bytes[0] == 'O' && bytes[1] == 'g' && bytes[2] == 'g' && bytes[3] == 'S')) {
        return nullptr;
    }
    auto src = std::unique_ptr<VorbisSource>(new VorbisSource());
    src->bytes_ = std::move(bytes);
    auto* img = new MemImage();
    img->data = src->bytes_.data();
    img->size = src->bytes_.size();
    img->pos = 0;
    src->vf_ = new OggVorbis_File();
    ov_callbacks cbs{};
    cbs.read_func = mem_read_cb;
    cbs.seek_func = mem_seek_cb;
    cbs.close_func = mem_close_cb;
    cbs.tell_func = mem_tell_cb;
    const int rc = ov_open_callbacks(img, src->vf_, nullptr, 0, cbs);
    if (rc != 0) {
        delete src->vf_;
        src->vf_ = nullptr;
        delete img;
        return nullptr;
    }
    // The vorbis_info pointer stays owned by vf_.
    const vorbis_info* vi = ov_info(src->vf_, -1);
    if (!vi) {
        return nullptr;
    }
    src->channels_ = vi->channels;
    src->rate_ = long(vi->rate);
    if (src->rate_ <= 0 || src->channels_ <= 0) return nullptr;
    return src;
}

long VorbisSource::read(float* planar, long max_frames) {
    if (!vf_ || max_frames <= 0) return 0;
    // ov_read_float fills per-channel pointers inside its internal buffers.
    float** pcm = nullptr;
    int bitstream = 0;
    long got = 0;
    int consecutive_errors = 0;
    while (got < max_frames) {
        long want = max_frames - got;
        if (want > 4096) want = 4096;
        const long ret = ov_read_float(vf_, &pcm, int(want), &bitstream);
        if (ret == 0) break; // clean EOF
        if (ret < 0) {
            // Error on this granule: skip forward and retry; give up after a
            // run of failures so a corrupt tail cannot spin the caller.
            if (++consecutive_errors > 64) break;
            continue;
        }
        consecutive_errors = 0;
        for (int c = 0; c < channels_; ++c) {
            for (long f = 0; f < ret; ++f) {
                planar[(long)c * max_frames + got + f] = pcm[c][f];
            }
        }
        got += ret;
    }
    return got;
}

bool VorbisSource::seek_zero() {
    if (!vf_) return false;
    return ov_pcm_seek(vf_, 0) == 0;
}
// ---------------------------------------------------------------------------
// §3 playback host — AudioSink + MediaPlayers.
// ---------------------------------------------------------------------------


namespace {
constexpr long kChunkFrames = 1024;
/// Prefetch window in source frames (~0.37 s at 44.1 kHz per channel).
constexpr size_t kPrefetchCapFrames = 16384;
constexpr size_t kNoFrame = std::numeric_limits<size_t>::max();
} // namespace

namespace {
// ---------------------------------------------------------------------------
// delivery diagnostics (OA_AUDIO_DIAG): after every put into a
// device stream, sample the stream water level (SDL_GetAudioStreamQueued,
// which reports BYTES — normalized to 44100 Hz stereo f32 frames here) and
// the wall gap since the previous put on the same source stream. A level
// that collapses to ~0 means the device drained that source dry — the
// audible stutter condition when a producer falls behind real-time
// consumption. Per-source per-second aggregates let a full-movie run be
// scored: seconds with dry hits / low water / large inter-push gaps are the
// glitch map. Default off: the sampling is one getenv check per put + a few
// atomics when disabled.
// ---------------------------------------------------------------------------
struct SinkDiag {
    bool enabled = false;
    std::chrono::steady_clock::time_point t0{};
    std::chrono::steady_clock::time_point last_push{};
    uint64_t puts = 0;
    uint64_t frames_put = 0;
    uint64_t dry_hits = 0;     // level after put < frames just put (source was empty)
    uint64_t low_hits = 0;     // level after put < 200 frames (~4.5 ms)
    uint64_t max_gap_us = 0;   // largest wall gap between two puts on this source
    // latency view: the running maximum queued level (that level
    // IS the audible latency) and the largest single chunk pushed (the
    // producer's cadence — a hitch shows up here first).
    uint32_t max_level = 0;
    uint32_t max_chunk = 0;
    std::mutex mu;             // guards the per-second table + event ring
    struct Sec {
        uint16_t min_level = 0xffff; // smallest level seen after a put that second
        uint16_t dry = 0;
        uint32_t puts = 0;
        uint32_t frames = 0;
        uint32_t max_gap_us = 0;
        uint64_t level_sum = 0; // sum of post-put levels (mean = level_sum/puts)
        uint32_t max_level = 0; // largest post-put level that second
    };
    std::vector<Sec> secs; // [second index]
    struct Ev {
        uint32_t t_ms = 0; // run time of the event
        uint32_t gap_us = 0;
        int level = -1;    // level after the put that revealed the dryness
    };
    std::deque<Ev> ring; // last events (cap kDiagRing)
};
constexpr size_t kDiagRing = 2048;
const char* kDiagSourceName[AudioSink::kStreamCount] = {"mix", "movie"};
SinkDiag& g_sink_diag(int source) {
    static SinkDiag d[AudioSink::kStreamCount];
    return d[source >= 0 && source < AudioSink::kStreamCount ? source : 0];
}

// ---------------------------------------------------------------------------
// mix-amplitude diagnostics (OA_AUDIO_MIXDIAG): the *signal*
// companion of the water-level diag above. SinkDiag can only see
// how much data reached a device stream (queue level), never whether that
// data carries sound — and with no audio device attached (headless, dummy
// driver) there is no stream at all. This one measures the samples the mixer
// produced, i.e. the last point every output path shares: per emitted bucket
// (one second by default) it accumulates the FINAL (clamped) interleaved
// stereo mix's sum of squares and peak, plus, per sound category, the source-
// domain sum of squares, the effective gain applied to it and the player's
// logical source position. That separates "the engine mixed nothing" from
// "the engine mixed signal that the host/device path then lost", and pins an
// intro->loop (A/B) handover to the exact bucket via the source position and
// the segment flag.
// Default off: one getenv check per tick when disabled.
// ---------------------------------------------------------------------------
struct MixDiag {
    bool enabled = false;
    uint64_t frames = 0;        // output frames mixed so far (44100 Hz clock)
    uint64_t bucket_frames = 0; // report one line per N output frames
    uint64_t sec_printed = 0;   // buckets already reported
    double mix_sumsq = 0.0;     // accumulated final-mix energy this bucket
    uint64_t mix_n = 0;         // final-mix samples this bucket (2 per frame)
    float mix_peak = 0.0f;
    // Per category [0]=Bgm [1]=Se [2]=Voice: source-domain energy + gain.
    double cat_sumsq[3] = {0.0, 0.0, 0.0};
    uint64_t cat_n[3] = {0, 0, 0};
    float cat_gain[3] = {0.0f, 0.0f, 0.0f};
    float cat_peak[3] = {0.0f, 0.0f, 0.0f};
    int cat_players[3] = {0, 0, 0};
    float bus[3] = {1.0f, 1.0f, 1.0f}; // bgm/se/voice bus * master, this second
    // BGM player state (last seen this second) — the A/B handover evidence.
    char bgm_file[64] = {0};
    double bgm_pos_s = 0.0; // player's logical source position, seconds
    int bgm_in_b = 0;
    int bgm_unplayable = 0;
    int bgm_silent = 0;
};
MixDiag& g_mix_diag() {
    static MixDiag d;
    return d;
}
double db_fs(double amplitude) {
    return 20.0 * std::log10(amplitude > 1e-7 ? amplitude : 1e-7);
}
/// Category index used by the per-second table (0/1/2 = Bgm/Se/Voice).
int mix_diag_cat(SoundCategory cat) {
    switch (cat) {
        case SoundCategory::Bgm: return 0;
        case SoundCategory::Se: return 1;
        case SoundCategory::Voice: return 2;
    }
    return 1;
}
/// Print one line per emitted bucket (no-op unless OA_AUDIO_MIXDIAG is set).
/// The bucket is one second by default; OA_AUDIO_MIXDIAG_MS=N narrows it
/// (e.g. 100) so a handover can be resolved below the second.
void mix_diag_flush_seconds() {
    MixDiag& d = g_mix_diag();
    if (d.bucket_frames == 0) d.bucket_frames = uint64_t(MediaPlayers::kOutputRate);
    const uint64_t now_sec = d.frames / d.bucket_frames;
    while (d.sec_printed < now_sec) {
        const uint64_t s = d.sec_printed;
        const double t_s = double(s * d.bucket_frames) / double(MediaPlayers::kOutputRate);
        const double mix_rms = d.mix_n ? std::sqrt(d.mix_sumsq / double(d.mix_n)) : 0.0;
        std::printf("[mixdiag] t=%.3fs mix rms=%7.2f peak=%7.2f", t_s, db_fs(mix_rms),
                    db_fs(double(d.mix_peak)));
        static const char* kCat[3] = {"bgm", "se", "voice"};
        for (int c = 0; c < 3; ++c) {
            const double src_rms =
                d.cat_n[c] ? std::sqrt(d.cat_sumsq[c] / double(d.cat_n[c])) : 0.0;
            std::printf(" | %s n=%d src=%7.2f pk=%7.2f g=%.3f bus=%.3f", kCat[c],
                        d.cat_players[c], db_fs(src_rms), db_fs(double(d.cat_peak[c])),
                        double(d.cat_gain[c]), double(d.bus[c]));
        }
        std::printf(" | bgmfile=%s pos=%.2fs segB=%d unplayable=%d silent=%d\n",
                    d.bgm_file[0] ? d.bgm_file : "-", d.bgm_pos_s, d.bgm_in_b,
                    d.bgm_unplayable, d.bgm_silent);
        // Roll the second: reset the accumulators.
        d.mix_sumsq = 0.0;
        d.mix_n = 0;
        d.mix_peak = 0.0f;
        for (int c = 0; c < 3; ++c) {
            d.cat_sumsq[c] = 0.0;
            d.cat_n[c] = 0;
            d.cat_gain[c] = 0.0f;
            d.cat_peak[c] = 0.0f;
            d.cat_players[c] = 0;
        }
        ++d.sec_printed;
    }
}
} // namespace

// ---------------------------------------------------------------------------
// signal-face capture (OA_AUDIO_REC=<prefix>): the engine's own
// samples on disk, so "every game makes a noise while BGM plays" becomes a
// measurement instead of a judgement. Five taps, all 44100 Hz stereo float32:
//   <prefix>.mix.wav    every frame the channel mixer produced, in order
//                       (BGM/SE/voice — the stream the device is fed)
//   <prefix>.movie.wav  every frame the movie pump pushed
//   <prefix>.dev.wav    SDL POST-MIX: exactly what the device was handed, all
//                       bound streams already summed AND underrun padding
//                       included — a starved queue shows up here as inserted
//                       exact zeros (the signal-face proof of a dropout)
//   <prefix>.tick.csv   per producer tick: t_ms,delta_ms,frames,level_before,
//                       level_after,paced (frames withheld by the pacer)
//   <prefix>.dev.csv    per device callback: t_ms,frames_pulled,mix_after,
//                       movie_after — the device's own pull cadence and what
//                       was left of each bound stream afterwards (a 0 means
//                       that stream was drained dry, i.e. padded with silence)
// Default off: one env check on the first capture call, then a bool test.
// ---------------------------------------------------------------------------
namespace {
void put_le32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
void put_le16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
}

/// Minimal streaming WAV writer: RIFF/WAVE + fmt(16, IEEE float) + fact +
/// data, 44100 Hz stereo f32. Sizes are patched at close.
struct WavWriter {
    std::FILE* f = nullptr;
    uint64_t frames = 0; // sample frames written (fact/data size source)
    bool open(const std::string& path) {
        f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        uint8_t h[56] = {0};
        std::memcpy(h, "RIFF", 4);
        put_le32(h + 4, 48); // 36 + 12 (fact) placeholder, patched at close
        std::memcpy(h + 8, "WAVE", 4);
        std::memcpy(h + 12, "fmt ", 4);
        put_le32(h + 16, 16);
        put_le16(h + 20, 3);      // WAVE_FORMAT_IEEE_FLOAT
        put_le16(h + 22, 2);      // channels
        put_le32(h + 24, 44100);  // sample rate
        put_le32(h + 28, 44100 * 2 * 4);
        put_le16(h + 32, 8);      // block align
        put_le16(h + 34, 32);     // bits per sample
        std::memcpy(h + 36, "fact", 4);
        put_le32(h + 40, 4);
        put_le32(h + 44, 0);
        std::memcpy(h + 48, "data", 4);
        put_le32(h + 52, 0);
        return std::fwrite(h, 1, sizeof(h), f) == sizeof(h);
    }
    void write(const float* interleaved, size_t n) {
        if (!f || n == 0) return;
        std::fwrite(interleaved, sizeof(float) * 2, n, f);
        frames += n;
    }
    void close() {
        if (!f) return;
        const uint32_t data_bytes = uint32_t(frames * 2 * sizeof(float));
        uint8_t v[4];
        std::fseek(f, 4, SEEK_SET);
        put_le32(v, 48 + data_bytes);
        std::fwrite(v, 1, 4, f);
        std::fseek(f, 44, SEEK_SET);
        put_le32(v, uint32_t(frames));
        std::fwrite(v, 1, 4, f);
        std::fseek(f, 52, SEEK_SET);
        put_le32(v, data_bytes);
        std::fwrite(v, 1, 4, f);
        std::fclose(f);
        f = nullptr;
    }
};

struct SignalCapture {
    bool inited = false;
    bool enabled = false;
    std::string prefix;
    WavWriter mix, movie, dev;
    std::FILE* tick_csv = nullptr;
    std::FILE* dev_csv = nullptr;
    std::mutex mu_mix, mu_movie, mu_dev;
    uint64_t mix_pushes = 0, mix_frames = 0;
    uint64_t movie_pushes = 0, movie_frames = 0;
    uint64_t dev_callbacks = 0, dev_frames = 0, dev_fmt_mismatch = 0;
    uint64_t dev_dry_mix = 0, dev_dry_movie = 0;
    uint64_t dev_min_mix = UINT64_MAX, dev_min_movie = UINT64_MAX;
    // Tick-log state (tick thread only).
    uint64_t t_ms = 0;
    uint64_t paced_ticks = 0;
};
SignalCapture& g_capture() {
    static SignalCapture c;
    return c;
}

/// Idempotent: opens the capture files on first call. `sink` (may be null in
/// a device-less probe) is remembered for the device-side level sampling.
void capture_begin(AudioSink* sink);
void capture_device(const SDL_AudioSpec* spec, float* buffer, int buflen, AudioSink* sink);

void capture_begin(AudioSink* sink) {
    SignalCapture& c = g_capture();
    if (c.inited) return;
    c.inited = true;
    const char* p = std::getenv("OA_AUDIO_REC");
    if (!p || !*p) return;
    c.prefix = p;
    const bool ok = c.mix.open(c.prefix + ".mix.wav") &&
                    c.movie.open(c.prefix + ".movie.wav") &&
                    c.dev.open(c.prefix + ".dev.wav");
    c.tick_csv = std::fopen((c.prefix + ".tick.csv").c_str(), "w");
    c.dev_csv = std::fopen((c.prefix + ".dev.csv").c_str(), "w");
    if (!ok || !c.tick_csv || !c.dev_csv) {
        std::fprintf(stderr, "[audiorec] cannot open capture files at %s.*\n", c.prefix.c_str());
        return;
    }
    c.enabled = true;
    if (c.tick_csv) std::fprintf(c.tick_csv, "t_ms,delta_ms,frames,level_before,level_after,paced\n");
    if (c.dev_csv) std::fprintf(c.dev_csv, "t_ms,frames_pulled,mix_after,movie_after\n");
    std::printf("[audiorec] capturing signal face to %s.{mix,movie,dev}.wav "
                "+ .{tick,dev}.csv\n", c.prefix.c_str());
    (void)sink;
}

void capture_mix(const float* interleaved, size_t frames) {
    SignalCapture& c = g_capture();
    if (!c.enabled) {
        capture_begin(nullptr);
        if (!c.enabled) return;
    }
    std::lock_guard<std::mutex> lk(c.mu_mix);
    c.mix.write(interleaved, frames);
    ++c.mix_pushes;
    c.mix_frames += frames;
}

void capture_tick(uint64_t t_ms, uint64_t delta_ms, size_t frames, size_t level_before,
                  size_t level_after, bool paced) {
    SignalCapture& c = g_capture();
    if (!c.enabled) {
        capture_begin(nullptr);
        if (!c.enabled) return;
    }
    std::lock_guard<std::mutex> lk(c.mu_mix);
    c.t_ms = t_ms;
    if (paced) ++c.paced_ticks;
    if (c.tick_csv) {
        std::fprintf(c.tick_csv, "%llu,%llu,%llu,%llu,%llu,%d\n",
                     (unsigned long long)t_ms, (unsigned long long)delta_ms,
                     (unsigned long long)frames, (unsigned long long)level_before,
                     (unsigned long long)level_after, paced ? 1 : 0);
    }
}

void capture_movie(const float* interleaved, size_t frames) {
    SignalCapture& c = g_capture();
    if (!c.enabled) {
        capture_begin(nullptr);
        if (!c.enabled) return;
    }
    std::lock_guard<std::mutex> lk(c.mu_movie);
    c.movie.write(interleaved, frames);
    ++c.movie_pushes;
    c.movie_frames += frames;
}

/// Device-side capture body: runs on the audio device thread with the final
/// device buffer (all bound streams summed, silence-padded where a stream ran
/// dry). `sink` supplies the post-pull water levels.
void capture_device(const SDL_AudioSpec* spec, float* buffer, int buflen,
                    AudioSink* sink) {
    SignalCapture& c = g_capture();
    if (!buffer || buflen <= 0) return;
    if (!spec || spec->freq != 44100 || spec->channels != 2) {
        if (!c.enabled) return;
        std::lock_guard<std::mutex> lk(c.mu_dev);
        ++c.dev_fmt_mismatch;
        return;
    }
    if (!c.enabled) return;
    const size_t frames = size_t(buflen) / (2 * sizeof(float));
    const size_t mix_after = sink ? sink->queued_frames(AudioSink::kStreamMix) : 0;
    const size_t movie_after = sink ? sink->queued_frames(AudioSink::kStreamMovie) : 0;
    std::lock_guard<std::mutex> lk(c.mu_dev);
    c.dev.write(buffer, frames);
    ++c.dev_callbacks;
    c.dev_frames += frames;
    if (mix_after < c.dev_min_mix) c.dev_min_mix = mix_after;
    if (movie_after < c.dev_min_movie) c.dev_min_movie = movie_after;
    if (c.dev_csv) {
        std::fprintf(c.dev_csv, "%llu,%llu,%llu,%llu\n",
                     (unsigned long long)SDL_GetTicks(), (unsigned long long)frames,
                     (unsigned long long)mix_after, (unsigned long long)movie_after);
    }
}

void capture_end() {
    SignalCapture& c = g_capture();
    if (!c.enabled) return;
    {
        std::lock_guard<std::mutex> lk(c.mu_mix);
        if (c.tick_csv) { std::fclose(c.tick_csv); c.tick_csv = nullptr; }
        c.mix.close();
    }
    {
        std::lock_guard<std::mutex> lk(c.mu_movie);
        c.movie.close();
    }
    {
        std::lock_guard<std::mutex> lk(c.mu_dev);
        if (c.dev_csv) { std::fclose(c.dev_csv); c.dev_csv = nullptr; }
        c.dev.close();
    }
    std::printf("[audiorec] %s: mix=%llu frames (%llu pushes) movie=%llu frames "
                "(%llu pushes) dev=%llu frames (%llu callbacks) min_mix=%lld "
                "min_movie=%lld fmtmismatch=%llu paced_ticks=%llu\n",
                c.prefix.c_str(), (unsigned long long)c.mix_frames,
                (unsigned long long)c.mix_pushes, (unsigned long long)c.movie_frames,
                (unsigned long long)c.movie_pushes, (unsigned long long)c.dev_frames,
                (unsigned long long)c.dev_callbacks,
                c.dev_min_mix == UINT64_MAX ? -1ll : (long long)c.dev_min_mix,
                c.dev_min_movie == UINT64_MAX ? -1ll : (long long)c.dev_min_movie,
                (unsigned long long)c.dev_fmt_mismatch,
                (unsigned long long)c.paced_ticks);
    c.enabled = false;
}

} // namespace

void AudioSink::note_device_pull(const SDL_AudioSpec* spec, int buflen) {
    if (!spec || buflen <= 0) return;
    if (spec->freq != 44100 || spec->channels != 2) return;
    ++device_callbacks_;
    // "Ran dry": SDL removed everything this stream had for this callback, so
    // the shortfall was padded with silence. That hole in the middle of a
    // continuous source (BGM) is the click/dropout this revision is about —
    // counted here on the DEVICE side (the only place it is observable) and
    // only while the stream is live (an idle stream is empty by definition).
    const uint64_t now = SDL_GetTicks();
    for (int i = 0; i < kStreamCount; ++i) {
        if (last_push_ms_[i] == 0 || now - last_push_ms_[i] > 250) continue;
        if (queued_frames(i) == 0) ++underruns_[i];
    }
}

void SDLCALL sink_postmix_cb(void* ud, const SDL_AudioSpec* spec, float* buffer,
                             int buflen) {
    auto* sink = static_cast<AudioSink*>(ud);
    if (sink) sink->note_device_pull(spec, buflen);
    capture_device(spec, buffer, buflen, sink);
}

void sink_diag_after_push(int source, SDL_AudioStream* stream, size_t frames) {
    SinkDiag& d = g_sink_diag(source);
    if (!d.enabled) {
        // One-time decision: env gate. The static initializer runs on the
        // first push (any thread); steady-state cost after that is an atomic.
        static const bool on = std::getenv("OA_AUDIO_DIAG") != nullptr;
        if (!on) return;
        d.enabled = true;
        d.t0 = std::chrono::steady_clock::now();
        d.last_push = d.t0;
    }
    const auto now = std::chrono::steady_clock::now();
    const uint64_t gap_us = now > d.last_push
                                ? uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                               now - d.last_push)
                                               .count())
                                : 0;
    d.last_push = now;
    // SDL_GetAudioStreamQueued returns BYTES (no framesize conversion in
    // SDL3); normalize to 44100 Hz stereo f32 frames (8 B/frame) so the
    // diagnostics read in the engine's sample unit.
    const int level = SDL_GetAudioStreamQueued(stream) / 8;
    const uint64_t t_ms = uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - d.t0).count());
    if (gap_us > d.max_gap_us) d.max_gap_us = gap_us;
    if (frames > d.max_chunk) d.max_chunk = uint32_t(frames);
    if (level > 0 && uint32_t(level) > d.max_level) d.max_level = uint32_t(level);
    ++d.puts;
    d.frames_put += frames;
    if (level >= 0 && uint64_t(level) < frames) ++d.dry_hits;
    if (level >= 0 && level < 200) ++d.low_hits; // <200 frames (~4.5 ms)
    if (d.dry_hits <= 64 || level < 200) {
        std::lock_guard<std::mutex> lk(d.mu);
        if (d.ring.size() >= kDiagRing) d.ring.pop_front();
        d.ring.push_back(SinkDiag::Ev{uint32_t(t_ms), uint32_t(gap_us), level});
    }
    std::lock_guard<std::mutex> lk(d.mu);
    const size_t sec = size_t(t_ms / 1000);
    if (d.secs.size() <= sec) d.secs.resize(sec + 1);
    SinkDiag::Sec& s = d.secs[sec];
    if (level >= 0 && level < s.min_level) s.min_level = uint16_t(level);
    if (level >= 0 && uint64_t(level) < frames) ++s.dry;
    ++s.puts;
    s.frames += uint32_t(frames);
    if (gap_us > s.max_gap_us) s.max_gap_us = uint32_t(gap_us);
    if (level >= 0) {
        s.level_sum += uint64_t(level);
        if (uint32_t(level) > s.max_level) s.max_level = uint32_t(level);
    }
}

void MediaPlayers::print_audio_diag(const char* label) const {
    for (int src = 0; src < AudioSink::kStreamCount; ++src) {
        const SinkDiag& d = g_sink_diag(src);
        if (!d.enabled) continue;
        std::printf("[audiodiag] %s [%s]: puts=%llu frames=%llu (%.2fs) dry=%llu "
                    "low<200=%llu max_gap=%lluus\n",
                    label ? label : "", kDiagSourceName[src],
                    (unsigned long long)d.puts, (unsigned long long)d.frames_put,
                    double(d.frames_put) / 44100.0, (unsigned long long)d.dry_hits,
                    (unsigned long long)d.low_hits, (unsigned long long)d.max_gap_us);
        // latency view: the queued level IS the audible latency,
        // so the running maximum is the worst standing delay this run ever
        // carried, and max_chunk is the producer cadence that caused it.
        std::printf("[audiodiag] %s [%s] latency: maxlevel=%u (%.1fms) "
                    "maxchunk=%u (%.1fms) bound=%s resync=%llu\n",
                    label ? label : "", kDiagSourceName[src], unsigned(d.max_level),
                    double(d.max_level) / 44.1, unsigned(d.max_chunk),
                    double(d.max_chunk) / 44.1,
                    (sink_ && sink_->bound_enabled()) ? "on" : "off(pre-fix)",
                    sink_ ? (unsigned long long)sink_->resyncs(src) : 0ull);
        unsigned shown = 0;
        const size_t nsec = std::min<size_t>(d.secs.size(), 360);
        for (size_t i = 0; i < nsec; ++i) {
            const SinkDiag::Sec& s = d.secs[i];
            const unsigned mean = s.puts ? unsigned(s.level_sum / s.puts) : 0;
            const bool bad = s.dry > 0 || s.min_level < 200 || s.max_gap_us >= 30000;
            std::printf("[audiodiag]   t=%zus min=%-6u mean=%-6u max=%-6u dry=%u "
                        "puts=%u gapmax=%uus%s\n",
                        i, unsigned(s.min_level), mean, unsigned(s.max_level),
                        unsigned(s.dry), unsigned(s.puts), unsigned(s.max_gap_us),
                        bad ? "  <--" : "");
            ++shown;
        }
        if (shown == 0) std::printf("[audiodiag]   (no seconds recorded)\n");
        std::lock_guard<std::mutex> lk(const_cast<SinkDiag&>(d).mu);
        size_t printed = 0;
        for (auto it = d.ring.rbegin(); it != d.ring.rend() && printed < 8; ++it) {
            if (it->level < 0 || uint32_t(it->level) < 200) {
                std::printf("[audiodiag]   ev t=%ums level=%d gap=%uus\n", it->t_ms,
                            it->level, it->gap_us);
                ++printed;
            }
        }
    }
}

namespace {

std::string channel_key(SoundCategory cat, const std::string& id) {
    switch (cat) {
        case SoundCategory::Bgm:
            return "bgm";
        case SoundCategory::Se:
            return "se|" + id;
        case SoundCategory::Voice:
            return "voice|" + id;
    }
    return "?|" + id;
}

/// Pull the next logical-stream frame into out (avail_ch floats). Returns
/// false when no further real frames exist (stream ended or silent forever).
/// Loop restarts (same source or A->B) happen transparently here.
bool pull_frame(MediaPlayers::Player& p,
                const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>&
                    loader,
                float* out) {
    for (;;) {
        if (p.planar_frames == 0) {
            if (p.unplayable || p.silent_forever || !p.src) return false;
            if (p.planar.size() < size_t(p.src_ch) * size_t(kChunkFrames)) {
                p.planar.resize(size_t(p.src_ch) * size_t(kChunkFrames));
            }
            const long got = p.src->read(p.planar.data(), kChunkFrames);
            if (got > 0) {
                p.planar_frames = size_t(got);
                p.planar_off = 0;
            } else {
                // End of the current source.
                bool restart_same = false;
                if (p.loop_play) {
                    if (!p.in_b_segment && p.loop_file) {
                        // A-B loop: the intro (foo_a) ended; switch to the
                        // loop segment (foo_b) once, then loop it forever.
                        if (auto bytes = loader(*p.loop_file)) {
                            auto b = VorbisSource::open(std::move(*bytes));
                            if (b && b->channels() == p.src_ch && b->rate() == long(p.rate)) {
                                p.src = std::move(b);
                                p.in_b_segment = true;
                                continue;
                            }
                        }
                        p.silent_forever = true;
                        return false;
                    }
                    restart_same = true;
                }
                if (restart_same) {
                    if (p.src->seek_zero()) continue;
                    p.silent_forever = true;
                    return false;
                }
                // Non-loop natural end: the caller marks silence
                // (zero_from) and, when no fade-out is running, finishes.
                return false;
            }
        }
        // Consume one planar frame (channel-major, stride kChunkFrames).
        if (p.src_ch == p.avail_ch) {
            for (long c = 0; c < p.avail_ch; ++c) {
                out[c] = p.planar[size_t(c) * size_t(kChunkFrames) + p.planar_off];
            }
        } else {
            // >2-channel sources average down to mono.
            float sum = 0.0f;
            for (long c = 0; c < p.src_ch; ++c) {
                sum += p.planar[size_t(c) * size_t(kChunkFrames) + p.planar_off];
            }
            out[0] = sum / float(p.src_ch);
        }
        ++p.planar_off;
        --p.planar_frames;
        return true;
    }
}

/// Pull frames up to logical index idx (so both idx and idx+1 exist), then
/// trim every frame below idx. Queries arrive in non-decreasing idx order.
///
/// With a decode-pool prefetch attached, the frames up to the worker's
/// produced watermark are copied out of the ring first (same deterministic
/// stream, byte-identical to synchronous decoding); anything beyond the
/// watermark falls through to the synchronous decoder, which realigns by
/// decoding-and-discarding up to the logical tail before appending.
void prepare_upto(MediaPlayers::Player& p,
                  const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>&
                      loader,
                  size_t idx) {
    const size_t stride = size_t(p.avail_ch);

    // Pool prefetch fast path.
    if (p.prefetch) {
        auto pf = p.prefetch;
        std::unique_lock<std::mutex> lk(pf->mu);
        const size_t tail = p.dq_start + p.dq.size() / stride;
        if (!pf->cancel && pf->produced > tail) {
            // The worker is normally far ahead; wait only a hairline when it
            // is mid-chunk, then fall back synchronously if still behind.
            pf->cv.wait_for(lk, std::chrono::milliseconds(1), [&] {
                return pf->cancel || pf->eof || pf->produced >= idx + 2;
            });
            const size_t to = std::min(idx + 1, pf->produced);
            for (size_t i = tail; i < to; ++i) {
                for (size_t c = 0; c < stride; ++c) {
                    p.dq.push_back(pf->ring[(i % pf->cap) * stride + c]);
                }
            }
            if (to > pf->consumed) {
                pf->consumed = to;
                pf->cv.notify_all(); // worker may be blocked on the ring cap
            }
        }
    }

    while (p.zero_from == kNoFrame &&
           p.dq_start + p.dq.size() / stride <= idx + 1) {
        // Synchronous fallback. The sync decoder restarts at stream zero
        // whenever it runs, so frames already supplied by the prefetch
        // worker must be decoded-and-discarded to reach the logical tail.
        const size_t tail = p.dq_start + p.dq.size() / stride;
        while (p.pull_seq < tail) {
            float tmp[8];
            if (!pull_frame(p, loader, tmp)) {
                p.zero_from = size_t(p.pull_seq);
                break;
            }
            ++p.pull_seq;
        }
        if (p.zero_from != kNoFrame) break;
        if (p.pull_seq != tail) continue; // keep aligning

        std::vector<float> frame(stride, 0.0f);
        if (!pull_frame(p, loader, frame.data())) {
            // Non-loop EOF or a given-up loop: silence from here on.
            p.zero_from = size_t(p.pull_seq);
            break;
        }
        ++p.pull_seq;
        p.dq.insert(p.dq.end(), frame.begin(), frame.end());
    }
    while (p.dq_start < idx && p.dq.size() >= stride) {
        p.dq.erase(p.dq.begin(), p.dq.begin() + stride);
        ++p.dq_start;
    }
}

/// Sample channel c of the frame at logical index idx (silence past zero_from).
float frame_sample(MediaPlayers::Player& p, size_t idx, long c) {
    if (idx >= p.zero_from) return 0.0f;
    const size_t phys = idx - p.dq_start;
    if (phys * size_t(p.avail_ch) + size_t(c) >= p.dq.size()) return 0.0f;
    return p.dq[phys * size_t(p.avail_ch) + size_t(c)];
}

// ---------------------------------------------------------------------------
// Decode-pool prefetch workers: each active channel decodes the SAME
// deterministic source-frame stream ahead of the mixer (loops included: the
// worker mirrors pull_frame's A-B switch / seek-zero restart rules, so it
// keeps producing across file boundaries and only exits at cancel or at a
// real give-up). The main-thread synchronous path stays authoritative;
// workers only make frames available earlier (output bytes are identical).
//
// Hosting model: wasm shares one DecodePool worker per slot (threads are
// precious there); native builds give every audio channel its OWN dedicated
// thread (low latency: a channel never queues behind another one's decode),
// which is destroyed when the channel ends (cancel/eof -> worker returns).
// ---------------------------------------------------------------------------

void cancel_prefetch(MediaPlayers::Player& p) {
    if (!p.prefetch) return;
    {
        std::lock_guard<std::mutex> lk(p.prefetch->mu);
        p.prefetch->cancel = true;
    }
    p.prefetch->cv.notify_all();
}

using LoaderFn =
    std::function<std::optional<std::vector<uint8_t>>(const std::string&)>;

/// The audio prefetch worker body (runs on a dedicated native thread or on
/// a DecodePool worker under wasm). Produces source frames into the ring
/// with pull_frame-equivalent loop semantics; exits only at cancel/eof.
void run_audio_worker(const std::shared_ptr<MediaPlayers::Player::Prefetch>& pf,
                      LoaderFn loader, std::string file, bool loop_play,
                      std::optional<std::string> loop_file, long src_ch,
                      long avail_ch) {
    auto open_file = [&](const std::string& f) -> std::unique_ptr<VorbisSource> {
        auto bytes = loader(f);
        return bytes ? VorbisSource::open(std::move(*bytes)) : nullptr;
    };
    auto give_up = [&] {
        std::lock_guard<std::mutex> lk(pf->mu);
        pf->eof = true;
        pf->cv.notify_all();
    };
    std::unique_ptr<VorbisSource> src = open_file(file);
    if (!src) {
        give_up();
        return;
    }
    const long s_ch = long(src->channels());
    if (s_ch < 1 || s_ch != src_ch) { // identical file => identical layout
        give_up();
        return;
    }
    const double rate = double(src->rate());
    bool in_b = false; // A-B loop currently in the loop segment (foo_b)
    std::vector<float> planar(size_t(src_ch) * kChunkFrames);
    std::vector<float> frame_buf(size_t(avail_ch) * kChunkFrames);
    for (;;) {
        // Backpressure: reserve room for the next chunk before decoding.
        {
            std::unique_lock<std::mutex> lk(pf->mu);
            pf->cv.wait(lk, [&] {
                return pf->cancel ||
                       pf->produced + kChunkFrames - pf->consumed <= pf->cap;
            });
            if (pf->cancel) return;
        }
        const long got = src->read(planar.data(), kChunkFrames);
        if (got <= 0) {
            // End of the current source: mirror pull_frame's loop rules.
            bool restart = false;
            if (loop_play && !in_b && loop_file) {
                // A-B loop: the intro (foo_a) ended; switch to the loop
                // segment (foo_b) once, then loop it forever.
                auto b = open_file(*loop_file);
                if (b && long(b->channels()) == src_ch && b->rate() == rate) {
                    src = std::move(b);
                    in_b = true;
                    restart = true;
                }
            } else if (loop_play) {
                restart = src->seek_zero();
            }
            if (!restart) {
                // Non-loop EOF or a loop that gave up (missing/incompatible
                // B segment, seek failure): stop producing. The synchronous
                // fallback re-runs the same rules on the main thread and
                // owns the silent_forever / finish semantics.
                give_up();
                return;
            }
            continue; // no frame produced: the next element comes from the
                      // restarted/segmented source
        }
        // Same mapping as pull_frame (planar is channel-major; >2-channel
        // sources average down to mono).
        for (long f = 0; f < got; ++f) {
            if (src_ch == avail_ch) {
                for (long c = 0; c < avail_ch; ++c) {
                    frame_buf[size_t(f) * size_t(avail_ch) + size_t(c)] =
                        planar[size_t(c) * kChunkFrames + size_t(f)];
                }
            } else {
                float sum = 0.0f;
                for (long c = 0; c < src_ch; ++c) {
                    sum += planar[size_t(c) * kChunkFrames + size_t(f)];
                }
                frame_buf[size_t(f)] = sum / float(src_ch);
            }
        }
        std::unique_lock<std::mutex> lk(pf->mu);
        for (long f = 0; f < got; ++f) {
            const size_t slot =
                ((pf->produced + size_t(f)) % pf->cap) * size_t(avail_ch);
            for (long c = 0; c < avail_ch; ++c) {
                pf->ring[slot + size_t(c)] =
                    frame_buf[size_t(f) * size_t(avail_ch) + size_t(c)];
            }
        }
        pf->produced += size_t(got);
        pf->cv.notify_all();
    }
}

std::shared_ptr<MediaPlayers::Player::Prefetch> start_prefetch(
    MediaPlayers::Player& p, const LoaderFn& loader, DecodePool* pool) {
    if (!pool || pool->threads() <= 0 || p.unplayable || !p.src) return nullptr;
    auto pf = std::make_shared<MediaPlayers::Player::Prefetch>();
    pf->cap = kPrefetchCapFrames;
    {
        std::lock_guard<std::mutex> lk(pf->mu);
        // Fixed-size worker buffers: no per-chunk heap growth (wasm memory
        // stays flat).
        pf->ring.assign(pf->cap * size_t(p.avail_ch), 0.0f);
    }
    const std::string file = p.file;
    const LoaderFn loader_copy = loader;
    const long src_ch = p.src_ch;
    const long avail_ch = p.avail_ch;
    const bool loop_play = p.loop_play;
    const std::optional<std::string> loop_file = p.loop_file;

#if defined(__EMSCRIPTEN__)
    // wasm: share the (configurable) DecodePool; one slot per channel.
    const bool ok = pool->submit_long([pf, loader_copy, file, loop_play, loop_file,
                                       src_ch, avail_ch] {
        run_audio_worker(pf, loader_copy, file, loop_play, loop_file, src_ch,
                         avail_ch);
    });
    if (!ok) return nullptr;
#else
    // Native: one DEDICATED thread per audio channel (low latency: a
    // channel never waits behind another channel's decode). The thread is
    // detached; it returns — and is destroyed — when the channel ends
    // (cancel) or the stream gives up (eof). It only touches its captured
    // copies (loader + prefetch state), never the players.
    try {
        std::thread worker([pf, loader_copy, file, loop_play, loop_file, src_ch,
                            avail_ch] {
            run_audio_worker(pf, loader_copy, file, loop_play, loop_file, src_ch,
                             avail_ch);
        });
        worker.detach();
    } catch (const std::exception&) {
        return nullptr; // thread creation failed: plain synchronous decode
    }
#endif
    return pf;
}

} // namespace

bool MediaPlayers::init()
{
    // SDL3 audio output subsystem + device sink. Windowed hosts try the
    // default playback device; headless hosts never init SDL audio and run
    // the engine in silent mode (decode + completion events still run).
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[app] SDL audio subsystem init failed: %s\n",
            SDL_GetError());
        return false;
    }
    sink_ = new AudioSink();
    if (!sink_->open()) {
        delete sink_;
        sink_ = nullptr;
        std::fprintf(stderr, "[app] no audio device; silent mode (media state "
            "machine still runs)\n");
        return false;
    }
    // bounded push (drop a standing latency backlog instead of
    // playing it late). `OA_AUDIO_NO_BOUND=1` is the negative control: it
    // restores the unbounded behaviour for A/B measurement.
    sink_->set_bound_enabled(std::getenv("OA_AUDIO_NO_BOUND") == nullptr);
    // negative control: the one-sided pacer (the level
    // parks wherever it lands instead of being held at the mark).
    mix_refill_enabled_ = std::getenv("OA_AUDIO_NO_REFILL") == nullptr;
    capture_begin(sink_); // install the device-face capture (env-gated)
    std::printf("[app] audio device open (44100 Hz stereo f32)\n");
    return true;
}

size_t MediaPlayers::audio_level_frames(int source) const {
    return sink_ ? sink_->queued_frames(source) : 0;
}

uint64_t MediaPlayers::device_underruns(int source) const {
    return sink_ ? sink_->underruns(source) : 0;
}

uint64_t MediaPlayers::device_callbacks() const {
    return sink_ ? sink_->device_callbacks() : 0;
}

void MediaPlayers::flush_video_audio() {
    // The negative control (OA_AUDIO_NO_BOUND / set_audio_bound_enabled(false))
    // restores the whole legacy behaviour: unbounded push AND the
    // heuristic-only flush (no deterministic EOF/stop flush). That is what the
    // before/after distributions compare.
    if (sink_ && sink_->bound_enabled()) sink_->clear_movie();
}

void MediaPlayers::set_audio_bound_enabled(bool on) {
    if (sink_) sink_->set_bound_enabled(on);
}

void MediaPlayers::set_mix_refill_enabled(bool on) { mix_refill_enabled_ = on; }

/// the mix pacer's water mark.
///
/// The original rule was "while the stream carries more than 2 device periods,
/// do not
/// produce" — a BOUND, which the producer can hold from above (it can always
/// stop producing) but not from below: producing exactly one wall-clock chunk
/// per tick only replaces what the device consumed, so the level parks
/// wherever the last withhold/hitch/startup left it, and its per-tick sawtooth
/// bottom is `level - chunk`. With 60 fps ticks (chunk 735 frames) and a
/// 1024-frame device period that bottom falls under one period regularly, and
/// a device callback that finds less than a whole period pads the shortfall
/// with silence: measured on the device face as 22 exact
/// silence holes of 0.9-1.9 ms in 8 s of BGM on the dummy AND the PipeWire
/// device — clicks 2-3 times a second in every game.
///
/// So the target the producer AIMS at must cover one device period plus the
/// producer's own cadence (the drain a callback can see between two pushes),
/// and the producer must be allowed to produce the missing frames instead of
/// only ever withholding. Both stay bounded:
///   * the cadence term is capped at 4 periods (a single long hitch cannot
///     inflate the mark),
///   * the whole target is capped at 8 periods (186 ms at 1024), so a standing
///     level stays in the "tens to ~180 ms" class — two orders of magnitude
///     below the 700-1500 ms standing delay the earlier one-sided pacer
///     produced — and above the target the pacer withholds EXACTLY as that
///     pacer did (no ratchet: any backlog still drains at 1x).
/// Below ~6 fps the mark saturates and ticks longer than the mark can still
/// starve — the same residual the earlier pacer already documented ("the delay
/// inside a slow segment is at least the frame time").
size_t MediaPlayers::mix_target_frames() const {
    if (!sink_) return 0;
    const size_t dev = sink_->device_frames() ? sink_->device_frames() : 1024;
    if (!mix_refill_enabled_) return 2 * dev; // one-sided law (negative control)
    const size_t chunk = last_tick_frames_ ? last_tick_frames_ : dev;
    const size_t capped = std::min(chunk, 4 * dev);
    size_t target = dev + capped + dev / 4; // period + cadence + jitter slack
    if (target < 2 * dev) target = 2 * dev; // the original value stays the floor
    if (target > 8 * dev) target = 8 * dev; // latency ceiling (186 ms @1024)
    return target;
}

void MediaPlayers::release()
{
    capture_end(); // flush + report the signal capture first
    if (sink_)
    {
        // the device-side dropout census, always reported (the
        // signal-face number behind "every BGM has a little noise").
        std::printf("[audiosink] callbacks=%llu underruns mix=%llu movie=%llu "
                    "resync mix=%llu movie=%llu\n",
                    (unsigned long long)sink_->device_callbacks(),
                    (unsigned long long)sink_->underruns(AudioSink::kStreamMix),
                    (unsigned long long)sink_->underruns(AudioSink::kStreamMovie),
                    (unsigned long long)sink_->resyncs(AudioSink::kStreamMix),
                    (unsigned long long)sink_->resyncs(AudioSink::kStreamMovie));
        delete sink_;
        sink_ = nullptr;
    }
}

MediaPlayers::~MediaPlayers() {
    // Retire prefetch workers (they outlive their Player through the shared
    // prefetch state) so a pool destructor can never join a stuck job.
    for (auto& [key, p] : players_) cancel_prefetch(*p);
}

void MediaPlayers::stop_all() {
    for (auto& [key, p] : players_) cancel_prefetch(*p);
    players_.clear();
}

void MediaPlayers::push_video_audio(const float* interleaved_stereo, size_t frames) {
    if (!sink_ || !interleaved_stereo || frames == 0) return;
    // The producer (VideoEngine) already applied its gains; keep the device
    // stream inside [-1, 1] like the channel mixer does.
    std::vector<float> clamped;
    const float* src = interleaved_stereo;
    bool needs_clamp = false;
    for (size_t i = 0; i < frames * 2; ++i) {
        const float v = interleaved_stereo[i];
        if (v > 1.0f || v < -1.0f) {
            needs_clamp = true;
            break;
        }
    }
    if (needs_clamp) {
        clamped.resize(frames * 2);
        for (size_t i = 0; i < frames * 2; ++i) {
            const float v = interleaved_stereo[i];
            clamped[i] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
        }
        src = clamped.data();
    }
    // movie audio rides its OWN bound device stream (source 1),
    // isolated from the channel mix — a stopped/skipped movie's buffered
    // tail must never play over the next sound.
    // The DETERMINISTIC version of that flush lives in the
    // VideoEngine start/stop/EOF hooks (set_audio_flush -> flush_video_audio),
    // which covers the EOF case the old heuristic could never see (EOF has no
    // "next push" to trigger it). This gap-based net stays as belt and braces
    // for hosts that never install the hook: a >250 ms push gap means a new
    // movie / restart / skip boundary. The bounded push below
    // additionally caps the stream's standing latency,
    static std::atomic<uint64_t> s_last_movie_push_ms{0};
    const uint64_t now_ms = SDL_GetTicks();
    const uint64_t prev_ms = s_last_movie_push_ms.exchange(now_ms);
    if (prev_ms != 0 && now_ms > prev_ms && now_ms - prev_ms > 250) {
        sink_->clear_movie();
    }
    capture_movie(src, frames); // movie stream signal face
    sink_->push_movie(src, frames); // bounded: kMovieBoundFrames
}
void MediaPlayers::update(uint64_t delta_ms, AudioEngine& engine, const FinishFn& on_finish) {
    // Output frames for this tick (44100 Hz clock).
    out_frame_frac_ += double(delta_ms) * double(kOutputRate) / 1000.0;
    size_t n = size_t(out_frame_frac_);
    out_frame_frac_ -= double(n);
    if (n == 0) return;

    // natural backpressure on the device stream. The queued
    // level IS the audible latency, and this producer's cadence is the app's
    // frame time: a hitch of D ms hands this call a D ms chunk, which lands
    // in the queue at once while the device empties the (small) standing
    // queue — so the queue adopts the hitch as its new level and, because
    // production and consumption are both 1x, nothing ever drains it again
    // (measured: a 400 ms hitch leaves a 400 ms standing delay through every
    // later second). While the stream still carries more
    // than the target, this tick's frames are NOT produced: the queued audio
    // keeps playing at the device's own pace (seamless — no clear, no skip
    // in the heard content) and the source simply resumes where the queue
    // ends, so the level walks back down to the target instead of becoming a
    // permanent delay. The un-produced credit is dropped for the same reason
    // (keeping it would refill the queue on the next tick).
    last_tick_frames_ = n;
    const size_t level_before = sink_ ? sink_->queued_frames(AudioSink::kStreamMix) : 0;
    if (sink_ && sink_->bound_enabled()) {
        const size_t target = mix_target_frames();
        if (target > 0) {
            if (!mix_refill_enabled_) {
                // one-sided rule (negative control): withhold only, never refill.
                if (level_before >= target) {
                    ++paced_out_ticks_;
                    out_frame_frac_ = 0.0;
                    capture_tick(SDL_GetTicks(), delta_ms, 0, level_before, level_before, true);
                    return;
                }
            } else {
                // hold the level AT the target from both sides.
                // The device drains ~n frames per tick, so a tick that lands
                // the level exactly on the target is `n + (target - level)`
                // frames: more than n when the queue is short (catch-up — the
                // frames are produced, never dropped, and the source simply
                // runs that little bit further ahead) and fewer than n when it
                // is long (a partial drain, the same walk-down the bound got by
                // withholding whole ticks, just without overshooting below the
                // mark). A whole chunk above the target still drops the tick
                // outright (the drain rule).
                const long long delta = (long long)target - (long long)level_before;
                if (delta <= -(long long)n) {
                    ++paced_out_ticks_;
                    out_frame_frac_ = 0.0;
                    capture_tick(SDL_GetTicks(), delta_ms, 0, level_before, level_before, true);
                    return;
                }
                if (delta > 0) {
                    n += size_t(delta);
                    refill_frames_ += size_t(delta);
                    ++refill_ticks_;
                } else if (delta < 0) {
                    n -= size_t(-delta);
                }
            }
        }
    }

    // Snapshot the playing channels and reconcile players.
    struct Entry {
        std::string key;
        const SoundChannel* ch = nullptr;
        SoundCategory cat = SoundCategory::Se;
        std::string id;
    };
    std::vector<Entry> active;
    if (const auto& bgm = engine.state().bgm_channel) {
        if (bgm->playing) {
            Entry e{"bgm", &*bgm, SoundCategory::Bgm, ""};
            active.push_back(e);
        }
    }
    for (const auto& [id, ch] : engine.state().se_channels) {
        if (ch.playing) {
            Entry e{channel_key(SoundCategory::Se, id), &ch, SoundCategory::Se, id};
            active.push_back(std::move(e));
        }
    }
    for (const auto& [id, ch] : engine.state().voice_channels) {
        if (ch.playing) {
            Entry e{channel_key(SoundCategory::Voice, id), &ch, SoundCategory::Voice, id};
            active.push_back(std::move(e));
        }
    }

    // Drop players whose channel is no longer playing.
    for (auto it = players_.begin(); it != players_.end();) {
        const bool still = std::any_of(active.begin(), active.end(),
                                       [&](const Entry& e) { return e.key == it->first; });
        if (still) {
            ++it;
        } else {
            cancel_prefetch(*it->second);
            it = players_.erase(it);
        }
    }

    // Create / refresh players.
    for (const Entry& e : active) {
        auto it = players_.find(e.key);
        const bool need_new =
            it == players_.end() || it->second->file != e.ch->file ||
            it->second->loop_play != e.ch->loop_play || it->second->loop_file != e.ch->loop_file ||
            it->second->started_at_ms != e.ch->started_at_ms ||
            it->second->category != e.cat || it->second->id != e.id;
        if (!need_new) continue;
        auto p = std::make_unique<Player>();
        p->category = e.cat;
        p->id = e.id;
        p->file = e.ch->file;
        p->loop_play = e.ch->loop_play;
        p->loop_file = e.ch->loop_file;
        p->started_at_ms = e.ch->started_at_ms;
        if (loader_) {
            if (auto bytes = loader_(e.ch->file)) {
                auto src = VorbisSource::open(std::move(*bytes));
                if (src) {
                    p->src = std::move(src);
                    p->src_ch = long(p->src->channels());
                    if (p->src_ch < 1) p->src_ch = 1;
                    p->avail_ch = p->src_ch > 2 ? 1 : p->src_ch;
                    p->rate = double(p->src->rate());
                    if (p->rate <= 0) p->rate = double(kOutputRate);
                }
            }
        }
        if (!p->src) p->unplayable = true;
        if (it == players_.end()) {
            players_[e.key] = std::move(p);
        } else {
            cancel_prefetch(*it->second); // replaced: retire the old worker first
            it->second = std::move(p);
        }
        // Optional decode-pool prefetch for the fresh player.
        players_[e.key]->prefetch = start_prefetch(*players_[e.key], loader_, pool_);
    }

    // Mix.
    std::vector<float> mix(2 * n, 0.0f);
    const float master = engine.state().master_volume;
    MixDiag& md = g_mix_diag();
    if (!md.enabled && std::getenv("OA_AUDIO_MIXDIAG") != nullptr) {
        md.enabled = true;
        // Optional sub-second resolution (default 1 s).
        const char* ms = std::getenv("OA_AUDIO_MIXDIAG_MS");
        const long ms_v = ms ? std::strtol(ms, nullptr, 10) : 0;
        md.bucket_frames = ms_v > 0
                               ? uint64_t(ms_v) * uint64_t(MediaPlayers::kOutputRate) / 1000u
                               : uint64_t(MediaPlayers::kOutputRate);
        if (md.bucket_frames == 0) md.bucket_frames = uint64_t(MediaPlayers::kOutputRate);
    }
    const bool mdiag = md.enabled;
    if (mdiag) {
        md.frames += n;
        md.bus[0] = engine.state().bgm_volume * master;
        md.bus[1] = engine.state().se_volume * master;
        md.bus[2] = engine.state().voice_volume * master;
    }
    struct Finish {
        SoundCategory cat;
        std::string id;
    };
    std::vector<Finish> finishes;

    for (const Entry& e : active) {
        Player& p = *players_[e.key];
        float cat_vol = 1.0f;
        switch (e.cat) {
            case SoundCategory::Bgm:
                cat_vol = engine.state().bgm_volume;
                break;
            case SoundCategory::Se:
                cat_vol = engine.state().se_volume;
                break;
            case SoundCategory::Voice:
                cat_vol = engine.state().voice_volume;
                break;
        }
        const float gain = e.ch->current_gain * cat_vol * master;
        const float pan = e.ch->current_pan;
        // Linear balance gains (same formula for mono and stereo: mono is
        // duplicated to L/R first, so each side scales like a mono source).
        const float gl = gain * (pan <= 0.0f ? 1.0f : 1.0f - pan);
        const float gr = gain * (pan >= 0.0f ? 1.0f : 1.0f + pan);
        // per-category source-domain energy for the mix diag.
        const int mcat = mdiag ? mix_diag_cat(e.cat) : 0;
        if (mdiag) {
            ++md.cat_players[mcat];
            md.cat_gain[mcat] = gain;
        }

        if (p.unplayable || p.silent_forever) {
            // Nothing decodable. Non-loop channels complete immediately
            // (deterministic, hang-free); loops keep a silent channel alive.
            if (mdiag && e.cat == SoundCategory::Bgm) {
                md.bgm_unplayable = p.unplayable ? 1 : 0;
                md.bgm_silent = p.silent_forever ? 1 : 0;
                std::snprintf(md.bgm_file, sizeof(md.bgm_file), "%s", p.file.c_str());
            }
            if (!p.reported && p.loop_play) {
                // a looped channel that cannot decode stays logically playing
                // but silent: no completion ever fires.
                p.reported = true;
            } else if (!p.reported && !p.loop_play) {
                p.reported = true;
                finishes.push_back({e.cat, e.id});
            }
            continue;
        }

        const double ratio = p.rate / double(kOutputRate);
        // The stream ends mid-block when the last real frame falls inside it.
        const double block_end = p.p + double(n) * ratio;
        for (size_t i = 0; i < n; ++i) {
            const double pos = p.p + double(i) * ratio;
            const long k = long(std::floor(pos));
            const float f = float(pos - double(k));
            prepare_upto(p, loader_, size_t(k));
            if (p.avail_ch == 1) {
                const float v0 = frame_sample(p, size_t(k), 0);
                const float v1 = frame_sample(p, size_t(k + 1), 0);
                const float v = v0 + (v1 - v0) * f;
                mix[2 * i] += v * gl;
                mix[2 * i + 1] += v * gr;
                if (mdiag) {
                    md.cat_sumsq[mcat] += double(v) * double(v) * 2.0;
                    md.cat_n[mcat] += 2;
                    if (std::fabs(v) > md.cat_peak[mcat]) md.cat_peak[mcat] = std::fabs(v);
                }
            } else {
                const float l0 = frame_sample(p, size_t(k), 0);
                const float l1 = frame_sample(p, size_t(k + 1), 0);
                const float r0 = frame_sample(p, size_t(k), 1);
                const float r1 = frame_sample(p, size_t(k + 1), 1);
                const float l = l0 + (l1 - l0) * f;
                const float r = r0 + (r1 - r0) * f;
                mix[2 * i] += l * gl;
                mix[2 * i + 1] += r * gr;
                if (mdiag) {
                    md.cat_sumsq[mcat] += double(l) * double(l) + double(r) * double(r);
                    md.cat_n[mcat] += 2;
                    if (std::fabs(l) > md.cat_peak[mcat]) md.cat_peak[mcat] = std::fabs(l);
                    if (std::fabs(r) > md.cat_peak[mcat]) md.cat_peak[mcat] = std::fabs(r);
                }
            }
        }
        p.p += double(n) * ratio;
        if (mdiag && e.cat == SoundCategory::Bgm) {
            md.bgm_pos_s = p.p / (p.rate > 0.0 ? p.rate : double(kOutputRate));
            md.bgm_in_b = p.in_b_segment ? 1 : 0;
            md.bgm_unplayable = p.unplayable ? 1 : 0;
            md.bgm_silent = p.silent_forever ? 1 : 0;
            std::snprintf(md.bgm_file, sizeof(md.bgm_file), "%s", p.file.c_str());
        }

        // Natural completion of a non-loop channel: when the last real frame
        // fell inside this block. Suppressed while an engine fade-out is
        // still running (the fade event ends the channel instead).
        if (!p.loop_play && !p.reported && p.zero_from != kNoFrame &&
            double(p.zero_from) <= block_end) {
            const bool fade_running =
                e.ch->fade && e.ch->fade->stop_on_complete &&
                (engine.state().clock_ms < e.ch->fade->start_ms + e.ch->fade->duration_ms);
            if (!fade_running) {
                p.reported = true;
                finishes.push_back({e.cat, e.id});
            }
        }
    }

    for (size_t i = 0; i < mix.size(); ++i) {
        if (mix[i] > 1.0f) mix[i] = 1.0f;
        else if (mix[i] < -1.0f) mix[i] = -1.0f;
        if (mdiag) {
            md.mix_sumsq += double(mix[i]) * double(mix[i]);
            ++md.mix_n;
            if (std::fabs(mix[i]) > md.mix_peak) md.mix_peak = std::fabs(mix[i]);
        }
    }
    if (mdiag) mix_diag_flush_seconds();
    capture_mix(mix.data(), n); // the produced signal, on disk
    if (sink_) sink_->push(mix.data(), n);
    capture_tick(SDL_GetTicks(), delta_ms, n, level_before,
                 sink_ ? sink_->queued_frames(AudioSink::kStreamMix) : 0, false);
    for (const Finish& f : finishes) {
        if (on_finish) on_finish(f.cat, f.id);
    }
}
} // namespace oa::media
