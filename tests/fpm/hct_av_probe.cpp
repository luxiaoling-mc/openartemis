// research/88 real-device audio probe — engine-level reproduction of the
// windowed app's movie-audio path (VideoEngine driver pump -> MediaPlayers
// movie stream -> real SDL device), one movie to EOF + a post-EOF window,
// with per-second engine counters and the OA_AUDIO_DIAG per-stream summary.
//
// REPORT-ONLY (not a ctest). Env:
//   OA_TEST_HCT_PFS      archive (77 when unset)
//   OA_HCT_AV_FILE       movie file (default movie_bai/dcpyzcv3t.mp4)
//   OA_HCT_AV_TICK=1     pump on the 16 ms tick clock (no driver thread)
//   OA_HCT_AV_LAYER=1    play on a layer channel instead of the overlay
//   OA_HCT_AV_POST       post-EOF observation seconds (default 6)
//   OA_HCT_AV_SECS       hard wall-clock cap in seconds (default 200)
//   OA_AUDIO_DIAG=1      per-stream delivery diagnostics (core/media/audio.cpp §3)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
#include "core/media/audio.h"  // AudioEngine + MediaPlayers (merged, research/114)
#include "core/media/decode_pool.h"
#include "core/media/video.h"

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pfs_path = std::getenv("OA_HCT_AV_PFS");
    if (!pfs_path || !*pfs_path) pfs_path = std::getenv("OA_TEST_HCT_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_HCT_PFS/OA_HCT_AV_PFS unset; skipping\n");
        return 77;
    }
    const char* file = std::getenv("OA_HCT_AV_FILE");
    if (!file || !*file) file = "movie_bai/dcpyzcv3t.mp4";
    const bool tick_mode = std::getenv("OA_HCT_AV_TICK") != nullptr;
    const bool layer_mode = std::getenv("OA_HCT_AV_LAYER") != nullptr;
    const int post_secs =
        std::getenv("OA_HCT_AV_POST") ? std::atoi(std::getenv("OA_HCT_AV_POST")) : 6;
    const double wall_cap =
        std::getenv("OA_HCT_AV_SECS") ? std::atof(std::getenv("OA_HCT_AV_SECS")) : 200.0;
    std::printf("[avprobe] file='%s' pump=%s channel=%s post=%ds\n", file,
                tick_mode ? "tick16" : "driver", layer_mode ? "layer" : "overlay",
                post_secs);

    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, true);
    oa::media::DecodePool pool(tick_mode ? 0 : 2);
    oa::media::AudioEngine ae;
    oa::media::MediaPlayers mp;
    mp.init();
    mp.set_loader([&fs](const std::string& f) -> std::optional<std::vector<uint8_t>> {
        return fs->read(f);
    });
    if (!tick_mode) mp.set_decode_pool(&pool);
    oa::media::VideoEngine ve;
    ve.set_loader([&fs](const std::string& f) -> std::optional<std::vector<uint8_t>> {
        return fs->read(f);
    });
    if (!tick_mode) ve.set_decode_pool(&pool);
    ve.set_audio_output([&mp](const float* d, size_t n) { mp.push_video_audio(d, n); });

    oa::media::VideoConfig cfg;
    cfg.file = file;
    cfg.skippable = true;
    if (layer_mode) {
        ve.play_layer("probe", cfg);
        std::printf("[avprobe] layer play '%s' playing=%d\n", file,
                    (int)ve.is_layer_playing("probe"));
    } else {
        ve.play_overlay(cfg);
        std::printf("[avprobe] overlay play '%s' playing=%d\n", file,
                    (int)ve.is_overlay_playing());
    }

    auto snap = [&] {
        const auto s = ve.state();
        const auto* ch = s.overlay_video ? &*s.overlay_video : nullptr;
        if (!ch) {
            const auto it = s.video_layers.find("probe");
            ch = it == s.video_layers.end() ? nullptr : &it->second;
        }
        return ch;
    };

    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    auto last = t0;
    auto last_print = t0;
    bool eof_reported = false;
    uint64_t last_frames = 0;
    double eof_wall = 0.0;
    size_t finish_events = 0;
    while (true) {
        const auto now = Clock::now();
        const double wall_s =
            std::chrono::duration<double>(now - t0).count();
        const uint64_t delta_ms = uint64_t(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count());
        last = now;
        if (delta_ms > 0) {
            mp.update(delta_ms, ae, [](oa::media::SoundCategory, const std::string&) {});
            if (tick_mode) ve.update(delta_ms);
        }
        // EOF events from the video channel (overlay id "" / layer "probe").
        for (const auto& ev : ve.poll_finish_events()) {
            ++finish_events;
            std::printf("[avprobe] finish event id='%s' wall=%.2fs\n",
                        ev.id.c_str(), wall_s);
        }
        const auto* ch = snap();
        const bool playing = ch && ch->playing;
        if (!playing && !eof_reported) {
            // give the last pushes a moment, then note the EOF wall time
            if (eof_wall == 0.0) eof_wall = wall_s;
            if (wall_s - eof_wall > 0.4) {
                eof_reported = true;
                std::printf("[avprobe] EOF wall=%.2fs frames=%llu(%.2fs) "
                            "active=%llu peak=%.4f finish_events=%zu\n",
                            wall_s, ch ? (unsigned long long)ch->audio_frames : 0,
                            ch ? double(ch->audio_frames) / 44100.0 : 0.0,
                            ch ? (unsigned long long)ch->audio_active_frames : 0,
                            ch ? ch->audio_peak : 0.0f, finish_events);
            }
        }
        if (ch && playing && wall_s - std::chrono::duration<double>(last_print - t0).count() >= 1.0) {
            last_print = now;
            const double rate = double(ch->audio_frames - last_frames) / 44100.0 / 1.0;
            std::printf("[avprobe] t=%6.2fs pos=%6llums frames=%llu(%.2fs) "
                        "sec_rate=%.3f\n",
                        wall_s, (unsigned long long)ch->position_ms,
                        (unsigned long long)ch->audio_frames,
                        double(ch->audio_frames) / 44100.0, rate);
            last_frames = ch->audio_frames;
        }
        const double since_eof = eof_reported ? wall_s - eof_wall : 0.0;
        if ((eof_reported && since_eof >= double(post_secs)) ||
            wall_s >= wall_cap)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    const double wall_s = std::chrono::duration<double>(Clock::now() - t0).count();
    const auto* ch = snap();
    std::printf("[avprobe] END wall=%.2fs frames=%llu(%.2fs)\n", wall_s,
                ch ? (unsigned long long)ch->audio_frames : 0,
                ch ? double(ch->audio_frames) / 44100.0 : 0.0);
    mp.print_audio_diag("avprobe");
    mp.release();
    return 0;
}
