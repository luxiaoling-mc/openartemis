// research/88: the wall-clock video driver must never credit idle time into
// a later channel's clocks. Real-machine listening found the story-head
// digest movie of 灵感满溢的甜蜜创想凸 starting ~16 s in: the driver loop
// measured its first active delta from the LAST ACTIVE iteration — i.e. the
// whole boot-logo→title gap — so the new channel's position jumped ~16 s at
// once (head frames skipped in presentation) and the audio pump burst ~16 s
// into the SDL movie stream, which paced it into a ~2.75 s standing backlog
// that (a) left the audible audio trailing the picture by seconds for the
// whole movie and (b) kept playing for seconds after the movie EOF.
//
// This test reproduces the exact geometry headless-with-driver: play one
// short movie to its natural EOF (driver goes idle), sit idle ~2 s, then
// play again. On the buggy driver the second play's first samples sit at
// position ≈ idle gap and the audio clock owes the same gap; after the fix
// the second play starts at ~0 and runs its full duration. The engine
// counters (position_ms / audio_frames) prove it without any audio device.
//
// Env: OA_TEST_HCT_PFS (movie_bai/logo.mp4, ~5.06 s digitally silent AAC
// track). Skip 77 when unset. Real-time run (~16 s wall).
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "core/fs/physfs_fs.h"
#include "core/media/decode_pool.h"
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

using Clock = std::chrono::steady_clock;

/// Drive one play to EOF; returns the wall seconds the channel played.
double play_to_eof(oa::media::VideoEngine& v, const char* what, double cap_s) {
    const auto t0 = Clock::now();
    size_t finish_events = 0;
    while (true) {
        const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
        for (const auto& ev : v.poll_finish_events()) {
            (void)ev;
            ++finish_events;
        }
        const auto s = v.state();
        const bool playing =
            (s.overlay_video && s.overlay_video->playing) ||
            [&] {
                for (const auto& [id, ch] : s.video_layers) {
                    (void)id;
                    if (ch.playing) return true;
                }
                return false;
            }();
        if (!playing && wall > 0.1) {
            std::printf("[gap] %s EOF wall=%.2fs finish_events=%zu\n", what, wall,
                        finish_events);
            check(finish_events == 1, "%s: exactly one finish event", what);
            return wall;
        }
        check(wall < cap_s, "%s: timed out after %.1fs (playing=%d)", what, wall,
              (int)playing);
        if (wall >= cap_s) return wall;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

uint64_t channel_audio_frames(oa::media::VideoEngine& v) {
    const auto s = v.state();
    if (s.overlay_video) return s.overlay_video->audio_frames;
    for (const auto& [id, ch] : s.video_layers) {
        (void)id;
        if (ch.playing) return ch.audio_frames;
    }
    return 0;
}

uint64_t channel_position_ms(oa::media::VideoEngine& v) {
    const auto s = v.state();
    if (s.overlay_video) return s.overlay_video->position_ms;
    for (const auto& [id, ch] : s.video_layers) {
        (void)id;
        if (ch.playing) return ch.position_ms;
    }
    return 0;
}
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pfs_path = std::getenv("OA_TEST_HCT_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_HCT_PFS unset; skipping\n");
        return 77;
    }
    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, true);
    const std::string file = "movie_bai/logo.mp4"; // ~5.06 s, silent AAC track
    if (!fs->exists(file)) {
        std::fprintf(stderr, "%s missing (loose movie_bai dir not mounted?)\n",
                     file.c_str());
        return 1;
    }
    oa::media::DecodePool pool(2); // real decode workers -> wall-clock driver
    oa::media::VideoEngine v;
    v.set_decode_pool(&pool);
    v.set_loader([&fs](const std::string& f) -> std::optional<std::vector<uint8_t>> {
        return fs->read(f);
    });

    // ---- first play: driver starts fresh, plays to EOF, goes idle --------
    oa::media::VideoConfig cfg;
    cfg.file = file;
    cfg.skippable = true;
    v.play_overlay(cfg);
    check(v.is_overlay_playing(), "first play started");
    const double first_s = play_to_eof(v, "first", 12.0);
    check(first_s > 4.5 && first_s < 7.0,
          "first play ran its full ~5.06 s (wall=%.2fs)", first_s);
    const uint64_t first_frames = channel_audio_frames(v);
    std::printf("[gap] first play frames=%llu (%.3fs)\n",
                (unsigned long long)first_frames, double(first_frames) / 44100.0);
    check(first_frames > 200000 && first_frames < 240000,
          "first play produced the whole silent track (frames=%llu)",
          (unsigned long long)first_frames);

    // ---- idle gap ~2 s (the boot-title stretch in the real app) ----------
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    check(!v.is_overlay_playing(), "channel idle after EOF");

    // ---- second play: must start from ~0, not from the idle gap ----------
    v.play_overlay(cfg);
    check(v.is_overlay_playing(), "second play started");
    // Sample shortly after the driver picks the play up (~10 ms poll).
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const uint64_t p2_pos = channel_position_ms(v);
    const uint64_t p2_frames = channel_audio_frames(v);
    std::printf("[gap] second play @0.25s: pos=%llums frames=%llu (%.3fs)\n",
                (unsigned long long)p2_pos, (unsigned long long)p2_frames,
                double(p2_frames) / 44100.0);
    // Buggy driver: position jumped by the whole 2 s idle gap (~2000 ms+ and
    // ~90k+ frames owed). Fixed driver: <= ~50 ms into the movie.
    check(p2_pos < 500, "second play starts at ~0 (pos=%llums, not the idle gap)",
          (unsigned long long)p2_pos);
    check(p2_frames < 30000,
          "audio clock owes ~0.25 s at the 0.25 s sample (frames=%llu)",
          (unsigned long long)p2_frames);
    const double second_s = play_to_eof(v, "second", 12.0);
    check(second_s > 4.0 && second_s < 7.0,
          "second play ran its FULL ~5.06 s (wall=%.2fs; the head was not "
          "skipped)",
          second_s);
    const uint64_t second_frames = channel_audio_frames(v);
    std::printf("[gap] second play frames=%llu (%.3fs)\n",
                (unsigned long long)second_frames, double(second_frames) / 44100.0);
    check(second_frames > 200000 && second_frames < 240000,
          "second play produced the whole track again (frames=%llu)",
          (unsigned long long)second_frames);

    v.stop_overlay();
    if (g_failures == 0) std::printf("tm_movie_gap_test: all ok\n");
    return g_failures == 0 ? 0 : 1;
}
