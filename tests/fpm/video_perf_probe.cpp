// Headless video performance probe (report-only, not a ctest).
// Measures the real per-frame video pipeline costs on a game archive's
// videos, stage by stage:
//   * theora decode + YCbCr->RGBA conversion per frame (main stream);
//   * the _m mask partner stream decode per frame;
//   * the full-frame alpha bake (luma-key x mask) per frame;
//   * the engine-level channel loop: VideoEngine play_layer + update()
//     driving (with the decode pool, exactly the runtime wiring), reported
//     as achieved revisions vs the stream's authored fps.
// No window, no renderer.
//
// usage: video_perf_probe <root.pfs> <video-name-substring> [mask_name_substring]
// knobs: OA_VPROBE_FRAMES (default 120)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "core/render/content_role.h"
#include "core/render/renderer.h"
#include "core/runtime/runtime.h"

#include "core/fs/physfs_fs.h"
#include "core/media/decode_pool.h"
#include "core/media/media_internal.h"
#include "core/media/video.h"

namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) {
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      Clock::now() - t0)
                      .count()) /
           1e6;
}
int env_int(const char* name, int dflt) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : dflt;
}
std::string find_video(oa::fs::IFileSystem& fs, const std::string& sub) {
    std::vector<std::string> queue = {"image/anime", "movie", "image/anime/"};
    std::vector<std::string> found;
    for (const std::string& dir : {std::string("image/anime"), std::string("movie")}) {
        auto items = fs.list(dir);
        if (!items) continue;
        for (const std::string& n : *items) {
            std::string tail = n.size() >= 4 ? n.substr(n.size() - 4) : n;
            for (char& c : tail) c = char(::tolower(c));
            if (tail != ".ogv" && tail != ".mp4" && tail != ".wmv") continue;
            if (!sub.empty() && n.find(sub) == std::string::npos) continue;
            std::string full = dir + "\\" + n;
            found.push_back(full);
        }
    }
    for (const std::string& f : found) {
        // prefer non-mask files (the probe drives the mask itself when present)
        if (f.find("_m.") == std::string::npos) return f;
    }
    return found.empty() ? std::string() : found.front();
}
} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("usage: video_perf_probe <root.pfs> <video-substring>\n");
        return 2;
    }
    const char* pfs_path = argv[1];
    const std::string sub = argv[2];
    const int frames = [] {
        const char* v = std::getenv("OA_VPROBE_FRAMES");
        return v && *v ? std::atoi(v) : 120;
    }();

    std::shared_ptr<oa::fs::IFileSystem> fs =
        std::make_shared<oa::fs::PhysFileSystem>(pfs_path); // sidecar dir on

    const std::string video = find_video(*fs, sub);
    if (video.empty()) {
        std::printf("[vprobe] no video matching '%s'\n", sub.c_str());
        return 3;
    }
    std::printf("[vprobe] video '%s'\n", video.c_str());
    auto bytes = fs->read(video);

    // ---- stage 1: main-stream decode + convert ----------------------------
    double open_ms = 0, first_ms = 0, decode_ms = 0, convert_ms = 0;
    int w = 0, h = 0;
    double fps = 0;
    bool theora = false;
    {
        const auto t0 = Clock::now();
        auto src = oa::media::TheoraSource::open(*bytes);
        open_ms = ms_since(t0);
        if (!src) {
            // Not theora (mp4/wmv...): the engine-loop stage below still
            // exercises the full pipeline through the ffmpeg backend.
            std::printf("[vprobe] not a theora stream; engine-loop stage only\n");
        } else {
            theora = true;
            const auto t1 = Clock::now();
            src->read_frame();
            first_ms = ms_since(t1);
            w = src->width();
            h = src->height();
            fps = src->frame_rate();
            std::printf("[vprobe] main: %dx%d @%.2ffps open=%.1f first_frame=%.1f ms\n",
                        w, h, fps, open_ms, first_ms);
            const int n = std::min(frames, 120);
            for (int i = 0; i < n; ++i) {
                const auto t2 = Clock::now();
                src->read_frame();
                decode_ms += ms_since(t2);
            }
            std::printf("[vprobe] main decode+convert: avg=%.3f ms/frame (%d frames, "
                        "theora decode + ycbcr->rgba)\n",
                        decode_ms / n, n);
        }
    }

    // ---- stage 2: mask partner decode + bake ------------------------------
    if (theora) {
    const std::string stem = video.substr(0, video.find_last_of('.'));
    const std::string mask_path = stem + "_m" + video.substr(video.find_last_of('.'));
    auto mask_bytes = fs->read(mask_path);
    if (mask_bytes) {
        const auto t0 = Clock::now();
        auto msrc = oa::media::TheoraSource::open(*mask_bytes);
        const double mopen = ms_since(t0);
        if (msrc && msrc->width() == w && msrc->height() == h) {
            msrc->read_frame();
            double mdec = 0, bake = 0;
            std::vector<uint8_t> main_rgba(size_t(w) * size_t(h) * 4);
            std::vector<uint8_t> out = msrc->rgba();
            const int n = std::min(frames, 120);
            for (int i = 0; i < n; ++i) {
                const auto t1 = Clock::now();
                msrc->read_frame();
                mdec += ms_since(t1);
                const auto t2 = Clock::now();
                // the engine's bake: alpha = luma-key(main) * mask-gray / 255
                for (size_t p = 0; p < main_rgba.size(); p += 4) {
                    const uint8_t lum = oa::media::rgba_luma(&out[p]);
                    const uint16_t a = oa::media::layer_video_key_alpha(lum);
                    out[p + 3] = uint8_t((a * uint16_t(msrc->rgba()[p])) / 255u);
                }
                bake += ms_since(t2);
            }
            std::printf("[vprobe] mask '%s': open=%.1f decode avg=%.3f ms/frame, "
                        "bake avg=%.3f ms/frame (%d frames)\n",
                        mask_path.c_str(), mopen, mdec / n, bake / n, n);
        } else {
            std::printf("[vprobe] mask '%s' not usable (open=%.1f)\n",
                        mask_path.c_str(), mopen);
        }
    } else {
        std::printf("[vprobe] no _m mask sibling\n");
    }
    } // theora

    // ---- stage 3: engine channel loop (runtime wiring: pool + driver) -----
    // The engine loop always runs (also for mp4/wmv through the optional
    // ffmpeg backend): the loop_pace below simulates the host tick stream.
    double loop_fps = fps;
    {
        oa::media::DecodePool pool(2);
        oa::media::VideoEngine ve;
        ve.set_loader([fs](const std::string& name) { return fs->read(name); });
        ve.set_decode_pool(&pool);
        oa::media::VideoConfig cfg;
        cfg.file = video;
        cfg.loop_play = true;
        ve.play_layer("probe", cfg);
        // Drive 5 simulated seconds in real-time 16 ms ticks — the pipe
        // worker needs wall time to decode ahead, exactly like the runtime's
        // host tick stream. Reports achieved revisions vs authored fps.
        uint64_t rev0 = 0, rev1 = 0;
        double busy = 0;
        int ticks = 0;
        const int total_ticks = 3125; // 5 s at 16 ms
        auto next = Clock::now();
        while (ticks < total_ticks) {
            next += std::chrono::milliseconds(16);
            const auto t0 = Clock::now();
            ve.update(16);
            busy += ms_since(t0);
            ++ticks;
            if (ticks == 16) rev0 = ve.frame_revision("probe");
            if (ticks == total_ticks - 1) rev1 = ve.frame_revision("probe");
            std::this_thread::sleep_until(next);
        }
        const double sim_s = double(total_ticks) * 16.0 / 1000.0;
        const uint64_t frames_played = rev1 > rev0 ? rev1 - rev0 : 0;
        const double authored = loop_fps > 0 ? loop_fps : 30.0;
        std::printf("[vprobe] engine loop: %llu frames in %.1f s simulated "
                    "(update busy avg=%.3f ms/tick) -> %.1f fps vs authored "
                    "%.2f fps %s\n",
                    (unsigned long long)frames_played, sim_s, busy / total_ticks,
                    frames_played / sim_s, authored,
                    frames_played / sim_s >= authored * 0.95 ? "REALTIME_OK"
                                                             : "LAGGING");
    }
    // ---- stage 4: windowed pump stage (optional, OA_VPROBE_GPU=1) --------
    // Hidden window + the real RenderEngine: drives RenderEngine::
    // pump_host_frames() per real-time tick with the channel live — the
    // exact core-side pump the app loop calls (video_frame -> fused keyed
    // upload / verbatim upload -> mark_dirty), against a real backend.
    if (env_int("OA_VPROBE_GPU", 0)) {
        const int sw = 1280, sh = 720;
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::printf("[vprobe] GPU stage: SDL_Init failed: %s\n", SDL_GetError());
            return 0;
        }
        SDL_Window* win = SDL_CreateWindow("oa-vprobe", sw, sh, SDL_WINDOW_HIDDEN);
        // A real GameRuntime so the pump reads rt->video(): boot headlessly,
        // attach the renderer, play the channel on the runtime's engine and
        // let RenderEngine::pump_host_frames stream it (the exact app path).
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        oa::render::RenderEngine re(&*fs, &rt);
        rt.boot_project();
        if (!win || !re.create_renderer(win, "sdl")) {
            std::printf("[vprobe] GPU stage: renderer creation failed\n");
        } else {
            oa::media::VideoConfig cfg2;
            cfg2.file = video;
            cfg2.loop_play = true;
            rt.video().play_layer("probe", cfg2);
                int presented = 0;
            const int ticks = env_int("OA_VPROBE_GPU_TICKS", 625); // 10 s
            auto next = Clock::now();
            for (int i = 0; i < ticks; ++i) {
                next += std::chrono::milliseconds(16);
                rt.tick(16, oa::runtime::FrameInput{});
                if (re.pump_host_frames(nullptr)) ++presented;
                std::this_thread::sleep_until(next);
            }
            const uint64_t uprev = re.host_video_rev("probe");
            std::printf("[vprobe] windowed pump: uploaded %d/%d ticks "
                        "(rev=%llu, ~%.1f fps) %s\n",
                        presented, ticks, (unsigned long long)uprev,
                        double(presented) / (double(ticks) * 16.0 / 1000.0),
                        uprev > 0 ? "OK" : "NO_UPLOAD");
        }
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
    }
    return 0;
}
