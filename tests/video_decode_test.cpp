// U9 real-decode test: drive oa::media::VideoEngine's Theora pump over the
// real game asset movie/logo.ogv (1280x720@60fps theora + vorbis track in
// fpm/root.pfs). Verifies: play starts with a decoded frame (RGBA 1280x720,
// revision 1), the frame clock decodes through EOF (~151 frames), the
// completion event is queued (the finish event the runtime polls), stop clears state, and the skip=2 / delaymargin play params stay
// on the channel through the decode path. Env: OA_TEST_FPM_PFS (skip code
// 77 when unset). No window, no UI: pure engine acceptance (research/29).
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/media/video.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_FPM_PFS unset; skipping\n");
        return 77;
    }
    try {
                oa::fs::PhysFileSystem fs(pfs_path, false);
        if (!fs.exists("movie/logo.ogv")) {
            std::fprintf(stderr, "movie/logo.ogv missing from the archive\n");
            return 1;
        }

        oa::media::VideoEngine v;
        v.set_loader([&fs](const std::string& file) -> std::optional<std::vector<uint8_t>> {
            return fs.read(file);
        });

        oa::media::VideoConfig cfg;
        cfg.file = "movie/logo.ogv";
        cfg.skippable = true; // skip=2 -> skippable (treated like 1,  TODO)
        cfg.loop_play = false;
        cfg.delay_margin_ms = 120; // layer jump-frame threshold, kept verbatim
        v.play_overlay(cfg);

        check(v.is_overlay_playing(), "overlay video playing after play");
        check(v.state().overlay_video.has_value(), "fullscreen channel exists");
        check(v.state().overlay_video->decoded, "decode attached (real playback)");
        check(v.state().overlay_video->file == "movie/logo.ogv", "channel file kept");
        check(v.state().overlay_video->skippable, "skip=2 kept (skippable)");
        check(v.state().overlay_video->delay_margin_ms && *v.state().overlay_video->delay_margin_ms == 120,
              "delaymargin=120 kept on the channel");

        // Frame 0 is current right after play: RGBA dims must be 1280x720.
        int w = 0, h = 0;
        const uint8_t* rgba = nullptr;
        uint64_t rev = 0;
        check(v.video_frame("", &w, &h, &rgba, &rev) && rgba, "current frame available");
        check(w == 1280 && h == 720, "frame dims 1280x720");
        check(rev == 1, "frame 0 decoded at play (revision 1)");
        uint64_t first_cksum = 0;
        {
            uint64_t c = first_cksum;
            for (size_t i = 0; i < size_t(w) * size_t(h) * 4; i += 4093)
                c = (c ^ rgba[i]) * 1099511628211ull;
            first_cksum = c;
        }

        // Run the channel clock until EOF. logo.ogv = 151 frames @ 60 fps =>
        // ~2517 ms; a 16 ms tick pump must finish within a few seconds.
        uint64_t finish_events = 0;
        bool finished = false;
        size_t decodes = 0;
        uint64_t cksum_at_half = 0;
        for (int tick = 0; tick < 400 && v.is_overlay_playing(); ++tick) {
            v.update(16);
            const auto evs = v.poll_finish_events();
            finish_events += evs.size();
            const uint64_t r2 = v.frame_revision("");
            if (r2 > decodes) decodes = r2;
            if (decodes == 76 && cksum_at_half == 0) {
                int w2, h2;
                const uint8_t* p2;
                if (v.video_frame("", &w2, &h2, &p2, nullptr) && p2) {
                    uint64_t c = 1469598103934665603ull;
                    for (size_t i = 0; i < size_t(w2) * size_t(h2) * 4; i += 4093)
                        c = (c ^ p2[i]) * 1099511628211ull;
                    cksum_at_half = c;
                }
            }
            if (!v.is_overlay_playing()) {
                finished = true;
                break;
            }
        }
        check(finished, "EOF reached: playing cleared by the decode pump");
        check(finish_events == 1, "one finish event queued at EOF (notify path)");
        const auto tail = v.poll_finish_events();
        check(tail.empty(), "poll drained the queue");
        check(decodes == 151, "all 151 frames decoded (frame clock to EOF)");
        check(v.frame_revision("") == 151, "final revision 151");
        {
            // Mid-stream frame content differs from frame 0 (the picture
            // animates): first_cksum was captured right after play
            // (video_frame staging is only valid until the next call).
            check(first_cksum != cksum_at_half, "frame content changes during playback");
        }
        // The decoded frame stays queryable until the host stops the channel
        // (runtime finish_video -> stop_overlay); then it must disappear.
        {
            int w3 = 0, h3 = 0;
            const uint8_t* p3 = nullptr;
            check(v.video_frame("", &w3, &h3, &p3, nullptr) && p3 && w3 == 1280,
                  "last frame still exposed before stop_overlay");
        }
        v.stop_overlay();
        check(!v.is_overlay_playing(), "stop_overlay clears the channel");
        check(!v.video_frame("", nullptr, nullptr, nullptr, nullptr),
              "no frame after stop_overlay");

        // Re-play with loop: the pump must restart at EOF, not finish.
        oa::media::VideoConfig lcfg;
        lcfg.file = "movie/logo.ogv";
        lcfg.loop_play = true;
        v.play_overlay(lcfg);
        check(v.is_overlay_playing(), "looped fullscreen playing");
        bool looped = false;
        for (int tick = 0; tick < 400; ++tick) {
            v.update(16);
            (void)v.poll_finish_events();
            const uint64_t r = v.frame_revision("");
            if (r > 151) looped = true; // passed the first cycle's last frame
            if (!v.is_overlay_playing()) break;
        }
        check(looped, "loop playback restarts past EOF (no finish)");
        check(v.is_overlay_playing(), "looped channel still playing after a cycle");
        v.stop_overlay();
        check(!v.is_overlay_playing(), "stop_overlay clears the channel");
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "exception: %s\n", e.what());
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("video_decode_test OK (logo.ogv 151 frames 1280x720@60, EOF + loop)\n");
    return 0;
}
