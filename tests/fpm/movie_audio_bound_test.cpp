// research/118 engine-level movie-audio bound + deterministic EOF flush.
//
// The queued level of the movie's bound device stream IS the audible A/V
// offset (the driver pump produces the container audio clock-locked to the
// picture's position_ms, so what sits queued is exactly what the listener
// hears late). Because production and consumption both run at 1x, a single
// stall of the pump (a synchronous decode, a descheduled driver thread)
// raises that level and nothing ever brings it back down: the movie plays
// with a standing offset and, at EOF, the same amount keeps sounding after
// the picture (the "audio is late after the video" report).
//
// This test drives the movie on the TICK clock (deterministic, no wall-time
// waiting) with a real device attached, so the producer deliberately outruns
// the device — the worst case for the bound:
//   arm 1 (bounded, the fix): the sampled level never exceeds the bound plus
//     one produced chunk, the A/V offset stays inside the bound, and the
//     channel's EOF leaves 0 frames queued (deterministic flush).
//   arm 2 (negative control, bound disabled = pre-fix): the same geometry
//     carries seconds of standing level and a non-empty tail after EOF —
//     proving the arm-1 assertions can fail.
//
// Env: OA_TEST_HCT_PFS (movie_bai/logo.mp4, ~5.06 s silent AAC track);
// SDL_AUDIODRIVER (default dummy). Skip 77 when the archive is unset.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "SDL3/SDL.h"
#include "core/fs/physfs_fs.h"
#include "core/media/audio.h"
#include "core/media/video.h"

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

constexpr size_t kBoundFrames = 5292; // AudioSink::kMovieBoundFrames (120 ms)
constexpr size_t kTickFrames = 705;   // one 16 ms tick of 44100 Hz stereo

struct ArmResult {
    size_t max_level = 0;
    double max_offset_ms = 0.0;
    size_t level_at_eof = 0;
    size_t final_audio_frames = 0;
    uint64_t position_ms = 0;
};

ArmResult run_arm(std::shared_ptr<const oa::fs::IFileSystem> fs, bool bounded) {
    oa::media::MediaPlayers mp;
    oa::media::AudioEngine ae;
    if (!mp.init()) {
        std::printf("no audio device; skipping\n");
        std::exit(77);
    }
    mp.set_audio_bound_enabled(bounded);
    mp.set_loader([&fs](const std::string& f) -> std::optional<std::vector<uint8_t>> {
        return fs->read(f);
    });
    oa::media::VideoEngine v;
    v.set_loader([&fs](const std::string& f) -> std::optional<std::vector<uint8_t>> {
        return fs->read(f);
    });
    v.set_audio_output([&mp](const float* d, size_t n) { mp.push_video_audio(d, n); });
    v.set_audio_flush([&mp]() { mp.flush_video_audio(); });

    oa::media::VideoConfig cfg;
    cfg.file = "movie_bai/logo.mp4";
    cfg.skippable = true;
    v.play_overlay(cfg);
    check(v.is_overlay_playing(), "arm(bounded=%d): overlay playing", (int)bounded);

    ArmResult r;
    bool eof = false;
    for (size_t tick = 1; tick < 2000 && !eof; ++tick) {
        v.update(16); // deterministic tick clock (no wall-time wait)
        v.poll_finish_events();
        const auto s = v.state();
        const auto* ch = s.overlay_video ? &*s.overlay_video : nullptr;
        if (!ch || !ch->playing) {
            eof = true;
            // The EOF edge: the engine flushes the movie stream here.
            r.level_at_eof = mp.audio_level_frames(1);
            r.final_audio_frames = ch ? ch->audio_frames : 0;
            r.position_ms = ch ? ch->position_ms : 0;
            break;
        }
        const size_t level = mp.audio_level_frames(1);
        if (level > r.max_level) r.max_level = level;
        // Audible A/V offset: the picture clock against the content the
        // listener is actually hearing (produced frames minus the queue).
        const double heard_ms = double(ch->audio_frames > level ? ch->audio_frames - level : 0)
                                / 44.1;
        const double off = double(ch->position_ms) - heard_ms;
        const double aoff = off < 0 ? -off : off;
        if (aoff > r.max_offset_ms) r.max_offset_ms = aoff;
    }
    check(eof, "arm(bounded=%d): movie reached EOF", (int)bounded);
    // The device drains asynchronously; the flush is deterministic, so the
    // tail must be gone right away (a 30 ms grace only covers the device
    // pulling what was already handed to it).
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    r.level_at_eof = mp.audio_level_frames(1);
    mp.release();
    return r;
}
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pfs_path = std::getenv("OA_TEST_HCT_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_HCT_PFS unset; skipping\n");
        return 77;
    }
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, true);
    if (!fs->exists("movie_bai/logo.mp4")) {
        std::fprintf(stderr, "movie_bai/logo.mp4 missing (loose dir not mounted?)\n");
        return 1;
    }

    const ArmResult fixed = run_arm(fs, true);
    std::printf("[movbound] bounded : max_level=%zu (%.1fms) max_av_offset=%.1fms "
                "level_at_eof=%zu audio_frames=%zu pos=%llums\n",
                fixed.max_level, double(fixed.max_level) / 44.1, fixed.max_offset_ms,
                fixed.level_at_eof, fixed.final_audio_frames,
                (unsigned long long)fixed.position_ms);
    check(fixed.max_level <= kBoundFrames + kTickFrames,
          "bounded: the movie stream never carries more than the bound+chunk "
          "(max_level=%zu frames = %.1f ms)",
          fixed.max_level, double(fixed.max_level) / 44.1);
    check(fixed.max_offset_ms <= 160.0,
          "bounded: the audible A/V offset stays inside the bound "
          "(max=%.1f ms)", fixed.max_offset_ms);
    check(fixed.level_at_eof == 0,
          "bounded: the EOF flush leaves no movie tail queued (level=%zu)",
          fixed.level_at_eof);
    check(fixed.final_audio_frames > 200000,
          "bounded: the whole silent track was produced (frames=%zu)",
          fixed.final_audio_frames);

    // Negative control: identical geometry with the bound off (pre-fix). The
    // producer outruns the device, so the level must run away — if it did
    // not, the assertions above would be vacuous.
    const ArmResult pre = run_arm(fs, false);
    std::printf("[movbound] pre-fix : max_level=%zu (%.1fms) max_av_offset=%.1fms "
                "level_at_eof=%zu\n",
                pre.max_level, double(pre.max_level) / 44.1, pre.max_offset_ms,
                pre.level_at_eof);
    check(pre.max_level >= 44100,
          "pre-fix (negative control): the unbounded stream really does carry "
          ">=1 s of standing level (max=%zu frames = %.1f ms)",
          pre.max_level, double(pre.max_level) / 44.1);
    check(pre.level_at_eof > 0,
          "pre-fix: the tail survives EOF without the deterministic flush "
          "(level=%zu)", pre.level_at_eof);

    if (g_failures == 0) std::printf("movie_audio_bound_test: all ok\n");
    return g_failures == 0 ? 0 : 1;
}
