// research/118 — audio output latency bound (root fix of the "sound delayed
// after the video" ratchet).
//
// The queued level of a device-bound SDL_AudioStream IS the audible latency:
// the device plays the queue in order, so what sits queued is what the
// listener hears late. Both producers push one wall-clock-sized chunk per
// tick, and production == consumption == 1x, so the queue level does not
// drain by itself: a single hitch of D ms pushes D ms in one chunk while the
// device empties the small standing queue, and from then on EVERY sound is
// heard D ms late for the rest of the session (a ratchet). research/86/87
// found the burst forms (double producer / idle-gap clock credit); this test
// pins the residual form — an ordinary frame hitch — and the fix: a STANDING
// backlog is dropped before the next chunk is appended, so the level can
// never carry a permanent delay.
//
// Arms (deterministic structure, real device clock):
//   1. bounded pump: 30 x 16 ms ticks -> small level; a 400 ms hitch; 45 more
//      ticks -> the standing level is back at the tick chunk (<= bound);
//      every sample stays <= bound + one chunk.
//   2. negative control (bound disabled = pre-fix behaviour): the same
//      sequence keeps the hitch's level forever — the test proves it can
//      tell the two apart (a green arm 1 with a green arm 2 would be
//      worthless).
//   3. movie stream: a stalled driver pushes a 400 ms chunk; the next chunk
//      drops the standing backlog (bounded), and flush_video_audio() empties
//      the stream deterministically (research/118's EOF/stop flush).
//
// The audio device is the SDL dummy backend by default (real-time paced, no
// hardware needed); SDL_AUDIODRIVER overrides it for a real-machine run.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "SDL3/SDL.h"
#include "core/media/audio.h"

namespace {
int g_failures = 0;
void check(bool cond, const char* fmt, ...) {
    if (cond) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "FAIL: %s\n", buf);
    ++g_failures;
}

using Clock = std::chrono::steady_clock;

constexpr int kMix = 0;
constexpr int kMovie = 1;
constexpr uint64_t kTickMs = 16;
constexpr size_t kTickFrames = size_t(44100 * kTickMs / 1000); // 705
constexpr uint64_t kHitchMs = 400;
constexpr size_t kHitchFrames = size_t(44100 * kHitchMs / 1000); // 17640
/// AudioSink's research/118 bound (mix stream). The test asserts against the
/// contract, not the constant: a standing level above one bound + one chunk
/// means the queue is carrying a delay that production cannot drain.
constexpr size_t kBoundFrames = 4410; // AudioSink::kMixBoundFrames

std::vector<float> silence(size_t frames) { return std::vector<float>(frames * 2, 0.0f); }

/// Tick the engine's mix exactly like the app does: one push per wall-clock
/// period, delta = measured wall time.
void pump(oa::media::MediaPlayers& mp, oa::media::AudioEngine& ae, uint64_t ms) {
    mp.update(ms, ae, nullptr);
}

void pump_ticks(oa::media::MediaPlayers& mp, oa::media::AudioEngine& ae, int n) {
    auto next = Clock::now();
    for (int i = 0; i < n; ++i) {
        next += std::chrono::milliseconds(kTickMs);
        std::this_thread::sleep_until(next);
        pump(mp, ae, kTickMs);
    }
}

uint64_t ms_of(size_t frames) { return uint64_t(frames) * 1000 / 44100; }
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // Default to the real-time dummy backend so the test runs anywhere; an
    // explicit SDL_AUDIODRIVER (real machine) still wins (env > hint).
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");

    oa::media::MediaPlayers mp;
    oa::media::AudioEngine ae;
    if (!mp.init()) {
        std::printf("no audio device available; skipping\n");
        return 77;
    }
    std::printf("[bound] device open; bound mix=%zu frames (%llu ms) via %s\n",
                kBoundFrames, (unsigned long long)ms_of(kBoundFrames),
                SDL_GetCurrentAudioDriver());
    // Nothing plays: the mix is silence, which is exactly the app's idle tick
    // load (research/86: the tick pushes the mix unconditionally).
    std::vector<float> mix = silence(kTickFrames);

    // ---- arm 1: bounded (the fix) ----------------------------------------
    pump_ticks(mp, ae, 30);
    const size_t l_before = mp.audio_level_frames(kMix);
    // The hitch: the app froze for 400 ms, so this tick owes 400 ms of audio.
    std::this_thread::sleep_for(std::chrono::milliseconds(kHitchMs));
    pump(mp, ae, kHitchMs);
    const size_t l_hitch = mp.audio_level_frames(kMix);
    // Drain window: the pacer withholds frames while the stream carries more
    // than its target, so the queued audio (which keeps playing seamlessly)
    // walks the level back down instead of ratcheting.
    size_t l_drain_mid = 0;
    for (int i = 0; i < 45; ++i) {
        pump_ticks(mp, ae, 1);
        if (i == 9) l_drain_mid = mp.audio_level_frames(kMix);
    }
    const size_t l_after = mp.audio_level_frames(kMix);
    const size_t target = mp.mix_target_frames();
    std::printf("[bound] arm1 bounded: before=%zu (%llums) after_hitch=%zu "
                "(%llums) drain@0.16s=%zu standing=%zu (%llums) target=%zu "
                "paced_out=%llu\n",
                l_before, (unsigned long long)ms_of(l_before), l_hitch,
                (unsigned long long)ms_of(l_hitch), l_drain_mid, l_after,
                (unsigned long long)ms_of(l_after), target,
                (unsigned long long)mp.paced_out_ticks());
    check(l_before <= kBoundFrames, "arm1: idle ticking keeps the level bounded");
    check(l_hitch >= kHitchFrames - kTickFrames,
          "arm1: the hitch chunk itself is what lands in the stream (level=%zu)",
          l_hitch);
    check(l_drain_mid < l_hitch,
          "arm1: the pacer drains the hitch backlog (mid=%zu < hitch=%zu)",
          l_drain_mid, l_hitch);
    check(mp.paced_out_ticks() > 0,
          "arm1: the pacer withheld frames instead of deepening the delay");
    // The standing level after the hitch must be back at the producer's
    // cadence, never the hitch. The bound is the target + TWO chunks: the
    // level is sampled right after the tick's push, and research/125's pacer
    // holds the level AT the mark from below too (the pre-125 one-sided rule
    // only ever parked under it), so the sample sits one chunk above the mark
    // (the drain that chunk covers). It is still 6.4x below the 400 ms hitch
    // burst, which is what this assertion has teeth for.
    check(l_after <= target + 2 * kTickFrames,
          "arm1: the hitch left no standing delay (level=%zu frames = %llums, "
          "hitch was %llums)",
          l_after, (unsigned long long)ms_of(l_after), (unsigned long long)kHitchMs);
    check(l_after < kHitchFrames / 4,
          "arm1: standing level is nowhere near the hitch burst (%zu)", l_after);

    // ---- arm 2: negative control (bound off = pre-fix) -------------------
    mp.set_audio_bound_enabled(false);
    pump_ticks(mp, ae, 30);
    std::this_thread::sleep_for(std::chrono::milliseconds(kHitchMs));
    pump(mp, ae, kHitchMs);
    pump_ticks(mp, ae, 45);
    const size_t l_unbounded = mp.audio_level_frames(kMix);
    std::printf("[bound] arm2 pre-fix (bound off): standing=%zu (%llums)\n",
                l_unbounded, (unsigned long long)ms_of(l_unbounded));
    check(l_unbounded >= kHitchFrames - size_t(4410),
          "arm2 (negative control): the unfixed producer really does ratchet "
          "the level up to the hitch (level=%zu frames = %llums)",
          l_unbounded, (unsigned long long)ms_of(l_unbounded));
    mp.set_audio_bound_enabled(true);

    // ---- arm 3: movie stream bound + deterministic flush -----------------
    std::vector<float> chunk = silence(kHitchFrames); // a stalled driver burst
    mp.push_video_audio(chunk.data(), chunk.size() / 2);
    const size_t m_hitch = mp.audio_level_frames(kMovie);
    std::vector<float> one = silence(kTickFrames); // the next driver iteration
    mp.push_video_audio(one.data(), one.size() / 2);
    const size_t m_after = mp.audio_level_frames(kMovie);
    std::printf("[bound] arm3 movie: after_burst=%zu (%llums) after_next=%zu "
                "(%llums)\n",
                m_hitch, (unsigned long long)ms_of(m_hitch), m_after,
                (unsigned long long)ms_of(m_after));
    check(m_after <= kBoundFrames + kTickFrames,
          "arm3: the stalled-driver burst leaves no standing movie delay "
          "(level=%zu frames = %llums)",
          m_after, (unsigned long long)ms_of(m_after));
    mp.flush_video_audio();
    check(mp.audio_level_frames(kMovie) == 0,
          "arm3: the deterministic flush empties the movie stream (EOF/stop "
          "must leave nothing behind)");

    mp.release();
    if (g_failures == 0) std::printf("audio_latency_bound_test: all ok\n");
    return g_failures == 0 ? 0 : 1;
}
