// Host shared state for the SDL3 app hosts (openartemis / openartemis_test).
//
// Options + AppState are the host boot/run state shared by the two hosts:
//   - src/app/main.cpp         compiles this header in both builds (the test
//     hooks below only exist when OA_TEST_BUILD=1, see main.cpp);
//   - src/app/app_test_drive.cpp (test build only) defines the autodrive
//     machinery and reads/writes the same AppState — the layout must be
//     identical in every TU of one binary, so OA_TEST_BUILD has the same
//     value here as in the including TU (main_test.cpp / app_test_drive.cpp
//     both define it to 1 before including this header).
#pragma once
#ifndef OA_TEST_BUILD
#define OA_TEST_BUILD 0
#endif

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <SDL3/SDL_stdinc.h>   // Uint64 / Uint32
#include <SDL3/SDL_video.h>    // SDL_Window
#include "core/runtime/runtime.h"
#include "core/render/renderer.h"

#include <cstdio>
#include "core/runtime/runtime_iet.h"
#include "core/media/image.h"
// ---- CLI options (hand-rolled parsing lives in main.cpp) ----
struct Options {
    // project.pfs: positional argument, or $OA_PFS when no argument is
    // given; empty means "no project" (SDL_AppInit fails with usage).
    // No machine-specific default exists (the dev tree used to hardcode
    // an absolute path here) — booting a project is always an explicit
    // choice.
    std::string pfs;
    uint64_t frames_target = 0; // 0 => run continuously (no self-check exit)
    int fps = 0;                // 0 => legacy ~8 ms delay + real-delta pacing
    bool headless = false;      // no window/renderer, virtual 16 ms ticks
    std::string dump;           // --dump PATH (PPM snapshot at end-of-run)
    bool dump_on_end = false;   // OA_DUMP_FRAME or --dump
    // project platform (ini section + script os var). Default is the build's
    // own OS family: an Android APK boots the [android]
    // section, the browser build [wasm], desktop keeps the historical
    // [windows] default (override with --platform NAME).
#if defined(__ANDROID__)
    std::string platform = "android";
#elif defined(__EMSCRIPTEN__)
    std::string platform = "wasm";
#else
    std::string platform = "windows";
#endif
    // Render backend line: "sdl" (default — the historical
    // SDL3 SDL_Render path, pixel behavior unchanged) or "gles" (native
    // GLES renderer with the same semantics). --renderer NAME or the
    // OA_RENDERER env var; ignored by --headless (no window, no renderer).
    std::string renderer = "sdl";
};

// Host run state: created in SDL_AppInit, driven by SDL_AppIterate; the
// OA_TEST_BUILD fields are the autodrive/probe state consumed by
// app_test_drive.cpp (test build only).
//
// NOTE for platform ports: this struct is the seam the windowed host uses
// for everything desktop-specific (SDL_Window/renderer state, input,
// pacing); a headless/android/wasm host shares the boot + tick core and
// needs only its own presentation fields here.
struct AppState
{
    Options opt;

    std::shared_ptr<oa::runtime::GameRuntime> rt;
    std::unique_ptr<oa::render::RenderEngine> oaRender;

    // The window/renderer are created only when running windowed (after the
    // shared boot/lambda setup below), so --headless never touches SDL video.
    SDL_Window* window = nullptr;

    oa::runtime::FrameInput input;
    bool mouse_left_prev = false;
    // wheel pulse: a mouse-wheel notch is a *momentary* key press — down
    // edge + ~2-frame isDown window + an explicit up edge (the engine/Lua
    // see isDown/isUpEdge exactly like a short physical key tap). Tracked
    // per vk (136 = up / 137 = down); SDL never delivers a wheel "release".
    std::map<int, int> wheel_hold_frames; // vk -> frames left counting down
    Uint64 last = 0;
    bool quit = false;
    uint64_t frames = 0;
    // 'rr' autodrive flow: raw story start + opening-video park watcher.
    bool ad_rr_video = false;
    bool ad_rr_started = false;
    uint64_t ad_rr_video_f = 0;
    int ad_rr_video_parks = 0;
    uint64_t ad_rr_start = 0;
    // a window resize / expose / pixel-size change arrived
    // since the last frame — the static-frame skip must repaint once at the
    // new window size (the letterbox presentation is window-driven).
    bool force_repaint = false;
#if OA_TEST_BUILD
    // title-button probe state (windowed; resolved once the title UI
    // registers, see probe block below).
    std::string probe_id;
    bool probe_rect_printed = false;
    bool hover_end_armed = false;
    uint64_t settle_frame_ = 0;
    bool end_committed_ = false;
    uint64_t metric_at_frame_ = 0;
    // robust plateau detection — the original smoke fired the metric at
    // the FIRST hovered no-transition frame, which can land mid-entrance (the
    // title still black/animating). The
    // plateau is now the documented "stable visible title": >= 1 s of
    // consecutive bright presented samples while hovered with no transition.
    int64_t bright_sample_count = -1; // consecutive visible plateau samples
    int64_t probe_click_at_ = -1;     // frame for the synthetic smoke click
#endif
    size_t s_last_drawn = 0;   // diagnostics from the last render
    size_t s_last_text_glyphs = 0; // text glyphs drawn
    // smoke: one synthetic left click on the parked probe after the
    // plateau metric so the run also reproduces the click dispatch log
    // (mouse-only; a single edge, never repeated).

    // In default continuous mode (no --frames) print a status heartbeat every
    // ~5 s; finite --frames runs keep the legacy milestone prints instead.
    Uint64 last_status_ms = 0;
    std::set<int> kbd_down; // held physical keys -> vk (real keyboard; Ctrl…)
    std::set<int> mouse_held; // held mouse buttons -> vk (1 left / 2 right / 3 middle)

    bool saw_title_init = false;

    // ---- app-host video (real Theora decode) -----------------------------
    // OA_VIDEO_DEMO=<logical file>: after boot the host plays that file
    // fullscreen through the runtime video engine (no game-UI integration —
    // a host capability demonstration), uploads each new decoded frame and
    // quits at EOF. Host texture name for the fullscreen surface.
#if OA_TEST_BUILD
    std::string video_demo_file;
    bool video_demo_started = false;
    uint64_t video_demo_start_frame = 0;
    bool video_demo_finished = false;
    bool video_demo_print_first = false;
#endif
    uint64_t fs_video_rev = 0;                    // last uploaded fullscreen rev
    std::map<std::string, uint64_t> layer_video_rev; // layer id -> uploaded rev
    // emote static frames (layer id -> uploaded revision).
    std::map<std::string, uint64_t> layer_emote_rev;

#if OA_TEST_BUILD
    // ---- auto-drive (OA_AUTODRIVE=exit|title|help|conf) -------------------
    // Deterministic windowed journey (the Windows analogue of the Linux
    // Xvfb+XTEST runs): boots the real game, drives title -> story -> the
    // flow's own UI with injected keys/clicks, and measures the PRESENTED
    // pixels during the black transition for story-text residue. Only active
    // in windowed runs that set the env var (never on plain runs).
    std::string ad_flow;      // "exit" / "title" / "help" / "conf"
    std::string ad_out;       // OA_UI_OUT dir for frame dumps ("" = none)
    int ad_stage = 0;         // 0 title park / 1 story park / 2 open flow /
                              // 3 dialog / 4 tail sampling / 5 done
    size_t ad_stage_f = 0;    // frames inside the current stage
    int ad_press_key = -1;    // key edge to inject on the next frame
    bool ad_click = false;    // click the parked target on the next frame
    int ad_cx = 0, ad_cy = 0; // click target (engine stage coords)
    // M8 help-flow: hover the parked target (no click) and hold it there.
    bool ad_hover = false;
    // L2 M10b conf-flow: synthetic pointer drag (left held while moving).
    int ad_drag_sx = 0, ad_drag_sy = 0, ad_drag_fx = 0, ad_drag_fy = 0;
    int ad_drag_total = 0; // 0 = no drag armed
    int ad_drag_left = 0;
    int ad_conf_sub = 0;       // conf-flow sub state
    size_t ad_conf_ev0 = 0;    // text_events snapshot for the current probe
    size_t ad_conf_pd0 = 0;    // pointer_dispatch_count snapshot for the probe
    // R10 (temp) rapid-hover stress: flow "r10" alternates the pointer
    // between config-screen buttons as fast as a real flick (1..N frames per
    // button) and samples the dock help geometry every frame for the user's
    // "tip text jumps far up" repro. Engine-state probe mirrors the headless
    // test; here it runs the real windowed render path.
    struct R10Target {
        std::string key;
        std::string layer;
        int cx = 0, cy = 0;
        double ref = 0.0; // resting text world top (node world f + ml.top)
    };
    std::vector<R10Target> r10_t;
    std::vector<int> r10_stable; // indexes into r10_t with a measured ref
    std::string r10_pair;        // OA_R10_PAIR "a,b": restrict stress pair
    int r10_sub = 0;             // 0 open settle / 1 learn / 2 stress
    int r10_phase = 0;           // frames inside the current sub
    int r10_cur = -1;            // index of the hovered target
    int r10_dwell = 0;           // frames left on the current target
    unsigned r10_seed = 7;
    int r10_alt_max = 800;       // OA_R10_ALTER
    int r10_dwell_fixed = 1;     // OA_R10_DWELL (>=1); frames per hover
    bool r10_png = true;         // OA_R10_PNG=1: periodic + anomaly PNGs
    bool r10_save_seed1 = false;  // save flow: slot-1 seed clicked
    bool r10_save_seed2 = false;  // save flow: slot-2 seed clicked
    std::string r10_seed_key;     // armed slot click ("" = none)
    int r10_trace_left = 0;      // verbatim frame samples (OA_R10_TRACE=N)
    double r10_lastT = -1e9;
    size_t r10_last_units = 999;
    int r10_alternations = 0;
    int r10_anomalies = 0;
    int r10_grace = 0;           // settle frames after a hover switch
    int r10_learn_i = -1;        // target being learned
    // R38 dock probe flow ("d38") state: story dock interactions.
    int ad_d38_sub = 0;
    int ad_d38_f = 0;
    int ad_d38_ons1 = -1; // dock-path: ad_d38_f when bt_save01 first appeared
    int ad_d38_ons3 = -1; // F6-path: ad_d38_f when bt_save01 first appeared
    // R10d title-round-trip flow ("r10t") state.
    int ad_t_sub = 0;
    int ad_t_f = 0;
    int ad_t_round = 0;
    long ad_t_dialog_at = -1;
    // R10 backlog regression flow ("r10blog") state.
    int ad_blog_sub = 0;         // 0 story walk / 1 wait open / 2 per-page
    int ad_blog_f = 0;
    int ad_blog_turns = 0;
    bool ad_blog_click_done = false;
    int ad_blog_page = 0;
    // quickload flow ("qld") state: quick save
    // (F4) -> quick load (F5) -> dock save/load/config + F6 must all work.
    int ad_qld_sub = 0;
    int ad_qld_armf = -1; // stage_f at which the current sub armed its input
    bool ad_qld_armed = false;
    int r10_learn_f = 0;
    double r10_learn_acc = 0.0;
    int r10_learn_n = 0;
    double r10_refT = -1e9;
    std::string r10_hid;         // help text layer seen during the run
    std::string r10_help = "500.z.help"; // slot sampled (conf); save flow uses 500.help
    double r10_ml_top = 0, r10_node_top = 0, r10_world_f = 0;
    size_t r10_units = 0;
    double r10_font = 0, r10_spacetop = 0;
    std::string r10_face;
    // visible-content layers at the story park (help detection baseline).
    std::vector<std::string> ad_base_text_layers;
    std::string ad_help_layer; // text layer that appeared while hovering
    int ad_help_phase = 0;     // M8 hover script phase (conf/away/save)
    // M8 pixel baseline (story page before the hover) for diff row scans.
    std::vector<uint8_t> ad_help_base_rgba;
    int ad_help_base_w = 0, ad_help_base_h = 0;
    uint64_t ad_help_seen_at = 0; // stage2 frame the help layer appeared
    size_t ad_bright_pixels = 0;   // luma>120 pixels on the last sampled frame
    double ad_mean_luma = 0;       // mean luma of the last sampled frame
    uint64_t ad_dark_bright_frames = 0; // dark frames with visible bright blobs
    uint64_t ad_sampled_frames = 0;
    uint64_t ad_ppm_count = 0;
    bool ad_rendered = false; // a fresh frame was presented this iterate
    std::string ad_sample_page;
    uint64_t ad_dlg_frame = 0; // frame the confirm dialog was seen at
    // Mouse-autocursor evidence state (exit/title flows): last [mouse] warp
    // target (as requested by the game script) and once-only report flag.
    int ad_warp_x = 0, ad_warp_y = 0;
    bool ad_dlg_ev = false;
    // OA_AUTODRIVE=nmfg — NekoMiko windowed journey to a
    // real fg scene, then window-pixel breathing evidence captures.
    int nm_stage = 0;     // 0 journey / 1 capture / 2 done
    int nm_hold = 0;      // frames holding the pointer after a click
    int nm_hover = 0;     // hover frames before a select-row click
    int nm_page_f = 0;    // settle frames on a freshly parked page
    int nm_cap = 0;       // captures taken (0..3)
    int nm_frame = 0;     // frames inside the capture stage
    int nm_phase = 0;     // 0 ready / 1 hiding / 2 hidden / 3 showing / 4 done
    // OA_NM_SELTEST: stage-3 continuation past the fg scenes to
    // the first real chapter select (sel_01), double-click, verify no crash.
    int nm_s3 = 0;        // 0 hunt select / 1 click1 / 2 wait-click2 / 3 verify
    int nm_s3_f = 0;      // frames inside the stage-3 sub-state
    int nm_adv = 0;       // story steps advanced after the double click
    int nm_s3x = 0, nm_s3y = 0; // last select row centre
    std::vector<uint8_t> nm_hide; // snapshot with emote layers hidden
    uint32_t nm_hw = 0, nm_hh = 0;
    int nm_turns = 0;     // story page turns
    bool nm_assert_ok = false; // OA_NM_ASSERT: a text frame verified
    bool nm_title = false;
    bool nm_sel_armed = false;
    std::string nm_last;
    uint32_t nm_aw = 0, nm_ah = 0, nm_bw = 0, nm_bh = 0, nm_cw = 0, nm_ch = 0;
    std::vector<uint8_t> nm_a, nm_b, nm_c; // capture snapshots
    // scale flow (OA_AUTODRIVE=scale): presentation
    // verification ladder — park on the static FPM title, then walk the
    // window through several sizes and prove the readback stays the fixed
    // stage frame while the presented window scales letterboxed.
    int sc_stage = 0;    // 0 title park wait / 1 static check / 2 ladder
    int sc_sub = 0;      // ladder index inside stage 2
    size_t sc_f = 0;     // frames inside the current sub
    int sc_idx = 0;      // current ladder size index (-1 after fail)
    int sc_shot = 0;     // evidence PNG counter (stage/win pairs)
    bool sc_failed = false;
    int sc_win_w = 0, sc_win_h = 0; // window size of the last capture
    uint64_t sc_checksum = 0;       // last stage-frame checksum
    std::vector<std::pair<int, int>> sc_sizes; // window sizes to walk
    std::string sc_page;
    // event-mapping stage: after the ladder, push REAL SDL mouse events
    // (window coordinates -> SDL_RenderCoordinatesFromWindow -> runtime hit
    // test) at the bt_start window point and assert hover + click dispatch.
    std::string sc_btn;
    int sc_bx = 0, sc_by = 0; // bt_start stage center
    int sc_ev_sub = 0;
#endif
};

// ---- Shared host helpers (main.cpp + app_test_drive.cpp; inline so the
// app_test_drive.cpp TU needs no extra link-time symbols) ----

inline const char* wait_kind_str(oa::runtime::WaitReason::Kind k) {
    using K = oa::runtime::WaitReason::Kind;
    switch (k) {
        case K::Generic: return "generic";
        case K::Generic0: return "wt0";
        case K::Timed: return "timed";
        case K::Stop: return "stop";
        case K::Se: return "se";
        case K::VideoLayer: return "video";
        case K::ScenarioTween: return "scenario-tween";
        case K::KeyWait: return "key";
    }
    return "?";
}


inline std::string wait_desc(const oa::runtime::WaitReason* w) {
    if (!w) return "none";
    std::string s = wait_kind_str(w->kind);
    if (!w->id.empty()) s += "('" + w->id + "')";
    return s;
}


inline bool write_ppm(const std::string& path, const oa::media::Image& img) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
    for (int y = 0; y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            const uint8_t* p = &img.rgba[(size_t(y) * img.w + x) * 4];
            std::fputc(p[0], f);
            std::fputc(p[1], f);
            std::fputc(p[2], f);
        }
    }
    std::fclose(f);
    return true;
}

#if OA_TEST_BUILD
// Autodrive machinery entry points — implemented in app_test_drive.cpp (the
// second TU of openartemis_test). main.cpp's small #if OA_TEST_BUILD hooks
// call these each frame; the user binary never references them.
bool ad_drive(AppState* s);
void ad_pre_tick(AppState* s);
void ad_sample(AppState* s, bool save_ppm);
#endif
