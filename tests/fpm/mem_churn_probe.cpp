// Memory-churn probe (report-only, not a ctest).
// Cycles the media lifecycles a long playthrough repeats (scene exits with
// live emote layers / video channels) against a real hidden-window renderer
// and samples process memory + renderer cache counters per cycle:
//   * video churn — play_layer(...)/stop_layer(...) of a masked and a keyed
//     1080p channel per cycle (decode state, frame staging, uploaded frame
//     textures, keyed upload path);
//   * emote churn — e:createEmoteLayer + e:enqueueTag('lydel') of a fresh
//     layer id per cycle (player, GPU canvas, atlas set, pump bookkeeping).
// A leak shows up as monotonic per-cycle growth AFTER the first cycle's
// warm-up (first cycle pays one-time allocations: atlases, fonts, pools).
//
// usage: mem_churn_probe <root.pfs> <video-name-substring> <psb-substring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "core/fs/physfs_fs.h"
#include "core/render/renderer.h"
#include "core/runtime/runtime.h"

namespace {
using Clock = std::chrono::steady_clock;

size_t rss_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return size_t(pmc.WorkingSetSize / (1024 * 1024));
#else
    return 0;
#endif
}
void tick_n(oa::runtime::GameRuntime& rt, int n) {
    oa::runtime::FrameInput in;
    for (int i = 0; i < n; ++i) rt.tick(16, in);
}
} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) {
        std::printf("usage: mem_churn_probe <root.pfs> <video-sub> <psb-sub>\n");
        return 2;
    }
    const std::string pfs_path = argv[1];
    const std::string video_a = argv[2]; // masked/verbatim channel file
    const std::string video_b = argv[3]; // unmasked/keyed channel file
    const std::string psb_sub = argv[4];

    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path);
    std::printf("[mem] boot\n");
    oa::runtime::GameRuntime rt(fs);
    rt.open_project("windows");
    rt.boot_project();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("[mem] SDL_Init failed: %s\n", SDL_GetError());
        return 3;
    }
    SDL_Window* win =
        SDL_CreateWindow("oa-mem", 1280, 720, SDL_WINDOW_HIDDEN);
    oa::render::RenderEngine re(fs.get(), &rt);
    if (!win || !re.create_renderer(win, "sdl")) {
        std::printf("[mem] renderer creation failed\n");
        return 4;
    }

    const bool no_pump = std::getenv("OA_MEM_NOPUMP") != nullptr;
    const bool gpu_mode = std::getenv("OA_MEM_GPU") != nullptr;
    oa::emote::emote_set_default_external_pose(gpu_mode);
    auto pump_cycle = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            rt.tick(16, oa::runtime::FrameInput{});
            if (!no_pump) {
                re.pump_host_frames(nullptr);
                // the app presents every frame; SDL's D3D11 backend defers
                // destroyed-texture release to present, so a probe that
                // never presents would measure its own deferred queue as a
                // leak.
                re.render_end();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    auto snapshot = [&](const char* tag) {
        std::printf(
            "[mem] %-22s rss=%4zuMB textures=%3zu decoded=%3zu "
            "canvases=%2zu atlases=%2zu players=%zu\n",
            tag, rss_mb(), re.host_texture_count(), re.decoded_count(),
            re.emote_canvas_count(), re.emote_atlas_count(),
            rt.emote_layers().size());
    };

    // ---- baseline ----------------------------------------------------------
    pump_cycle(200);
    snapshot("baseline");

    // ---- video channel churn ----------------------------------------------
    // alternate masked (verbatim upload) / unmasked (keyed upload) channels
    // with a FRESH id per cycle: every cycle creates and destroys one
    // channel's decode state, staging and uploaded texture.
    const char* vids[] = {video_a.c_str(), video_b.c_str()};
    int vi = 0;
    for (int cycle = 1; cycle <= 10; ++cycle) {
        const std::string id = "vc" + std::to_string(cycle);
        oa::media::VideoConfig cfg;
        cfg.file = vids[vi % 2];
        cfg.loop_play = true;
        rt.video().play_layer(id, cfg);
        pump_cycle(40);
        rt.video().stop_layer(id);
        pump_cycle(10);
        char tag[64];
        std::snprintf(tag, sizeof(tag), "video cycle %d (%s)", cycle,
                      vi % 2 ? "keyed" : "verbatim");
        snapshot(tag);
        ++vi;
    }

    // ---- emote layer churn -------------------------------------------------
    for (int cycle = 1; cycle <= 8; ++cycle) {
        const std::string id = "churn" + std::to_string(cycle);
        std::string code = "e:createEmoteLayer{id='" + id +
                           "', files={[[image\\fg\\jsa_6.psb]]}, width=1920, "
                           "height=1620}";
        rt.interpreter().lua_bridge().run_code(code.c_str(), "mem_churn_probe");
        pump_cycle(30);
        rt.interpreter().lua_bridge().run_code(
            ("e:enqueueTag('lydel', {id='" + id + "'})").c_str(),
            "mem_churn_probe");
        pump_cycle(15);
        char tag[64];
        std::snprintf(tag, sizeof(tag), "emote cycle %d", cycle);
        snapshot(tag);
    }

    snapshot("final");
    std::printf("[mem] done (a leak = counters/rss still climbing at the "
                "last cycles; cycle 1 pays one-time warm-up)\n");
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
