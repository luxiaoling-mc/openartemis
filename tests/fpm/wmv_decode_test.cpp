// research/54: real WMV playback acceptance over the REAL NekoMiko boot asset
// movie/logo_emote.wmv (ASF container, wmv3 1920x1080@60, ~4.13 s) through
// the VideoEngine decode pump and the optional FFmpeg backend (FfmpegSource,
// vcpkg "ffmpeg" port; built only when OA_HAVE_FFMPEG is defined). The file
// is the one NekoMiko's boot brand flow plays via estag{"video",...}
// (script/brandlogo.ast "logo_emote" entry) and the pre-research/54 engine
// silently skipped (no Ogg/Theora stream -> logical immediate finish).
//
// Verifies (mirror of video_decode_test.cpp over movie/logo.ogv):
//   - play starts with a decoded frame (RGBA 1920x1080, revision 1)
//   - the frame clock decodes the whole movie through EOF (~248 frames) and
//     queues exactly one completion event
//   - the picture animates (mid-stream content differs from frame 0)
//   - loop playback restarts past EOF (seek_zero) without finishing
//   - stop clears channel state
// Env: OA_TEST_NEKOMIKO_PFS (skip code 77 when unset or when the engine was
// built without FFmpeg). No window, no UI: pure engine acceptance.
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
#if !defined(OA_HAVE_FFMPEG) || !OA_HAVE_FFMPEG
    std::printf("wmv_decode_test skipped: engine built without FFmpeg\n");
    return 77;
#endif
    const char* pfs_path = std::getenv("OA_TEST_NEKOMIKO_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_NEKOMIKO_PFS unset; skipping\n");
        return 77;
    }
    try {
                oa::fs::PhysFileSystem fs(pfs_path, false);
        const std::string file = "movie/logo_emote.wmv";
        if (!fs.exists(file)) {
            std::fprintf(stderr, "movie/logo_emote.wmv missing from the archive\n");
            return 1;
        }

        oa::media::VideoEngine v;
        v.set_loader([&fs](const std::string& logical)
                         -> std::optional<std::vector<uint8_t>> {
            return fs.read(logical);
        });

        oa::media::VideoConfig cfg;
        cfg.file = file;
        cfg.skippable = true; // skip=2 -> skippable (treated like 1,  TODO)
        cfg.loop_play = false;
        cfg.delay_margin_ms = 120; // layer jump-frame threshold, kept verbatim
        v.play_overlay(cfg);

        check(v.is_overlay_playing(), "overlay video playing after play");
        check(v.state().overlay_video.has_value(), "fullscreen channel exists");
        check(v.state().overlay_video->decoded, "decode attached (real playback)");
        check(v.state().overlay_video->file == file, "channel file kept");
        check(v.state().overlay_video->skippable, "skip=2 kept (skippable)");
        check(v.state().overlay_video->delay_margin_ms &&
                  *v.state().overlay_video->delay_margin_ms == 120,
              "delaymargin=120 kept on the channel");

        // Frame 0 is current right after play: the NekoMiko logo is
        // 1920x1080 (ffprobe of the real asset).
        int w = 0, h = 0;
        const uint8_t* rgba = nullptr;
        uint64_t rev = 0;
        check(v.video_frame("", &w, &h, &rgba, &rev) && rgba, "current frame available");
        check(w == 1920 && h == 1080, "frame dims 1920x1080");
        check(rev == 1, "frame 0 decoded at play (revision 1)");
        uint64_t first_cksum = 0;
        {
            uint64_t c = first_cksum;
            for (size_t i = 0; i < size_t(w) * size_t(h) * 4; i += 4093)
                c = (c ^ rgba[i]) * 1099511628211ull;
            first_cksum = c;
        }

        // Run the channel clock until EOF. logo_emote.wmv ~= 248 frames @
        // 60 fps => ~4.13 s; a 16 ms tick pump must finish within a few
        // hundred ticks.
        uint64_t finish_events = 0;
        bool finished = false;
        uint64_t decodes = 0;
        uint64_t cksum_at_half = 0;
        for (int tick = 0; tick < 3000 && v.is_overlay_playing(); ++tick) {
            v.update(16);
            const auto evs = v.poll_finish_events();
            finish_events += evs.size();
            const uint64_t r2 = v.frame_revision("");
            if (r2 > decodes) decodes = r2;
            if (decodes >= 120 && cksum_at_half == 0) {
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
        // Full movie decode: ~4.13 s at 60 fps. Tolerate a couple of frames
        // of container/decoder rounding, but the whole movie must have been
        // consumed (not the 0-frame immediate-finish fallback of the
        // Theora-only surface).
        std::printf("[wmv] decoded %llu frames to EOF\n", (unsigned long long)decodes);
        check(decodes >= 230 && decodes <= 320, "full movie decoded to EOF (~248)");
        check(v.frame_revision("") == decodes, "final revision == decoded frames");
        {
            // Mid-stream frame content differs from frame 0 (the logo
            // animates): first_cksum was captured right after play
            // (video_frame staging is only valid until the next call).
            check(first_cksum != cksum_at_half, "frame content changes during playback");
            std::printf("[wmv] frame0 cksum=%016llx mid cksum=%016llx\n",
                        (unsigned long long)first_cksum, (unsigned long long)cksum_at_half);
        }
        // The decoded frame stays queryable until the host stops the channel
        // (runtime finish_video -> stop_overlay); then it must disappear.
        {
            int w3 = 0, h3 = 0;
            const uint8_t* p3 = nullptr;
            check(v.video_frame("", &w3, &h3, &p3, nullptr) && p3 && w3 == 1920,
                  "last frame still exposed before stop_overlay");
        }
        v.stop_overlay();
        check(!v.is_overlay_playing(), "stop_overlay clears the channel");
        check(!v.video_frame("", nullptr, nullptr, nullptr, nullptr),
              "no frame after stop_overlay");

        // Re-play with loop: the pump must restart (seek_zero) at EOF, not
        // finish — mirror of the ogg loop path over the ffmpeg backend.
        oa::media::VideoConfig lcfg;
        lcfg.file = file;
        lcfg.loop_play = true;
        v.play_overlay(lcfg);
        check(v.is_overlay_playing(), "looped fullscreen playing");
        bool looped = false;
        for (int tick = 0; tick < 3000; ++tick) {
            v.update(16);
            (void)v.poll_finish_events();
            const uint64_t r = v.frame_revision("");
            if (r > decodes + 10) looped = true; // passed the first cycle
            if (!v.is_overlay_playing()) break;
        }
        check(looped, "loop playback restarts past EOF (no finish)");
        check(v.is_overlay_playing(), "looped channel still playing after a cycle");
        v.stop_overlay();
        check(!v.is_overlay_playing(), "stop_overlay clears the channel");

        // Optional evidence dump (OA_WMV_DUMP=/tmp/dir): PPM of the first,
        // mid-stream and last decoded frames of the one-shot pass (frame 0
        // was consumed above, so replay once more without loop and dump at
        // rev 1, at ~half the movie and at EOF). The NekoMiko logo movie is
        // white at its head and tail — the mid dump carries the logo.
        if (const char* dir = std::getenv("OA_WMV_DUMP"); dir && *dir) {
            oa::media::VideoConfig dcfg;
            dcfg.file = file;
            dcfg.skippable = true;
            v.play_overlay(dcfg);
            auto dump_ppm = [&](const char* name) {
                int w4 = 0, h4 = 0;
                const uint8_t* p4 = nullptr;
                if (!v.video_frame("", &w4, &h4, &p4, nullptr) || !p4) return;
                const std::string path = std::string(dir) + "/" + name;
                FILE* f = std::fopen(path.c_str(), "wb");
                if (!f) return;
                std::fprintf(f, "P6\n%d %d\n255\n", w4, h4);
                for (size_t i = 0; i < size_t(w4) * size_t(h4); ++i) {
                    std::fputc(p4[i * 4 + 0], f);
                    std::fputc(p4[i * 4 + 1], f);
                    std::fputc(p4[i * 4 + 2], f);
                }
                std::fclose(f);
                std::printf("[wmv] dumped %s (%dx%d)\n", path.c_str(), w4, h4);
            };
            dump_ppm("wmv_frame0.ppm");
            for (int tick = 0; tick < 3000 && v.is_overlay_playing(); ++tick) {
                v.update(16);
                (void)v.poll_finish_events();
                if (v.frame_revision("") >= 120) break; // ~half the movie
            }
            dump_ppm("wmv_frame_mid.ppm");
            for (int tick = 0; tick < 3000 && v.is_overlay_playing(); ++tick) {
                v.update(16);
                (void)v.poll_finish_events();
            }
            dump_ppm("wmv_frame_last.ppm");
            v.stop_overlay();
        }
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "exception: %s\n", e.what());
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("wmv_decode_test OK (logo_emote.wmv ~248 frames 1920x1080@60, "
                "EOF + loop)\n");
    return 0;
}
