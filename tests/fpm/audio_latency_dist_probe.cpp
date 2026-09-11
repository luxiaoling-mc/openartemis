// research/118 diagnostic (report-only; NOT a ctest): the distribution of the
// audible audio delay across runs, before/after the bounded-push fix.
//
// Geometry = the reported one (snll story-start video → sounds after it): the
// app's frame clock is what feeds the device stream, so the queued level of
// that stream IS the delay a sound started right now would suffer.
//   phase 1  "before the video": 60 ticks x 16.7 ms (60 fps)
//   phase 2  "the video":         12 ticks x random 100-600 ms (decode/render
//                                 hitches — the app's frame time is the
//                                 producer cadence, so this is the burst)
//   phase 3  "after the video":  120 ticks x 16.7 ms (60 fps again)
// Sampled per phase: the standing queued level (frames -> ms of delay).
//
// Arms (`OA_LAT_ARM=both|pre|post`):
//   post = bounded push + latency-drain pacer + deterministic flush (fixed)
//   pre  = same binary with the bound disabled (OA_AUDIO_NO_BOUND semantics)
// The two arms differ only in that switch, and both run the SAME hitch
// schedules (seeded), so the delta is the fix, not the weather.
//
// Env: OA_LAT_RUNS (default 24), OA_LAT_SEED (default 118),
//      OA_LAT_LOAD (optional: N busy threads as machine-load perturbation),
//      SDL_AUDIODRIVER (default dummy; set pipewire/alsa for a real device).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "SDL3/SDL.h"
#include "core/media/audio.h"

namespace {
using Clock = std::chrono::steady_clock;
constexpr int kMix = 0;
constexpr int kMovie = 1;

struct Sample {
    // Standing delay (ms) at the end of each phase.
    double before_ms = 0.0;
    double video_end_ms = 0.0;
    double after_0p5s_ms = 0.0;
    double after_end_ms = 0.0;
    double max_ms = 0.0;
    double target_ms = 0.0;
    uint64_t paced_out = 0;
};

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t i = size_t(std::min<double>(double(v.size() - 1),
                                             std::floor(p * double(v.size()))));
    return v[i];
}

/// One run: fresh engine state (no players) and an emptied device stream.
Sample one_run(oa::media::MediaPlayers& mp, oa::media::AudioEngine& ae, bool bound,
               uint32_t seed, std::mt19937& rng) {
    mp.set_audio_bound_enabled(bound);
    mp.flush_video_audio();
    // Drain the mix stream back to empty by ticking with nothing produced
    // (update() with the bound on is a no-op while the level is above target;
    // with the bound off, push silence and let the device eat it).
    for (int i = 0; i < 40 && mp.audio_level_frames(kMix) > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!bound) mp.update(1, ae, nullptr);
    }
    Sample s;
    s.target_ms = double(mp.mix_target_frames()) / 44.1;
    auto tick = [&](uint64_t ms) { mp.update(ms, ae, nullptr); };
    auto level_ms = [&](int src) { return double(mp.audio_level_frames(src)) / 44.1; };

    auto next = Clock::now();
    for (int i = 0; i < 60; ++i) { // phase 1
        next += std::chrono::milliseconds(17);
        std::this_thread::sleep_until(next);
        tick(17);
    }
    s.before_ms = level_ms(kMix);
    // phase 2: the video section — frame times of 100..600 ms
    std::uniform_int_distribution<int> hitch(100, 600);
    for (int i = 0; i < 12; ++i) {
        const int ms = hitch(rng);
        next += std::chrono::milliseconds(ms);
        std::this_thread::sleep_until(next);
        tick(uint64_t(ms));
        s.max_ms = std::max(s.max_ms, level_ms(kMix));
    }
    s.video_end_ms = level_ms(kMix);
    // phase 3: after the video — 60 fps again; sample the delay a sound
    // started now would suffer (first 0.5 s, then the standing level).
    for (int i = 0; i < 120; ++i) {
        next += std::chrono::milliseconds(17);
        std::this_thread::sleep_until(next);
        tick(17);
        s.max_ms = std::max(s.max_ms, level_ms(kMix));
        if (i == 29) s.after_0p5s_ms = level_ms(kMix);
    }
    s.after_end_ms = level_ms(kMix);
    s.paced_out = mp.paced_out_ticks();
    (void)seed;
    return s;
}

void report(const char* arm, const std::vector<Sample>& v) {
    std::vector<double> before, video_end, after05, after_end, mx;
    for (const Sample& s : v) {
        before.push_back(s.before_ms);
        video_end.push_back(s.video_end_ms);
        after05.push_back(s.after_0p5s_ms);
        after_end.push_back(s.after_end_ms);
        mx.push_back(s.max_ms);
    }
    auto line = [&](const char* what, std::vector<double> x) {
        std::printf("[latdist] %-4s %-18s P50=%7.1fms P95=%7.1fms max=%7.1fms\n",
                    arm, what, pct(x, 0.50), pct(x, 0.95), pct(x, 1.0));
    };
    std::printf("[latdist] %-4s runs=%zu target=%.1fms\n", arm, v.size(),
                v.empty() ? 0.0 : v[0].target_ms);
    line("before-video", before);
    line("video-end", video_end);
    line("after-video+0.5s", after05);
    line("after-video-stand", after_end);
    line("run-max", mx);
    uint64_t paced = 0;
    for (const Sample& s : v) paced += s.paced_out;
    std::printf("[latdist] %-4s latency-drain ticks=%llu\n", arm,
                (unsigned long long)paced);
}
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    const int runs = std::getenv("OA_LAT_RUNS") ? std::atoi(std::getenv("OA_LAT_RUNS")) : 24;
    const uint32_t seed =
        std::getenv("OA_LAT_SEED") ? uint32_t(std::atoi(std::getenv("OA_LAT_SEED"))) : 118u;
    const std::string arm = std::getenv("OA_LAT_ARM") ? std::getenv("OA_LAT_ARM") : "both";
    // Optional machine-load perturbation (the "different load" arm of the
    // distribution protocol): busy threads make the hitches land differently.
    const int load = std::getenv("OA_LAT_LOAD") ? std::atoi(std::getenv("OA_LAT_LOAD")) : 0;
    std::vector<std::thread> loaders;
    std::atomic<bool> stop{false};
    for (int i = 0; i < load; ++i) {
        loaders.emplace_back([&stop] {
            double x = 0.0;
            while (!stop.load()) {
                for (int k = 0; k < 200000; ++k) x += std::sin(double(k));
            }
            if (x > 1e30) std::printf("");
        });
    }

    oa::media::MediaPlayers mp;
    oa::media::AudioEngine ae;
    if (!mp.init()) {
        std::printf("no audio device available; skipping\n");
        for (auto& t : loaders) t.join();
        return 77;
    }
    std::printf("[latdist] driver=%s runs=%d seed=%u load=%d arm=%s target=%.1fms\n",
                SDL_GetCurrentAudioDriver(), runs, seed, load, arm.c_str(),
                double(mp.mix_target_frames()) / 44.1);

    // Identical hitch schedules per arm (same seed, fresh RNG each arm).
    if (arm == "pre" || arm == "both") {
        std::mt19937 rng(seed);
        std::vector<Sample> v;
        for (int i = 0; i < runs; ++i) v.push_back(one_run(mp, ae, false, seed, rng));
        report("pre", v);
    }
    if (arm == "post" || arm == "both") {
        std::mt19937 rng(seed);
        std::vector<Sample> v;
        for (int i = 0; i < runs; ++i) v.push_back(one_run(mp, ae, true, seed, rng));
        report("post", v);
    }
    stop.store(true);
    for (auto& t : loaders) t.join();
    mp.release();
    return 0;
}
