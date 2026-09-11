// openartemis_test autodrive implementation — an independent translation unit
// (test build only; listed in src/app/CMakeLists.txt next to main_test.cpp).
//
// TEST-ONLY: deterministic windowed UI journeys (OA_AUTODRIVE=exit|title|help|
// conf|r10save|r10t|r10blog|qld|d38|nmfg|scale|fontscan|kdemote|langidle|
// slnywalk), synthetic-input
// probes, pixel metrics and the evidence dumps. The plain user binary never
// compiles this file. main.cpp / main_test.cpp keep only the small
// #if OA_TEST_BUILD hooks that call into this TU (ad_drive / ad_pre_tick /
// ad_sample; declared in app_host.h).
//
// OA_TEST_BUILD is forced to 1 here before app_host.h so this TU sees the
// same AppState layout as main_test.cpp's TU; the nested `#if OA_TEST_BUILD`
// guards kept inside the moved text below then all read true, exactly as
// they did when the block still sat inside main.cpp.
#define OA_TEST_BUILD 1

// SDL_main.h redefines main/WinMain unless one of these is set (MSVC links it
// as a duplicate of main_test.obj's entry, LNK2005/LNK1169). This TU has no
// entry point of its own — main_test.cpp (the main.cpp include) owns it — so
// ask SDL not to inject one.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_render.h>
#include <algorithm>
#include <filesystem>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <cstdint>
#include <cstring>
#include <sys/stat.h>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_save.h"
#include "core/emote/emote_player.h"
#include "core/media/image.h"
#include "core/render/renderer.h"
#include "core/render/layer.h"
#include "core/version.h"

#include "app_host.h"

// The PNG encoder lives in oa::media (image.h); these drivers keep the
// historical test-only oa::runtime shim for the calls below.
namespace oa {
namespace runtime {
using oa::media::encode_png;
} // namespace runtime
} // namespace oa

// ---------------------------------------------------------------------------
// auto-drive helpers: windowed deterministic UI journey + presented-pixel
// metrics (text residue on the black transition pages of the title-return /
// exit flows). The state machine runs after each rendered frame and arms the
// next frame's synthetic input; ad_pre_tick applies it before rt->tick.
// ---------------------------------------------------------------------------
static bool ad_wait_stop(const oa::runtime::WaitReason* w) {
    return w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty();
}
static bool ad_has_handler(const oa::runtime::GameRuntime* rt, const char* key) {
    for (const oa::render::Layer* l : rt->scene().draw_order()) {
        const auto* h = rt->scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k != h->params.end() && k->second == key) return true;
    }
    return false;
}
static bool ad_story_parked(const oa::runtime::GameRuntime* rt) {
    const oa::runtime::WaitReason* w = rt->current_wait();
    if (!(w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                w->kind == oa::runtime::WaitReason::Kind::Generic0)))
        return false;
    // FPM parks the story text on 1.80.mw.adv_adv; NekoMiko's mw template
    // targets game.mwid..".mw.adv" instead (message.lua chgmsg id).
    const oa::render::MessageLayer* ml = rt->text().layer("1.80.mw.adv_adv");
    if (!ml) ml = rt->text().layer("1.80.mw.adv");
    return ml && ml->reveal_index >= ml->char_count && !ml->reveal_pending;
}
/// Click the world center of the first layer carrying a click handler row
/// with `key` (the same targeting the headless journeys use).
static void ad_arm_click_key(AppState* s, const char* key) {
    for (const oa::render::Layer* l : s->rt->scene().draw_order()) {
        const auto* h = s->rt->scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end() || k->second != key) continue;
        if (const auto r = s->oaRender->layer_world_rect(*l)) {
            s->ad_cx = int((*r)[0] + (*r)[2] / 2);
            s->ad_cy = int((*r)[1] + (*r)[3] / 2);
            s->ad_click = true;
        }
        return;
    }
}


/// World center of the layer holding a `drag` handler row with `key` (the
/// xslider thumb). Returns false when the layer is absent.
static bool ad_find_drag_layer(AppState* s, const char* key, int* cx, int* cy) {
    for (const oa::render::Layer* l : s->rt->scene().draw_order()) {
        const auto* h = s->rt->scene().find_event_handler(l->id, "drag");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end() || k->second != key) continue;
        if (const auto r = s->oaRender->layer_world_rect(*l)) {
            *cx = int((*r)[0] + (*r)[2] / 2);
            *cy = int((*r)[1] + (*r)[3] / 2);
            return true;
        }
    }
    return false;
}

/// L2 M10b conf-flow: arm a synthetic pointer drag — the left button goes
/// down at (sx,sy) and stays held while the pointer walks to (fx,fy) over
/// `frames` ticks (engine drag_update moves the grabbed layer).
static void ad_arm_drag(AppState* s, int sx, int sy, int fx, int fy, int frames) {
    s->ad_drag_sx = sx;
    s->ad_drag_sy = sy;
    s->ad_drag_fx = fx;
    s->ad_drag_fy = fy;
    s->ad_drag_total = frames > 0 ? frames : 1;
    s->ad_drag_left = s->ad_drag_total;
    std::printf("[conf] drag (%d,%d)->(%d,%d) over %d frames\n", sx, sy, fx, fy,
                s->ad_drag_total);
}

/// L2 M10b conf-flow probe report: dump the uihelp layer (500.z.help) and the
/// text-settings preview layer (500.z.sample) engine state at the current
/// frame, snapshot a PNG, and show the ScenarioLine event delta since the
/// probe start (a delta > 0 proves a help [print] ran even when the layer
/// page ends up empty — the printed-then-cleared smoking gun).
// R10d windowed: dump the dock help slot geometry (centering-var outcome).
static void ad_tip_dump(AppState* s, oa::runtime::GameRuntime* rt,
                        const char* tag) {
    const char* ids[] = {"500.z.help", "500.help"};
    for (const char* id : ids) {
        const oa::render::MessageLayer* ml = rt->text().layer(id);
        if (!ml) continue;
        std::string sample;
        for (const auto& u : ml->page)
            if (!u.data.empty()) {
                sample = u.data.substr(0, 14);
                break;
            }
        const std::string sid = rt->scene().bound_scene_id(id);
        const oa::render::Layer* bn = rt->scene().find(sid);
        oa::render::Affine2 wt;
        double wy = -1e9;
        if (rt->scene().world_transform(sid, &wt)) wy = wt.f;
        std::printf("[r10t] %s tip id=%s node_top=%.1f world=%.1f ml_top=%.1f "
                    "T=%.1f units=%zu font=%.1f sample='%s'\n",
                    tag, id, bn ? bn->top : -1e9, wy, ml->top, wy + ml->top,
                    ml->page.size(), ml->font.size(), sample.c_str());
        if (!s->ad_out.empty()) {
            oa::media::Image img;
            if (s->oaRender->snapshot_renderer(img) && img.w > 0 && img.h > 0) {
                char path[512];
                std::snprintf(path, sizeof(path), "%s/r10t_%s.png",
                              s->ad_out.c_str(), tag);
                const std::vector<uint8_t> png = oa::runtime::encode_png(
                    uint32_t(img.w), uint32_t(img.h), img.rgba);
                if (!png.empty()) {
                    FILE* f = std::fopen(path, "wb");
                    if (f) {
                        std::fwrite(png.data(), 1, png.size(), f);
                        std::fclose(f);
                    }
                }
            }
        }
        return;
    }
    std::printf("[r10t] %s tip: no help slot layer\n", tag);
}

// backlog regression: dump every visible backlog row layer (name/text/
// sub) geometry + first-line sample, save a PNG, and report the measured
// per-row state (the .2 sub rows' top = Lua get_message_layer_height outcome).
static void ad_blog_dump(AppState* s, oa::runtime::GameRuntime* rt,
                         int page) {
    std::printf("[blog] ---- page %d ----\n", page);
    for (int i = 1; i <= 6; ++i) {
        char idb[64];
        for (const char* suf : {".0", ".1", ".2"}) {
            std::snprintf(idb, sizeof(idb), "500.z.bt.tx.%d.%s", i, suf + 1);
            const oa::render::MessageLayer* ml = rt->text().layer(idb);
            if (!ml) continue;
            std::string sample;
            for (const auto& u : ml->page) {
                if (u.kind != oa::render::PageUnit::Kind::Newline &&
                    !u.data.empty()) {
                    sample = u.data.substr(0, 20);
                    break;
                }
            }
            std::string sid = rt->scene().bound_scene_id(idb);
            oa::render::Affine2 wt;
            double wy = -1e9;
            if (rt->scene().world_transform(sid, &wt)) wy = wt.f;
            std::printf("[blog] row%d%s id=%s top=%.1f world=%.1f units=%zu "
                        "chars=%zu sample='%s'\n",
                        i, suf, idb, ml->top, wy, ml->page.size(),
                        ml->char_count, sample.c_str());
        }
    }
    if (!s->ad_out.empty()) {
        oa::media::Image img;
        if (s->oaRender->snapshot_renderer(img) && img.w > 0 && img.h > 0) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/blog_pg%d.png",
                          s->ad_out.c_str(), page);
            const std::vector<uint8_t> png = oa::runtime::encode_png(
                uint32_t(img.w), uint32_t(img.h), img.rgba);
            if (!png.empty()) {
                FILE* f = std::fopen(path, "wb");
                if (f) {
                    std::fwrite(png.data(), 1, png.size(), f);
                    std::fclose(f);
                }
            }
        }
    }
}
static void ad_conf_report(AppState* s, const char* tag) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    const oa::render::TextEngine& t = rt->text();
    const size_t ev = rt->text_events();
    const size_t pd = rt->pointer_dispatch_count();
    std::printf("[conf] %s f=%zu text_ev=%zu (+%zu) pointer_dispatch=%zu (+%zu)\n",
                tag, s->frames, ev, ev - s->ad_conf_ev0, pd, pd - s->ad_conf_pd0);
    for (const char* id : {"500.z.help", "500.z.sample"}) {
        const oa::render::MessageLayer* ml = t.layer(id);
        if (!ml) {
            std::printf("[conf]   layer %s: absent\n", id);
            continue;
        }
        std::string sample;
        size_t units = ml->page.size();
        for (const auto& u : ml->page) {
            if (u.kind != oa::render::PageUnit::Kind::Newline && !u.data.empty()) {
                sample = u.data.substr(0, 18);
                break;
            }
        }
        std::printf("[conf]   layer %s: units=%zu char_count=%zu reveal=%zu/%zu "
                    "pending=%d visible=%d font_size=%.1f left=%.1f top=%.1f "
                    "sample='%s'\n",
                    id, units, ml->char_count, ml->reveal_index, ml->char_count,
                    ml->reveal_pending ? 1 : 0,
                    rt->scene().is_message_layer_visible(id) ? 1 : 0,
                    ml->font.size(), ml->left, ml->top, sample.c_str());
        // M10c: help slot placement — the bound scene node's lyprop top (the
        // uihelp centering wrote it) and the world position the text gets.
        const std::string sid = rt->scene().bound_scene_id(id);
        if (!sid.empty()) {
            const oa::render::Layer* bn = rt->scene().find(sid);
            if (bn) {
                oa::render::Affine2 wt;
                double wy = 0;
                if (rt->scene().world_transform(sid, &wt)) wy = wt.f;
                std::printf("[conf]   node %s top=%.1f world_y=%.1f "
                            "(text y≈%.1f+%.1f)\n",
                            sid.c_str(), bn->top, wy, wy + ml->top, ml->top);
            }
        }
    }
    std::printf("[conf]   active_layer='%s' layers=%zu\n",
                t.active_layer_id().c_str(), rt->scene().size());
    {
        std::string vis;
        for (const std::string& v : t.visible_content_layers()) {
            if (!vis.empty()) vis += ", ";
            vis += v;
        }
        std::printf("[conf]   visible-content=[%s]\n", vis.c_str());
    }
    if (!s->ad_out.empty()) {
        oa::media::Image img;
        if (s->oaRender->snapshot_renderer(img) && img.w > 0 && img.h > 0) {
            const std::vector<uint8_t> png =
                oa::runtime::encode_png(uint32_t(img.w), uint32_t(img.h), img.rgba);
            if (!png.empty()) {
                char path[512];
                std::snprintf(path, sizeof(path), "%s/conf_%s_%03llu.png",
                              s->ad_out.c_str(), tag,
                              (unsigned long long)s->ad_ppm_count);
                ++s->ad_ppm_count;
                FILE* f = std::fopen(path, "wb");
                if (f) {
                    std::fwrite(png.data(), 1, png.size(), f);
                    std::fclose(f);
                }
            }
        }
    }
}

/// M8 help-flow: snapshot the presented frame, save a PNG into ad_out and
/// (when a baseline exists) print the vertical runs of NEW bright pixels
/// (luma lift > 50 vs baseline) — the help text/box pixels stand out from
/// the parked story page.
/// Generic evidence shot: snapshot the presented frame into
/// <out>/<tag>_<nnn>.png (no extra analysis).
static void ad_png_shot(AppState* s, const char* tag) {
    if (s->ad_out.empty()) return;
    oa::media::Image img;
    if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0) return;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03llu.png", s->ad_out.c_str(), tag,
                  (unsigned long long)s->ad_ppm_count);
    ++s->ad_ppm_count;
    const std::vector<uint8_t> png = oa::runtime::encode_png(
        uint32_t(img.w), uint32_t(img.h), img.rgba);
    if (png.empty()) return;
    FILE* f = std::fopen(path, "wb");
    if (f) {
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
    }
}

static void ad_help_shot(AppState* s, const char* tag) {
    if (s->ad_out.empty()) return;
    oa::media::Image img;
    if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0) return;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/help_%s_%03llu.png", s->ad_out.c_str(),
                  tag, (unsigned long long)s->ad_ppm_count);
    ++s->ad_ppm_count;
    const std::vector<uint8_t> png = oa::runtime::encode_png(
        uint32_t(img.w), uint32_t(img.h), img.rgba);
    if (!png.empty()) {
        FILE* f = std::fopen(path, "wb");
        if (f) {
            std::fwrite(png.data(), 1, png.size(), f);
            std::fclose(f);
        }
    }
    if (s->ad_help_base_rgba.size() !=
        size_t(s->ad_help_base_w) * s->ad_help_base_h * 4) {
        std::printf("[help] %s no-baseline\n", tag);
        return;
    }
    const int w = s->ad_help_base_w, h = s->ad_help_base_h;
    // per-row count of lifted pixels (luma_new > luma_base + 15: low-contrast
    // help text counts too) and the horizontal span of the peak row's runs.
    std::vector<int> row_cnt(size_t(h), 0);
    std::vector<int> row_minx(size_t(h), w), row_maxx(size_t(h), -1);
    std::vector<std::string> row_runs;
    row_runs.resize(size_t(h));
    for (int y = 0; y < h; ++y) {
        const uint8_t* b = s->ad_help_base_rgba.data() + size_t(y) * w * 4;
        const uint8_t* p = img.rgba.data() + size_t(y) * w * 4;
        int cnt = 0, mn = w, mx = -1;
        std::string runs;
        int run0 = -1;
        for (int x = 0; x < w; ++x) {
            const double lb = (double(b[x * 4]) + b[x * 4 + 1] + b[x * 4 + 2]) / 3.0;
            const double lp = (double(p[x * 4]) + p[x * 4 + 1] + p[x * 4 + 2]) / 3.0;
            const bool lifted = lp > lb + 15.0;
            if (lifted) {
                ++cnt;
                if (x < mn) mn = x;
                if (x > mx) mx = x;
                if (run0 < 0) run0 = x;
            } else if (run0 >= 0) {
                if (runs.size() < 96) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%s%d..%d ", runs.empty() ? "" : ",",
                                  run0, x - 1);
                    runs += buf;
                }
                run0 = -1;
            }
        }
        if (run0 >= 0 && runs.size() < 96) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s%d..%d", runs.empty() ? "" : ",", run0,
                          w - 1);
            runs += buf;
        }
        row_cnt[size_t(y)] = cnt;
        row_minx[size_t(y)] = mn;
        row_maxx[size_t(y)] = mx;
        row_runs[size_t(y)] = runs;
    }
    // merge consecutive rows with >= 2 lifted pixels into bands
    std::printf("[help] %s diff-bands:", tag);
    int y = 0;
    int bands = 0;
    while (y < h) {
        if (row_cnt[size_t(y)] < 2) { ++y; continue; }
        int y0 = y;
        while (y < h && row_cnt[size_t(y)] >= 2) ++y;
        if (++bands > 16) continue;
        int peak = 0;
        int best = y0;
        for (int yy = y0; yy < y; ++yy)
            if (row_cnt[size_t(yy)] > peak) {
                peak = row_cnt[size_t(yy)];
                best = yy;
            }
        std::printf(" y[%d..%d](%drows peak=%d x=%d..%d runs={%s})", y0, y - 1, y - y0,
                    peak, row_minx[size_t(best)], row_maxx[size_t(best)],
                    row_runs[size_t(best)].c_str());
        // ascii shape of the peak band core (only if reasonably small)
        if (y - y0 <= 48) {
            const int x0 = row_minx[size_t(best)] - 4;
            const int x1 = row_maxx[size_t(best)] + 4;
            if (x1 - x0 <= 220 && x1 > x0) {
                std::printf("\n[help]    ascii y=%d..%d x=%d..%d:\n", y0, y - 1, x0, x1);
                static const char kShade[] = " .:-=+*#%@";
                for (int yy = y0; yy < y; ++yy) {
                    std::string line;
                    for (int xx = x0; xx < x1; ++xx) {
                        const uint8_t* b = s->ad_help_base_rgba.data() +
                                           (size_t(yy) * w + size_t(xx)) * 4;
                        const uint8_t* p = img.rgba.data() +
                                           (size_t(yy) * w + size_t(xx)) * 4;
                        const double lb = (double(b[0]) + b[1] + b[2]) / 3.0;
                        const double lp = (double(p[0]) + p[1] + p[2]) / 3.0;
                        if (lp <= lb + 30.0) {
                            line.push_back(' ');
                            continue;
                        }
                        const double d = lp - lb;
                        int lvl = int((d - 30.0) / 200.0 * 9.0);
                        if (lvl < 0) lvl = 0;
                        if (lvl > 9) lvl = 9;
                        line.push_back(kShade[lvl]);
                    }
                    std::printf("[help]    |%s|\n", line.c_str());
                }
            }
        }
    }
    std::printf(" bands=%d\n", bands);
}

void ad_pre_tick(AppState* s) {
    // release the previous synthetic click (the engine sees a clean up edge
    // on the following frame, exactly like a physical release)
    s->input.left_down = false;
    if (s->ad_press_key >= 0) {
        s->input.key_down_edges.push_back(s->ad_press_key);
        s->input.keys_down.insert(s->ad_press_key);
        s->ad_press_key = -1;
    }
    if (s->ad_hover) {
        // M8: hold the pointer on the target (rollover-driven uihelp).
        s->input.mouse_x = s->ad_cx;
        s->input.mouse_y = s->ad_cy;
    }
    if (s->ad_click) {
        s->input.mouse_x = s->ad_cx;
        s->input.mouse_y = s->ad_cy;
        s->input.left_down = true;
        s->mouse_left_prev = false; // arm a fresh left edge
        s->ad_click = false;
        std::printf("[ad] click (%d,%d)\n", s->ad_cx, s->ad_cy);
    }
    // L2 M10b conf-flow: held-pointer drag (the left button stays down while
    // the pointer walks from the start to the target). The engine sees the
    // down edge on the first frame and the movement on every following one.
    if (s->ad_drag_left > 0) {
        const int done = s->ad_drag_total - s->ad_drag_left;
        if (done == 0) s->mouse_left_prev = false; // fresh down edge
        const double tt = s->ad_drag_total > 0 ? double(done) / double(s->ad_drag_total)
                                               : 1.0;
        s->input.mouse_x = int(double(s->ad_drag_sx) +
                               double(s->ad_drag_fx - s->ad_drag_sx) * tt + 0.5);
        s->input.mouse_y = int(double(s->ad_drag_sy) +
                               double(s->ad_drag_fy - s->ad_drag_sy) * tt + 0.5);
        s->input.left_down = true;
        --s->ad_drag_left;
    }
}
/// Sample the presented frame (right after render_end): mean luma + count of
/// luma>120 pixels. On dark frames a bright residue = text painted over the
/// black page. Dumps a PPM for a few interesting frames.
void ad_sample(AppState* s, bool save_ppm) {
    oa::media::Image img;
    if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0) return;
    uint64_t bright = 0;
    double sum = 0;
    const size_t n = size_t(img.w) * size_t(img.h);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &img.rgba[i * 4];
        const double luma = (double(p[0]) + p[1] + p[2]) / 3.0;
        sum += luma;
        if (luma > 120.0) ++bright;
    }
    s->ad_bright_pixels = bright;
    s->ad_mean_luma = n ? sum / double(n) : 0.0;
    ++s->ad_sampled_frames;
    if (save_ppm && !s->ad_out.empty()) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/ad_%03llu.ppm", s->ad_out.c_str(),
                      (unsigned long long)s->ad_ppm_count++);
        write_ppm(path, img);
    }
}
/// Post-frame state machine + metrics driver. Returns true when the run
/// should quit (flow finished or the journey failed).
/// Phenomenon-C evidence PNG of the just-presented frame (the renderer still
/// holds it when ad_drive runs — after render_end, before the next clear).
static void ad_qld_capture(AppState* s, const char* tag) {
    if (s->ad_out.empty()) return;
    oa::media::Image img;
    if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0) return;
    const std::vector<uint8_t> png =
        oa::runtime::encode_png(uint32_t(img.w), uint32_t(img.h), img.rgba);
    if (png.empty()) return;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/qld_%s_%03llu.png", s->ad_out.c_str(), tag,
                  (unsigned long long)s->ad_ppm_count++);
    FILE* f = std::fopen(path, "wb");
    if (f) {
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
    }
}

// ---------------------------------------------------------------------------
// NekoMiko windowed fg journey (OA_AUTODRIVE=nmfg).
// Mirrors the headless nekomiko_p0 driver: language/gamestart select rows
// are hovered then clicked (cursor model, pointer held while the choice
// animates), the title はじめから is clicked, story pages turn with Enter,
// until the real emote fg layers appear. Then window-pixel captures (start,
// ~2.6 s and ~5.2 s later — half a breathing period apart) prove the idle
// breathing loop moves actual presented pixels at 1920x1080.
// ---------------------------------------------------------------------------
#if OA_TEST_BUILD
static std::string nm_sample_text(const oa::runtime::GameRuntime* rt) {
    const oa::render::MessageLayer* ml = rt->text().layer("1.80.mw.adv_adv");
    if (!ml) return "-";
    for (const auto& u : ml->page)
        if (!u.data.empty()) return u.data.substr(0, 24);
    return "-";
}
static bool nm_is_page_wait(const oa::runtime::WaitReason* w) {
    if (!w) return false;
    return w->kind == oa::runtime::WaitReason::Kind::Generic ||
           w->kind == oa::runtime::WaitReason::Kind::Generic0;
}
static bool nm_find_select(AppState* s, int* cx, int* cy) {
    for (const oa::render::Layer* l : s->rt->scene().draw_order()) {
        const auto* h = s->rt->scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto it = h->params.find("name");
        if (it == h->params.end() || it->second != "select") continue;
        if (const auto r = s->oaRender->layer_world_rect(*l)) {
            *cx = int((*r)[0] + (*r)[2] / 2);
            *cy = int((*r)[1] + (*r)[3] / 2);
            return true;
        }
    }
    return false;
}
static void nm_save_png(AppState* s, const char* tag, const std::vector<uint8_t>& rgba,
                        uint32_t w, uint32_t h) {
    if (s->ad_out.empty() || rgba.empty()) return;
    const std::vector<uint8_t> png =
        oa::runtime::encode_png(uint32_t(w), uint32_t(h), rgba);
    if (png.empty()) return;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/nmfg_%s_%03llu.png", s->ad_out.c_str(), tag,
                  (unsigned long long)s->ad_ppm_count++);
    FILE* f = std::fopen(path, "wb");
    if (f) {
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
        std::printf("[nmfg] saved %s\n", path);
    }
}
static void nm_capture(AppState* s, const char* tag) {
    oa::media::Image img;
    if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0) {
        std::printf("[nmfg] capture %s skipped (renderer busy)\n", tag);
        return;
    }
    if (strcmp(tag, "a") == 0) {
        s->nm_a = img.rgba;
        s->nm_aw = uint32_t(img.w);
        s->nm_ah = uint32_t(img.h);
    } else if (strcmp(tag, "b") == 0) {
        s->nm_b = img.rgba;
        s->nm_bw = uint32_t(img.w);
        s->nm_bh = uint32_t(img.h);
    } else {
        s->nm_c = img.rgba;
        s->nm_cw = uint32_t(img.w);
        s->nm_ch = uint32_t(img.h);
    }
    nm_save_png(s, tag, img.rgba, uint32_t(img.w), uint32_t(img.h));
    std::printf("[nmfg] capture %s %dx%d\n", tag, img.w, img.h);
}
static uint64_t nm_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const size_t px = std::min(a.size(), b.size()) / 4;
    uint64_t d = 0;
    for (size_t i = 0; i < px; ++i) {
        if (std::abs(int(a[i * 4]) - int(b[i * 4])) > 24 ||
            std::abs(int(a[i * 4 + 1]) - int(b[i * 4 + 1])) > 24 ||
            std::abs(int(a[i * 4 + 2]) - int(b[i * 4 + 2])) > 24)
            ++d;
    }
    return d;
}
// generic story/dialog sample: any visible text layer (same rule as the
// headless journey driver)
static bool nm_reveal_done(const oa::runtime::GameRuntime* rt) {
    for (const std::string& id : rt->text().visible_content_layers()) {
        const oa::render::MessageLayer* ml = rt->text().layer(id);
        if (!ml) continue;
        if (ml->reveal_pending || ml->reveal_index < ml->char_count) return false;
    }
    return true;
}
// text/emote window diagnostics. With OA_NM_DIAG set the
// journey dumps window PNGs and message-window geometry at diagnostic
// moments so text pixels can be verified in the actual presented frame.
static void nm_diag(AppState* s, const char* tag) {
    const bool assert_text = std::getenv("OA_NM_ASSERT") != nullptr;
    if (!std::getenv("OA_NM_DIAG") && !assert_text) return;
    oa::media::Image img;
    if (s->oaRender->snapshot_renderer(img) && img.w > 0 &&
        std::getenv("OA_NM_DIAG")) {
        nm_save_png(s, tag, img.rgba, uint32_t(img.w), uint32_t(img.h));
    }
    const oa::runtime::GameRuntime* rt = s->rt.get();
    // window-pixel text assertion: story text must produce
    // glyphs AND dark glyph pixels inside the message window area
    // (NekoMiko mw window: y 856..1080, text column range x 380..1500).
    if (assert_text) {
        bool has_text = false;
        for (const std::string& id : rt->text().visible_content_layers()) {
            const oa::render::MessageLayer* ml = rt->text().layer(id);
            if (!ml) continue;
            for (const auto& u : ml->page)
                if (!u.data.empty()) { has_text = true; break; }
            if (has_text) break;
        }
        if (has_text && img.w > 0 && img.h > 0) {
            uint64_t glyphs = s->oaRender->last_frame_glyphs();
            uint64_t dark = 0;
            uint64_t lumSum = 0;
            const int y0 = int(img.h) * 870 / 1080;
            const int y1 = int(img.h) * 1070 / 1080;
            const int x0 = int(img.w) * 400 / 1920;
            const int x1 = int(img.w) * 1500 / 1920;
            for (int y = 0; y < img.h; ++y)
                for (int x = 0; x < img.w; x += 8) {
                    const uint8_t* p = &img.rgba[(size_t(y) * img.w + x) * 4];
                    lumSum += (uint32_t(p[0]) + p[1] + p[2]) / 3;
                }
            const double mean = double(lumSum) /
                                (double(img.h) * double((img.w + 7) / 8));
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const uint8_t* p = &img.rgba[(size_t(y) * img.w + x) * 4];
                    if ((uint32_t(p[0]) + p[1] + p[2]) / 3 < 110) ++dark;
                }
            std::printf("[nmtext] %s glyphs=%llu darkPx=%llu mean=%.0f\n", tag,
                        (unsigned long long)glyphs, (unsigned long long)dark, mean);
            if (mean < 12.0) {
                // transient black frame (visible-window swap on WM-less Xvfb):
                // retried at the next diagnostic moment
                std::printf("[nmtext] %s skipped (black frame)\n", tag);
            } else if (glyphs == 0 || dark < 800) {
                std::fprintf(stderr,
                             "[nmtext] FAIL: story text not visible in window "
                             "(glyphs=%llu darkPx=%llu mean=%.0f)\n",
                             (unsigned long long)glyphs, (unsigned long long)dark,
                             mean);
                std::exit(1);
            } else {
                s->nm_assert_ok = true;
            }
        }
    }
    std::printf("[nmdiag] %s glyphs=%zu wait=%s\n", tag,
                s->oaRender->last_frame_glyphs(), ad_wait_stop(rt->current_wait())
                    ? "stop"
                    : (nm_is_page_wait(rt->current_wait()) ? "page" : "other"));
    for (const std::string& id : rt->text().visible_content_layers()) {
        const oa::render::MessageLayer* ml = rt->text().layer(id);
        if (!ml) continue;
        std::string page;
        for (const auto& u : ml->page)
            if (!u.data.empty()) { page = u.data.substr(0, 40); break; }
        std::printf("[nmdiag]   text layer '%s' left=%.0f top=%.0f w=%.0f h=%.0f "
                    "font=%.1f sample='%s'\n",
                    id.c_str(), ml->left, ml->top, ml->width, ml->height,
                    ml->font.size(), page.c_str());
    }
    for (const oa::render::Layer* l : rt->scene().draw_order()) {
        if (l->id.find("mw") == std::string::npos && l->id.find("@msg") == std::string::npos)
            continue;
        if (const auto r = s->oaRender->layer_world_rect(*l))
            std::printf("[nmdiag]   scene '%s' rect=(%.0f,%.0f %.0fx%.0f) file='%s'\n",
                        l->id.c_str(), (*r)[0], (*r)[1], (*r)[2], (*r)[3],
                        l->file.c_str());
    }
    // emote/bg draw-order + geometry + texture state
    size_t idx = 0;
    for (const oa::render::Layer* l : rt->scene().draw_order()) {
        // 内容来源状态判定(旧的 `__emote_layer__:` 前缀
        // 嗅探已随保留命名空间一起删除)。
        const bool isEmote =
            oa::render::kind_of(*l) == oa::render::LayerKind::Emote ||
            l->id.find(".fg.") != std::string::npos;
        const bool isBg = l->file.find("/bg/") != std::string::npos ||
                          l->id.find(".bg.") != std::string::npos;
        if (!isEmote && !isBg) { ++idx; continue; }
        const oa::render::ContentRole& role = oa::render::content_role_of(*l);
        const std::string up =
            s->oaRender->texture_uploaded(role.texture_key(*l)) ? "tex" : "-";
        char rect[96] = "";
        if (const auto r = s->oaRender->layer_world_rect(*l))
            std::snprintf(rect, sizeof(rect), "(%.0f,%.0f %.0fx%.0f)", (*r)[0], (*r)[1],
                          (*r)[2], (*r)[3]);
        std::printf("[nmdiag]   order#%zu '%s' file='%s' %s vis=%d alpha=%.3f clip=%s tex=%s\n",
                    idx, l->id.c_str(), l->file.c_str(), rect, l->visible ? 1 : 0,
                    l->alpha, l->has_clip ? "yes" : "no", up.c_str());
        ++idx;
    }
    std::fflush(stdout);
}
static bool ad_nmfg_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    ++s->nm_frame;
    if (s->nm_stage == 0) {
        if (s->nm_hold > 0) {
            // keep the pointer on the clicked row while the choice animates
            s->ad_hover = true;
            --s->nm_hold;
            return false;
        }
        s->ad_hover = false;
        // select rows (language select / 確定 / ...)
        int cx = 0, cy = 0;
        if (nm_find_select(s, &cx, &cy)) {
            if (!s->nm_sel_armed) {
                s->nm_sel_armed = true;
                s->nm_hover = 0;
            }
            if (s->nm_hover < 24) {
                s->ad_cx = cx;
                s->ad_cy = cy;
                s->ad_hover = true;
                ++s->nm_hover;
                return false;
            }
            s->ad_cx = cx;
            s->ad_cy = cy;
            s->ad_click = true;
            s->nm_hold = 140; // choice animation + branch jump room
            s->nm_hover = 0;
            std::printf("[nmfg] select row clicked @(%d,%d) f=%llu\n", cx, cy,
                        (unsigned long long)s->ad_stage_f);
            return false;
        }
        s->nm_sel_armed = false;
        // title はじめから
        const oa::runtime::WaitReason* w = rt->current_wait();
        if (!s->nm_title && ad_wait_stop(w) && ad_has_handler(rt, "bt_start")) {
            ad_arm_click_key(s, "bt_start");
            s->nm_hold = 160;
            s->nm_title = true;
            std::printf("[nmfg] title はじめから clicked\n");
            return false;
        }
        if (!s->nm_title) return false; // boot still in progress
        // story page turns (Enter)
        if (s->nm_turns >= 8 && s->nm_frame % 300 == 0 && s->nm_stage == 0) {
            std::string vis;
            for (const std::string& id : rt->text().visible_content_layers()) vis += id + " ";
            std::printf("[nmfg] parkdbg wait=%s vis='%s' sample='%s' reveal=%d f=%llu\n",
                        ad_wait_stop(w) ? "stop"
                                        : (nm_is_page_wait(w) ? "page" : "other"),
                        vis.c_str(), nm_sample_text(rt).c_str(), (int)nm_reveal_done(rt),
                        (unsigned long long)s->nm_frame);
        }
        if (nm_is_page_wait(w) && !rt->text().visible_content_layers().empty()) {
            if (!nm_reveal_done(rt)) return false;
            const std::string now = nm_sample_text(rt);
            if (now != s->nm_last) {
                s->nm_last = now;
                s->nm_page_f = 0;
                return false;
            }
            if (s->nm_page_f < 8) {
                ++s->nm_page_f;
                return false;
            }
            s->nm_page_f = 0;
            s->ad_press_key = 13; // Enter
            s->nm_hold = 30;
            ++s->nm_turns;
            if (s->nm_turns <= 1 || s->nm_turns % 8 == 0) nm_diag(s, "page");
            if (s->nm_turns % 5 == 0 || rt->emote_layer_events() > 0)
                std::printf("[nmfg] turn %d sample='%s' emote=%zu f=%llu\n",
                            s->nm_turns, now.c_str(), rt->emote_layer_events(),
                            (unsigned long long)s->ad_stage_f);
            if (rt->emote_layer_events() > 0) {
                std::printf("[nmfg] fg emote layers live: entering capture\n");
                nm_diag(s, "fgenter");
                s->nm_stage = 1;
                s->nm_frame = 0;
                s->nm_hold = 0;
            }
            return false;
        }
        return false;
    }
    if (s->nm_stage == 1) {
        // Before the first capture, hide every emote
        // layer for a few frames and snapshot — the visible-vs-hidden
        // difference localises the 立绘 pixels in the actual window
        // composite (independent of the background art).
        if (s->nm_phase == 0) {
            if (s->nm_frame >= 30) {
                for (const auto& [id, st] : rt->emote_layers()) {
                    rt->interpreter().enqueue_tag(
                        "lyprop", {{"id", id}, {"visible", "0"}});
                }
                std::printf("[emotevis] hiding %zu emote layer(s)\n",
                            rt->emote_layers().size());
                s->nm_phase = 1;
                s->nm_frame = 0;
            }
            return false;
        }
        if (s->nm_phase == 1) {
            if (s->nm_frame >= 4) {
                oa::media::Image img;
                if (s->oaRender->snapshot_renderer(img) && img.w > 0) {
                    s->nm_hide = img.rgba;
                    s->nm_hw = uint32_t(img.w);
                    s->nm_hh = uint32_t(img.h);
                    nm_save_png(s, "h", img.rgba, uint32_t(img.w), uint32_t(img.h));
                    std::printf("[emotevis] hidden snapshot %dx%d\n", img.w, img.h);
                }
                for (const auto& [id, st] : rt->emote_layers()) {
                    rt->interpreter().enqueue_tag(
                        "lyprop", {{"id", id}, {"visible", "1"}});
                }
                s->nm_phase = 2;
                s->nm_frame = 0;
            }
            return false;
        }
        if (s->nm_phase == 2) {
            if (s->nm_frame >= 4) {
                std::printf("[emotevis] emote layers restored\n");
                s->nm_phase = 3;
                s->nm_frame = 0;
            }
            return false;
        }
        // capture start, +~2.6 s (half a 5 s breathing loop), +~5.2 s
        // capture with retry: a frame may catch the renderer mid-swap and
        // snapshot empty — retry a few frames later before advancing.
        if (s->nm_cap == 0) {
            if (s->nm_frame >= 30) {
                const size_t before = s->nm_a.size();
                nm_capture(s, "a");
                if (s->nm_a.size() > before) s->nm_cap = 1;
            }
        } else if (s->nm_cap == 1) {
            if (s->nm_frame >= 30 + 155) {
                const size_t before = s->nm_b.size();
                nm_capture(s, "b");
                if (s->nm_b.size() > before) s->nm_cap = 2;
            }
        } else if (s->nm_cap == 2) {
            if (s->nm_frame >= 30 + 310) {
                const size_t before = s->nm_c.size();
                nm_capture(s, "c");
                if (s->nm_c.size() > before) s->nm_cap = 3;
            }
        }
        if (s->nm_cap >= 3 && s->nm_frame >= 30 + 460) {
            const uint64_t dAB = nm_diff(s->nm_a, s->nm_b);
            const uint64_t dAC = nm_diff(s->nm_a, s->nm_c);
            const uint64_t dVH = nm_diff(s->nm_a, s->nm_hide);
            std::printf("[emotevis] visible-vs-hidden diff=%llu px (%.2f%%)\n",
                        (unsigned long long)dVH,
                        s->nm_aw > 0 && s->nm_ah > 0
                            ? 100.0 * double(dVH) / double(s->nm_aw * s->nm_ah)
                            : 0.0);
            // localise the emote pixels: bbox + centre-of-mass of the diff
            uint64_t sx = 0, sy = 0;
            int mnX = 100000, mnY = 100000, mxX = -1, mxY = -1;
            {
                const size_t w = s->nm_aw ? s->nm_aw : 1920;
                const size_t h = s->nm_ah ? s->nm_ah : 1080;
                for (size_t y = 0; y < h; ++y) {
                    for (size_t x = 0; x < w; ++x) {
                        const uint8_t* pa =
                            &s->nm_a[(y * w + x) * 4];
                        const uint8_t* pb =
                            &s->nm_hide[(y * w + x) * 4];
                        if (std::abs(int(pa[0]) - int(pb[0])) > 24 ||
                            std::abs(int(pa[1]) - int(pb[1])) > 24 ||
                            std::abs(int(pa[2]) - int(pb[2])) > 24) {
                            if (int(x) < mnX) mnX = int(x);
                            if (int(x) > mxX) mxX = int(x);
                            if (int(y) < mnY) mnY = int(y);
                            if (int(y) > mxY) mxY = int(y);
                            sx += x;
                            sy += y;
                        }
                    }
                }
            }
            std::printf("[emotevis] diff bbox=(%d,%d)-(%d,%d) centre=(%llu,%llu)\n",
                        mnX, mnY, mxX, mxY,
                        (unsigned long long)(sx ? sx / (dVH ? dVH : 1) : 0),
                        (unsigned long long)(sy ? sy / (dVH ? dVH : 1) : 0));
            const double pxA = s->nm_aw > 0 && s->nm_ah > 0
                                   ? 100.0 * double(dAB) / double(s->nm_aw * s->nm_ah)
                                   : 0.0;
            const double pxC = s->nm_cw > 0 && s->nm_ch > 0
                                   ? 100.0 * double(dAC) / double(s->nm_cw * s->nm_ch)
                                   : 0.0;
            std::printf("[nmfg] window=%ux%u diffAB=%llu (%.2f%%) diffAC=%llu (%.2f%%)\n",
                        s->nm_aw, s->nm_ah, (unsigned long long)dAB, pxA,
                        (unsigned long long)dAC, pxC);
            // window-pixel 立绘 regression — the emote
            // figure must actually composite into the presented window. The
            // visible-vs-hidden diff localises the figure (bg art is
            // identical in both snapshots); counts in the expected character
            // band (head rows ~60..300, figure body down to the message
            // window edge at y=855; feet at stage row 900 hide behind the
            // mw box) must dominate. The old failure (texture uploaded but
            // no quad → layer skipped) left only the ~3k px click-wait icon
            // blinking at y 940..1080, which fails every threshold below.
            if (s->nm_aw > 0 && s->nm_ah > 0 && !s->nm_a.empty() &&
                s->nm_hide.size() >= s->nm_a.size()) {
                const size_t w = s->nm_aw;
                const size_t h = s->nm_ah;
                auto band_diff = [&](size_t y0, size_t y1) {
                    uint64_t d = 0;
                    for (size_t y = y0; y < y1 && y < h; ++y)
                        for (size_t x = 0; x < w; ++x) {
                            const uint8_t* pa = &s->nm_a[(y * w + x) * 4];
                            const uint8_t* pb = &s->nm_hide[(y * w + x) * 4];
                            if (std::abs(int(pa[0]) - int(pb[0])) > 24 ||
                                std::abs(int(pa[1]) - int(pb[1])) > 24 ||
                                std::abs(int(pa[2]) - int(pb[2])) > 24)
                                ++d;
                        }
                    return d;
                };
                const uint64_t cVH = band_diff(60, 900); // whole figure band
                const uint64_t cHead = band_diff(60, 300);
                const uint64_t cFeet = band_diff(700, 850);
                const uint64_t cTop = [&] {
                    // head must sit ~80..140 rows below the window top (the
                    // surface mapping anchors the head marker ~110 px below
                    // the surface top) — a figure pushed to the
                    // top edge (rows 0..50 lit) is a view-model regression.
                    uint64_t d = 0;
                    for (size_t y = 0; y < 50 && y < h; ++y)
                        for (size_t x = 0; x < w; ++x) {
                            const uint8_t* pa = &s->nm_a[(y * w + x) * 4];
                            const uint8_t* pb = &s->nm_hide[(y * w + x) * 4];
                            if (std::abs(int(pa[0]) - int(pb[0])) > 24 ||
                                std::abs(int(pa[1]) - int(pb[1])) > 24 ||
                                std::abs(int(pa[2]) - int(pb[2])) > 24)
                                ++d;
                        }
                    return d;
                }();
                // breathing motion A/B is pose-phase dependent (renders are
                // throttled, so which idle phase a capture lands on varies
                // per run); take the max over the A/B and A/C pairings.
                auto band_diff_ab = [&](const std::vector<uint8_t>& o) {
                    uint64_t d = 0;
                    for (size_t y = 60; y < 900 && y < h; ++y)
                        for (size_t x = 0; x < w; ++x) {
                            const uint8_t* pa = &s->nm_a[(y * w + x) * 4];
                            const uint8_t* pb = &o[(y * w + x) * 4];
                            if (std::abs(int(pa[0]) - int(pb[0])) > 24 ||
                                std::abs(int(pa[1]) - int(pb[1])) > 24 ||
                                std::abs(int(pa[2]) - int(pb[2])) > 24)
                                ++d;
                        }
                    return d;
                };
                const uint64_t bAB = band_diff_ab(s->nm_b);
                const uint64_t bAC = band_diff_ab(s->nm_c);
                const uint64_t bMax = std::max(bAB, bAC);
                std::printf(
                    "[emotevis] band rows60-900=%llu head=%llu feet=%llu top50=%llu "
                    "breathAB=%llu breathAC=%llu\n",
                    (unsigned long long)cVH, (unsigned long long)cHead,
                    (unsigned long long)cFeet, (unsigned long long)cTop,
                    (unsigned long long)bAB, (unsigned long long)bAC);
                    // Full-res full-body surface mapping calibration:
                    // surface mapping measures cVH ~283k..890k px depending
                    // on which girls/pose phase the capture lands on, with
                    // head top at rows ~83..150 and rows 0..50 empty. Keep
                    // robust margins: the old failure classes fail loudly —
                    // invisible layer: ~3k px icon-only; figure pushed to the
                    // window top: rows 0..50 lit + mnY=0; too
                    // small: cVH far below.
                    const bool okFig = cVH > 150000 && cFeet > 40000 &&
                                       cTop < 15000 && mnY >= 30 && mxY >= 950;
                    // Breathing pairing A/B is pose-phase dependent; in a
                    // visible window on WM-less Xvfb the swap/present timing
                    // shifts which idle poses the captures land on and some
                    // readbacks come back black (visible
                    // readback is unreliable for pixel automation). Enforce
                    // the full assertion set only in the deterministic
                    // hidden regression; visible runs print the same
                    // numbers as evidence (user re-verification is visual).
                    const bool hidden = std::getenv("OA_AD_VISIBLE") == nullptr;
                    if (std::getenv("OA_NM_ASSERT") && hidden) {
                        if (!okFig || bMax <= 40000) {
                            std::fprintf(stderr,
                                         "[emotevis] FAIL: 立绘 not visible in "
                                         "window (fig=%s breath=%s)\n",
                                         okFig ? "ok" : "NO",
                                         bMax > 40000 ? "ok" : "NO");
                            std::exit(1);
                        }
                        std::printf("[emotevis] PASS: 立绘 composited in window\n");
                        s->nm_assert_ok = true;
                    }
            }
            s->nm_stage = 2;
            if (std::getenv("OA_NM_ASSERT") && !s->nm_assert_ok) {
                std::fprintf(stderr,
                             "[nmtext] FAIL: no story text frame verified in "
                             "window pixels\n");
                std::exit(1);
            }
            if (std::getenv("OA_NM_SELTEST")) {
                // Keep driving to the chapter-1 select (sel_01).
                s->nm_stage = 3;
                s->nm_frame = 0;
                return false;
            }
            return true; // journey + evidence done
        }
        return false;
    }
    // ---- OA_NM_SELTEST: chapter select double-click crash ----
    // The story continues past the fg scenes; page-turn with Enter until the
    // real select rows (name=select) appear, hover + click option 1, then a
    // second click 20 frames later (real-machine double click), then prove
    // the story advances past the choice without a duplicate clicknext chain
    // crashing (the pre-fix engine dies in select.lua select_clicknext once
    // the leftover chain runs after select_reset cleared scr.select).
    if (s->nm_stage == 3) {
        oa::runtime::GameRuntime* rt3 = s->rt.get();
        const oa::runtime::WaitReason* w3 = rt3->current_wait();
        int cx = 0, cy = 0;
        const bool have_sel = nm_find_select(s, &cx, &cy);
        if (have_sel) { s->nm_s3x = cx; s->nm_s3y = cy; }
        if (s->nm_s3 == 0) {
            if (have_sel) {
                s->nm_s3 = 1;
                s->nm_s3_f = 0;
                std::printf("[nmclk] select rows found @(%d,%d) f=%llu\n", cx, cy,
                            (unsigned long long)s->nm_frame);
                return false;
            }
            if (nm_is_page_wait(w3) && !rt3->text().visible_content_layers().empty()) {
                if (!nm_reveal_done(rt3)) return false;
                const std::string now = nm_sample_text(rt3);
                if (now != s->nm_last) {
                    s->nm_last = now;
                    s->nm_page_f = 0;
                    return false;
                }
                if (s->nm_page_f < 6) {
                    ++s->nm_page_f;
                    return false;
                }
                s->nm_page_f = 0;
                // turn with a real mw click (like the player): keeps the
                // FPM click/flg state identical to a manual run
                s->ad_cx = 600;
                s->ad_cy = 980;
                s->ad_click = true;
                ++s->nm_turns;
                if (s->nm_turns % 10 == 0)
                    std::printf("[nmclk] turn %d sample='%s' f=%llu\n", s->nm_turns,
                                now.c_str(), (unsigned long long)s->nm_frame);
                return false;
            }
            if (s->nm_s3_f++ > 24000) {
                std::printf("[nmclk] FAIL: no select rows within 24000 frames\n");
                return true;
            }
            return false;
        }
        if (s->nm_s3 == 1) { // no pre-hover: click straight (btn_clickex moves
                             // the cursor on click, like a real double click)
            s->ad_hover = false;
            s->ad_cx = s->nm_s3x;
            s->ad_cy = s->nm_s3y;
            s->ad_click = true; // click 1
            s->nm_s3 = 2;
            s->nm_s3_f = 0;
            std::printf("[nmclk] click1 @(%d,%d) f=%llu\n", s->nm_s3x, s->nm_s3y,
                        (unsigned long long)s->nm_frame);
            return false;
        }
        if (s->nm_s3 == 2) { // short gap, then click 2 (real double click)
            const int gap = [] { // OA_NM_SELGAP frames (~default 6 = ~200 ms)
                const char* g = std::getenv("OA_NM_SELGAP");
                if (!g || *g == 0) return 6;
                const int v = std::atoi(g);
                return v > 0 ? v : 6;
            }();
            if (++s->nm_s3_f < gap) return false;
            if (have_sel) { s->nm_s3x = cx; s->nm_s3y = cy; }
            s->ad_cx = s->nm_s3x;
            s->ad_cy = s->nm_s3y;
            s->ad_click = true; // click 2
            s->nm_s3 = 3;
            s->nm_s3_f = 0;
            std::printf("[nmclk] click2 @(%d,%d) f=%llu\n", s->nm_s3x, s->nm_s3y,
                        (unsigned long long)s->nm_frame);
            return false;
        }
        // verify: the story must keep advancing past the double click.
        // A duplicated exit chain crashes in select_clicknext (Lua error /
        // stall) before the story moves on. Each further page turn counts.
        {
            s->ad_hover = false;
            if (s->nm_s3_f++ > 3000) {
                std::printf("[nmclk] FAIL: story did not advance after the "
                            "double click (leftover chain crash?)\n");
                return true;
            }
            const std::string now = nm_sample_text(rt3);
            if (!now.empty() && now != s->nm_last) {
                s->nm_last = now;
                ++s->nm_adv;
                std::printf("[nmclk] advanced %d, now '%s'\n", s->nm_adv,
                            now.c_str());
                if (s->nm_adv >= 8) {
                    std::printf("[nmclk] PASS: story advanced %d steps past the "
                                "double click\n",
                                s->nm_adv);
                    return true;
                }
                s->nm_page_f = 0;
                return false;
            }
            if (nm_is_page_wait(w3) && !rt3->text().visible_content_layers().empty()) {
                if (!nm_reveal_done(rt3)) return false;
                if (s->nm_page_f < 6) {
                    ++s->nm_page_f;
                    return false;
                }
                s->nm_page_f = 0;
                ++s->nm_adv; // a page turn past the click = story alive
                std::printf("[nmclk] turn past click: %d f=%llu\n", s->nm_adv,
                            (unsigned long long)s->nm_frame);
                if (s->nm_adv >= 8) {
                    std::printf("[nmclk] PASS: story advanced %d steps past the "
                                "double click\n",
                                s->nm_adv);
                    return true;
                }
                s->ad_cx = 600;
                s->ad_cy = 980;
                s->ad_click = true; // mw click page turn
                return false;
            }
            return false;
        }
    }
    return true;
}
#endif

// ---------------------------------------------------------------------------
// OA_AUTODRIVE=scale: presentation verification ladder.
// Parks on the FPM title (a fully static scene once the entrance settles)
// and proves the offscreen-target architecture end to end:
//   [S1] every stage snapshot is exactly stage_w x stage_h whatever the
//        window size (window-size-independent readback);
//   [S2] the stage frame is pixel-identical across a window-size ladder
//        (checksum; the parked title is static, so any influence of the
//        window on the render target would show as a diff);
//   [S3] the presented window surface follows the letterbox math: fitted
//        content size = stage * min(win/stage), centered, uniform scale
//        (no stretch), black bars outside, and window<->stage coordinate
//        conversions round-trip exactly (the mouse/click mapping).
// Window resizing needs a real window: run with OA_AD_VISIBLE=1 (Xvfb).
// Ladder: the initial window size is verified first, then OA_SCALE_SIZES
// (default "1600x900,1024x768,720x1280") are walked via SDL_SetWindowSize.
// ---------------------------------------------------------------------------
static uint64_t ad_scale_hash(const oa::media::Image& img) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < img.rgba.size(); ++i)
        h = (h ^ img.rgba[i]) * 1099511628211ull;
    return h;
}
static void ad_scale_fail(AppState* s, const char* what) {
    std::printf("[scale] FAIL: %s\n", what);
    s->quit = true;
    std::exit(1);
}
static void ad_scale_save_png(AppState* s, const char* tag, int w, int h,
                              const std::vector<uint8_t>& rgba) {
    if (s->ad_out.empty() || rgba.empty() || w <= 0 || h <= 0) return;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/scale_%s_%04d.png", s->ad_out.c_str(),
                  tag, s->sc_shot++);
    const std::vector<uint8_t> png =
        oa::runtime::encode_png(uint32_t(w), uint32_t(h), rgba);
    if (!png.empty()) {
        FILE* f = std::fopen(path, "wb");
        if (f) {
            std::fwrite(png.data(), 1, png.size(), f);
            std::fclose(f);
        }
    }
}
static bool ad_scale_check_step(AppState* s, int ww, int wh, uint64_t ref) {
    // Runs after the scene settled at window size ww x wh: stage readback
    // [S1] size + [S2] pixel identity + [S3] letterbox/event mapping.
    oa::media::Image img;
    if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0) {
        std::printf("[scale] FAIL: stage snapshot failed at %dx%d\n", ww, wh);
        return false;
    }
    const int sw = s->oaRender->stage_w_, sh = s->oaRender->stage_h_;
    if (img.w != sw || img.h != sh) {
        std::printf("[scale] FAIL: readback %dx%d != stage %dx%d at window "
                    "%dx%d (window-size-dependent readback)\n",
                    img.w, img.h, sw, sh, ww, wh);
        return false;
    }
    const uint64_t c = ad_scale_hash(img);
    if (c != ref) {
        std::printf("[scale] FAIL: stage frame changed across window sizes "
                    "(%016llx != ref %016llx at %dx%d)\n",
                    (unsigned long long)c, (unsigned long long)ref, ww, wh);
        return false;
    }
    // letterbox geometry + event mapping (S3) — all SDL state, no pixels
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!s->oaRender->stage_to_window_coordinates(0.0f, 0.0f, &x0, &y0) ||
        !s->oaRender->stage_to_window_coordinates(float(sw), float(sh), &x1, &y1)) {
        std::printf("[scale] FAIL: coordinate conversion unavailable\n");
        return false;
    }
    const double sxp = (double(x1) - x0) / sw; // window px per stage px
    const double syp = (double(y1) - y0) / sh;
    const double cx = (x0 + x1) / 2.0, cy = (y0 + y1) / 2.0;
    const bool uniform = std::fabs(sxp - syp) < 1e-6;
    const bool centered = std::fabs(cx - ww / 2.0) <= 0.5 &&
                          std::fabs(cy - wh / 2.0) <= 0.5;
    const bool inside = x0 >= -0.5 && y0 >= -0.5 && x1 <= ww + 0.5 &&
                        y1 <= wh + 0.5;
    const double fit = std::min(double(ww) / sw, double(wh) / sh);
    const int exw = int(std::lround(sw * fit)), exh = int(std::lround(sh * fit));
    // window-surface readback size == fitted content size (S3, windowed runs)
    oa::media::Image wsurf;
    char wsurf_info[64] = "-";
    bool surf_ok = false;
    if (s->oaRender->read_window_surface(wsurf) && wsurf.w > 0 && wsurf.h > 0) {
        surf_ok = std::abs(wsurf.w - exw) <= 1 && std::abs(wsurf.h - exh) <= 1;
        std::snprintf(wsurf_info, sizeof(wsurf_info), "%dx%d", wsurf.w, wsurf.h);
        ad_scale_save_png(s, "win", wsurf.w, wsurf.h, wsurf.rgba);
    }
    // round trips: window -> stage -> window at sampled window points
    bool roundtrip = true;
    const std::pair<float, float> pts[] = {
        { float(ww) / 2.0f, float(wh) / 2.0f },
        { x0 + 5.0f, y0 + 5.0f },
        { x1 - 5.0f, y1 - 5.0f },
    };
    for (const auto& p : pts) {
        float sx2 = 0, sy2 = 0, wx2 = 0, wy2 = 0;
        if (!s->oaRender->get_renderer_coordinates(p.first, p.second, &sx2, &sy2) ||
            !s->oaRender->stage_to_window_coordinates(sx2, sy2, &wx2, &wy2) ||
            std::fabs(wx2 - p.first) > 0.5 || std::fabs(wy2 - p.second) > 0.5)
            roundtrip = false;
    }
    std::printf("[scale] size %dx%d: stage=%dx%d cksum=%016llx win-surf=%s "
                "(fit %dx%d) scale=(%.4f,%.4f) center=(%.1f,%.1f) "
                "uniform=%d centered=%d inside=%d roundtrip=%d surf=%d\n",
                ww, wh, img.w, img.h, (unsigned long long)c, wsurf_info, exw,
                exh, sxp, syp, cx, cy, (int)uniform, (int)centered,
                (int)inside, (int)roundtrip, (int)surf_ok);
    if (!uniform || !centered || !inside || !roundtrip) {
        std::printf("[scale] FAIL: letterbox/event mapping broken at %dx%d\n",
                    ww, wh);
        return false;
    }
    if (!surf_ok)
        std::printf("[scale] note: window-surface readback skipped or odd "
                    "(%s, expected %dx%d)\n", wsurf_info, exw, exh);
    ad_scale_save_png(s, "stage", img.w, img.h, img.rgba);
    return true;
}
static bool ad_scale_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    ++s->sc_f;
    const int sw = s->oaRender->stage_w_, sh = s->oaRender->stage_h_;
    if (s->sc_stage == 0) {
        // parse the resize ladder (sizes AFTER the initial window)
        const char* v = std::getenv("OA_SCALE_SIZES");
        const std::string spec = v && *v ? v : "1600x900,1024x768,720x1280";
        size_t pos = 0;
        while (pos <= spec.size()) {
            const size_t comma = spec.find(',', pos);
            const std::string part = spec.substr(
                pos, comma == std::string::npos ? std::string::npos : comma - pos);
            const size_t x = part.find('x');
            if (x != std::string::npos && x > 0 && x + 1 < part.size()) {
                const int pw = std::atoi(part.substr(0, x).c_str());
                const int ph = std::atoi(part.substr(x + 1).c_str());
                if (pw > 0 && ph > 0) s->sc_sizes.push_back({ pw, ph });
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (s->sc_sizes.empty() && std::getenv("OA_AD_VISIBLE"))
            ad_scale_fail(s, "no ladder sizes (OA_SCALE_SIZES=WxH,WxH,..)");
        if (!std::getenv("OA_AD_VISIBLE")) {
            // Hidden windows cannot be resized on the GL backend (the
            // drawable is not rebuilt): degrade to the initial-size stage
            // checks + the SDL-event mapping (reads/coordinates are still
            // window-independent — that is what hidden runs prove).
            std::printf("[scale] note: hidden window — ladder reduced to the "
                        "initial size (use OA_AD_VISIBLE=1 for the full "
                        "resize ladder)\n");
            s->sc_sizes.clear();
        }
        s->sc_stage = 1;
        s->sc_f = 0;
        s->sc_sub = 0;
        s->sc_checksum = 0;
        s->sc_idx = 0;
        std::printf("[scale] ladder start; initial window then %zu sizes "
                    "(stage %dx%d)\n", s->sc_sizes.size(), sw, sh);
        return false;
    }
    if (s->sc_stage == 1) {
        // wait for the parked title AND a static bright frame (entrance over)
        if (!ad_wait_stop(rt->current_wait()) || !ad_has_handler(rt, "bt_start")) {
            if (s->sc_f > 9000) {
                std::printf("[scale] FAIL: title never parked (wait=%s)\n",
                            wait_desc(rt->current_wait()).c_str());
                s->quit = true;
                return true;
            }
            return false;
        }
        if (s->sc_f % 12 != 0) return false;
        oa::media::Image img;
        if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0)
            return false;
        const uint64_t c = ad_scale_hash(img);
        double sum = 0;
        for (size_t i = 0; i < img.rgba.size(); i += 4)
            sum += (img.rgba[i] + img.rgba[i + 1] + img.rgba[i + 2]) / 3.0;
        const double luma =
            img.rgba.empty() ? 0.0 : sum / (img.rgba.size() / 4.0);
        if (c == s->sc_checksum && luma > 60.0)
            ++s->sc_sub;
        else
            s->sc_sub = 0;
        s->sc_checksum = c;
        if (s->sc_sub >= 3) {
            // static confirmed: reference checksum + initial-size checks
            int ww0 = 0, wh0 = 0;
            SDL_GetWindowSize(s->window, &ww0, &wh0);
            s->sc_win_w = ww0;
            s->sc_win_h = wh0;
            std::printf("[scale] static title f=%llu %dx%d checksum=%016llx "
                        "luma=%.0f\n",
                        (unsigned long long)s->frames, ww0, wh0,
                        (unsigned long long)c, luma);
            if (!ad_scale_check_step(s, ww0, wh0, c))
                ad_scale_fail(s, "initial-size checks");
            s->sc_stage = 2;
            s->sc_f = 0;
            s->sc_sub = 0;
        } else if (s->sc_f > 12000) {
            std::printf("[scale] FAIL: title never became static (luma=%.0f "
                        "sub=%d)\n", luma, s->sc_sub);
            s->quit = true;
            return true;
        }
        return false;
    }
    if (s->sc_stage == 3) {
        // Event-mapping proof with REAL SDL events at the current (last
        // ladder) window size: window-coordinate mouse events must convert
        // through SDL_RenderCoordinatesFromWindow onto the bt_start button
        // (hover), and a real click must dispatch its Lua chain (story start).
        const SDL_WindowID wid = SDL_GetWindowID(s->window);
        // Pushed SDL events run through the same AppEvent -> 
        // SDL_RenderCoordinatesFromWindow -> FrameInput chain as real input.
        auto push_motion = [&](float wx, float wy) {
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.windowID = wid;
            e.motion.x = wx;
            e.motion.y = wy;
            SDL_PushEvent(&e);
        };
        if (s->sc_ev_sub == 0) {
            // move the pointer to an empty corner first (clean hover edge)
            if (s->sc_f < 4) {
                push_motion(2.0f, 2.0f);
                return false;
            }
            s->sc_ev_sub = 1;
            s->sc_f = 0;
            return false;
        }
        if (s->sc_ev_sub == 1) {
            // hover the button center (window coords from the stage point)
            float wx = 0, wy = 0;
            if (!s->oaRender->stage_to_window_coordinates(
                    float(s->sc_bx), float(s->sc_by), &wx, &wy)) {
                ad_scale_fail(s, "stage->window conversion failed (hover)");
            }
            push_motion(wx, wy);
            if (s->sc_f == 2)
                std::printf("[scale] hover push @ window (%.1f,%.1f) for stage "
                            "(%d,%d)\n", wx, wy, s->sc_bx, s->sc_by);
            if (s->sc_f < 10) return false;
            if (!rt->is_hovered(s->sc_btn)) {
                std::printf("[scale] FAIL: %s not hovered after window-coord "
                            "motion (mouse=%d,%d)\n",
                            s->sc_btn.c_str(), s->input.mouse_x, s->input.mouse_y);
                s->quit = true;
                return true;
            }
            int cww = 0, cwh = 0;
            SDL_GetWindowSize(s->window, &cww, &cwh);
            std::printf("[scale] hover PASS: %s hovered via window coords at "
                        "window %dx%d\n", s->sc_btn.c_str(), cww, cwh);
            s->sc_ev_sub = 2;
            s->sc_f = 0;
            return false;
        }
        if (s->sc_ev_sub == 2) {
            // real click through the app's own event protocol: arm the
            // autodrive click (ad_pre_tick applies it before the tick, the
            // same path every deterministic flow uses). The mouse position
            // still came from the pushed window-coordinate SDL motion events
            // above, so the dispatch proves the full event mapping.
            if (s->sc_f == 2) {
                s->ad_cx = s->sc_bx;
                s->ad_cy = s->sc_by;
                s->ad_click = true;
                std::printf("[scale] click armed on %s (stage %d,%d)\n",
                            s->sc_btn.c_str(), s->sc_bx, s->sc_by);
            }
            if (s->sc_f == 20) {
                const oa::runtime::WaitReason* w = rt->current_wait();
                std::printf("[scale] post-click wait=%s text_ev=%zu\n",
                            wait_desc(w).c_str(), rt->text_events());
            }
            if (s->sc_f < 300) return false;
            const oa::runtime::WaitReason* w2 = rt->current_wait();
            const bool started = !(w2 && w2->kind == oa::runtime::WaitReason::Kind::Stop &&
                                   w2->id.empty());
            if (!started) {
                std::printf("[scale] FAIL: story did not start after the real "
                            "window-coord click\n");
                s->quit = true;
                return true;
            }
            std::printf("[scale] click PASS: real window-coord click started "
                        "the game (wait=%s)\n", wait_desc(w2).c_str());
            std::printf("[scale] PASS: %zu window sizes verified (stage "
                        "readback fixed at %dx%d, letterbox uniform/centered, "
                        "conversions exact, mouse mapping live)\n",
                        s->sc_sizes.size() + 1, sw, sh);
            return true;
        }
        return false;
    }
    // ---- stage 2: the window-size ladder ----
    if (s->sc_idx >= int(s->sc_sizes.size())) {
        // ladder done -> stage 3: real-SDL-event mouse mapping at the final
        // window size (bt_start hover + click through the app event path).
        s->sc_stage = 3;
        s->sc_sub = 0;
        s->sc_f = 0;
        s->sc_btn.clear();
        for (const oa::render::Layer* l : rt->scene().draw_order()) {
            const auto* h = rt->scene().find_event_handler(l->id, "click");
            if (!h) continue;
            const auto k = h->params.find("key");
            if (k == h->params.end() || k->second != "bt_start") continue;
            if (const auto r = s->oaRender->layer_world_rect(*l)) {
                s->sc_btn = l->id;
                s->sc_bx = int((*r)[0] + (*r)[2] / 2);
                s->sc_by = int((*r)[1] + (*r)[3] / 2);
                std::printf("[scale] event target %s stage=(%d,%d) "
                            "world=(%.0f,%.0f %.0fx%.0f)\n",
                            l->id.c_str(), s->sc_bx, s->sc_by, (*r)[0], (*r)[1],
                            (*r)[2], (*r)[3]);
            }
            break;
        }
        if (s->sc_btn.empty())
            ad_scale_fail(s, "bt_start layer not found for the event test");
        return false;
    }
    const int tw = s->sc_sizes[size_t(s->sc_idx)].first;
    const int th = s->sc_sizes[size_t(s->sc_idx)].second;
    if (s->sc_sub == 0) {
        std::printf("[scale] resize -> %dx%d\n", tw, th);
        if (!SDL_SetWindowSize(s->window, tw, th))
            std::printf("[scale] note: SDL_SetWindowSize failed: %s\n",
                        SDL_GetError());
        s->sc_sub = 1;
        s->sc_f = 0;
        return false;
    }
    if (s->sc_sub == 1) {
        if (s->sc_f % 8 != 0) return false;
        int ww = 0, wh = 0;
        SDL_GetWindowSize(s->window, &ww, &wh);
        if (ww != tw || wh != th) {
            if (s->sc_f > 1200) {
                std::printf("[scale] FAIL: window never reached %dx%d "
                            "(stuck at %dx%d — hidden window / no display?)\n",
                            tw, th, ww, wh);
                s->quit = true;
                return true;
            }
            return false; // resize event still in flight
        }
        // wait until the repaint re-settled on the reference frame
        oa::media::Image img;
        if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0)
            return false;
        if (ad_scale_hash(img) != s->sc_checksum) {
            if (s->sc_f > 1200) {
                std::printf("[scale] FAIL: repaint at %dx%d never settled on "
                            "the static frame\n", ww, wh);
                s->quit = true;
                return true;
            }
            return false;
        }
        if (!ad_scale_check_step(s, ww, wh, s->sc_checksum))
            ad_scale_fail(s, "ladder checks");
        ++s->sc_idx;
        s->sc_sub = 0;
        s->sc_f = 0;
        return false;
    }
    return false;
}

// 'rr' flow: 常轨脱离ReReCall style raw story start (bypass the title UI,
// the framework's title_start2 resets and reads the route script directly),
// then watch the route-opening [video] parks: log in/out frame numbers and
// the overlay playing state so the OP playback can be verified visually.
static bool ad_rr_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    ++s->ad_stage_f;
    if (s->ad_stage == 0) {
        const oa::runtime::WaitReason* w = rt->current_wait();
        if (!(ad_wait_stop(w) && ad_has_handler(rt, "bt_start"))) return false;
        // park settle, then jump straight into the route script (same lua
        // call the probe test uses)
        if (s->ad_stage_f < 60) return false;
        try {
            rt->interpreter().lua_bridge().run_code("title_start2('莉々子-00')", "rr");
            std::printf("[rr] raw start 莉々子-00 f=%llu\n",
                        (unsigned long long)s->frames);
        } catch (const std::exception& e) {
            std::printf("[rr] raw start FAILED: %s\n", e.what());
            s->quit = true;
            return false;
        }
        s->ad_rr_start = s->frames;
        s->ad_stage = 1;
        s->ad_stage_f = 0;
        return false;
    }
    if (s->ad_stage == 1) { // wait for the OP park, then switch to stage4
        // (main.cpp samples presented pixels every rendered frame at
        // stage>=4: the residue detector prints RESIDUE + dumps PPMs when
        // the mean luma sits below 18 with bright pixels elsewhere — i.e.
        // a black video surface. GL vs software renders are compared this
        // way without needing X grabs.)
        const oa::runtime::WaitReason* w = rt->current_wait();
        if (w && w->kind == oa::runtime::WaitReason::Kind::Stop &&
            w->id == "video") {
            std::printf("[rr] OP park in f=%llu; pixel sampling on\n",
                        (unsigned long long)s->frames);
            s->ad_stage = 4;
            s->ad_stage_f = 0;
        }
        if (s->frames > 4000) { // no OP seen: give up quietly
            s->quit = true;
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// HCT (灵感满溢的甜蜜创想凸) story-head movie journey — real
// windowed app + real audio device, the exact route the user hears: boot ->
// title -> Lua title_start2('05_天梨01') (the 天梨/final-story entry, same
// shortcut the headless probe tests use) -> the digest movie
// movie_bai/dcpyzcv3t.mp4 plays to its natural EOF -> keep running a post
// window so the AppQuit OA_AUDIO_DIAG dump covers EOF drain + the first
// post-movie sounds. OA_HCT_AD_ENTRY overrides the entry script;
// OA_HCT_AD_POST sets the post-EOF frames (default 360 ~= 6 s).
// ---------------------------------------------------------------------------
static bool ad_hctdig_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    ++s->ad_stage_f;
    static const std::string entry = [] {
        const char* v = std::getenv("OA_HCT_AD_ENTRY");
        return v && *v ? std::string(v) : std::string("05_天梨01");
    }();
    if (s->ad_stage == 0) { // parked title -> jump the story entry
        const oa::runtime::WaitReason* w = rt->current_wait();
        if (!(ad_wait_stop(w) && ad_has_handler(rt, "bt_start"))) return false;
        if (s->ad_stage_f < 60) return false;
        try {
            rt->interpreter().lua_bridge().run_code(
                ("title_start2('" + entry + "')").c_str(), "hctdig");
            std::printf("[hctdig] raw start '%s' f=%llu\n", entry.c_str(),
                        (unsigned long long)s->frames);
        } catch (const std::exception& e) {
            std::printf("[hctdig] raw start FAILED: %s\n", e.what());
            s->quit = true;
            return false;
        }
        s->ad_rr_start = s->frames;
        s->ad_stage = 1;
        s->ad_stage_f = 0;
        return false;
    }
    if (s->ad_stage == 1) { // wait for the story-head movie park
        const oa::runtime::WaitReason* w = rt->current_wait();
        if (w && w->kind == oa::runtime::WaitReason::Kind::Stop &&
            w->id == "video") {
            const auto vs = rt->video().state();
            const auto* ch = vs.overlay_video ? &*vs.overlay_video : nullptr;
            s->ad_rr_video = true;
            s->ad_rr_video_f = s->frames;
            ++s->ad_rr_video_parks;
            std::printf("[hctdig] movie park f=%llu file='%s' audio_on=%d\n",
                        (unsigned long long)s->frames,
                        ch ? ch->file.c_str() : "?", ch ? (int)ch->audio_on : 0);
            s->ad_stage = 2;
            s->ad_stage_f = 0;
            return false;
        }
        if (s->frames > 5000) { // no movie park: give up
            std::printf("[hctdig] no movie park seen by f=%llu; giving up\n",
                        (unsigned long long)s->frames);
            s->quit = true;
        }
        return false;
    }
    if (s->ad_stage == 2) { // movie EOF: wait for the park to release
        const oa::runtime::WaitReason* w = rt->current_wait();
        const bool still_parked =
            w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id == "video";
        // head-timeline probe: sample pos/rev shortly after the
        // park opens (diagnostic prints).
        if (s->ad_stage_f == 20 || s->ad_stage_f == 60 || s->ad_stage_f == 120) {
            const auto vs = rt->video().state();
            const auto* ch = vs.overlay_video ? &*vs.overlay_video : nullptr;
            std::printf("[hctdig] head probe +%lluf pos=%llums rev=%llu "
                        "aframes=%llu\n",
                        (unsigned long long)s->ad_stage_f,
                        ch ? (unsigned long long)ch->position_ms : 0,
                        (unsigned long long)rt->video().frame_revision(""),
                        ch ? (unsigned long long)ch->audio_frames : 0);
        }
        if (!still_parked &&
            !(rt->video().state().overlay_video &&
              rt->video().state().overlay_video->playing)) {
            const uint64_t dur = s->frames - s->ad_rr_video_f;
            std::printf("[hctdig] movie released f=%llu (park dur=%llu frames "
                        "~%.1fs); post window on\n",
                        (unsigned long long)s->frames,
                        (unsigned long long)dur, double(dur) / 60.0);
            s->ad_stage = 3;
            s->ad_stage_f = 0;
            return false;
        }
        if (s->frames > 4000 + s->ad_rr_video_f + 3000) {
            std::printf("[hctdig] movie never released; giving up\n");
            s->quit = true;
        }
        return false;
    }
    // stage 3: post-movie observation window, then end the run (AppQuit
    // prints the OA_AUDIO_DIAG per-stream summary).
    if (s->ad_stage_f >= (std::getenv("OA_HCT_AD_POST")
                              ? std::max(0, std::atoi(std::getenv("OA_HCT_AD_POST")))
                              : 360)) {
        std::printf("[hctdig] post window done f=%llu wait=%s\n",
                    (unsigned long long)s->frames,
                    wait_desc(rt->current_wait()).c_str());
        s->quit = true;
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// rr evidence flows (常轨脱离ReReCall, iMel-2022 framework). 'rrtext': after
// the route OP releases, wait for the parked story page and dump the adv
// message-layer font params (outline/shadow evidence) + a fresh PPM set.
// 'rrconf': title -> config page 2 (text-speed/sample page) and watch the
// preview-text layer "500.z.text" (config01 font table).
// ---------------------------------------------------------------------------
static void ad_rr_layer_dump(oa::runtime::GameRuntime* rt, const char* tag,
                             const std::string& id) {
    const oa::render::MessageLayer* ml = rt->text().layer(id);
    if (!ml) {
        std::printf("[%s] layer %s: absent\n", tag, id.c_str());
        return;
    }
    std::string sample;
    for (const auto& u : ml->page)
        if (u.kind != oa::render::PageUnit::Kind::Newline && !u.data.empty()) {
            sample = u.data.substr(0, 24);
            break;
        }
    std::printf("[%s] layer %s: layered=%d positioned=%d units=%zu chars=%zu "
                "reveal=%zu/%zu pending=%d hidden=%d visible=%d size=%.1f "
                "left=%.1f top=%.1f width=%.1f height=%.1f sample='%s'\n",
                tag, id.c_str(), ml->layered ? 1 : 0, ml->positioned ? 1 : 0,
                ml->page.size(), ml->char_count, ml->reveal_index,
                ml->char_count, ml->reveal_pending ? 1 : 0,
                ml->text_hidden ? 1 : 0,
                rt->scene().is_message_layer_visible(id) ? 1 : 0,
                ml->font.size(), ml->left, ml->top, ml->width, ml->height,
                sample.c_str());
    for (const auto& u : ml->page) {
        if (u.kind == oa::render::PageUnit::Kind::Newline || u.data.empty())
            continue;
        std::string raw;
        for (const auto& [k, v] : u.font.raw) {
            if (!raw.empty()) raw += " ";
            raw += k + "=" + v;
        }
        std::printf("[%s]   unit font raw: %s\n", tag, raw.c_str());
        break;
    }
    const std::string sid = rt->scene().bound_scene_id(id);
    if (!sid.empty()) {
        const oa::render::Layer* bn = rt->scene().find(sid);
        oa::render::Affine2 wt;
        const bool wt_ok = rt->scene().world_transform(sid, &wt);
        std::printf("[%s]   bound node %s alpha=%.2f vis=%d world=(%s)\n",
                    tag, sid.c_str(), bn ? bn->alpha : -1.0,
                    bn ? (bn->visible ? 1 : 0) : -1,
                    wt_ok ? "yes" : "no");
    }
}
static void ad_rr_text_report(AppState* s, oa::runtime::GameRuntime* rt,
                              const char* tag) {
    std::printf("[%s] f=%zu wait=%s layers=%zu text_ev=%zu glyphs=%zu\n", tag,
                s->frames, wait_desc(rt->current_wait()).c_str(),
                rt->scene().size(), rt->text_events(), s->s_last_text_glyphs);
    size_t n = 0;
    for (const std::string& id : rt->text().visible_content_layers()) {
        const oa::render::MessageLayer* ml = rt->text().layer(id);
        if (!ml || ml->page.empty()) continue;
        if (n++ >= 4) break;
        ad_rr_layer_dump(rt, tag, id);
    }
    if (!s->ad_out.empty()) ad_png_shot(s, tag);
}
static bool ad_rrtext_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    ++s->ad_stage_f;
    if (s->ad_stage == 0) {
        const oa::runtime::WaitReason* w = rt->current_wait();
        if (!(ad_wait_stop(w) && ad_has_handler(rt, "bt_start"))) return false;
        if (s->ad_stage_f < 60) return false;
        try {
            rt->interpreter().lua_bridge().run_code("title_start2('莉々子-00')", "rr");
            std::printf("[rrtext] raw start 莉々子-00 f=%llu\n",
                        (unsigned long long)s->frames);
        } catch (const std::exception& e) {
            std::printf("[rrtext] raw start FAILED: %s\n", e.what());
            s->quit = true;
            return false;
        }
        s->ad_stage = 1;
        s->ad_stage_f = 0;
        return false;
    }
    if (s->ad_stage == 1) { // wait for the OP park, then its release
        const oa::runtime::WaitReason* w = rt->current_wait();
        if (w && w->kind == oa::runtime::WaitReason::Kind::Stop &&
            w->id == "video") {
            std::printf("[rrtext] OP park f=%llu; waiting release\n",
                        (unsigned long long)s->frames);
            s->ad_stage = 2;
            s->ad_stage_f = 0;
        }
        if (s->frames > 4000) {
            std::printf("[rrtext] no OP park; abort\n");
            s->quit = true;
        }
        return false;
    }
    if (s->ad_stage == 2) { // story body park after the OP
        bool parked = false;
        for (const std::string& id : rt->text().visible_content_layers()) {
            const oa::render::MessageLayer* ml = rt->text().layer(id);
            if (!ml || ml->page.empty() || ml->char_count == 0) continue;
            if (ml->reveal_pending ||
                ml->reveal_index < ml->char_count)
                continue;
            // the body adv layer: story window text (skip name/measure)
            if (id.find(".mw.") == std::string::npos) continue;
            parked = true;
            break;
        }
        if (parked) {
            std::printf("[rrtext] story parked f=%llu; dumping\n",
                        (unsigned long long)s->frames);
            ad_rr_text_report(s, rt, "rrtext_story");
            s->ad_ppm_count = 0; // fresh PPM set of the parked story page
            s->ad_stage = 4;
            s->ad_stage_f = 0;
        }
        if (s->frames > 7000) {
            std::printf("[rrtext] story park never seen; abort\n");
            s->quit = true;
        }
        return false;
    }
    if (s->ad_stage == 4) { // sample a handful of frames, then quit
        if (s->ad_stage_f == 30) {
            ad_rr_text_report(s, rt, "rrtext_story2");
            s->quit = true;
        }
        return false;
    }
    return false;
}
// 'rrconf': title -> config -> page 2 (the sample-text page: conf_page_
// sampletext=2 in the Windows table), watch "500.z.text" (config01 preview).
static bool ad_rrconf_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    ++s->ad_stage_f;
    if (s->ad_stage == 0) { // title park
        const oa::runtime::WaitReason* w = rt->current_wait();
        if (!(ad_wait_stop(w) && ad_has_handler(rt, "bt_conf"))) return false;
        if (s->ad_stage_f < 60) return false;
        ad_arm_click_key(s, "bt_conf");
        s->ad_stage = 1;
        s->ad_stage_f = 0;
        std::printf("[rrconf] click title config\n");
        return false;
    }
    if (s->ad_stage == 1) { // config page 1 open; click the page2 tab
        if (ad_has_handler(rt, "bt_exit") && ad_has_handler(rt, "page2") &&
            s->ad_stage_f > 90) {
            ad_arm_click_key(s, "page2");
            s->ad_stage = 2;
            s->ad_stage_f = 0;
            std::printf("[rrconf] click page2 tab\n");
        }
        if (s->frames > 2500) {
            std::printf("[rrconf] config page never opened; abort\n");
            s->quit = true;
        }
        return false;
    }
    if (s->ad_stage == 2) { // page 2 open: watch the preview text layer
        const oa::render::MessageLayer* ml = rt->text().layer("500.z.text");
        const bool sample_page = ad_has_handler(rt, "mspeed") ||
                                 ad_has_handler(rt, "aspeed") ||
                                 ad_has_handler(rt, "sl0201") ||
                                 ad_has_handler(rt, "mw_alpha");
        if (s->ad_stage_f % 60 == 0) {
            ad_rr_layer_dump(rt, "rrconf", "500.z.text");
            if (ml && ml->char_count > 0) ad_png_shot(s, "rrconf_text");
        }
        if (s->ad_stage_f == 1) s->ad_conf_ev0 = rt->text_events();
        if (s->ad_stage_f == 700) {
            ad_rr_text_report(s, rt, "rrconf_end");
            const bool alive = ml && ml->char_count > 0 &&
                               !ml->reveal_pending &&
                               ml->reveal_index >= ml->char_count &&
                               rt->scene().is_message_layer_visible(
                                   "500.z.text");
            std::printf("[rrconf] VERDICT sample page=%s preview %s "
                        "text_ev_delta=%zu\n",
                        sample_page ? "open" : "NOT-OPEN",
                        alive ? "ALIVE" : "BLANK/DEAD", 
                        rt->text_events() - s->ad_conf_ev0);
            s->quit = true;
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// OA_AUTODRIVE=so90 — Select Oblige (天选庶民的真命之选, r10-era framework)
// windowed evidence journey for the "mw-face avatar / backlog
// turns the scene background black" report. Walks story pages from boot,
// snapshots every page park, and when the mw-face offscreen-group layers
// (1.80.mw.1.fa / .t.y.x.p) appear it holds + frame-shots the avatar, then
// presses F8 (backlog) and frame-shots the open backlog. Every shot logs
// [so90] mean luma + bright ratio so the log alone quantifies a blackout.
// Snapshot cadence is thinned to every third frame (evidence volume).
// ---------------------------------------------------------------------------
static void ad_so90_shot(AppState* s, const char* tag) {
    if (s->ad_out.empty()) return;
    oa::media::Image img;
    if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0) return;
    const size_t n = size_t(img.w) * size_t(img.h);
    uint64_t bright = 0;
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &img.rgba[i * 4];
        const double luma = (double(p[0]) + p[1] + p[2]) / 3.0;
        sum += luma;
        if (luma > 120.0) ++bright;
    }
    std::printf("[so90] %s shot f=%llu luma=%.1f bright=%.1f%%\n", tag,
                (unsigned long long)s->frames,
                n ? sum / double(n) : 0.0,
                100.0 * double(bright) / double(n ? n : 1));
    const std::vector<uint8_t> png =
        oa::runtime::encode_png(uint32_t(img.w), uint32_t(img.h), img.rgba);
    if (png.empty()) return;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/so90_%s_%03llu.png", s->ad_out.c_str(),
                  tag, (unsigned long long)s->ad_ppm_count);
    ++s->ad_ppm_count;
    FILE* f = std::fopen(path, "wb");
    if (f) {
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
    }
}

static bool ad_so90_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    // journey-local state (one flow per process)
    static int sub = 0;             // 0 title park / 1 story walk / 2 backlog
    static int hold = 0;            // hold frames on the avatar park
    static int blog_f = 0;          // frames since F8 was armed
    static int page = 0;            // parked pages walked
    static int shot_i = 0;          // shot thinning counter
    static bool prev_parked = false;
    static bool prev_avatar = false;
    static bool f8_armed = false;
    static bool f8_open = false;
    static bool f8_shot = false;
    static std::string last_sig;
    using K = oa::runtime::WaitReason::Kind;
    const oa::runtime::WaitReason* w = rt->current_wait();
    switch (sub) {
        case 0: {
            // wait for the parked title with bt_start, then start the story
            if (!(w && w->kind == K::Stop && w->id.empty())) break;
            bool has = false;
            for (const oa::render::Layer* l : rt->scene().draw_order()) {
                const auto* h = rt->scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto k = h->params.find("key");
                if (k != h->params.end() && k->second == "bt_start") has = true;
            }
            if (!has) break;
            ad_arm_click_key(s, "bt_start");
            sub = 1;
            std::printf("[so90] story start clicked\n");
            break;
        }
        case 1: {
            // walk parked story pages; watch the mask-group signature
            std::string sig;
            bool avatar = false;
            for (const oa::render::Layer* l : rt->scene().draw_order()) {
                const bool im = l->props.count("intermediate_render") != 0;
                const bool mk = l->props.count("intermediate_render_mask") != 0;
                if (!im && !mk) continue;
                if (l->id == "1.0") continue; // stage root (constant)
                sig += l->id;
                sig += '|';
                if (l->id.find(".fa") != std::string::npos) avatar = true;
            }
            if (sig != last_sig) {
                std::printf("[so90] f=%llu page=%d sig-change avatar=%d sig='%s'\n",
                            (unsigned long long)s->frames, page, (int)avatar,
                            sig.c_str());
                last_sig = sig;
            }
            // F8 already sent: follow the backlog open + shot it
            if (f8_armed && !f8_open) {
                ++blog_f;
                if (blog_f > 500) {
                    std::printf("[so90] backlog never opened\n");
                    s->quit = true;
                    break;
                }
                if (blog_f % 3 == 1) ad_so90_shot(s, "bgopen");
                const oa::render::MessageLayer* ml =
                    rt->text().layer("500.z.bt.tx.1.1");
                if (ml && !ml->page.empty()) {
                    f8_open = true;
                    blog_f = 0;
                    std::printf("[so90] backlog open f=%llu\n",
                                (unsigned long long)s->frames);
                }
                break;
            }
            if (f8_open) {
                ++blog_f;
                if (blog_f % 2 == 1) ad_so90_shot(s, "blog");
                if (blog_f > 240) {
                    std::printf("[so90] DONE pages=%d\n", page);
                    s->quit = true;
                }
                break;
            }
            const bool parked = (w && (w->kind == K::Generic ||
                                       w->kind == K::Generic0 ||
                                       w->kind == K::Timed || w->kind == K::Se ||
                                       w->kind == K::KeyWait)) &&
                rt->text().page_has_visible_text("1.80.mw.adv_adv");
            if (parked && avatar) {
                // avatar pages: hold + per-frame shots, then F8
                if (!prev_avatar)
                    std::printf("[so90] AVATAR page start f=%llu page=%d\n",
                                (unsigned long long)s->frames, page);
                if (hold < 60) {
                    ++hold;
                    if (hold % 3 == 1) ad_so90_shot(s, "face");
                } else if (!f8_armed) {
                    f8_armed = true;
                    blog_f = 0;
                    std::printf("[so90] arming F8 backlog at f=%llu\n",
                                (unsigned long long)s->frames);
                    s->ad_press_key = 119; // F8 backlog
                }
                prev_avatar = avatar;
                prev_parked = parked;
                break;
            }
            if (parked && !avatar && !prev_parked) {
                // a fresh parked page without the avatar: shot + advance
                ++page;
                if (page >= 32) ad_so90_shot(s, "pg");
                std::printf("[so90] page=%d f=%llu (advance)\n", page,
                            (unsigned long long)s->frames);
                if (page > 200) {
                    std::printf("[so90] page cap reached (no avatar)\n");
                    s->quit = true;
                    break;
                }
                s->input.mouse_x = 640;
                s->input.mouse_y = 600;
                s->ad_click = true;
            }
            hold = 0;
            f8_armed = false;
            f8_open = false;
            prev_parked = parked;
            prev_avatar = avatar;
            break;
        }
    }
    return false;
}


// ---------------------------------------------------------------------------
// OA_AUTODRIVE=slnyface — きら☆かの (slny, pf8 single file + loose savedata,
// 1280x720, iMel-2022-era framework with E-mote PSB standing portraits)
// windowed story-page journey (角色立绘部件错乱 defect).
// Optionally resumes a mid-story engine save (OA_SLNY_RESUME, default
// slny_mid1.dat — written by the slny_face_probe at a heroine page), walks
// parked story pages and screenshots every park; pages that carry a live
// E-mote layer hold 36 frames and frame-shot (poses animate via the game's
// own progress clock). Every shot logs mean luma + bright ratio + the emote
// layer inventory (id / canvas / revision / world rect).
// ---------------------------------------------------------------------------
static void ad_slny_shot(AppState* s, const char* tag) {
    if (s->ad_out.empty()) return;
    oa::media::Image img;
    if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0) return;
    const size_t n = size_t(img.w) * size_t(img.h);
    uint64_t bright = 0;
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &img.rgba[i * 4];
        const double luma = (double(p[0]) + p[1] + p[2]) / 3.0;
        sum += luma;
        if (luma > 120.0) ++bright;
    }
    std::printf("[slny] %s shot f=%llu luma=%.1f bright=%.1f%%\n", tag,
                (unsigned long long)s->frames,
                n ? sum / double(n) : 0.0,
                100.0 * double(bright) / double(n ? n : 1));
    const std::vector<uint8_t> png = oa::runtime::encode_png(
        uint32_t(img.w), uint32_t(img.h), img.rgba);
    if (png.empty()) return;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/slny_%s_%03llu.png", s->ad_out.c_str(),
                  tag, (unsigned long long)s->ad_ppm_count);
    ++s->ad_ppm_count;
    FILE* f = std::fopen(path, "wb");
    if (f) {
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
    }
}
static void ad_slny_emote_log(AppState* s, const char* tag) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    const auto& ems = rt->emote_layers();
    if (ems.empty()) return;
    for (const auto& [id, st] : ems) {
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        bool ok = false;
        for (const oa::render::Layer* l : rt->scene().draw_order()) {
            if (!l || l->id != id) continue;
            if (const auto r = s->oaRender->layer_world_rect(*l)) {
                x0 = (*r)[0];
                y0 = (*r)[1];
                rw = (*r)[2];
                rh = (*r)[3];
                ok = true;
            }
            break;
        }
        std::printf("[slny] %s emote id='%s' %dx%d rev=%llu world=%s(%.0f,%.0f"
                    " %.0fx%.0f) file='%s'\n",
                    tag, id.c_str(), st.width, st.height,
                    (unsigned long long)st.revision, ok ? "" : "no-rect ",
                    x0, y0, rw, rh, st.file.c_str());
    }
}
static bool ad_slnyface_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    static int sub = 0;           // 0 title+load / 1 walk
    static bool loaded = false;
    static int page = 0;          // parked pages walked
    static bool prev_parked = false;
    static int hold = 0;          // hold frames on an emote park
    static bool prev_emote = false;
    static int shot_thin = 0;     // non-emote page shot thinning
    static const char* resume = nullptr;
    static int cap = 0;
    if (!resume) {
        resume = std::getenv("OA_SLNY_RESUME");
        if (!resume || !*resume) resume = "slny_mid1.dat";
        const char* v = std::getenv("OA_SLNY_PAGES");
        cap = (v && *v) ? std::atoi(v) : 60;
        std::printf("[slny] journey resume=%s page cap=%d\n", resume, cap);
    }
    using K = oa::runtime::WaitReason::Kind;
    const oa::runtime::WaitReason* w = rt->current_wait();
    switch (sub) {
        case 0: {
            if (!(w && w->kind == K::Stop && w->id.empty())) break;
            if (!loaded) {
                loaded = true;
                const bool ok = rt->load_game_from(resume, -1);
                std::printf("[slny] f=%llu resume load %s ok=%d\n",
                            (unsigned long long)s->frames, resume, (int)ok);
                if (ok) {
                    sub = 1; // story already in progress; no title to click
                    std::printf("[slny] f=%llu walking from the loaded save\n",
                                (unsigned long long)s->frames);
                }
                break; // settle the load (transitions/waits)
            }
            bool has = false;
            for (const oa::render::Layer* l : rt->scene().draw_order()) {
                const auto* h = rt->scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto k = h->params.find("key");
                if (k != h->params.end() && k->second == "bt_start") has = true;
            }
            if (!has) break;
            ad_arm_click_key(s, "bt_start");
            sub = 1;
            std::printf("[slny] f=%llu bt_start clicked\n",
                        (unsigned long long)s->frames);
            break;
        }
        case 1: {
            const bool emote = !rt->emote_layers().empty();
            // fully-revealed adv text + clickable wait = a parked story page
            bool text_ready = false;
            for (const std::string& id : rt->text().visible_content_layers()) {
                if (id.find(".mw.") == std::string::npos) continue;
                const oa::render::MessageLayer* ml = rt->text().layer(id);
                if (ml && ml->char_count > 0 && !ml->reveal_pending &&
                    ml->reveal_index >= ml->char_count) {
                    text_ready = true;
                    break;
                }
            }
            const bool parked =
                (w && (w->kind == K::Generic || w->kind == K::Generic0 ||
                       w->kind == K::Timed || w->kind == K::Se ||
                       w->kind == K::KeyWait)) &&
                text_ready;
            if (emote != prev_emote)
                std::printf("[slny] f=%llu emote change %d->%d page=%d\n",
                            (unsigned long long)s->frames, (int)prev_emote,
                            (int)emote, page);
            if (parked && emote && !prev_parked) {
                // fresh emote park: log + enter the hold sequence
                std::printf("[slny] f=%llu EMOTE page start page=%d\n",
                            (unsigned long long)s->frames, page + 1);
                ad_slny_emote_log(s, "emote-page");
                ++page;
                prev_parked = true;
                prev_emote = emote;
                hold = 0;
                break;
            }
            if (parked && emote && prev_parked) {
                // holding on the emote park: frame-shots while the pose
                // animates, then advance to the next page
                ++hold;
                if (hold == 2 || hold == 12 || hold == 24)
                    ad_slny_shot(s, "emote");
                if (hold == 4) {
                    // A/B: force a CPU pose render of the exact
                    // displayed pose and dump the canvas PNG next to the
                    // GPU-composited stage shot.
                    static int cpu_dumps = 0;
                    if (cpu_dumps < 6 && !s->ad_out.empty()) {
                        const auto& ems = rt->emote_layers();
                        for (const auto& [id, st] : ems) {
                            if (!st.player) continue;
                            st.player->set_external_pose(false);
                            st.player->render_now();
                            const int W = st.player->width();
                            const int H = st.player->height();
                            const std::vector<uint8_t>& rgba =
                                st.player->rgba();
                            st.player->set_external_pose(true);
                            if (rgba.size() != size_t(W) * H * 4 || W <= 0 ||
                                H <= 0)
                                continue;
                            char path[512];
                            std::snprintf(path, sizeof(path),
                                          "%s/slny_cpu_canvas_%03llu.png",
                                          s->ad_out.c_str(),
                                          (unsigned long long)s->ad_ppm_count);
                            ++s->ad_ppm_count;
                            const std::vector<uint8_t> png =
                                oa::runtime::encode_png(uint32_t(W),
                                                          uint32_t(H), rgba);
                            if (!png.empty()) {
                                FILE* f = std::fopen(path, "wb");
                                if (f) {
                                    fwrite(png.data(), 1, png.size(), f);
                                    fclose(f);
                                }
                            }
                            // figure alpha bbox on the canvas
                            int minX = W, minY = H, maxX = -1, maxY = -1;
                            for (int y = 0; y < H; ++y)
                                for (int x = 0; x < W; ++x)
                                    if (rgba[(size_t(y) * W + x) * 4 + 3] >
                                        8) {
                                        if (x < minX) minX = x;
                                        if (x > maxX) maxX = x;
                                        if (y < minY) minY = y;
                                        if (y > maxY) maxY = y;
                                    }
                            std::printf(
                                "[slny] f=%llu CPU canvas id='%s' %dx%d "
                                "bbox=(%d,%d)-(%d,%d) dumped\n",
                                (unsigned long long)s->frames, id.c_str(), W,
                                H, minX, minY, maxX, maxY);
                        }
                    }
                }
                if (hold >= 36) {
                    prev_parked = false;
                    prev_emote = false;
                    hold = 0;
                    if (page >= cap) {
                        std::printf("[slny] DONE emote pages=%d\n", page);
                        s->quit = true;
                        break;
                    }
                    s->input.mouse_x = 640;
                    s->input.mouse_y = 600;
                    s->ad_click = true; // advance
                }
                break;
            }
            if (parked && !emote && !prev_parked) {
                // fresh parked page without emote: thin shots, then advance
                ++page;
                if (++shot_thin % 3 == 0) ad_slny_shot(s, "pg");
                std::printf("[slny] f=%llu page=%d (no emote)\n",
                            (unsigned long long)s->frames, page);
                if (page >= cap) {
                    std::printf("[slny] DONE pages=%d (no emote reached)\n",
                                page);
                    s->quit = true;
                    break;
                }
                s->input.mouse_x = 640;
                s->input.mouse_y = 600;
                s->ad_click = true;
            }
            prev_parked = parked;
            prev_emote = emote;
            break;
        }
    }
    return false;
}


// ---------------------------------------------------------------------------
// OA_AUTODRIVE=bt91blog — btjy (HENPRI-family, 1600x900, 2022-era kiyopi
// framework) backlog speaker-icon geometry journey (backlog
// 小人头像过大). Drives: title lang select (bt_cn/bt_cn_next) -> bt_start ->
// story page parks (adv text full + clickable wait) with per-park screenshots
// -> lua adv_backlog -> screenshots of the open backlog rows. Pixel shots go
// to OA_UI_OUT; every shot logs mean luma + bright ratio.
// ---------------------------------------------------------------------------
static void ad_bt91_shot(AppState* s, const char* tag) {
    if (s->ad_out.empty()) return;
    oa::media::Image img;
    if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0) return;
    const size_t n = size_t(img.w) * size_t(img.h);
    uint64_t bright = 0;
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &img.rgba[i * 4];
        const double luma = (double(p[0]) + p[1] + p[2]) / 3.0;
        sum += luma;
        if (luma > 120.0) ++bright;
    }
    std::printf("[bt91] %s shot f=%llu luma=%.1f bright=%.1f%%\n", tag,
                (unsigned long long)s->frames,
                n ? sum / double(n) : 0.0,
                100.0 * double(bright) / double(n ? n : 1));
    const std::vector<uint8_t> png = oa::runtime::encode_png(
        uint32_t(img.w), uint32_t(img.h), img.rgba);
    if (png.empty()) return;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/bt91_%s_%03llu.png", s->ad_out.c_str(),
                  tag, (unsigned long long)s->ad_ppm_count);
    ++s->ad_ppm_count;
    FILE* f = std::fopen(path, "wb");
    if (f) {
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
    }
}

static bool bt91_story_text_ready(const oa::runtime::GameRuntime* rt) {
    for (const std::string& id : rt->text().visible_content_layers()) {
        if (id.find(".mw.") == std::string::npos) continue;
        const oa::render::MessageLayer* ml = rt->text().layer(id);
        if (ml && ml->char_count > 0 && !ml->reveal_pending &&
            ml->reveal_index >= ml->char_count)
            return true;
    }
    return false;
}

static bool ad_bt91blog_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    // journey-local state (one flow per process)
    static int sub = 0;             // 0 nav / 1 walk / 2 blog shots
    static int parks = 0;
    static bool parked_prev = false;
    static int last_park_f = -1;
    static int page_budget = 20;
    static int settle = 0;
    ++s->ad_stage_f;
    if (sub == 0) {
        // navigate in order: bt_cn -> bt_cn_next -> bt_start (each clicked at
        // most once; skip after a budget)
        const char* v = std::getenv("OA_BT91_PAGES");
        if (v && *v && std::atoi(v) > 0) page_budget = std::atoi(v);
        static int navi = 0;
        static int nav_wait = 0;
        static const char* kNav[] = {"bt_cn", "bt_cn_next", "bt_start"};
        if (navi < 3) {
            ++nav_wait;
            const char* key = kNav[navi];
            if (ad_has_handler(rt, key)) {
                ad_arm_click_key(s, key);
                std::printf("[bt91] nav click '%s' f=%llu\n", key,
                            (unsigned long long)s->frames);
                ++navi;
                nav_wait = 0;
                if (navi == 3) {  // bt_start clicked: begin the story walk
                    sub = 1;
                    parks = 0;
                    parked_prev = false;
                    settle = 120;
                }
            } else if (nav_wait > 7000) {
                std::printf("[bt91] nav skip '%s' (never appeared)\n", key);
                ++navi;
                nav_wait = 0;
                if (navi >= 3) {  // exhausted but story may already run
                    sub = 1;
                    parks = 0;
                    parked_prev = false;
                }
            }
            return false;
        }
        std::printf("[bt91] nav exhausted; abort\n");
        s->quit = true;
        return false;
    }
    if (sub == 1) {
        // story walk: park on full story text + clickable wait
        const oa::runtime::WaitReason* w = rt->current_wait();
        const bool clickw = w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                                  w->kind == oa::runtime::WaitReason::Kind::Generic0 ||
                                  w->kind == oa::runtime::WaitReason::Kind::Timed ||
                                  w->kind == oa::runtime::WaitReason::Kind::Se);
        const bool ready = bt91_story_text_ready(rt);
        const bool parked = clickw && ready;
        if (parked && !parked_prev && last_park_f != int(s->frames)) {
            last_park_f = int(s->frames);
            ++parks;
            std::printf("[bt91] park %d f=%llu\n", parks,
                        (unsigned long long)s->frames);
            ad_bt91_shot(s, "park");
            {
                bool av = false;
                for (const oa::render::Layer* l : rt->scene().draw_order()) {
                    if (l->id.find(".mw.bb.") == std::string::npos) continue;
                    const oa::render::Layer* bb = l;
                    (void)bb;
                    av = true;
                    break;
                }
                if (av) {
                    std::printf("[bt91] avatar visible at park %d\n", parks);
                    for (size_t q = 0; q < 6; ++q) {
                        ad_bt91_shot(s, "avatar");
                        oa::runtime::FrameInput id2;
                        rt->tick(16, id2);
                    }
                }
            }
            if (parks >= page_budget) {
                std::printf("[bt91] opening backlog via lua f=%llu\n",
                            (unsigned long long)s->frames);
                rt->interpreter().lua_bridge().call_plain("adv_backlog");
                sub = 2;
                settle = 300;
                return false;
            }
            // click the message window area to advance
            const int cx = rt->project_.config.stage_width / 2;
            const int cy = rt->project_.config.stage_height - 150;
            s->input.mouse_x = cx;
            s->input.mouse_y = cy;
            s->ad_click = true;
            parked_prev = true;
            return false;
        }
        if (!parked) parked_prev = false;
        // guard: stuck without a park for too long -> advance click anyway
        if (clickw && !parked && ready && s->ad_stage_f % 1200 == 60) {
            const int cx = rt->project_.config.stage_width / 2;
            const int cy = rt->project_.config.stage_height - 150;
            s->input.mouse_x = cx;
            s->input.mouse_y = cy;
            s->ad_click = true;
        }
        if (parks > 0 && parks < page_budget && s->ad_stage_f % 5400 == 60)
            ad_bt91_shot(s, "walk");
        if (s->ad_stage_f > 360000) {
            std::printf("[bt91] walk budget exceeded parks=%d; open blog\n",
                        parks);
            rt->interpreter().lua_bridge().call_plain("adv_backlog");
            sub = 2;
            settle = 300;
        }
        return false;
    }
    // sub == 2: blog open -> settle + screenshots
    if (settle-- > 0) return false;
    // same-run A/B: when OA_BT99_AB is set, each blog frame is
    // captured twice in one process — anchor-frame confinement ON (the fix,
    // already what the main loop rendered) and OFF (the legacy crop)
    // — so pre/post pixels share the exact same scene state (the journeys'
    // ~30-frame run-to-run drift otherwise shifts the backlog page between
    // runs and poisons cross-run frame diffs).
    if (std::getenv("OA_BT99_AB") && s->oaRender) {
        auto ab_capture = [&](const char* tag) {
            if (s->ad_out.empty()) return;
            oa::media::Image img;
            if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 ||
                img.h <= 0)
                return;
            const std::vector<uint8_t> png = oa::runtime::encode_png(
                uint32_t(img.w), uint32_t(img.h), img.rgba);
            if (png.empty()) return;
            char path[512];
            std::snprintf(path, sizeof(path), "%s/bt99_%s_%03llu.png",
                          s->ad_out.c_str(), tag,
                          (unsigned long long)s->ad_ppm_count);
            ++s->ad_ppm_count;
            FILE* f = std::fopen(path, "wb");
            if (f) {
                std::fwrite(png.data(), 1, png.size(), f);
                std::fclose(f);
            }
            std::printf("[bt91] AB %s f=%llu\n", tag,
                        (unsigned long long)s->frames);
        };
        auto redraw = [&]() {  // render the settled scene into the stage
            const oa::render::Compositor& sc = s->rt->scene();
            s->oaRender->render_beigin();
            s->oaRender->draw_scene(sc, s->rt->all_delete_fade());
        };
        ab_capture("fix");
        s->oaRender->set_anchor_frame_enabled(false);
        s->oaRender->render_clear_tex_cache();
        redraw();
        ab_capture("legacy");
        s->oaRender->set_anchor_frame_enabled(true);
        s->oaRender->render_clear_tex_cache();
        redraw();
        std::printf("[bt91] AB pair done f=%llu\n",
                    (unsigned long long)s->frames);
    } else {
        ad_bt91_shot(s, "blog");
    }
    static int blog_shots = 0;
    if (++blog_shots >= 5) {
        std::printf("[bt91] DONE blog_shots=%d parks=%d\n", blog_shots, parks);
        s->quit = true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// OA_AUTODRIVE=bt96snow — btjy (HENPRI, 1600x900, Qruppo/kiyopi CN) story-start
// snowfall defect journey: the opening pages of d01_c01_1.ast
// bind a looping bg movie layer {"bg", file="snow03", movie=1, id=8, lv=8,
// path=":ani/"} (image/anime/snow03.ogv). User report: no snow visible.
// Drives boot -> lang (bt_cn/bt_cn_next) -> bt_start -> parks on the first
// story pages; per park dumps the bg-zone layer subtree + [video] channel
// state and takes spaced screenshots to sample the layer animation, then
// advances. OA_BT96_PAGES: parks to walk before quitting (default 3).
// ---------------------------------------------------------------------------
static void ad_bt96_parkdump(oa::runtime::GameRuntime* rt) {
    // bg-zone subtree (ids contain ".bg."): roots 1.0.bx.by.bs.<lv>.bg.<id>
    // plus the addImageID content chain (.t.y.x.p.m.a.a.b leaf carries the
    // bound texture;视频帧绑定层的内容来源见 Layer::content —— file
    // 里不再有保留命名空间串)。
    for (const oa::render::Layer* l : rt->scene().draw_order()) {
        if (l->id.find(".bg.") == std::string::npos) continue;
        const oa::render::Layer& x = *l;
        const char* vm = x.visible ? "1" : "0";
        const char* cs = x.script_created ? "sc" : "--";
        if (x.file.empty()) {
            std::printf("[bt96] ly %-58s vis=%s a=%.3f L=%.0f T=%.0f %s\n",
                        x.id.c_str(), vm, x.alpha, x.left, x.top, cs);
        } else {
            std::printf("[bt96] ly %-58s vis=%s a=%.3f L=%.0f T=%.0f"
                        " w=%.0f h=%.0f clip=%s file=%s\n",
                        x.id.c_str(), vm, x.alpha, x.left, x.top, x.width,
                        x.height,
                        x.has_clip ? "Y" : "-",
                        x.file.c_str());
        }
    }
}

static void ad_bt96_videodump(oa::runtime::GameRuntime* rt) {
    oa::media::VideoEngine& ve = rt->video();
    const auto st = ve.state();
    for (const auto& [id, ch] : st.video_layers) {
        int w = 0, h = 0;
        const uint8_t* px = nullptr;
        uint64_t rev = 0;
        std::string dims = "?";
        if (ch.playing && ch.decoded &&
            ve.video_frame(id, &w, &h, &px, &rev)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%dx%d", w, h);
            dims = buf;
        }
        std::printf("[bt96] video id='%s' file='%s' playing=%d decoded=%d"
                    " loop=%d skip=%d pos=%llums %s rev=%llu\n",
                    id.c_str(), ch.file.c_str(), ch.playing ? 1 : 0,
                    ch.decoded ? 1 : 0, ch.loop_play ? 1 : 0,
                    ch.skippable ? 1 : 0, (unsigned long long)ch.position_ms,
                    dims.c_str(), (unsigned long long)ve.frame_revision(id));
    }
}

/// park screenshot + motion sample: snapshot the renderer, write PNG, print
/// luma/bright, and (when diff) compare against the previous snapshot.
static void ad_bt96_shot(AppState* s, const char* tag, bool diff) {
    if (s->ad_out.empty()) return;
    oa::media::Image img;
    if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0)
        return;
    const size_t n = size_t(img.w) * size_t(img.h);
    uint64_t bright = 0;
    double sum = 0;
    uint64_t white = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &img.rgba[i * 4];
        const double luma = (double(p[0]) + p[1] + p[2]) / 3.0;
        sum += luma;
        if (luma > 120.0) ++bright;
        if (p[0] > 200 && p[1] > 200 && p[2] > 200) ++white;
    }
    std::printf("[bt96] %s shot f=%llu luma=%.1f bright=%.1f%% white=%.3f%%\n",
                tag, (unsigned long long)s->frames,
                n ? sum / double(n) : 0.0,
                100.0 * double(bright) / double(n ? n : 1),
                100.0 * double(white) / double(n ? n : 1));
    if (diff) {
        static int prv_w = 0, prv_h = 0;
        static std::vector<uint8_t> prv;
        if (prv_w == img.w && prv_h == img.h && !prv.empty()) {
            uint64_t changed = 0, changed_top = 0, newwhite = 0;
            int bx0 = img.w, by0 = img.h, bx1 = -1, by1 = -1;
            for (int y = 0; y < img.h; ++y) {
                const bool top = y < img.h / 2;
                for (int x = 0; x < img.w; ++x) {
                    const uint8_t* p = &img.rgba[(size_t(y) * img.w + x) * 4];
                    const uint8_t* q =
                        &prv[(size_t(y) * img.w + x) * 4];
                    if (p[0] == q[0] && p[1] == q[1] && p[2] == q[2])
                        continue;
                    ++changed;
                    if (top) ++changed_top;
                    if (p[0] > 200 && p[1] > 200 && p[2] > 200) ++newwhite;
                    if (x < bx0) bx0 = x;
                    if (x > bx1) bx1 = x;
                    if (y < by0) by0 = y;
                    if (y > by1) by1 = y;
                }
            }
            std::printf("[bt96] diff changed=%llu top-half=%llu newwhite=%llu"
                        " bbox=(%d,%d)-(%d,%d)\n",
                        (unsigned long long)changed,
                        (unsigned long long)changed_top,
                        (unsigned long long)newwhite, bx0, by0, bx1, by1);
        }
        prv_w = img.w;
        prv_h = img.h;
        prv.assign(img.rgba.begin(), img.rgba.end());
    }
    const std::vector<uint8_t> png = oa::runtime::encode_png(
        uint32_t(img.w), uint32_t(img.h), img.rgba);
    if (png.empty()) return;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/bt96_%s_%03llu.png", s->ad_out.c_str(),
                  tag, (unsigned long long)s->ad_ppm_count);
    ++s->ad_ppm_count;
    FILE* f = std::fopen(path, "wb");
    if (f) {
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
    }
}

static bool ad_bt96snow_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    static int sub = 0;               // 0 nav / 1 walk parks
    static int parks = 0;
    static int page_budget = 3;
    static bool parked_prev = false;
    static int last_park_f = -1;
    static int cap_phase = 0;         // capture cadence inside a park
    static int dumped = 0;            // verbose dump counter (first 2 parks)
    static int stuck = 0;
    if (sub == 0) {
        const char* v = std::getenv("OA_BT96_PAGES");
        if (v && *v && std::atoi(v) > 0) page_budget = std::atoi(v);
        static int navi = 0;
        static int nav_wait = 0;
        static const char* kNav[] = {"bt_cn", "bt_cn_next", "bt_start"};
        if (navi < 3) {
            ++nav_wait;
            const char* key = kNav[navi];
            if (ad_has_handler(rt, key)) {
                ad_arm_click_key(s, key);
                std::printf("[bt96] nav click '%s' f=%llu\n", key,
                            (unsigned long long)s->frames);
                ++navi;
                nav_wait = 0;
                if (navi == 3) {
                    sub = 1;
                    parks = 0;
                    parked_prev = false;
                    stuck = 0;
                }
            } else if (nav_wait > 7000) {
                std::printf("[bt96] nav skip '%s' (never appeared)\n", key);
                ++navi;
                nav_wait = 0;
                if (navi >= 3) {
                    sub = 1;
                    parks = 0;
                    parked_prev = false;
                    stuck = 0;
                }
            }
            return false;
        }
        std::printf("[bt96] nav exhausted; abort\n");
        s->quit = true;
        return false;
    }
    // sub == 1: story walk with per-park capture + dumps
    const oa::runtime::WaitReason* w = rt->current_wait();
    const bool clickw = w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                              w->kind == oa::runtime::WaitReason::Kind::Generic0 ||
                              w->kind == oa::runtime::WaitReason::Kind::Timed ||
                              w->kind == oa::runtime::WaitReason::Kind::Se);
    const bool ready = bt91_story_text_ready(rt);
    const bool parked = clickw && ready;
    if (parked && !parked_prev && last_park_f != int(s->frames)) {
        last_park_f = int(s->frames);
        ++parks;
        cap_phase = 0;
        std::string sample;
        for (const std::string& id : rt->text().visible_content_layers()) {
            if (id.find(".mw.") == std::string::npos) continue;
            const oa::render::MessageLayer* ml = rt->text().layer(id);
            if (ml && !ml->page.empty()) {
                for (const auto& u : ml->page) {
                    if (!u.data.empty()) {
                        sample = u.data.substr(0, 18);
                        break;
                    }
                }
            }
            if (!sample.empty()) break;
        }
        std::printf("[bt96] park %d f=%llu text='%s'\n", parks,
                    (unsigned long long)s->frames, sample.c_str());
        if (dumped < 2) {
            ++dumped;
            std::printf("[bt96] --- park %d bg-zone layer dump ---\n", parks);
            ad_bt96_parkdump(rt);
            std::printf("[bt96] --- park %d video channel dump ---\n", parks);
            ad_bt96_videodump(rt);
        }
        ad_bt96_shot(s, "park", false);
        cap_phase = 1;
        parked_prev = true;  // hold this park: sample below, then advance
    }
    if (parked && cap_phase > 0) {
        // sample the animation: one shot every ~24 frames, 5 samples
        if (cap_phase <= 5 && s->ad_stage_f % 24 == 0) {
            ad_bt96_shot(s, "move", cap_phase > 1);
            ++cap_phase;
        }
        if (cap_phase > 5) {
            if (parks >= page_budget) {
                std::printf("[bt96] DONE parks=%d f=%llu\n", parks,
                            (unsigned long long)s->frames);
                s->quit = true;
                return false;
            }
            // advance to the next story page
            const int cx = rt->project_.config.stage_width / 2;
            const int cy = rt->project_.config.stage_height - 150;
            s->input.mouse_x = cx;
            s->input.mouse_y = cy;
            s->ad_click = true;
            parked_prev = true;
            cap_phase = 0;
            stuck = 0;
            return false;
        }
        return false;
    }
    if (!parked) {
        parked_prev = false;
        ++stuck;
        if (parks > 0 && stuck > 4000) {
            std::printf("[bt96] stuck after park %d; abort\n", parks);
            s->quit = true;
        }
        // guard: clickable-but-not-parked for a long while -> nudge
        if (clickw && ready && s->ad_stage_f % 2400 == 60) {
            const int cx = rt->project_.config.stage_width / 2;
            const int cy = rt->project_.config.stage_height - 150;
            s->input.mouse_x = cx;
            s->input.mouse_y = cy;
            s->ad_click = true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// OA_AUTODRIVE=bt119cg — btjy (HENPRI CN, 1600x900, Qruppo/kiyopi) extra
// CG-gallery 差分 (variant) switch journey.
// Drives: title language select -> title -> force-unlock the
// gallery through the shape of the game's OWN commented debug hook
// (extra_cgopen: gscr.evset/gscr.ev, injected over the Lua bridge — harness
// only, no game data touched) -> click bt_extra -> click a thumbnail that has
// several variants (default cg03 = ev_com_01, 7 variants) -> repeated clicks
// inside the viewer, dumping after every click: the Lua viewer state
// (flg.excgbuff.count/max, flg.cgcg, btn.cursor, t.check), the 600/700 layer
// subtrees (file/clip/props/world rect) and the presented-frame FNV hash +
// pixel diff against the previous shot.
//   OA_BT119_CG=cg03      thumbnail click key
//   OA_BT119_CLICKS=6     clicks inside the viewer
//   OA_BT119_AT=1310,880  neutral click point (no button under it)
//   OA_BT119_THUMB=1      first click lands on the thumbnail centre instead
// ---------------------------------------------------------------------------
static void ad_bt119_shot(AppState* s, const char* tag) {
    if (s->ad_out.empty()) return;
    oa::media::Image img;
    if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0) return;
    static std::vector<uint8_t> prev;
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : img.rgba) { h ^= b; h *= 1099511628211ull; }
    size_t diff = 0;
    if (prev.size() == img.rgba.size()) {
        for (size_t i = 0; i + 2 < img.rgba.size(); i += 4)
            if (prev[i] != img.rgba[i] || prev[i + 1] != img.rgba[i + 1] ||
                prev[i + 2] != img.rgba[i + 2])
                ++diff;
    }
    std::printf("[cg119] SHOT %-16s %dx%d hash=%016llx diffpx=%zu\n", tag,
                img.w, img.h, (unsigned long long)h, diff);
    const std::vector<uint8_t> png =
        oa::runtime::encode_png(uint32_t(img.w), uint32_t(img.h), img.rgba);
    if (!png.empty()) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/bt119_%s_%03llu.png",
                      s->ad_out.c_str(), tag,
                      (unsigned long long)s->ad_ppm_count++);
        if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(png.data(), 1, png.size(), f);
            std::fclose(f);
        }
    }
    prev = img.rgba;
}

static void ad_bt119_layer_dump(AppState* s, const char* prefix) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    const size_t n = std::strlen(prefix);
    for (const oa::render::Layer* l : rt->scene().draw_order()) {
        if (l->id.size() < n || l->id.compare(0, n, prefix) != 0) continue;
        std::string props;
        for (const char* k : {"left", "top", "width", "height", "clip", "x",
                              "y", "xscale", "yscale", "zoom", "anchorx",
                              "anchory", "alpha", "visible",
                              "intermediate_render", "intermediate_render_mask",
                              "layermode", "reversex", "reversey"}) {
            const auto it = l->props.find(k);
            if (it != l->props.end())
                props += std::string(" ") + k + "=" + it->second;
        }
        char wb[160] = "";
        if (const auto r = s->oaRender->layer_world_rect(*l))
            std::snprintf(wb, sizeof(wb), "world=(%.1f,%.1f %.1fx%.1f)",
                          (*r)[0], (*r)[1], (*r)[2], (*r)[3]);
        std::printf(
            "[cg119] L %-36s vis=%d ovis=%d a=%.0f file='%s' "
            "clip=%d(%.0f,%.0f,%.0f,%.0f) size=(%.0fx%.0f) mask='%s'%s %s\n",
            l->id.c_str(), (int)l->visible,
            (int)rt->scene().is_effectively_visible(l->id), l->alpha,
            l->file.c_str(), (int)l->has_clip, l->clip_x, l->clip_y, l->clip_w,
            l->clip_h, l->width, l->height, l->mask.c_str(), props.c_str(), wb);
    }
    std::fflush(stdout);
}

static bool ad_bt119_any_prefix(const oa::runtime::GameRuntime* rt,
                                const char* prefix) {
    const size_t n = std::strlen(prefix);
    for (const oa::render::Layer* l : rt->scene().draw_order())
        if (l->id.size() >= n && l->id.compare(0, n, prefix) == 0) return true;
    return false;
}

/// World rect (centre + size) of the first layer carrying a click handler row
/// with `key` — the gallery thumbnails.
static bool ad_find_click_rect(AppState* s, const char* key, int* cx, int* cy,
                               int* w, int* h) {
    for (const oa::render::Layer* l : s->rt->scene().draw_order()) {
        const auto* row = s->rt->scene().find_event_handler(l->id, "click");
        if (!row) continue;
        const auto k = row->params.find("key");
        if (k == row->params.end() || k->second != key) continue;
        if (const auto r = s->oaRender->layer_world_rect(*l)) {
            *cx = int((*r)[0] + (*r)[2] / 2);
            *cy = int((*r)[1] + (*r)[3] / 2);
            *w = int((*r)[2]);
            *h = int((*r)[3]);
            return true;
        }
    }
    return false;
}

// Harness-only Lua injection: the game ships `extra_cgopen` commented out in
// system/extend/user.lua; the journey installs the same shape (plus the full
// variant sweep the gallery reader expects). OA_BT119_TRACE=0 runs the same
// journey WITHOUT the viewer/click-chain monkey patches (clean evidence run:
// the variant switch must be visible from the layer dump + frame hashes
// alone).
static const char* kAd119Inject = R"LUA(
_G.oa119_unlock = function()
  local n = 0
  for set, v in pairs(csv.extra_cgmode) do
    gscr.evset[set] = true
    for i = 3, table.maxn(v) do gscr.ev[v[i]] = true; n = n + 1 end
  end
  return n
end
gscr.extraopen = true
if set_eval then set_eval("g.extraopen=1") end
-- title_init already ran with extraopen false -> bt_extra sits in the btn
-- group's disabled set, and the game's own eventFilter (adv/keyevent.lua
-- event_lyevent: "nm and getBtnStat(nm) -> r=1") then swallows every click on
-- it before the engine dispatches. Re-enable it for the journey.
if setBtnStat then setBtnStat("bt_extra", nil) end
print("[cg119] lua unlock variants=" .. tostring(oa119_unlock()) ..
      " extraopen=" .. tostring(gscr.extraopen))
)LUA";

// Trace half of the injection — installed only when OA_BT119_TRACE != 0
// (evidence: the viewer/click-chain call trace).
static const char* kAd119Trace = R"LUA(
-- viewer trace: which variant the chain is drawing (flg.excgbuff count/max).
local _oc = extra_cg_check
function extra_cg_check(e, p)
  local v = flg.excgbuff or {}
  print(string.format("[cg119] lua cgcheck IN cgcg=%s bt=%s cursor=%s pname=%s count=%s max=%s",
        tostring(flg.cgcg), tostring(exf.cgcgcursor or (p and p.name) or btn.cursor),
        tostring(btn.cursor), tostring(p and p.name), tostring(v.count), tostring(v.max)))
  local ok, err = pcall(_oc, e, p)
  if not ok then print("[cg119] lua cgcheck ERR " .. tostring(err)) end
  local ok2, tc = pcall(function() return tostring(e:var("t.check")) end)
  print(string.format("[cg119] lua cgcheck OUT count=%s tcheck=%s",
        tostring(flg.excgbuff and flg.excgbuff.count), tostring(tc)))
end
-- click-chain traces: the game's own setonpush/event filter
-- path that is supposed to turn a left click into a dummy click.
local _esp = event_setonpush
function event_setonpush(p, f)
  local r = _esp(p, f)
  print(string.format("[cg119] lua filter setonpush key=%s -> r=%s gmode=%s waitflag=%s",
        tostring(p and p.key), tostring(r),
        tostring(getGameMode and getGameMode("all")),
        tostring(flg.waitflag and "set" or "nil")))
  return r
end
local _spc = setonpush_calllua
function setonpush_calllua(e, param)
  print(string.format("[cg119] lua push IN key=%s adv=%s ui=%s btnstop=%s cursor=%s keycode=%s",
        tostring(param and param.key), tostring(param and param.adv),
        tostring(param and param.ui), tostring(flg.btnstop),
        tostring(btn and btn.cursor), tostring(flg.keycode)))
  local ok, err = pcall(_spc, e, param)
  if not ok then print("[cg119] lua push ERR " .. tostring(err)) end
  print(string.format("[cg119] lua push OUT exclick=%s keycode=%s",
        tostring(flg.exclick), tostring(flg.keycode)))
end
local _sec = setexclick
function setexclick(no)
  print("[cg119] lua setexclick " .. tostring(no))
  _sec(no)
end
local _vs = vsync
function vsync()
  if flg.exclick then
    print("[cg119] lua vsync injecting exclick=" .. tostring(flg.exclick))
  end
  _vs()
end
local _ov = extra_cg_viewer
function extra_cg_viewer()
  local v = flg.excgbuff or {}
  print(string.format("[cg119] lua cgview count=%s max=%s set=%s n=%s",
        tostring(v.count), tostring(v.max), tostring(v.set), tostring(v[v.count])))
  _ov()
end
)LUA";

static const char* kAd119State = R"LUA(
local v = flg.excgbuff or {}
print(string.format("[cg119] lua state count=%s max=%s set=%s n=%s cgcg=%s cursor=%s btnname=%s appexname=%s",
      tostring(v.count), tostring(v.max), tostring(v.set), tostring(v[v.count]),
      tostring(flg.cgcg), tostring(btn.cursor), tostring(btn.name),
      tostring(appex and appex.name)))
)LUA";

static void ad_bt119_wait_dump(AppState* s, const char* tag) {
    const oa::runtime::WaitReason* w = s->rt->current_wait();
    using K = oa::runtime::WaitReason::Kind;
    const char* kn = "none";
    if (w) {
        switch (w->kind) {
            case K::Generic: kn = "generic"; break;
            case K::Generic0: kn = "wt0"; break;
            case K::Timed: kn = "timed"; break;
            case K::Stop: kn = "stop"; break;
            case K::Se: kn = "se"; break;
            case K::VideoLayer: kn = "video"; break;
            case K::ScenarioTween: kn = "sctween"; break;
            case K::KeyWait: kn = "keywait"; break;
        }
    }
    std::printf("[cg119] WAIT %s kind=%s id='%s' from_queue=%d queued=%zu buttons=",
                tag, kn, w ? w->id.c_str() : "-",
                (int)s->rt->interpreter().last_wait_from_queue(),
                s->rt->interpreter().queued_tag_count());
    if (w)
        for (const std::string& b : w->buttons) std::printf("'%s' ", b.c_str());
    std::printf(" script=%s line=%zu hovered=",
                s->rt->interpreter().current_script()
                    ? s->rt->interpreter().current_script()->c_str()
                    : "-",
                s->rt->interpreter().current_line());
    for (const std::string& h : s->rt->hovered_layers())
        std::printf("%s ", h.c_str());
    std::printf("\n");
    std::fflush(stdout);
}

static void ad_bt119_lua_state(AppState* s) {
    try {
        s->rt->interpreter().lua_bridge().run_code(kAd119State, "oa119state");
    } catch (const std::exception& e) {
        std::printf("[cg119] state query failed: %s\n", e.what());
    }
    std::fflush(stdout);
}

/// Hit-test the engine's own way at (x,y) — the same providers main.cpp hands
/// the runtime (proves whether a synthetic click position is a
/// hit at all before blaming the dispatch chain).
static void ad_bt119_hit_probe(AppState* s, int x, int y) {
    auto hits = s->rt->scene().hit_test_all(
        double(x), double(y), oa::render::RenderEngine::hit_size,
        static_cast<void*>(s->oaRender.get()),
        oa::render::RenderEngine::alpha_sampler);
    std::printf("[cg119] HIT (%d,%d) hide=%d hits=%zu:", x, y,
                (int)s->rt->hide_active(), hits.size());
    for (const std::string& id : hits) {
        const bool any = s->rt->scene().has_any_enabled_handler(id);
        std::printf(" %s%s", id.c_str(), any ? "" : "(no-handler)");
    }
    std::printf("\n");
    std::fflush(stdout);
}

static bool ad_bt119_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    static int sub = 0; // 0 nav / 1 title+unlock / 2 gallery / 3 arm click /
                        // 4 settle+dump
    static int navi = 0;
    static int nav_wait = 0;
    static int settle = 0;
    static int clicks = 0;
    static int click_budget = 6;
    static int thumb_first = 0;
    static std::string cg_key = "cg03";
    static int at_x = 1310, at_y = 880;

    if (sub == 0) {
        static const char* kNav[] = {"bt_cn", "bt_cn_next"};
        if (navi < 2) {
            ++nav_wait;
            if (ad_has_handler(rt, kNav[navi])) {
                ad_arm_click_key(s, kNav[navi]);
                std::printf("[cg119] nav click '%s'\n", kNav[navi]);
                ++navi;
                nav_wait = 0;
            } else if (nav_wait > 7000) {
                std::printf("[cg119] nav skip '%s'\n", kNav[navi]);
                ++navi;
                nav_wait = 0;
            }
            return false;
        }
        if (!(ad_wait_stop(rt->current_wait()) &&
              ad_has_handler(rt, "bt_start"))) {
            if (s->ad_stage_f > 20000) {
                std::printf("[cg119] no title; abort\n");
                s->quit = true;
            }
            return false;
        }
        const char* v = std::getenv("OA_BT119_CG");
        if (v && *v) cg_key = v;
        v = std::getenv("OA_BT119_CLICKS");
        if (v && *v && std::atoi(v) > 0) click_budget = std::atoi(v);
        v = std::getenv("OA_BT119_AT");
        if (v && *v) std::sscanf(v, "%d,%d", &at_x, &at_y);
        v = std::getenv("OA_BT119_THUMB");
        thumb_first = (v && *v && std::atoi(v) != 0) ? 1 : 0;
        // instruction-level trace of the ui.asb viewer loop:
        // prints every executed instruction of system/ui.asb while the
        // journey runs, so the parked position is provable.
        rt->interpreter().on_step = [](const std::string& script, size_t line,
                                       const oa::runtime::Instruction& i) {
            if (script.find("ui.asb") == std::string::npos) return;
            static const char* kKeep[] = {"exkey", "wt",  "wt0",    "calllua",
                                          "jump",  "call", "return",
                                          "stop",  "lydel", "setonpush"};
            bool keep = false;
            for (const char* k : kKeep)
                if (i.tag == k) keep = true;
            if (!keep) return;
            std::string p;
            for (const auto& [k, v] : i.params) p += " " + k + "=" + v;
            std::printf("[cg119] STEP %s:%zu [%s]%s\n", script.c_str(), line,
                        i.tag.c_str(), p.c_str());
            std::fflush(stdout);
        };
        try {
            rt->interpreter().lua_bridge().run_code(kAd119Inject, "oa119");
            const char* tr = std::getenv("OA_BT119_TRACE");
            if (!(tr && *tr && std::atoi(tr) == 0))
                rt->interpreter().lua_bridge().run_code(kAd119Trace, "oa119tr");
        } catch (const std::exception& e) {
            std::printf("[cg119] inject failed: %s\n", e.what());
            s->quit = true;
            return false;
        }
        std::printf("[cg119] title reached: bt_extra handler=%d\n",
                    (int)ad_has_handler(rt, "bt_extra"));
        ad_bt119_lua_state(s);
        ad_bt119_layer_dump(s, "500.");
        ad_arm_click_key(s, "bt_extra");
        ad_bt119_hit_probe(s, s->ad_cx, s->ad_cy);
        std::printf("[cg119] click bt_extra at (%d,%d)\n", s->ad_cx, s->ad_cy);
        sub = 1;
        settle = 60;
        return false;
    }
    if (sub == 1) {
        if (--settle > 0) return false;
        if (!ad_has_handler(rt, cg_key.c_str())) {
            if (s->ad_stage_f > 5000) {
                std::printf("[cg119] gallery thumbnail '%s' absent; keys:\n",
                            cg_key.c_str());
                for (const oa::render::Layer* l : rt->scene().draw_order()) {
                    const auto* h =
                        rt->scene().find_event_handler(l->id, "click");
                    if (!h) continue;
                    const auto k = h->params.find("key");
                    if (k != h->params.end())
                        std::printf("[cg119]   key '%s' @ %s\n",
                                    k->second.c_str(), l->id.c_str());
                }
                s->quit = true;
            }
            return false;
        }
        std::printf("[cg119] === gallery open, clicking '%s' ===\n",
                    cg_key.c_str());
        ad_bt119_layer_dump(s, "500.z.bt.");
        ad_arm_click_key(s, cg_key.c_str());
        std::printf("[cg119] click '%s' at (%d,%d)\n", cg_key.c_str(), s->ad_cx,
                    s->ad_cy);
        sub = 2;
        settle = 120;
        return false;
    }
    if (sub == 2) {
        if (--settle > 0) return false;
        if (!ad_bt119_any_prefix(rt, "600.1")) {
            if (s->ad_stage_f > 5000) {
                std::printf("[cg119] CG viewer did not open\n");
                s->quit = true;
            }
            return false;
        }
        std::printf("[cg119] === CG viewer open (state #0) ===\n");
        ad_bt119_wait_dump(s, "open");
        ad_bt119_lua_state(s);
        ad_bt119_layer_dump(s, "600");
        ad_bt119_layer_dump(s, "700");
        ad_bt119_shot(s, "s0");
        sub = 3;
        settle = 0;
        return false;
    }
    if (sub == 3) {
        if (--settle > 0) return false;
        if (clicks >= click_budget) {
            std::printf("[cg119] DONE clicks=%d\n", clicks);
            s->quit = true;
            return false;
        }
        ++clicks;
        if (clicks == 1 && thumb_first) {
            int cx = 0, cy = 0, w = 0, h = 0;
            if (ad_find_click_rect(s, cg_key.c_str(), &cx, &cy, &w, &h)) {
                std::printf("[cg119] click #%d on thumbnail '%s' centre "
                            "(%d,%d) %dx%d\n",
                            clicks, cg_key.c_str(), cx, cy, w, h);
                s->ad_cx = cx;
                s->ad_cy = cy;
            }
        } else {
            std::printf("[cg119] click #%d at neutral (%d,%d)\n", clicks, at_x,
                        at_y);
            s->ad_cx = at_x;
            s->ad_cy = at_y;
        }
        ad_bt119_hit_probe(s, s->ad_cx, s->ad_cy);
        // A/B: the game's exkeyin sets flg.keycode (the
        // "waiting key list" name); the click->dummy-click path in
        // setonpush_calllua keys off it. Re-arming it right before the click
        // reproduces the reference engine's "queue held behind the queued
        // [@] wait" state and isolates the defect to the queue drain.
        if (std::getenv("OA_BT119_FAKEKEYCODE"))
            rt->interpreter().lua_bridge().run_code(
                "if flg then flg.keycode = 'MANUAL' end", "oa119kc");
        s->ad_click = true;
        sub = 4;
        settle = 40;
        return false;
    }
    // sub == 4: settle, then dump everything for this click.
    if (--settle > 0) return false;
    char tag[32];
    std::snprintf(tag, sizeof(tag), "s%d", clicks);
    std::printf("[cg119] === after click #%d ===\n", clicks);
    ad_bt119_wait_dump(s, "post-click");
    ad_bt119_lua_state(s);
    ad_bt119_layer_dump(s, "600");
    ad_bt119_layer_dump(s, "700");
    ad_bt119_shot(s, tag);
    sub = 3;
    settle = 0;
    return false;
}

// ---------------------------------------------------------------------------
// OA_AUTODRIVE=n2title — NUKITASHI2 (1600x900, pf6 base + .000/.020 volumes,
// Qruppo/kiyopi-family CN build) title-UI crash reproduction.
// User journey: title -> Exit -> confirm-dialog cancel -> Config
// -> "[app] tick error: message.lua:568 getTextBlockFloat ipairs(nil)".
// Handles the first-boot language page (bt_cn -> bt_cn_next) when present.
// OA_N2_DELAY: settle frames between dialog-cancel and the Config click.
// ---------------------------------------------------------------------------
static bool ad_n2title_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    static int sub = -4;   // -4 lang wait / -3 lang next / -2 -> title /
                           // -1 title page / 0 exit clicked / 1 cancelled /
                           // 2 config clicked / 3 watchdog
    static int settle = 0;
    static int cfg_delay = 1;
    static int cfg_watch = 0;
    static bool cfg_open = false;
    static int lang_wait = 0;
    const oa::runtime::WaitReason* w = rt->current_wait();
    if (sub <= -2) {
        // ---- first-boot language page (fresh save root) -------------------
        if (sub == -4) {
            if (ad_has_handler(rt, "bt_cn")) {
                ++lang_wait;
                if (lang_wait > 30) {
                    ad_arm_click_key(s, "bt_cn");
                    std::printf("[n2] f=%llu lang click bt_cn\n",
                                (unsigned long long)s->frames);
                    sub = -3;
                    lang_wait = 0;
                }
            } else if (ad_has_handler(rt, "bt_cn_next")) {
                ad_arm_click_key(s, "bt_cn_next");
                std::printf("[n2] f=%llu lang click bt_cn_next\n",
                            (unsigned long long)s->frames);
                sub = -2;
            } else if (ad_has_handler(rt, "bt_conf")) {
                sub = -1;  // no language page: straight to title
            } else if (++lang_wait > 3000) {
                std::printf("[n2] f=%llu boot never reached lang/title; abort\n",
                            (unsigned long long)s->frames);
                s->quit = true;
            }
            return false;
        }
        if (sub == -3) {  // bt_cn picked -> the *_next key appears
            if (ad_has_handler(rt, "bt_cn_next")) {
                ad_arm_click_key(s, "bt_cn_next");
                std::printf("[n2] f=%llu lang click bt_cn_next\n",
                            (unsigned long long)s->frames);
                sub = -2;
            } else if (++lang_wait > 3000) {
                std::printf("[n2] f=%llu bt_cn_next never appeared\n",
                            (unsigned long long)s->frames);
                s->quit = true;
            }
            return false;
        }
        // sub == -2: language page closing; fall through to title detection
        sub = -1;
    }
    const bool title_parked = ad_wait_stop(w) && ad_has_handler(rt, "bt_conf");
    if (sub == -1) {
        if (!title_parked) return false;
        std::printf("[n2] f=%llu TITLE parked\n", (unsigned long long)s->frames);
        // dump every click-handler key with its world rect (title evidence)
        for (const oa::render::Layer* l : rt->scene().draw_order()) {
            const auto* h = rt->scene().find_event_handler(l->id, "click");
            if (!h) continue;
            const auto k = h->params.find("key");
            if (k == h->params.end()) continue;
            const auto r = s->oaRender->layer_world_rect(*l);
            std::printf("[n2]   key '%-10s' layer=%s rect=(%.0f,%.0f %.0fx%.0f)\n",
                        k->second.c_str(), l->id.c_str(),
                        r ? (*r)[0] : -1.0, r ? (*r)[1] : -1.0,
                        r ? (*r)[2] : 0.0, r ? (*r)[3] : 0.0);
        }
        // OA_N2_EXIT=0: skip the exit-confirm journey and click Config
        // directly (isolates whether the exit dialog is a precondition).
        const char* vex = std::getenv("OA_N2_EXIT");
        if (vex && *vex && std::atoi(vex) == 0) {
            const char* ck = std::getenv("OA_N2_CONFKEY");
            const char* key = (ck && *ck) ? ck : "bt_conf";
            ad_arm_click_key(s, key);
            std::printf("[n2] f=%llu click %s directly (no exit)\n",
                        (unsigned long long)s->frames, key);
            sub = 2;
            cfg_watch = 0;
            cfg_open = false;
            return false;
        }
        if (!ad_has_handler(rt, "bt_exit")) {
            std::printf("[n2] f=%llu no bt_exit; abort\n",
                        (unsigned long long)s->frames);
            s->quit = true;
            return false;
        }
        ad_arm_click_key(s, "bt_exit");
        std::printf("[n2] f=%llu click bt_exit\n", (unsigned long long)s->frames);
        sub = 0;
        settle = 0;
        return false;
    }
    if (sub == 0) {  // exit confirm dialog expected (bt_yes/bt_no)
        static int cancel_delay = 2;
        static bool cancel_init = false;
        if (!cancel_init) {
            const char* v = std::getenv("OA_N2_CANCEL");
            if (v && *v && std::atoi(v) >= 0) cancel_delay = std::atoi(v);
            cancel_init = true;
            std::printf("[n2] cancel click delay=%d frames (OA_N2_CANCEL)\n",
                        cancel_delay);
        }
        if (ad_has_handler(rt, "bt_yes") || ad_has_handler(rt, "bt_no")) {
            if (ad_has_handler(rt, "bt_no")) {
                if (settle >= cancel_delay) {
                    std::printf("[n2] f=%llu dialog up (yes=%d no=%d)\n",
                                (unsigned long long)s->frames,
                                ad_has_handler(rt, "bt_yes") ? 1 : 0,
                                ad_has_handler(rt, "bt_no") ? 1 : 0);
                    const char* vyes = std::getenv("OA_N2_YES");
                    if (vyes && *vyes && std::atoi(vyes) == 1) {
                        ad_arm_click_key(s, "bt_yes");
                        std::printf("[n2] f=%llu click bt_yes (exit YES)\n",
                                    (unsigned long long)s->frames);
                        sub = 4;  // exit path watchdog
                        settle = 0;
                        return false;
                    }
                    ad_arm_click_key(s, "bt_no");
                    std::printf("[n2] f=%llu click bt_no (cancel)\n",
                                (unsigned long long)s->frames);
                    sub = 1;
                    settle = 0;
                    const char* v = std::getenv("OA_N2_DELAY");
                    if (v && *v && std::atoi(v) >= 0) cfg_delay = std::atoi(v);
                    std::printf("[n2] config click delay=%d frames\n", cfg_delay);
                } else {
                    ++settle;
                }
            }
        } else if (rt->exit_requested()) {
            std::printf("[n2] f=%llu exit requested without dialog (bt_no "
                        "variant?) - done\n",
                        (unsigned long long)s->frames);
            s->quit = true;
        } else if (++settle > 3000) {
            std::printf("[n2] f=%llu dialog never appeared; abort\n",
                        (unsigned long long)s->frames);
            s->quit = true;
        }
        return false;
    }
    if (sub == 1) {  // cancel: wait for the dialog buttons to go away
        static const char* cfg_key = nullptr;
        if (!cfg_key) {
            const char* ck = std::getenv("OA_N2_CONFKEY");
            cfg_key = (ck && *ck) ? ck : "bt_conf";
            std::printf("[n2] config key = %s\n", cfg_key);
        }
        if (!ad_has_handler(rt, "bt_no") && !ad_has_handler(rt, "bt_yes")) {
            if (settle >= cfg_delay) {
                ad_arm_click_key(s, cfg_key);
                std::printf("[n2] f=%llu click %s (config)\n",
                            (unsigned long long)s->frames, cfg_key);
                sub = 2;
                cfg_watch = 0;
                cfg_open = false;
            }
            ++settle;
        } else if (++settle > 3000) {
            std::printf("[n2] f=%llu dialog did not close; abort\n",
                        (unsigned long long)s->frames);
            s->quit = true;
        }
        return false;
    }
    // sub == 2/3: config opened or crashed — heartbeat until frame budget
    if (sub == 2) sub = 3;
    if (sub == 4) {  // exit YES path: engine should request exit
        if (rt->exit_requested()) {
            std::printf("[n2] f=%llu EXIT REQUESTED (yes path clean)\n",
                        (unsigned long long)s->frames);
            s->quit = true;
        } else if (++cfg_watch > 3000) {
            std::printf("[n2] f=%llu yes path: no exit requested; abort\n",
                        (unsigned long long)s->frames);
            s->quit = true;
        }
        return false;
    }
    if (!cfg_open) {
        const char* ck = std::getenv("OA_N2_CONFKEY");
        const char* key = (ck && *ck) ? ck : "bt_conf";
        if (!ad_has_handler(rt, key)) {
            cfg_open = true;
            std::printf("[n2] f=%llu CONFIG OPENED (%s gone)\n",
                        (unsigned long long)s->frames, key);
        }
    }
    if (rt->exit_requested()) {
        std::printf("[n2] f=%llu exit_requested observed\n",
                    (unsigned long long)s->frames);
    }
    if (++cfg_watch % 500 == 0) {
        std::printf("[n2] f=%llu watchdog alive cfg=%s wait_kind=%d\n",
                    (unsigned long long)s->frames, cfg_open ? "open" : "?",
                    w ? int(w->kind) : -1);
    }
    if (cfg_watch > 6000) {
        std::printf("[n2] f=%llu NO CRASH within %d frames after config; "
                    "cfg_open=%d\n", (unsigned long long)s->frames,
                    cfg_watch, cfg_open ? 1 : 0);
        s->quit = true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// OA_AUTODRIVE=sakura — snll (サクラノ詩) title petal journey:
// boots the real project windowed, clicks the ja language button when the
// language page appears, then parks on the title where the sakura.ogv petal
// layer plays with its auto-detected sakura_m.ogv mask partner. Evidence:
// engine channel state (mask_on, frame stats over the composite frames),
// host-path checks (the layer texture upload / scene binding), and PNG
// snapshots whose motion-pixel color census answers the "visible effect"
// question (petals stay white; the mask erases the soft-edge ring; pink on
// the title comes only from the art below). OA_AD_VISIBLE=1 + Xvfb needed.
// ---------------------------------------------------------------------------
namespace {
struct SakuraJourney {
    int sub = 0;             // 0 lang wait / 1 park title / 2 sampling / 3 done
    uint64_t sub_f = 0;
    uint64_t clicks = 0;
    uint64_t lang_enum_f = 0; // frame of the last clickable-layers printout
    uint64_t title_f = 0;     // frame the sakura channel appeared
    uint64_t last_cap_f = 0;  // frame of the last snapshot
    uint64_t engine_stats_f = 0;
    uint64_t shots = 0;
    std::vector<uint8_t> last;
    uint32_t lw = 0, lh = 0;
    // Motion-pixel census totals (per shot pair) over the sampling window.
    uint64_t changed = 0;
    uint64_t m_white = 0, m_warm = 0, m_pink = 0, m_other = 0;
    bool overlay_brand_seen = false;
};
} // namespace

static bool ad_sakura_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    static SakuraJourney j;
    ++j.sub_f;

    oa::media::VideoEngine& ve = rt->video();
    const auto vst = ve.state();

    // The overlay channel plays the boot brand (logo.mp4 etc.); never click
    // while a fullscreen movie covers the stage.
    const bool overlay_playing = vst.overlay_video && vst.overlay_video->playing;
    if (overlay_playing) {
        j.overlay_brand_seen = true;
        if (j.sub_f % 300 == 1)
            std::printf("[sakura] f=%llu boot overlay '%s' playing pos=%llums\n",
                        (unsigned long long)s->frames,
                        vst.overlay_video->file.c_str(),
                        (unsigned long long)vst.overlay_video->position_ms);
    }

    if (j.sub == 0) {
        // Wait out the boot overlay, then click the ja language button
        // (btn01 ja at ~(716,364) 488x118, center (960,423))
        // once the language page is actually under the cursor. Progress out
        // of this stage happens only when the sakura channel appears.
        const uint64_t hard_cap = 60 * 60; // 60 s of frames
        if (j.sub_f > hard_cap) {
            std::printf("[sakura] f=%llu stage0 cap hit; giving up (overlay=%d)\n",
                        (unsigned long long)s->frames, overlay_playing ? 1 : 0);
            s->quit = true;
            return true;
        }
        // Title already up without any click (a primed save root)?
        bool sakura_up = false;
        for (const auto& [id, ch] : vst.video_layers) {
            if (ch.file.find("sakura") != std::string::npos && ch.playing) sakura_up = true;
        }
        if (sakura_up) {
            j.title_f = s->frames;
            j.sub = 1;
            j.sub_f = 0;
            std::printf("[sakura] f=%llu title already up (no click needed)\n",
                        (unsigned long long)s->frames);
            return false;
        }
        if (overlay_playing || rt->transition().is_in_progress(rt->now_ms()))
            return false; // wait for the stage to settle
        if (j.clicks < 4 && j.sub_f > 120) {
            // Only click when the snll language button (id 600.1.0 = btn01
            // ja, geometry 716,364 488x118) is under the point.
            bool under_lang = false;
            for (const oa::render::Layer* l : rt->scene().draw_order()) {
                if (l->id != "600.1.0" && l->id != "600.3.0" && l->id != "600.4.0")
                    continue;
                if (!rt->scene().find_event_handler(l->id, "click")) continue;
                if (const auto r = s->oaRender->layer_world_rect(*l)) {
                    if (960 >= (*r)[0] && 960 < (*r)[0] + (*r)[2] &&
                        423 >= (*r)[1] && 423 < (*r)[1] + (*r)[3])
                        under_lang = true;
                }
            }
            if (j.sub_f - j.lang_enum_f > 1200) {
                j.lang_enum_f = j.sub_f;
                std::printf("[sakura] f=%llu wait_kind=%d lang-btn-under=%d "
                            "clickables:\n",
                            (unsigned long long)s->frames,
                            rt->current_wait() ? int(rt->current_wait()->kind) : -1,
                            under_lang ? 1 : 0);
                int n = 0;
                for (const oa::render::Layer* l : rt->scene().draw_order()) {
                    if (!rt->scene().find_event_handler(l->id, "click")) continue;
                    if (const auto r = s->oaRender->layer_world_rect(*l)) {
                        if (n++ < 40)
                            std::printf("[sakura]   clickable %-24s rect=(%.0f,%.0f "
                                        "%.0fx%.0f)%s\n",
                                        l->id.c_str(), (*r)[0], (*r)[1], (*r)[2],
                                        (*r)[3], under_lang ? "  <-- under (960,423)" : "");
                    }
                }
            }
            if (under_lang) {
                s->ad_cx = 960;
                s->ad_cy = 423;
                s->ad_click = true;
                ++j.clicks;
                j.sub_f = 0;
                std::printf("[sakura] f=%llu click #%llu at (960,423)\n",
                            (unsigned long long)s->frames,
                            (unsigned long long)j.clicks);
            }
        }
        return false;
    }

    if (j.sub == 1) {
        // Park on the title: wait for the sakura layer channel with its mask
        // partner and for the host texture to be bound+uploaded.
        bool sakura_up = false;
        std::string sakura_id;
        for (const auto& [id, ch] : vst.video_layers) {
            if (ch.file.find("sakura") != std::string::npos && ch.playing) {
                sakura_up = true;
                sakura_id = id;
            }
        }
        if (!sakura_up) {
            if (j.sub_f > 60 * 60) {
                std::printf("[sakura] f=%llu stage1 cap hit: sakura channel "
                            "never appeared\n",
                            (unsigned long long)s->frames);
                s->quit = true;
                return true;
            }
            // A later click target may have appeared; allow one more click.
            if (j.clicks < 4 && j.sub_f > 300) {
                j.sub = 0;
                j.sub_f = 0;
            }
            return false;
        }
        const auto& ch = vst.video_layers.at(sakura_id);
        std::printf("[sakura] f=%llu title sakura channel id='%s' file='%s' "
                    "playing=%d decoded=%d mask_on=%d loop=%d pos=%llums\n",
                    (unsigned long long)s->frames, sakura_id.c_str(),
                    ch.file.c_str(), ch.playing ? 1 : 0, ch.decoded ? 1 : 0,
                    ch.mask_on ? 1 : 0, ch.loop_play ? 1 : 0,
                    (unsigned long long)ch.position_ms);
        const oa::render::TextureKey tkey =
            oa::render::VideoContent::frame_key(sakura_id);
        std::printf("[sakura] f=%llu host texture uploaded=%d; bound layers:\n",
                    (unsigned long long)s->frames,
                    s->oaRender->texture_uploaded(tkey) ? 1 : 0);
        for (const oa::render::Layer* l : rt->scene().draw_order()) {
            // 绑定判定 = 层的内容来源状态(视频帧域),不再比对 file 拼写。
            if (l->id != sakura_id ||
                oa::render::kind_of(*l) != oa::render::LayerKind::Video)
                continue;
            std::printf("[sakura]   bound layer %-24s vis=%d alpha=%.2f "
                        "file='%s'\n",
                        l->id.c_str(), l->visible ? 1 : 0, l->alpha,
                        l->file.c_str());
        }
        if (!ch.mask_on) {
            std::printf("[sakura] f=%llu FAIL: sakura channel not masked\n",
                        (unsigned long long)s->frames);
            s->quit = true;
            return true;
        }
        j.title_f = s->frames;
        j.sub = 2;
        j.sub_f = 0;
        return false;
    }

    if (j.sub == 2) {
        // Sampling window (~12 s at 60 fps): engine composite-frame stats
        // every 30 frames, a snapshot every 90 frames; consecutive-snapshot
        // diff = motion census (petal pixels + revealed art).
        const uint64_t window = 60 * 12;
        if (j.sub_f > window) {
            const double nshot = double(j.shots ? j.shots - 1 : 1);
            std::printf("[sakura] ---- motion census over %llu shot pairs: "
                        "changed=%llu white=%llu (%.1f%%) warm=%llu (%.1f%%) "
                        "pink=%llu (%.1f%%) other=%llu (%.1f%%)\n",
                        (unsigned long long)(j.shots ? j.shots - 1 : 0),
                        (unsigned long long)j.changed, (unsigned long long)j.m_white,
                        nshot ? 100.0 * double(j.m_white) / double(j.changed) : 0.0,
                        (unsigned long long)j.m_warm,
                        nshot ? 100.0 * double(j.m_warm) / double(j.changed) : 0.0,
                        (unsigned long long)j.m_pink,
                        nshot ? 100.0 * double(j.m_pink) / double(j.changed) : 0.0,
                        (unsigned long long)j.m_other,
                        nshot ? 100.0 * double(j.m_other) / double(j.changed) : 0.0);
            std::printf("[sakura] journey done; shots=%llu\n",
                        (unsigned long long)j.shots);
            s->quit = true;
            return true;
        }
        // Engine-level composite stats on the live channel.
        if (j.sub_f >= j.engine_stats_f + 30) {
            j.engine_stats_f = j.sub_f;
            for (const auto& [id, ch] : vst.video_layers) {
                if (ch.file.find("sakura") == std::string::npos) continue;
                int w = 0, h = 0;
                const uint8_t* px = nullptr;
                uint64_t rev = 0;
                if (ve.video_frame(id, &w, &h, &px, &rev) && px) {
                    uint64_t core = 0, vis = 0, ring = 0;
                    for (uint64_t i = 0; i < uint64_t(w) * uint64_t(h); ++i) {
                        const uint8_t* p = px + i * 4;
                        const int lum = (int(p[0]) * 77 + int(p[1]) * 150 +
                                         int(p[2]) * 29 + 128) >> 8;
                        if (p[3] >= 200) ++core;
                        if (p[3] >= 64) ++vis;
                        if (lum >= 150 && p[3] <= 40) ++ring;
                    }
                    std::printf("[sakura] engine f=%llu rev=%llu core=%llu "
                                "vis=%llu ring_white=%llu (pos=%llums)\n",
                                (unsigned long long)s->frames,
                                (unsigned long long)rev, (unsigned long long)core,
                                (unsigned long long)vis, (unsigned long long)ring,
                                (unsigned long long)ch.position_ms);
                }
            }
        }
        if (j.sub_f >= j.last_cap_f + 90) {
            j.last_cap_f = j.sub_f;
            oa::media::Image img;
            if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0)
                return false;
            if (!s->ad_out.empty()) {
                const std::vector<uint8_t> png = oa::runtime::encode_png(
                    uint32_t(img.w), uint32_t(img.h), img.rgba);
                if (!png.empty()) {
                    char path[512];
                    std::snprintf(path, sizeof(path), "%s/sakura_t_%02llu.png",
                                  s->ad_out.c_str(),
                                  (unsigned long long)j.shots);
                    FILE* f = std::fopen(path, "wb");
                    if (f) {
                        std::fwrite(png.data(), 1, png.size(), f);
                        std::fclose(f);
                    }
                }
            }
            // Motion census vs the previous snapshot (every 3rd pixel).
            if (!j.last.empty() && j.last.size() == img.rgba.size() && j.shots > 0) {
                for (uint64_t i = 0; i < img.rgba.size() / 4; i += 3) {
                    const uint8_t* a = &j.last[i * 4];
                    const uint8_t* b = &img.rgba[i * 4];
                    const int dr = a[0] > b[0] ? a[0] - b[0] : b[0] - a[0];
                    const int dg = a[1] > b[1] ? a[1] - b[1] : b[1] - a[1];
                    const int db = a[2] > b[2] ? a[2] - b[2] : b[2] - a[2];
                    if (dr <= 12 && dg <= 12 && db <= 12) continue;
                    ++j.changed;
                    const int r = b[0], g = b[1], bl = b[2];
                    if (r >= 235 && g >= 235 && bl >= 220 && (r - g) <= 12)
                        ++j.m_white;
                    else if (r > 120 && (r - g) >= 24 && (r - bl) >= 24 && bl >= g - 12)
                        ++j.m_pink;
                    else if (r > 90 && g > 90 && bl > 90) {
                        const int lum = (r * 77 + g * 150 + bl * 29) >> 8;
                        if (lum >= 150)
                            ++j.m_warm;
                        else
                            ++j.m_other;
                    } else {
                        ++j.m_other;
                    }
                }
            }
            j.last = img.rgba;
            j.lw = uint32_t(img.w);
            j.lh = uint32_t(img.h);
            ++j.shots;
            std::printf("[sakura] shot %llu at f=%llu %dx%d (changed so far=%llu)\n",
                        (unsigned long long)j.shots, (unsigned long long)s->frames,
                        img.w, img.h, (unsigned long long)j.changed);
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// OA_AUTODRIVE=sn101mont — snll (サクラノ詩) prologue recap-montage defect
// journey (diagnostics only): boot -> language (cn 600.3.0,
// center (960,580) per the language-button geometry) -> title bt_start ->
// walk the opening pages ("暗転。"..."《总集篇》") -> the auto recap montage (block
// 0003_00005 of script/c20_01a.ast: 回想_白枠 CG lv700 + ノイズa movie lv610
// + ~60 hard bg/fg cuts each followed by extrans(500) + シャッター夜_縦 wipe).
// During the montage it logs per-image bg-signature changes with the engine
// clock (rhythm), the BGM/SE bus state (bgm34 -> stop -> bgm40, se809), the
// layer-video channel state (ノイズa strip: playing/decoded/mask_on), scene
// bindings for the strip, and mean-luma of sampled shots (the bright-strip
// baseline ~160 when the strip is invisible). After the montage parks on the
// letter page it dumps clickable handlers and probes click-advance
// liveness (input dead after montage?), then walks the letter phase pages
// recording per-park transition activity. Requires OA_AD_VISIBLE=1 + Xvfb;
// OA_UI_OUT dir receives shots. report-only (no ctest).
// ---------------------------------------------------------------------------
static std::string sn101_text_sample(const oa::runtime::GameRuntime* rt) {
    for (const std::string& id : rt->text().visible_content_layers()) {
        if (id.find(".mw.") == std::string::npos) continue;
        const oa::render::MessageLayer* ml = rt->text().layer(id);
        if (!ml) continue;
        for (const auto& u : ml->page) {
            if (u.kind != oa::render::PageUnit::Kind::Newline && !u.data.empty())
                return u.data.substr(0, 14);
        }
    }
    return std::string();
}

static void sn101_clickables(AppState* s, const char* tag) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    std::printf("[sn101] %s clickables (f=%llu):\n", tag,
                (unsigned long long)s->frames);
    std::map<std::string, std::string> seen;
    int n = 0;
    for (const oa::render::Layer* l : rt->scene().draw_order()) {
        const auto* h = rt->scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        const std::string key = k == h->params.end() ? std::string() : k->second;
        std::string rect;
        if (const auto r = s->oaRender->layer_world_rect(*l)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "(%.0f,%.0f %.0fx%.0f)", (*r)[0],
                          (*r)[1], (*r)[2], (*r)[3]);
            rect = buf;
        }
        if (!key.empty() && seen.count(key)) continue;
        if (!key.empty()) seen[key] = l->id;
        std::printf("[sn101]   clickable id=%-28s key='%s' rect=%s vis=%d\n",
                    l->id.c_str(), key.c_str(), rect.c_str(), l->visible);
        if (++n > 60) break;
    }
}

static void sn101_audio(const oa::runtime::GameRuntime* rt, const char* tag) {
    const auto st = rt->audio().state();
    if (st.bgm_channel) {
        const auto& b = *st.bgm_channel;
        std::printf("[sn101] %s audio bgm file='%s' playing=%d loop=%d "
                    "gain=%d fade=%s started=%llu clock=%llu\n",
                    tag, b.file.c_str(), b.playing ? 1 : 0, b.loop_play ? 1 : 0,
                    b.raw_gain, b.fade ? "Y" : "-",
                    (unsigned long long)b.started_at_ms,
                    (unsigned long long)st.clock_ms);
    } else {
        std::printf("[sn101] %s audio bgm: (none)\n", tag);
    }
    int n = 0;
    for (const auto& [id, ch] : st.se_channels) {
        if (!ch.playing) continue;
        if (n++ < 8)
            std::printf("[sn101] %s audio se id='%s' file='%s' loop=%d\n", tag,
                        id.c_str(), ch.file.c_str(), ch.loop_play ? 1 : 0);
    }
    if (!n) std::printf("[sn101] %s audio se: none playing\n", tag);
}

static void sn101_audio2(oa::runtime::GameRuntime* rt, const char* tag) {
    // host decode-pump view: how many players the host keeps for the engine
    // channels (a playing BGM engine channel with 0 players = no audio at
    // all even though the logic state says playing).
    std::printf("[sn101] %s host audio players=%zu bgm_playing=%d\n", tag,
                rt->media_players().active_players(),
                rt->audio().is_bgm_playing() ? 1 : 0);
}

static void sn101_video(const oa::runtime::GameRuntime* rt, const char* tag) {
    const auto st = rt->video().state();
    int n = 0;
    for (const auto& [id, ch] : st.video_layers) {
        if (!ch.playing && n > 0) continue;
        // 绑定判定 = 层的内容来源状态(视频帧域),不再比对
        // 保留命名空间 file 串。
        bool bound = false;
        for (const oa::render::Layer* l : rt->scene().draw_order()) {
            if (l->id == id && l->visible &&
                oa::render::kind_of(*l) == oa::render::LayerKind::Video)
                bound = true;
        }
        std::printf("[sn101] %s video id='%s' file='%s' playing=%d decoded=%d "
                    "loop=%d mask_on=%d bound=%d pos=%llums\n",
                    tag, id.c_str(), ch.file.c_str(), ch.playing ? 1 : 0,
                    ch.decoded ? 1 : 0, ch.loop_play ? 1 : 0, ch.mask_on ? 1 : 0,
                    bound ? 1 : 0, (unsigned long long)ch.position_ms);
        if (++n >= 10) break;
    }
    if (!n) std::printf("[sn101] %s video: none\n", tag);
}

/// lv0 story bg signature: files of .bg. layers under 1.0.bx.by.bs (the ADV
/// image tree; montage rows rewrite them). Empty when no bg subtree exists.
static std::string sn101_bg_sig(const oa::runtime::GameRuntime* rt) {
    std::string out;
    for (const oa::render::Layer* l : rt->scene().draw_order()) {
        if (l->id.rfind("1.0.bx.by.bs.", 0) != 0) continue;
        if (l->id.find(".bg.") == std::string::npos) continue;
        if (l->file.empty()) continue;
        if (!out.empty()) out += "|";
        out += l->id + "=" + l->file;
    }
    return out;
}

static bool sn101_shot(AppState* s, const char* tag, double* luma_out) {
    oa::media::Image img;
    if (!s->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0)
        return false;
    const size_t n = size_t(img.w) * size_t(img.h);
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &img.rgba[i * 4];
        sum += (double(p[0]) + p[1] + p[2]) / 3.0;
    }
    if (luma_out) *luma_out = n ? sum / double(n) : 0.0;
    if (!s->ad_out.empty()) {
        static uint64_t sn_seq = 0;
        const std::vector<uint8_t> png = oa::runtime::encode_png(
            uint32_t(img.w), uint32_t(img.h), img.rgba);
        if (!png.empty()) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/sn101_%s_%03llu.png",
                          s->ad_out.c_str(), tag, (unsigned long long)sn_seq);
            ++sn_seq;
            FILE* f = std::fopen(path, "wb");
            if (f) {
                std::fwrite(png.data(), 1, png.size(), f);
                std::fclose(f);
            }
        }
    }
    return true;
}

static bool ad_sn101mont_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    struct J {
        int sub = 0; // 0 lang / 1 title / 2 pre-montage walk / 3 montage /
                     // 4 post-montage letter diag / 5 letter walk / 6 done
        uint64_t sub_f = 0;
        int lang_clicks = 0;
        int pre_parks = 0;
        std::string pre_last;
        uint64_t mont_f = 0, mont_ms = 0;
        int imgs = 0;
        std::string bg_last;
        uint64_t bg_last_ms = 0, bg_last_f = 0;
        double luma_acc = 0;
        int luma_n = 0;
        uint64_t shot_f = 0;
        std::string bgm_last;
        int bgm_changes = 0;
        uint64_t audio_f = 0, video_f = 0;
        int post_parks = 0;
        std::string post_last;
        int diag_phase = 0;
        uint64_t diag_f = 0;
        int clicks_after = 0;
        uint64_t diag_click_f = 0;
        bool bgm34_seen = false, bgm40_seen = false, se809_seen = false;
        int stuck = 0;
    };
    static J j;
    ++j.sub_f;
    const oa::runtime::WaitReason* w = rt->current_wait();
    const int wk = w ? int(w->kind) : -1;
    const auto vst_now = rt->video().state();

    // ---------------- sub 0: language page -> (cn 600.3.0 at (960,580)) ----
    if (j.sub == 0) {
        if (ad_has_handler(rt, "bt_start") || !sn101_text_sample(rt).empty()) {
            std::printf("[sn101] f=%llu lang phase done (bt_start=%d text='%s')\n",
                        (unsigned long long)s->frames,
                        ad_has_handler(rt, "bt_start") ? 1 : 0,
                        sn101_text_sample(rt).c_str());
            j.sub = 1;
            j.sub_f = 0;
            return false;
        }
        if (j.sub_f % 600 == 60) sn101_clickables(s, "lang");
        const bool overlay_playing = vst_now.overlay_video && vst_now.overlay_video->playing;
        if (overlay_playing || rt->transition().is_in_progress(rt->now_ms()))
            return false;
        if (j.sub_f > 240 && j.lang_clicks < 8) {
            s->ad_cx = 960;
            s->ad_cy = 580;
            s->ad_click = true;
            ++j.lang_clicks;
            j.sub_f = 0;
            std::printf("[sn101] f=%llu lang click #%d (960,580)\n",
                        (unsigned long long)s->frames, j.lang_clicks);
        } else if (j.lang_clicks >= 8 && j.sub_f > 600) {
            std::printf("[sn101] lang clicks exhausted; sub0 abort\n");
            s->quit = true;
        }
        return false;
    }

    // ---------------- sub 1: title -> bt_start -> first park ----------------
    if (j.sub == 1) {
        if (!sn101_text_sample(rt).empty()) {
            std::printf("[sn101] f=%llu title done; first park text='%s'\n",
                        (unsigned long long)s->frames,
                        sn101_text_sample(rt).c_str());
            j.sub = 2;
            j.sub_f = 0;
            return false;
        }
        if (j.sub_f % 600 == 60) sn101_clickables(s, "title");
        if (j.sub_f > 200 && j.lang_clicks < 8 && ad_has_handler(rt, "bt_start")) {
            ad_arm_click_key(s, "bt_start");
            ++j.lang_clicks;
            j.sub_f = 0;
            std::printf("[sn101] f=%llu click bt_start #%d\n",
                        (unsigned long long)s->frames, j.lang_clicks);
        } else if (j.sub_f % 600 == 250 && j.lang_clicks < 8) {
            // fallback: any clickable whose key contains "start"
            for (const oa::render::Layer* l : rt->scene().draw_order()) {
                const auto* h = rt->scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto k = h->params.find("key");
                if (k == h->params.end() ||
                    k->second.find("start") == std::string::npos)
                    continue;
                if (const auto r = s->oaRender->layer_world_rect(*l)) {
                    s->ad_cx = int((*r)[0] + (*r)[2] / 2);
                    s->ad_cy = int((*r)[1] + (*r)[3] / 2);
                    s->ad_click = true;
                    ++j.lang_clicks;
                    j.sub_f = 0;
                    std::printf("[sn101] f=%llu fallback click key='%s'\n",
                                (unsigned long long)s->frames, k->second.c_str());
                }
                break;
            }
        }
        if (j.sub_f > 60 * 60) {
            std::printf("[sn101] title cap hit; abort\n");
            s->quit = true;
        }
        return false;
    }

    // -------- sub 2: pre-montage page walk (until the montage starts) ------
    if (j.sub == 2) {
        const bool parked = w && (wk == int(oa::runtime::WaitReason::Kind::Generic) ||
                                  wk == int(oa::runtime::WaitReason::Kind::Generic0));
        const bool ready = bt91_story_text_ready(rt);
        bool in_montage = false;
        for (const oa::render::Layer* l : rt->scene().draw_order()) {
            if (l->file.find("回想") != std::string::npos) in_montage = true;
        }
        for (const auto& [id, ch] : vst_now.video_layers) {
            if (ch.file.find("ノイズ") != std::string::npos && ch.playing)
                in_montage = true;
        }
        if (in_montage) {
            std::printf("[sn101] f=%llu MONTAGE START (pre parks=%d last='%s')\n",
                        (unsigned long long)s->frames, j.pre_parks,
                        j.pre_last.c_str());
            j.sub = 3;
            j.sub_f = 0;
            j.mont_f = s->frames;
            j.mont_ms = rt->now_ms();
            j.bg_last.clear();
            sn101_audio(rt, "mont-start");
            sn101_audio2(rt, "mont-start");
            sn101_video(rt, "mont-start");
            return false;
        }
        if (parked && ready) {
            const std::string smp = sn101_text_sample(rt);
            if (smp != j.pre_last) {
                j.pre_last = smp;
                ++j.pre_parks;
                std::printf("[sn101] f=%llu PRE park %d wait_kind=%d text='%s'\n",
                            (unsigned long long)s->frames, j.pre_parks, wk,
                            smp.c_str());
                if (j.pre_parks == 1) {
                    sn101_clickables(s, "pre1");
                    sn101_audio(rt, "pre1");
                }
            }
            if (j.sub_f > 240) {
                const int cx = rt->project_.config.stage_width / 2;
                const int cy = rt->project_.config.stage_height - 150;
                s->input.mouse_x = cx;
                s->input.mouse_y = cy;
                s->ad_click = true;
                j.sub_f = 0;
                std::printf("[sn101] f=%llu pre advance click (park %d)\n",
                            (unsigned long long)s->frames, j.pre_parks);
            }
        }
        if (j.pre_parks > 30 && j.sub_f > 600) {
            std::printf("[sn101] pre walk too long w/o montage; abort\n");
            s->quit = true;
        }
        return false;
    }

    // -------------------- sub 3: the montage itself ------------------------
    if (j.sub == 3) {
        const std::string smp = sn101_text_sample(rt);
        if (!smp.empty() && smp != "《总集篇》" && j.imgs > 0) {
            std::printf("[sn101] f=%llu MONTAGE END imgs=%d luma_avg=%.1f "
                        "(n=%d) text='%s'\n",
                        (unsigned long long)s->frames, j.imgs,
                        j.luma_n ? j.luma_acc / j.luma_n : 0.0, j.luma_n,
                        smp.c_str());
            sn101_audio(rt, "mont-end");
            sn101_audio2(rt, "mont-end");
            sn101_video(rt, "mont-end");
            j.sub = 4;
            j.sub_f = 0;
            j.diag_phase = 0;
            return false;
        }
        if (j.sub_f > 60 * 90) {
            std::printf("[sn101] montage cap hit imgs=%d luma_avg=%.1f\n",
                        j.imgs, j.luma_n ? j.luma_acc / j.luma_n : 0.0);
            s->quit = true;
            return false;
        }
        const std::string sig = sn101_bg_sig(rt);
        if (sig != j.bg_last && !sig.empty()) {
            const uint64_t nowms = rt->now_ms();
            if (j.imgs > 0) {
                std::printf("[sn101] f=%llu image %3d at t=%llums dwell=%llums "
                            "wk=%d wait='%s'\n",
                            (unsigned long long)s->frames, j.imgs,
                            (unsigned long long)nowms,
                            (unsigned long long)(nowms - j.bg_last_ms), wk,
                            w ? w->id.c_str() : "");
            } else {
                std::printf("[sn101] f=%llu image 0 at t=%llums wk=%d wait='%s'\n",
                            (unsigned long long)s->frames, (unsigned long long)nowms,
                            wk, w ? w->id.c_str() : "");
            }
            j.bg_last = sig;
            j.bg_last_ms = nowms;
            ++j.imgs;
            j.bg_last_f = s->frames;
        }
        {
            const auto st = rt->audio().state();
            std::string bgmfile;
            if (st.bgm_channel) bgmfile = st.bgm_channel->file;
            if (bgmfile != j.bgm_last) {
                if (!j.bgm_last.empty() || !bgmfile.empty()) {
                    std::printf("[sn101] f=%llu bgm file '%s' -> '%s' (playing=%d)\n",
                                (unsigned long long)s->frames, j.bgm_last.c_str(),
                                bgmfile.c_str(),
                                st.bgm_channel && st.bgm_channel->playing ? 1 : 0);
                    if (bgmfile.find("bgm34") != std::string::npos) j.bgm34_seen = true;
                    if (bgmfile.find("bgm40") != std::string::npos) j.bgm40_seen = true;
                }
                j.bgm_last = bgmfile;
                ++j.bgm_changes;
            }
            for (const auto& [id, ch] : st.se_channels) {
                if (ch.file.find("se809") != std::string::npos && ch.playing)
                    j.se809_seen = true;
            }
        }
        // wait-kind trace during the montage (per-image wait skeleton);
        // nominal duration printed where the wait machine knows one.
        {
            static int twk = -999;
            static std::string twid;
            if (wk != twk || (w ? w->id : std::string()) != twid) {
                twk = wk;
                twid = w ? w->id : std::string();
                uint64_t dur = 0;
                if (w && w->kind == oa::runtime::WaitReason::Kind::Timed)
                    dur = w->milliseconds;
                std::printf("[sn101] f=%llu wait-> %d id='%s' dur=%llums\n",
                            (unsigned long long)s->frames, wk, twid.c_str(),
                            (unsigned long long)dur);
            }
        }
        if (j.sub_f >= j.video_f + 60 * 10) {
            j.video_f = j.sub_f;
            sn101_video(rt, "mont-v");
            sn101_audio2(rt, "mont-v");
        }
        if (j.sub_f >= j.shot_f + 60) {
            j.shot_f = j.sub_f;
            double luma = 0;
            if (j.luma_n % 8 == 0)
                sn101_shot(s, "mont", &luma);
            else {
                oa::media::Image img;
                if (s->oaRender->snapshot_renderer(img) && img.w > 0) {
                    const size_t n = size_t(img.w) * size_t(img.h);
                    double sum = 0;
                    for (size_t i = 0; i < n; ++i) {
                        const uint8_t* p = &img.rgba[i * 4];
                        sum += (double(p[0]) + p[1] + p[2]) / 3.0;
                    }
                    luma = n ? sum / double(n) : 0.0;
                }
            }
            if (luma > 0) {
                j.luma_acc += luma;
                ++j.luma_n;
            }
        }
        return false;
    }

    // ------------- sub 4: letter page input-liveness diagnosis -------------
    if (j.sub == 4) {
        if (j.diag_phase == 0) {
            std::printf("[sn101] f=%llu POST park text='%s' wait_kind=%d "
                        "wait_id='%s'\n",
                        (unsigned long long)s->frames, sn101_text_sample(rt).c_str(),
                        wk, w ? w->id.c_str() : "");
            sn101_clickables(s, "post");
            sn101_audio(rt, "post");
            ++j.post_parks;
            j.post_last = sn101_text_sample(rt);
            j.diag_phase = 1;
            j.diag_f = j.sub_f;
            return false;
        }
        // probe clicks (2 s apart); advance to the next park is success
        const std::string smp = sn101_text_sample(rt);
        if (smp != j.post_last && j.post_parks >= 1) {
            j.post_last = smp;
            ++j.post_parks;
            std::printf("[sn101] f=%llu POST advance OK -> park %d text='%s'\n",
                        (unsigned long long)s->frames, j.post_parks, smp.c_str());
            j.diag_f = j.sub_f;
            j.clicks_after = 0;
            if (j.post_parks >= 3) {
                std::printf("[sn101] journey DONE post_parks=%d f=%llu "
                            "(bgm34=%d bgm40=%d se809=%d)\n",
                            j.post_parks, (unsigned long long)s->frames,
                            j.bgm34_seen ? 1 : 0, j.bgm40_seen ? 1 : 0,
                            j.se809_seen ? 1 : 0);
                s->quit = true;
            }
            return false;
        }
        if (j.clicks_after < 3 && j.sub_f - j.diag_click_f > 90) {
            j.diag_click_f = j.sub_f;
            const int cx = rt->project_.config.stage_width / 2;
            const int cy = rt->project_.config.stage_height - 150;
            s->input.mouse_x = cx;
            s->input.mouse_y = cy;
            s->ad_click = true;
            ++j.clicks_after;
            std::printf("[sn101] f=%llu POST probe click #%d\n",
                        (unsigned long long)s->frames, j.clicks_after);
        } else if (j.clicks_after >= 3 && j.sub_f - j.diag_click_f > 240) {
            std::printf("[sn101] WARN: no advance after 3 probe clicks "
                        "(post parks=%d f=%llu); continue walking\n",
                        j.post_parks, (unsigned long long)s->frames);
            sn101_clickables(s, "post-stuck");
            j.sub = 5;
            j.sub_f = 0;
            return false;
        }
        if (j.sub_f > 60 * 30) {
            std::printf("[sn101] post diag cap; quit\n");
            s->quit = true;
        }
        return false;
    }

    // ------- sub 5: letter-phase walk w/ per-park transition dumps ---------
    {
        const bool parked = w &&
                            (wk == int(oa::runtime::WaitReason::Kind::Generic) ||
                             wk == int(oa::runtime::WaitReason::Kind::Generic0));
        const bool ready = bt91_story_text_ready(rt);
        if (parked && ready) {
            const std::string smp = sn101_text_sample(rt);
            if (smp != j.post_last) {
                j.post_last = smp;
                ++j.post_parks;
                std::printf("[sn101] f=%llu LETTER park %d text='%s'\n",
                            (unsigned long long)s->frames, j.post_parks,
                            smp.c_str());
            }
            if (j.post_parks > 20) {
                std::printf("[sn101] journey DONE post_parks=%d f=%llu\n",
                            j.post_parks, (unsigned long long)s->frames);
                s->quit = true;
                return false;
            }
            if (j.sub_f > 150) {
                const int cx = rt->project_.config.stage_width / 2;
                const int cy = rt->project_.config.stage_height - 150;
                s->input.mouse_x = cx;
                s->input.mouse_y = cy;
                s->ad_click = true;
                j.sub_f = 0;
                if (j.post_parks == 10) sn101_clickables(s, "letter-mid");
            }
        }
        if (j.sub_f > 60 * 60) {
            std::printf("[sn101] letter walk cap; quit\n");
            s->quit = true;
        }
        return false;
    }
}

// ---------------------------------------------------------------------------
// 'fontscan' (phenomenon J: cross-title story-text edge census). Works on any
// iMel-family project: optional first-boot language page (bt_cn/bt_cn_next),
// title park with bt_start, click it, then the first fully parked story page.
// Dumps every visible content text layer (id/bbox + font raw keys — the
// outline/outlinecolor/shadow/style面 the renderer consumes) and writes a PNG
// of the parked frame under OA_UI_OUT for offline glyph-edge sampling.
// ---------------------------------------------------------------------------
static void ad_fontscan_dump(AppState* s, oa::runtime::GameRuntime* rt,
                             const char* tag) {
    std::printf("[fontscan] %s f=%llu wait=%s layers=%zu\n", tag,
                (unsigned long long)s->frames,
                wait_desc(rt->current_wait()).c_str(), rt->scene().size());
    size_t n = 0;
    for (const std::string& id : rt->text().visible_content_layers()) {
        const oa::render::MessageLayer* ml = rt->text().layer(id);
        if (!ml || ml->page.empty() || ml->char_count == 0) continue;
        std::string sample;
        for (const auto& u : ml->page)
            if (u.kind != oa::render::PageUnit::Kind::Newline && !u.data.empty()) {
                sample = u.data.substr(0, 20);
                break;
            }
        std::printf("[fontscan]  layer %s chars=%zu reveal=%zu/%zu pending=%d "
                    "visible=%d size=%.1f box=(%.0f,%.0f %.0fx%.0f) sample='%s'\n",
                    id.c_str(), ml->char_count, ml->reveal_index, ml->char_count,
                    ml->reveal_pending ? 1 : 0,
                    rt->scene().is_message_layer_visible(id) ? 1 : 0,
                    ml->font.size(), ml->left, ml->top, ml->width, ml->height,
                    sample.c_str());
        ++n;
        if (n > 8) break;
    }
    // Font raw keys of the story body layer(s) — one line per distinct raw set.
    std::string seen;
    size_t printed = 0;
    for (const std::string& id : rt->text().visible_content_layers()) {
        const oa::render::MessageLayer* ml = rt->text().layer(id);
        if (!ml || ml->page.empty() || ml->char_count == 0) continue;
        std::string raw;
        for (const auto& [k, v] : ml->font.raw) {
            if (!raw.empty()) raw += " ";
            raw += k + "=" + v;
        }
        if (raw.empty() || raw == seen) continue;
        seen = raw;
        std::printf("[fontscan]  raw[%s]: %s\n", id.c_str(), raw.c_str());
        if (++printed >= 6) break;
    }
    if (!s->ad_out.empty()) ad_png_shot(s, tag);
}
/// First click-handler layer matching `key` (or, when `key` is empty, whose
/// texture name carries `file_sub`) whose *world rect center* lands inside the
/// stage. `ad_has_handler`/`ad_arm_click_key` search registered rows blindly:
/// the language page pre-registers every bt_*_next row (hidden), and animated
/// title menus sit at x<0 until they slide in, so a blind key search clicks
/// into nothing (rr: bt_start at -1158,135; fpm: a * at 1849,68).
static const oa::render::Layer* ad_find_clickable(const AppState* s,
    const char* key, const char* file_sub, bool need_file) {
    const int sw = s->rt->project_.config.stage_width;
    const int sh = s->rt->project_.config.stage_height;
    for (const oa::render::Layer* l : s->rt->scene().draw_order()) {
        const auto it = l->event_handlers.find("click");
        if (it == l->event_handlers.end()) continue;
        // the same gate hit_test_all applies before a layer can take an event
        // (own visible + every ancestor): the language page's *_next rows are
        // registered from the start but hidden until a language is picked.
        if (!s->rt->scene().is_effectively_visible(l->id)) continue;
        const auto k = it->second.params.find("key");
        if (k == it->second.params.end()) continue;
        if (need_file) {
            std::string f = l->file + " " + l->path;
            std::transform(f.begin(), f.end(), f.begin(), ::tolower);
            if (f.find(file_sub) == std::string::npos) continue;
            if (k->second.find("_next") != std::string::npos) continue;
        } else if (k->second != key) {
            continue;
        }
        const auto r = s->oaRender->layer_world_rect(*l);
        if (!r) continue;
        const double cx = (*r)[0] + (*r)[2] / 2, cy = (*r)[1] + (*r)[3] / 2;
        if (cx < 1 || cy < 1 || cx > sw - 1 || cy > sh - 1) continue;
        return l;
    }
    return nullptr;
}
/// Arm a click on the world center of `l` (same targeting ad_arm_click_key uses).
static void ad_click_layer(AppState* s, const oa::render::Layer* l) {
    if (!l) return;
    if (const auto r = s->oaRender->layer_world_rect(*l)) {
        s->ad_cx = int((*r)[0] + (*r)[2] / 2);
        s->ad_cy = int((*r)[1] + (*r)[3] / 2);
        s->ad_click = true;
    }
}
static bool ad_fontscan_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    ++s->ad_stage_f;
    static int sub = 0;        // 0 title/lang wait, 1 story wait, 2 dump+quit
    static int lang_f = 0;     // settle counter for the language page
    static int story_f = 0;    // stage frames while nothing is parked
    static int parked_f = 0;   // consecutive frames on a revealed story page
    static int pages_left = -1; // OA_FONTSCAN_PAGES: pages to walk first
    if (pages_left < 0) {
        const char* pv = std::getenv("OA_FONTSCAN_PAGES");
        pages_left = (pv && *pv) ? std::max(0, std::atoi(pv)) : 0;
    }
    const auto key_is = [](const char* want) {
        return [want](const oa::render::Layer&,
                      const oa::render::LayerEventHandler& h) {
            const auto k = h.params.find("key");
            return k != h.params.end() && k->second == want;
        };
    };
    const oa::render::Layer* start;
    if (sub == 0) {
        // First-boot language page (fresh save root). Two data shapes exist:
        // bt_cn/bt_cn_next (N-series title overlay) and btnNN over
        // :ui/game/lang/bt_cn.png (slny/tg3). Both are addressed by data, not
        // by project id: pick = the clickable whose key is bt_cn, else the one
        // whose texture is the CN language button; confirm = key bt_cn_next.
        if (++lang_f > 30) {
            const oa::render::Layer* pick = ad_find_clickable(s, "bt_cn", "", false);
            if (!pick) pick = ad_find_clickable(s, "", "bt_cn", true);
            const oa::render::Layer* confirm =
                ad_find_clickable(s, "bt_cn_next", "", false);
            const oa::render::Layer* sel = confirm ? confirm : pick;
            if (sel) {
                ad_click_layer(s, sel);
                std::printf("[fontscan] f=%llu lang click %s (%d,%d)\n",
                            (unsigned long long)s->frames,
                            confirm ? "confirm" : "pick-cn", s->ad_cx, s->ad_cy);
                lang_f = 0;
                return false;
            }
        }
        // Title: enter the story (bt_start).
        start = ad_find_clickable(s, "bt_start", "", false);
        if (start) {
            ad_click_layer(s, start);
            std::printf("[fontscan] f=%llu title click bt_start (%d,%d)\n",
                        (unsigned long long)s->frames, s->ad_cx, s->ad_cy);
            sub = 1;
            return false;
        }
        if (lang_f > 40000) {
            std::printf("[fontscan] title never reached; abort\n");
            s->quit = true;
        }
        return false;
    }
    if (sub == 1) {
        // A parked story body: any visible content text layer whose id sits in
        // the message window ("…mw…"), has text, and finished revealing. The
        // hard-coded 1.80.mw.adv_adv id of ad_story_parked only matches the
        // 1.80 message template, and the tg3/newer line ships another one.
        std::string parked_id;
        for (const std::string& id : rt->text().visible_content_layers()) {
            const oa::render::MessageLayer* ml = rt->text().layer(id);
            if (!ml || ml->page.empty() || ml->char_count == 0) continue;
            if (id.find("mw") == std::string::npos) continue;
            if (ml->reveal_pending || ml->reveal_index < ml->char_count) continue;
            bool has_text = false;
            for (const auto& u : ml->page)
                if (u.kind != oa::render::PageUnit::Kind::Newline && !u.data.empty()) {
                    has_text = true;
                    break;
                }
            if (!has_text) continue;
            parked_id = id;
            break;
        }
        if (!parked_id.empty()) {
            // settle on the page so the window fade/reveal is finished before
            // the frame is frozen (a mid-reveal capture has almost no ink).
            if (++parked_f > 60) {
                if (pages_left > 0) {
                    --pages_left;
                    parked_f = 0;
                    const int sw = rt->project_.config.stage_width;
                    const int sh = rt->project_.config.stage_height;
                    s->ad_cx = sw / 2;
                    s->ad_cy = int(sh * 0.87);
                    s->ad_click = true;
                    std::printf("[fontscan] f=%llu advance to page (left=%d)\n",
                                (unsigned long long)s->frames, pages_left);
                    return false;
                }
                std::printf("[fontscan] story body layer '%s'\n", parked_id.c_str());
                ad_fontscan_dump(s, rt, "story");
                sub = 2;
                s->quit = true;
                return false;
            }
        } else {
            parked_f = 0;
            // Nothing parked yet: advance the ADV page (click the message
            // window lower band, the standard click area) so prologue
            // clicks/choices do not stall the walk.
            if (++story_f % 90 == 0) {
                const int sw = rt->project_.config.stage_width;
                const int sh = rt->project_.config.stage_height;
                s->ad_cx = sw / 2;
                s->ad_cy = int(sh * 0.87);
                s->ad_click = true;
                std::printf("[fontscan] f=%llu advance click (%d,%d)\n",
                            (unsigned long long)s->frames, s->ad_cx, s->ad_cy);
            }
        }
        if (s->frames > 40000) {
            std::printf("[fontscan] story page never parked; abort\n");
            ad_fontscan_dump(s, rt, "story_timeout");
            s->quit = true;
        }
        return false;
    }
    s->quit = true;
    return false;
}

// ---------------------------------------------------------------------------
// OA_AUTODRIVE=langidle — first-boot LANGUAGE page dwell journey (research/131:
// "waiting a little on the language page without clicking freezes the game";
// investigated on ArStreamLoveVoyage — iMel/krkrsdl3 family, 1280x720,
// pc/<lang>/game/lang/*).
// Stage 0 waits for the language rows (the click row whose texture is bt_cn —
// the same data-addressed pick ad_fontscan_drive falls back to; ASLV's rows are
// key=btn01..btn04 over pc/xx/game/lang/bt_*.png), stage 1 idles
// OA_LANGIDLE_FRAMES frames (default 1800) and prints a bounded heartbeat every
// OA_LANGIDLE_BEAT frames (default 60): frame / wall ms / worst single-frame
// wall time / wait / layers / handler layers / live tweens / transition /
// stage luma + whole-frame hash + the scene/text/tween event counters — so a
// freeze is localised to one frame and "nothing advances any more" has numbers.
// Stage 2 clicks the CN row (what the user does after waiting) and reports
// whether the page actually LEFT (= the click still dispatches after the
// dwell); stage 3 walks title bt_start -> the first parked ADV page so the
// "still clickable after the dwell" claim ends in frames/layers numbers.
// Knobs: OA_LANGIDLE_FRAMES dwell frames (default 1800), OA_LANGIDLE_BEAT
// heartbeat interval (default 60), OA_LANGIDLE_WALK story walk click cap
// (default 200), OA_LANGIDLE_HOVER=1 parks the synthetic pointer on the CN row
// for the whole dwell + click (the real-machine case: the cursor rests on the
// page, so the framework's rollover chain runs while nothing is clicked).
// ---------------------------------------------------------------------------
/// One bounded heartbeat line of the langidle dwell: liveness (frame / wall
/// clock / worst single-frame time), engine step (wait), scene size, live
/// timers and the presented picture (luma + whole-frame hash).
static void ad_langidle_beat(AppState* s, const char* tag, const char* extra,
                             int left, uint64_t t0, uint64_t worst_ms) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    double luma = -1;
    uint64_t hash = 0;
    oa::media::Image img;
    if (s->oaRender->snapshot_renderer(img) && img.w > 0 && img.h > 0) {
        luma = oa::media::band_luma(img, 0, img.h);
        hash = ad_scale_hash(img);
    }
    size_t handler_layers = 0;
    for (const oa::render::Layer* l : rt->scene().draw_order())
        if (!l->event_handlers.empty()) ++handler_layers;
    std::printf("[langidle] %s f=%llu wall=%llums left=%d worst_frame=%llums "
                "wait=%s layers=%zu handlers=%zu tweens=%d trans=%d luma=%.1f "
                "hash=%016llx lev=%zu tev=%zu twn=%zu%s%s\n",
                tag, (unsigned long long)s->frames,
                (unsigned long long)(SDL_GetTicks() - t0), left,
                (unsigned long long)worst_ms,
                wait_desc(rt->current_wait()).c_str(), rt->scene().size(),
                handler_layers, (int)rt->scene().has_tweens(),
                (int)rt->transition().is_in_progress(rt->now_ms()), luma,
                (unsigned long long)hash, rt->scene_layer_events(),
                rt->text_events(), rt->tween_completion_calls(),
                (extra && *extra) ? " " : "", (extra && *extra) ? extra : "");
}
/// World centre of a layer, or false when it has no rect.
static bool ad_layer_center(AppState* s, const oa::render::Layer* l, int* cx,
                            int* cy) {
    if (!l) return false;
    const auto r = s->oaRender->layer_world_rect(*l);
    if (!r) return false;
    *cx = int((*r)[0] + (*r)[2] / 2);
    *cy = int((*r)[1] + (*r)[3] / 2);
    return true;
}
static bool ad_langidle_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    static int sub = 0;   // 0 page wait / 1 dwell / 2 post-click / 3 title / 4 walk
    static int dwell_left = -1, beat = 60, walk_cap = 200, settle = 0;
    static int hover = 0, walk_f = 0, parked_f = 0, adv = 0;
    static uint64_t t0 = 0, arrived = 0, last_f = 0, worst = 0, cap = 0;
    if (dwell_left < 0) {
        const char* v = std::getenv("OA_LANGIDLE_FRAMES");
        dwell_left = (v && *v) ? std::max(0, std::atoi(v)) : 1800;
        if ((v = std::getenv("OA_LANGIDLE_BEAT")) && *v)
            beat = std::max(1, std::atoi(v));
        if ((v = std::getenv("OA_LANGIDLE_WALK")) && *v)
            walk_cap = std::max(1, std::atoi(v));
        hover = (v = std::getenv("OA_LANGIDLE_HOVER")) && *v &&
                std::atoi(v) != 0;
        cap = uint64_t(600 + dwell_left + 1200 + walk_cap * 90 + 6000);
        t0 = SDL_GetTicks();
        last_f = t0;
        std::printf("[langidle] dwell=%d beat=%d walk_cap=%d hover=%d cap=%llu\n",
                    dwell_left, beat, walk_cap, hover, (unsigned long long)cap);
    }
    // Worst single-frame wall time inside the current beat window: a stall
    // inside tick/render/present shows up here (and in a missing heartbeat)
    // before the process would ever be killed by the outer timeout.
    const uint64_t now = SDL_GetTicks();
    if (now > last_f) {
        const uint64_t d = now - last_f;
        if (d > worst) worst = d;
    }
    last_f = now;
    if (s->frames > cap) {
        std::printf("[langidle] cap reached f=%llu sub=%d; abort\n",
                    (unsigned long long)s->frames, sub);
        std::printf("[langidle] VERDICT FAIL (cap; no story page)\n");
        std::exit(1);
    }
    const oa::render::Layer* cn = ad_find_clickable(s, "", "bt_cn", true);
    int cx = 0, cy = 0;
    const bool have = ad_layer_center(s, cn, &cx, &cy);
    if (sub == 0) {
        if (!cn || !have) return false;
        arrived = s->frames;
        t0 = SDL_GetTicks();
        last_f = t0;
        worst = 0;
        std::printf("[langidle] language page row '%s' at f=%llu (%d,%d)\n",
                    cn->id.c_str(), (unsigned long long)s->frames, cx, cy);
        sub = 1;
        return false;
    }
    if (sub == 1) {
        if (hover && have) {
            // the real-machine case: the cursor rests on the page, so the
            // framework's rollover chain (btn_over -> se/slider/uihelp) runs
            // while nothing is clicked.
            s->ad_cx = cx;
            s->ad_cy = cy;
            s->ad_hover = true;
        }
        if (dwell_left > 0) {
            --dwell_left;
            if ((s->frames - arrived) % uint64_t(beat) == 0) {
                char extra[160];
                std::snprintf(extra, sizeof(extra), "row=%s at=(%d,%d)",
                              cn ? cn->id.c_str() : "-", s->ad_cx, s->ad_cy);
                ad_langidle_beat(s, "dwell", extra, dwell_left, t0, worst);
                worst = 0;
            }
            return false;
        }
        if (!cn || !have) {
            std::printf("[langidle] DEFECT: the language row vanished during "
                        "the dwell (f=%llu)\n",
                        (unsigned long long)s->frames);
            std::printf("[langidle] VERDICT FAIL (row gone during dwell)\n");
            std::exit(1);
        }
        ad_click_layer(s, cn);
        std::printf("[langidle] dwell done f=%llu wall=%llums; click cn row "
                    "'%s' (%d,%d)\n",
                    (unsigned long long)s->frames,
                    (unsigned long long)(SDL_GetTicks() - t0), cn->id.c_str(), cx,
                    cy);
        std::printf("[ad] click (%d,%d)\n", cx, cy);
        sub = 2;
        settle = 0;
        return false;
    }
    if (sub == 2) {
        ++settle;
        if (cn && settle % 60 == 0)
            ad_langidle_beat(s, "postclick", "row=still-there", settle, t0, worst);
        if (!cn) {
            s->ad_hover = false;
            std::printf("[langidle] page left f=%llu (+%d frames after the "
                        "click) layers=%zu wait=%s\n",
                        (unsigned long long)s->frames, settle,
                        rt->scene().size(),
                        wait_desc(rt->current_wait()).c_str());
            sub = 3;
            return false;
        }
        if (settle > 900) {
            std::printf("[langidle] DEFECT: the click after the dwell had no "
                        "effect — still on the language page (f=%llu layers=%zu "
                        "wait=%s tweens=%d)\n",
                        (unsigned long long)s->frames, rt->scene().size(),
                        wait_desc(rt->current_wait()).c_str(),
                        (int)rt->scene().has_tweens());
            if (!s->ad_out.empty()) ad_png_shot(s, "langidle_stuck");
            std::printf("[langidle] VERDICT FAIL (click ignored after dwell)\n");
            std::exit(1);
        }
        return false;
    }
    if (sub == 3) {
        const oa::render::Layer* start = ad_find_clickable(s, "bt_start", "", false);
        if (start && ad_layer_center(s, start, &cx, &cy)) {
            ad_click_layer(s, start);
            std::printf("[langidle] f=%llu title click bt_start (%d,%d)\n",
                        (unsigned long long)s->frames, cx, cy);
            std::printf("[ad] click (%d,%d)\n", cx, cy);
            sub = 4;
            walk_f = 0;
            parked_f = 0;
        }
        return false;
    }
    // sub 4: same body as ad_fontscan_drive's story walk (kept local so the
    // fontscan journey's timing stays untouched): advance the parked ADV page
    // until a revealed message layer is on stage.
    {
        std::string parked_id;
        for (const std::string& id : rt->text().visible_content_layers()) {
            const oa::render::MessageLayer* ml = rt->text().layer(id);
            if (!ml || ml->page.empty() || ml->char_count == 0) continue;
            if (id.find("mw") == std::string::npos) continue;
            if (ml->reveal_pending || ml->reveal_index < ml->char_count) continue;
            bool has_text = false;
            for (const auto& u : ml->page)
                if (u.kind != oa::render::PageUnit::Kind::Newline && !u.data.empty()) {
                    has_text = true;
                    break;
                }
            if (!has_text) continue;
            parked_id = id;
            break;
        }
        if (!parked_id.empty()) {
            if (++parked_f > 60) {
                const oa::render::MessageLayer* ml = rt->text().layer(parked_id);
                std::printf("[langidle] story page f=%llu wait=%s layers=%zu "
                            "body='%s' chars=%d\n",
                            (unsigned long long)s->frames,
                            wait_desc(rt->current_wait()).c_str(),
                            rt->scene().size(), parked_id.c_str(),
                            ml ? int(ml->char_count) : 0);
                std::printf("[langidle] VERDICT OK (dwell -> click -> title -> "
                            "story; clicks=%d)\n", adv);
                if (!s->ad_out.empty()) ad_png_shot(s, "langidle_story");
                s->quit = true;
                return false;
            }
        } else {
            parked_f = 0;
            if (++walk_f % 90 == 0 && adv < walk_cap) {
                const int sw = rt->project_.config.stage_width;
                const int sh = rt->project_.config.stage_height;
                s->ad_cx = sw / 2;
                s->ad_cy = int(sh * 0.87);
                s->ad_click = true;
                ++adv;
                std::printf("[langidle] f=%llu advance click (%d,%d) n=%d\n",
                            (unsigned long long)s->frames, s->ad_cx, s->ad_cy, adv);
            }
        }
        return false;
    }
}

// ---------------------------------------------------------------------------
// OA_AUTODRIVE=kdemote — KukkoroDays (qureate / iMel 2020, 1280x720; pf8
// root.pfs plus .000/.001/.040/.099 volumes, sidecar qureate/ savedata).
// Reaches the first genuine E-mote [fg] scene of script 00_共通1 (chapter-1
// page 87: image/hd/fg/cat/tca_a00.psb, char カトレア) and holds there:
// first-boot language page -> brand logo -> title bt_start -> gamestart
// ([dialog] name field completes logically, then the Yes/No [select]) ->
// page-by-page ADV walk. The journey exists for the "every emote scene
// errors out" defect (research/127): it logs wait/page/emote-layer state at
// every step and, once a live E-mote layer exists, holds on the page and
// dumps the layer inventory, the composed variable sample and the CPU pose
// canvas (positive evidence that the pose really plays).
// Knobs: OA_KDE_PAGES advance-click cap (default 400), OA_KDE_HOLD hold
// frames on the emote page (default 240), OA_KDE_GAP frames between ADV
// clicks (default 10).
// ---------------------------------------------------------------------------
/// Visible [select] rows of an Artemis select group: the message-window rows
/// carry the click handler params name="select" key="CLICK" (mainloop.lua
/// clickStart maps key CLICK to the row the pointer landed on). Returns how
/// many rows are on stage and writes the world centre of row `want`
/// (0-based, draw order) — the same effective-visibility + in-stage gating
/// ad_find_clickable applies.
static int kd_select_rows(AppState* s, int want, int* cx, int* cy) {
    const int sw = s->rt->project_.config.stage_width;
    const int sh = s->rt->project_.config.stage_height;
    int n = 0;
    for (const oa::render::Layer* l : s->rt->scene().draw_order()) {
        const auto* h = s->rt->scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto it = h->params.find("name");
        if (it == h->params.end() || it->second != "select") continue;
        if (!s->rt->scene().is_effectively_visible(l->id)) continue;
        const auto r = s->oaRender->layer_world_rect(*l);
        if (!r) continue;
        const double x = (*r)[0] + (*r)[2] / 2, y = (*r)[1] + (*r)[3] / 2;
        if (x < 1 || y < 1 || x > sw - 1 || y > sh - 1) continue;
        if (n == want && cx && cy) {
            *cx = int(x);
            *cy = int(y);
        }
        ++n;
    }
    return n;
}
/// Emote-layer inventory of the current frame: layer id / canvas / pose
/// revision / playing timelines / a composed-variable sample.
static void kd_emote_dump(AppState* s, const char* tag, int clicks) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    const auto& ems = rt->emote_layers();
    std::printf("[kdemote] %s f=%llu clicks=%d wait=%s scene_layers=%zu "
                "emote_layers=%zu\n",
                tag, (unsigned long long)s->frames, clicks,
                wait_desc(rt->current_wait()).c_str(), rt->scene().size(),
                ems.size());
    for (const auto& [id, st] : ems) {
        double x = 0, y = 0, w = 0, h = 0;
        bool rect = false;
        for (const oa::render::Layer* l : rt->scene().draw_order()) {
            if (!l || l->id != id) continue;
            if (const auto r = s->oaRender->layer_world_rect(*l)) {
                x = (*r)[0];
                y = (*r)[1];
                w = (*r)[2];
                h = (*r)[3];
                rect = true;
            }
            break;
        }
        std::string sample;
        if (st.player) {
            const auto vars = st.player->variables();
            for (const char* k : {"face_cheek", "face_tears", "face_talk",
                                  "body_UD", "arm_type"}) {
                const auto v = vars.find(k);
                if (v == vars.end()) continue;
                char b[64];
                std::snprintf(b, sizeof(b), "%s%s=%.2f", sample.empty() ? "" : ",",
                              k, v->second);
                sample += b;
            }
        }
        std::printf("[kdemote]  layer id='%s' %dx%d rev=%llu vars=%zu fg='%s' "
                    "idle='%s' file='%s' world=%s(%.0f,%.0f %.0fx%.0f) [%s]\n",
                    id.c_str(), st.width, st.height,
                    (unsigned long long)st.revision,
                    st.player ? st.player->variables().size() : size_t(0),
                    st.player ? st.player->foreground_timeline().c_str() : "-",
                    st.player ? st.player->idle_timeline().c_str() : "-",
                    st.file.c_str(), rect ? "" : "no-rect ", x, y, w, h,
                    sample.c_str());
    }
}
/// CPU pose raster of the live E-mote layers (A/B next to the GPU-composited
/// stage shot): forces one non-external render, reports the figure's alpha
/// bbox + opaque pixel count, writes <out>/kdemote_canvas_<layer>.png.
static void kd_emote_canvas(AppState* s, const char* tag) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    for (const auto& [id, st] : rt->emote_layers()) {
        if (!st.player) continue;
        st.player->set_external_pose(false);
        st.player->render_now();
        const int W = st.player->width(), H = st.player->height();
        const std::vector<uint8_t>& rgba = st.player->rgba();
        st.player->set_external_pose(true);
        if (W <= 0 || H <= 0 || rgba.size() != size_t(W) * size_t(H) * 4) {
            std::printf("[kdemote] %s canvas id='%s' unavailable (%dx%d)\n", tag,
                        id.c_str(), W, H);
            continue;
        }
        int minX = W, minY = H, maxX = -1, maxY = -1;
        uint64_t opaque = 0;
        uint64_t ink = 0; // non-background-ish pixels (any channel < 240)
        for (int yy = 0; yy < H; ++yy)
            for (int xx = 0; xx < W; ++xx) {
                const uint8_t* p = &rgba[(size_t(yy) * W + xx) * 4];
                if (p[3] <= 8) continue;
                ++opaque;
                if (p[0] < 240 || p[1] < 240 || p[2] < 240) ++ink;
                if (xx < minX) minX = xx;
                if (xx > maxX) maxX = xx;
                if (yy < minY) minY = yy;
                if (yy > maxY) maxY = yy;
            }
        std::printf("[kdemote] %s canvas id='%s' %dx%d opaque=%llu ink=%llu "
                    "bbox=(%d,%d)-(%d,%d)\n",
                    tag, id.c_str(), W, H, (unsigned long long)opaque,
                    (unsigned long long)ink, minX, minY, maxX, maxY);
        if (s->ad_out.empty()) continue;
        char path[512];
        std::snprintf(path, sizeof(path), "%s/kdemote_canvas_%s.png",
                      s->ad_out.c_str(), tag);
        const std::vector<uint8_t> png =
            oa::runtime::encode_png(uint32_t(W), uint32_t(H), rgba);
        if (png.empty()) continue;
        FILE* f = std::fopen(path, "wb");
        if (f) {
            std::fwrite(png.data(), 1, png.size(), f);
            std::fclose(f);
        }
    }
}
static bool ad_kdemote_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    static int sub = 0;       // 0 boot/lang/title, 1 story walk, 2 emote hold
    static int clicks = 0;    // story advance clicks
    static int lang_clicks = 0;
    static int gap = 0;       // frames left before the next armed input
    static int hold = 0;      // frames held on the emote page
    static int cap = -1, hold_len = -1, gap_len = -1;
    if (cap < 0) {
        const char* v = std::getenv("OA_KDE_PAGES");
        cap = (v && *v) ? std::max(1, std::atoi(v)) : 400;
        const char* hv = std::getenv("OA_KDE_HOLD");
        hold_len = (hv && *hv) ? std::max(1, std::atoi(hv)) : 240;
        const char* gv = std::getenv("OA_KDE_GAP");
        gap_len = (gv && *gv) ? std::max(1, std::atoi(gv)) : 10;
        std::printf("[kdemote] journey clicks_cap=%d hold=%d gap=%d\n", cap,
                    hold_len, gap_len);
    }
    using K = oa::runtime::WaitReason::Kind;
    const oa::runtime::WaitReason* w = rt->current_wait();
    if (sub == 0) {
        // First-boot language page (selectlanguage.ast: four select rows),
        // then the brand logo, then the title. Enter the story on bt_start.
        if (gap > 0) {
            --gap;
            return false;
        }
        const oa::render::Layer* start = ad_find_clickable(s, "bt_start", "", false);
        if (start) {
            ad_click_layer(s, start);
            std::printf("[kdemote] f=%llu title click bt_start (%d,%d)\n",
                        (unsigned long long)s->frames, s->ad_cx, s->ad_cy);
            sub = 1;
            gap = 120;
            return false;
        }
        int cx = 0, cy = 0;
        if (kd_select_rows(s, 0, &cx, &cy) > 0) {
            s->ad_cx = cx;
            s->ad_cy = cy;
            s->ad_click = true;
            std::printf("[kdemote] f=%llu language select row click #%d (%d,%d)\n",
                        (unsigned long long)s->frames, ++lang_clicks, cx, cy);
            gap = 120;
            return false;
        }
        if (s->frames > 40000) {
            std::printf("[kdemote] title never reached (lang_clicks=%d); abort\n",
                        lang_clicks);
            kd_emote_dump(s, "abort-boot", clicks);
            s->quit = true;
        }
        return false;
    }
    if (sub == 1) {
        if (!rt->emote_layers().empty()) {
            std::printf("[kdemote] f=%llu EMOTE LAYER after %d advance clicks\n",
                        (unsigned long long)s->frames, clicks);
            kd_emote_dump(s, "emote-appear", clicks);
            sub = 2;
            hold = 0;
            return false;
        }
        if (gap > 0) {
            --gap;
            return false;
        }
        int cx = 0, cy = 0;
        const int rows = kd_select_rows(s, 0, &cx, &cy);
        if (rows > 0) {
            // gamestart's Yes/No select (and any in-chapter choice): row 1.
            s->ad_cx = cx;
            s->ad_cy = cy;
            s->ad_click = true;
            std::printf("[kdemote] f=%llu select rows=%d -> click row1 (%d,%d)\n",
                        (unsigned long long)s->frames, rows, cx, cy);
            gap = 30;
            return false;
        }
        const bool clickable =
            w && (w->kind == K::Generic || w->kind == K::Generic0);
        if (clickable && nm_reveal_done(rt)) {
            const int sw = rt->project_.config.stage_width;
            const int sh = rt->project_.config.stage_height;
            s->ad_cx = sw / 2;
            s->ad_cy = int(sh * 0.87); // ADV advance band (mainloop click area)
            s->ad_click = true;
            ++clicks;
            if (clicks % 20 == 0 || clicks < 3)
                std::printf("[kdemote] f=%llu advance click %d (%d,%d) wait=%s\n",
                            (unsigned long long)s->frames, clicks, s->ad_cx,
                            s->ad_cy, wait_desc(w).c_str());
            if (clicks >= cap) {
                std::printf("[kdemote] click cap %d reached with no emote layer; "
                            "abort\n",
                            cap);
                kd_emote_dump(s, "abort-walk", clicks);
                s->quit = true;
                return false;
            }
            gap = gap_len;
            return false;
        }
        if (s->frames > 400000) {
            std::printf("[kdemote] story walk timed out at click %d; abort\n", clicks);
            kd_emote_dump(s, "abort-timeout", clicks);
            s->quit = true;
        }
        return false;
    }
    // sub == 2: hold on the emote page and collect playback evidence. The
    // first [fg] of chapter 1 carries pass=1, whose framework path ends with
    // layer:skip() (both slots end on purpose — "one-pass display, no
    // animation", emote.lua:229), so the pose there is static by design.
    // Advance one page and keep sampling: the following [fg] takes the plain
    // playTimeline + fadeInTimeline("通常待機") path, where the foreground
    // expression and the idle breathing loop must both stay live and the pose
    // revision must keep climbing.
    ++hold;
    static uint64_t rev_first = 0;
    auto rev_now = [&]() -> uint64_t {
        uint64_t r = 0;
        for (const auto& [id, st] : rt->emote_layers()) r = std::max(r, st.revision);
        return r;
    };
    auto slots_dead = [&]() -> bool {
        for (const auto& [id, st] : rt->emote_layers())
            if (st.player && (!st.player->foreground_timeline().empty() ||
                              !st.player->idle_timeline().empty()))
                return false;
        return true;
    };
    if (hold == 1) {
        rev_first = rev_now();
        kd_emote_dump(s, "hold-start", clicks);
    }
    if (hold == 30) {
        kd_emote_canvas(s, "hold30");
        ad_png_shot(s, "kdemote_emote");
    }
    // One advance attempt per station while both slots are still empty (the
    // pass=1 page); alternate the ADV band and the stage centre so a message
    // window that is off (page 87 is a [msgoff] page) cannot swallow it.
    if (hold == 45 || hold == 75 || hold == 105) {
        if (slots_dead()) {
            const int sw = rt->project_.config.stage_width;
            const int sh = rt->project_.config.stage_height;
            const bool centre = (hold == 75);
            s->ad_cx = sw / 2;
            s->ad_cy = centre ? sh / 2 : int(sh * 0.87);
            s->ad_click = true;
            std::printf("[kdemote] f=%llu advance past the pass=1 emote page "
                        "(%s)\n",
                        (unsigned long long)s->frames,
                        centre ? "centre" : "adv-band");
        }
    }
    if (hold % 30 == 0 && hold > 1) kd_emote_dump(s, "anim", clicks);
    if (hold == 120) {
        kd_emote_canvas(s, "anim120");
        ad_png_shot(s, "kdemote_emote120");
    }
    if (hold >= hold_len) {
        const uint64_t rev_last = rev_now();
        kd_emote_dump(s, "final", clicks);
        kd_emote_canvas(s, "final");
        std::printf("[kdemote] pose revision first=%llu last=%llu growth=%lld "
                    "slots_live=%d\n",
                    (unsigned long long)rev_first, (unsigned long long)rev_last,
                    (long long)rev_last - (long long)rev_first,
                    slots_dead() ? 0 : 1);
        std::printf("[kdemote] DONE clicks=%d hold=%d\n", clicks, hold);
        s->quit = true;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// OA_AUTODRIVE=slnywalk — きら☆かの (slny; iMel-2022 framework, 1280x720,
// pf8 root.pfs + loose sss/ sidecar, E-mote PSB standing portraits).
// Story-line walk with a per-page PART-LEVEL fingerprint (research/132: the
// user's "the portrait draws fine at first, then a few lines later the parts
// scramble"). The journey walks the ADV story one parked page at a time and,
// on every parked page, records for each live E-mote layer:
//   * the pose geometry as parts (canvas bbox + identity per part, plus a
//     whole-pose vertex hash) via EmotePlayer::collect_pose_parts — the
//     part-level unit research/94 lacked;
//   * the CPU pose raster metrics (opaque px / alpha bbox / 8-connected
//     component count / largest interior empty row band) — the canvas-dump
//     unit, but at silhouette-structure granularity instead of whole-frame
//     luma;
//   * the composed variable domain (every label, exact value), the foreground
//     slot list with clocks, the idle slot clock and the pose revision — who
//     wrote what, and when.
// When the page cap is reached it prints the page table, the variable drift
// table (page 1 -> page N, per label) and the part drift table: which part
// moved how far RELATIVE to the figure as a whole, and on which page.
// Knobs: OA_SLW_PAGES page cap (default 24), OA_SLW_SETTLE frames held after a
// fresh park before the fingerprint (default 20), OA_SLW_GAP frames between
// the fingerprint and the advance click (default 20), OA_SLW_CANVAS=0 skips
// the PNG dumps, OA_SLW_BOOTCAP boot/never-parked frame cap (default 40000).
// ---------------------------------------------------------------------------
/// One draw part of a captured pose: identity + canvas-space bbox.
struct SlwPart {
    int source = -1;
    int icon = -1;
    std::string node; // OA_EMOTE_MESHDBG node path ("" when the flag is off)
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
/// One captured parked page (the research/132 evidence unit).
struct SlwPage {
    int shot = 0;          // fingerprint index (1-based)
    int line = 0;          // parked story line index (1-based)
    int phase = 0;         // 1 = early (right after the tag) / 2 = settled
    uint64_t frame = 0;
    std::string layer;     // emote layer id
    std::string file;      // psb path
    std::string parked;    // the message layer the page parked on
    int W = 0, H = 0;
    uint64_t rev = 0;
    bool recreated = false; // revision went backwards = a new player instance
    std::string fg;        // foreground slot list
    std::string idle;
    double idle_t = -1;
    size_t vars = 0;
    uint64_t varsHash = 0;
    uint64_t partHash = 0;
    uint64_t canvasHash = 0;
    std::vector<SlwPart> parts;
    bool canvas = false;
    uint64_t opaque = 0;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    int px0 = 0, py0 = 0, px1 = 0, py1 = 0; // parts union bbox (GPU path)
    double world = 0, world_y = 0, world_w = 0, world_h = 0; // layer world rect
    bool has_world = false;
    int comps = 0, runs = 0;
    int gapRows = 0, gapAt = -1;
    std::string vsample;   // composed variable domain, compact
    std::map<std::string, double> vmap;
};
static std::vector<SlwPage> s_slw_pages;
static std::map<std::string, uint64_t> s_slw_last_rev;
static uint64_t slw_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}
static uint64_t slw_quant(double v) { // 1/1024 px so float noise cannot flip it
    if (!std::isfinite(v)) return 0x7ff8deadbeefULL;
    return uint64_t(int64_t(std::llround(v * 1024.0)));
}
static int slw_find(std::vector<int>& parent, int a) {
    while (parent[size_t(a)] != a) {
        parent[size_t(a)] = parent[size_t(parent[size_t(a)])];
        a = parent[size_t(a)];
    }
    return a;
}
/// Pose-part fingerprint: per-part canvas bbox + a hash over every vertex.
static void slw_collect_parts(const oa::emote::EmotePlayer& p,
                             SlwPage* rec) {
    std::vector<oa::emote::EmoteDrawPart> parts;
    std::string err;
    if (!p.collect_pose_parts(&parts, &err)) {
        std::printf("[slnywalk]   collect_pose_parts failed: %s\n", err.c_str());
        return;
    }
    uint64_t h = 1469598103934665603ULL;
    for (const auto& pt : parts) {
        SlwPart r;
        r.source = pt.source;
        r.icon = pt.icon;
        r.node = pt.nodePath;
        r.x0 = 1e30, r.y0 = 1e30, r.x1 = -1e30, r.y1 = -1e30;
        for (const auto& v : pt.verts) {
            r.x0 = std::min(r.x0, v.x);
            r.y0 = std::min(r.y0, v.y);
            r.x1 = std::max(r.x1, v.x);
            r.y1 = std::max(r.y1, v.y);
            h = slw_mix(h, slw_quant(v.x));
            h = slw_mix(h, slw_quant(v.y));
        }
        if (pt.verts.empty()) r.x0 = r.y0 = r.x1 = r.y1 = 0;
        rec->parts.push_back(std::move(r));
    }
    rec->partHash = h;
}
/// Alpha-silhouette metrics of the CPU pose raster: opaque px, bbox,
/// 8-connected component count (run-length union-find) and the largest empty
/// row band strictly inside the figure's bbox — the "detached part" unit.
static void slw_canvas_metrics(const std::vector<uint8_t>& rgba, int W, int H,
                               SlwPage* rec) {
    struct Run {
        int y, x0, x1;
    };
    std::vector<Run> runs;
    std::vector<int> parent, prev, cur;
    std::vector<uint8_t> row_has(size_t(H), 0);
    rec->opaque = 0;
    rec->bx0 = W, rec->by0 = H, rec->bx1 = -1, rec->by1 = -1;
    for (int y = 0; y < H; ++y) {
        const uint8_t* row = &rgba[size_t(y) * size_t(W) * 4];
        cur.clear();
        int x = 0;
        while (x < W) {
            if (row[size_t(x) * 4 + 3] <= 8) {
                ++x;
                continue;
            }
            const int s = x;
            while (x < W && row[size_t(x) * 4 + 3] > 8) ++x;
            const int e = x - 1;
            runs.push_back(Run{y, s, e});
            parent.push_back(int(runs.size()) - 1);
            cur.push_back(int(runs.size()) - 1);
            rec->opaque += size_t(e - s + 1);
            row_has[size_t(y)] = 1;
            if (s < rec->bx0) rec->bx0 = s;
            if (e > rec->bx1) rec->bx1 = e;
            if (y < rec->by0) rec->by0 = y;
            if (y > rec->by1) rec->by1 = y;
        }
        size_t a = 0, b = 0;
        while (a < prev.size() && b < cur.size()) {
            const Run& ra = runs[size_t(prev[a])];
            const Run& rb = runs[size_t(cur[b])];
            if (ra.x1 + 1 < rb.x0) {
                ++a;
                continue;
            }
            if (rb.x1 + 1 < ra.x0) {
                ++b;
                continue;
            }
            const int pa = slw_find(parent, prev[a]);
            const int pb = slw_find(parent, cur[b]);
            if (pa != pb) parent[size_t(std::max(pa, pb))] = std::min(pa, pb);
            if (ra.x1 < rb.x1)
                ++a;
            else
                ++b;
        }
        prev = cur;
    }
    rec->runs = int(runs.size());
    int comps = 0;
    for (size_t i = 0; i < runs.size(); ++i)
        if (slw_find(parent, int(i)) == int(i)) ++comps;
    rec->comps = comps;
    int best = 0, at = -1, band = 0;
    for (int y = rec->by0 + 1; y < rec->by1; ++y) {
        if (row_has[size_t(y)]) {
            band = 0;
            continue;
        }
        if (++band > best) {
            best = band;
            at = y - band + 1;
        }
    }
    rec->gapRows = best;
    rec->gapAt = at;
}
/// Downsampled box-filter copy (canvas PNGs stay readable at 1/2).
static std::vector<uint8_t> slw_down2(const std::vector<uint8_t>& src, int W,
                                      int H, int* ow, int* oh) {
    const int w = std::max(1, W / 2), h = std::max(1, H / 2);
    std::vector<uint8_t> out(size_t(w) * size_t(h) * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c) {
                int acc = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(W - 1, x * 2 + dx);
                        const int sy = std::min(H - 1, y * 2 + dy);
                        acc += src[(size_t(sy) * size_t(W) + size_t(sx)) * 4 +
                                   size_t(c)];
                    }
                out[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)] =
                    uint8_t(acc / 4);
            }
    *ow = w;
    *oh = h;
    return out;
}
static void slw_write_png(const std::string& dir, const char* name,
                          const std::vector<uint8_t>& rgba, int w, int h) {
    if (dir.empty() || w <= 0 || h <= 0 ||
        rgba.size() != size_t(w) * size_t(h) * 4)
        return;
    const std::vector<uint8_t> png =
        oa::runtime::encode_png(uint32_t(w), uint32_t(h), rgba);
    if (png.empty()) return;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/%s", dir.c_str(), name);
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
}
/// Fingerprint every live E-mote layer of the current parked page.
static void slw_capture(AppState* s, int line, int phase,
                        const std::string& parked, int settle_frames,
                        bool dump_png) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    const auto& ems = rt->emote_layers();
    if (ems.empty()) {
        if (phase == 2)
            std::printf("[slnywalk] line=%d f=%llu park='%s' emote=0 layers=%zu "
                        "wait=%s\n",
                        line, (unsigned long long)s->frames, parked.c_str(),
                        rt->scene().size(),
                        wait_desc(rt->current_wait()).c_str());
        return;
    }
    for (const auto& [id, st] : ems) {
        if (!st.player) continue;
        SlwPage rec;
        rec.line = line;
        rec.phase = phase;
        rec.frame = s->frames;
        rec.layer = id;
        rec.file = st.file;
        rec.parked = parked;
        rec.W = st.width;
        rec.H = st.height;
        rec.rev = st.revision;
        const auto lr = s_slw_last_rev.find(id);
        rec.recreated = lr != s_slw_last_rev.end() && st.revision < lr->second;
        s_slw_last_rev[id] = st.revision;
        rec.idle = st.player->idle_timeline();
        rec.idle_t = st.player->idle_time();
        {
            std::string fg;
            char b[160];
            for (const auto& fs : st.player->foreground_slots()) {
                std::snprintf(b, sizeof(b), "%s[%s t=%.1f fl=%d]",
                              fg.empty() ? "" : " ", fs.label.c_str(), fs.t,
                              fs.flags);
                fg += b;
            }
            rec.fg = fg.empty() ? "-" : fg;
        }
        rec.vmap = st.player->variables();
        rec.vars = rec.vmap.size();
        uint64_t vh = 1469598103934665603ULL;
        for (const auto& [k, v] : rec.vmap) {
            char b[64];
            std::snprintf(b, sizeof(b), "%s=%.2f", k.c_str(), v);
            if (!rec.vsample.empty()) rec.vsample += " ";
            rec.vsample += b;
            for (char c : k) vh = slw_mix(vh, uint64_t(uint8_t(c)));
            vh = slw_mix(vh, slw_quant(v));
        }
        rec.varsHash = vh;
        slw_collect_parts(*st.player, &rec);
        rec.px0 = rec.py0 = 0;
        rec.px1 = rec.py1 = -1;
        for (const auto& p : rec.parts) {
            if (p.x1 < p.x0) continue;
            if (rec.px1 < rec.px0) {
                rec.px0 = int(std::floor(p.x0));
                rec.py0 = int(std::floor(p.y0));
                rec.px1 = int(std::ceil(p.x1));
                rec.py1 = int(std::ceil(p.y1));
                continue;
            }
            rec.px0 = std::min(rec.px0, int(std::floor(p.x0)));
            rec.py0 = std::min(rec.py0, int(std::floor(p.y0)));
            rec.px1 = std::max(rec.px1, int(std::ceil(p.x1)));
            rec.py1 = std::max(rec.py1, int(std::ceil(p.y1)));
        }
        // same-run CPU pose raster (research/94's canvas dump; the pose is
        // re-evaluated from the same composed variables the GPU path uses)
        st.player->set_external_pose(false);
        st.player->render_now();
        const int W = st.player->width(), H = st.player->height();
        const std::vector<uint8_t>& rgba = st.player->rgba();
        st.player->set_external_pose(true);
        if (W > 0 && H > 0 && rgba.size() == size_t(W) * size_t(H) * 4) {
            rec.canvas = true;
            slw_canvas_metrics(rgba, W, H, &rec);
            // pixel identity of the CPU raster (the visual truth of this pose;
            // the same build+page+phase must reproduce it exactly)
            uint64_t ch = 1469598103934665603ULL;
            for (int yy = rec.by0; yy <= rec.by1; ++yy)
                for (int xx = rec.bx0; xx <= rec.bx1; ++xx) {
                    const uint8_t* p =
                        &rgba[(size_t(yy) * size_t(W) + size_t(xx)) * 4];
                    if (p[3] <= 8) continue;
                    ch = slw_mix(ch, (uint64_t(p[0]) << 16) |
                                         (uint64_t(p[1]) << 8) |
                                         uint64_t(p[2]) |
                                         (uint64_t(p[3]) << 24));
                }
            rec.canvasHash = ch;
        }
        rec.shot = int(s_slw_pages.size()) + 1;
        // where the portrait actually lands on stage: the layer's world rect
        // and the canvas content bbox mapped through it (the user-visible
        // placement; research/94 left the view mapping uncalibrated).
        double wx = 0, wy = 0, ww = 0, wh = 0;
        bool hasrect = false;
        for (const oa::render::Layer* l : rt->scene().draw_order()) {
            if (!l || l->id != id) continue;
            if (const auto r = s->oaRender->layer_world_rect(*l)) {
                wx = (*r)[0];
                wy = (*r)[1];
                ww = (*r)[2];
                wh = (*r)[3];
                hasrect = true;
            }
            break;
        }
        rec.world = hasrect ? wx : 0;
        rec.world_y = hasrect ? wy : 0;
        rec.world_w = ww;
        rec.world_h = wh;
        rec.has_world = hasrect;
        char name[128];
        std::printf(
            "[slnywalk] shot=%d line=%d ph=%d f=%llu id='%s' %dx%d rev=%llu%s "
            "parts=%zu pHash=%016llx cHash=%016llx vHash=%016llx vars=%zu "
            "comps=%d gap=%d@%d "
            "opaque=%llu cbbox=(%d,%d)-(%d,%d) pbbox=(%d,%d)-(%d,%d) settle=%d\n",
            rec.shot, line, phase, (unsigned long long)s->frames, id.c_str(),
            rec.W, rec.H, (unsigned long long)rec.rev,
            rec.recreated ? "(NEW-PLAYER)" : "", rec.parts.size(),
            (unsigned long long)rec.partHash,
            (unsigned long long)rec.canvasHash,
            (unsigned long long)rec.varsHash,
            rec.vars, rec.comps, rec.gapRows, rec.gapAt,
            (unsigned long long)rec.opaque, rec.bx0, rec.by0, rec.bx1, rec.by1,
            rec.px0, rec.py0, rec.px1, rec.py1, settle_frames);
        std::printf("[slnywalk]   fg=%s idle=%s@%.1f file='%s' park='%s'\n",
                    rec.fg.c_str(), rec.idle.c_str(), rec.idle_t,
                    rec.file.c_str(), rec.parked.c_str());
        std::printf("[slnywalk]   world=%s(%.0f,%.0f %.0fx%.0f) onstage=(%.0f,%.0f)"
                    "-(%.0f,%.0f)\n",
                    rec.has_world ? "" : "no-rect ", rec.world, rec.world_y,
                    rec.world_w, rec.world_h,
                    rec.has_world && rec.W > 0 ? rec.world + double(rec.bx0) * rec.world_w / rec.W : 0.0,
                    rec.has_world && rec.H > 0 ? rec.world_y + double(rec.by0) * rec.world_h / rec.H : 0.0,
                    rec.has_world && rec.W > 0 ? rec.world + double(rec.bx1) * rec.world_w / rec.W : 0.0,
                    rec.has_world && rec.H > 0 ? rec.world_y + double(rec.by1) * rec.world_h / rec.H : 0.0);
        std::printf("[slnywalk]   vars: %s\n", rec.vsample.c_str());
        if (dump_png && !s->ad_out.empty()) {
            if (rec.canvas) {
                int ow = 0, oh = 0;
                const std::vector<uint8_t> half =
                    slw_down2(rgba, W, H, &ow, &oh);
                std::snprintf(name, sizeof(name),
                              "slnywalk_s%02d_ph%d_canvas_%dx%d.png", rec.shot,
                              phase, ow, oh);
                slw_write_png(s->ad_out, name, half, ow, oh);
            }
            oa::media::Image img;
            if (s->oaRender->snapshot_renderer(img) && img.w > 0 && img.h > 0) {
                std::snprintf(name, sizeof(name),
                              "slnywalk_s%02d_ph%d_stage.png", rec.shot, phase);
                slw_write_png(s->ad_out, name, img.rgba, img.w, img.h);
            }
        }
        s_slw_pages.push_back(std::move(rec));
    }
}
/// Part identity across pages: node path when the mesh dump flag is on,
/// otherwise source/icon.
static std::string slw_part_key(const SlwPart& p) {
    if (!p.node.empty()) return p.node;
    char b[64];
    std::snprintf(b, sizeof(b), "s%d/i%d", p.source, p.icon);
    return b;
}
/// End-of-run report: page table, variable drift and part drift. Both drift
/// tables are computed WITHIN one emote layer and one capture phase, against
/// that group's first shot — a page 1 that happens to be the message-window
/// mini face is not a reference for the standing portrait.
static void slw_report() {
    std::printf("[slnywalk] PAGE TABLE shots=%zu\n", s_slw_pages.size());
    std::printf("[slnywalk] %4s %4s %3s %7s %6s %6s %5s %5s %7s %8s %8s %6s "
                "%-22s %-18s\n",
                "shot", "line", "ph", "frame", "parts", "cbbox", "comps",
                "gap", "opaque", "pHash", "cHash", "rev", "fg", "idle");
    for (const auto& r : s_slw_pages) {
        char bb[40], pb[40];
        std::snprintf(bb, sizeof(bb), "%d,%d-%d,%d", r.bx0, r.by0, r.bx1,
                      r.by1);
        std::snprintf(pb, sizeof(pb), "%d,%d-%d,%d", r.px0, r.py0, r.px1,
                      r.py1);
        std::printf("[slnywalk] %4d %4d %3d %7llu %6zu %6s %5d %5d %7llu "
                    "%8llx %8llx %6llu%s %-22s %-18s | parts bbox %s\n",
                    r.shot, r.line, r.phase, (unsigned long long)r.frame,
                    r.parts.size(), bb, r.comps, r.gapRows,
                    (unsigned long long)r.opaque,
                    (unsigned long long)(r.partHash & 0xffffffffULL),
                    (unsigned long long)(r.canvasHash & 0xffffffffULL),
                    (unsigned long long)r.rev, r.recreated ? "*" : "",
                    r.fg.c_str(), r.idle.c_str(), pb);
    }
    if (s_slw_pages.size() < 2) {
        std::printf("[slnywalk] DRIFT: only %zu fingerprint(s); nothing to "
                    "compare\n",
                    s_slw_pages.size());
        return;
    }
    // group key: layer + phase (each group is compared against its first shot)
    std::vector<std::string> groups;
    auto group_of = [](const SlwPage& r) {
        char b[128];
        std::snprintf(b, sizeof(b), "%s|ph%d", r.layer.c_str(), r.phase);
        return std::string(b);
    };
    for (const auto& r : s_slw_pages) {
        const std::string g = group_of(r);
        if (std::find(groups.begin(), groups.end(), g) == groups.end())
            groups.push_back(g);
    }
    for (const std::string& g : groups) {
        std::vector<const SlwPage*> pages;
        for (const auto& r : s_slw_pages)
            if (group_of(r) == g) pages.push_back(&r);
        if (pages.size() < 2) continue;
        std::printf("[slnywalk] == group '%s' shots=%zu file='%s' ==\n",
                    g.c_str(), pages.size(), pages.front()->file.c_str());
        const SlwPage& a = *pages.front();
        std::printf("[slnywalk] VAR DRIFT (ref shot=%d line=%d vars=%zu)\n",
                    a.shot, a.line, a.vars);
        int drifted = 0;
        for (const auto& [k, v0] : a.vmap) {
            double lo = v0, hi = v0;
            int first = -1;
            for (const SlwPage* r : pages) {
                const auto it = r->vmap.find(k);
                if (it == r->vmap.end()) continue;
                lo = std::min(lo, it->second);
                hi = std::max(hi, it->second);
                if (first < 0 && std::fabs(it->second - v0) > 0.01)
                    first = r->shot;
            }
            if (first < 0) continue;
            ++drifted;
            std::printf("[slnywalk]   %-18s ref=%.2f range=[%.2f..%.2f] "
                        "firstDiff=shot%d\n",
                        k.c_str(), v0, lo, hi, first);
        }
        if (!drifted)
            std::printf("[slnywalk]   every variable identical to the "
                        "reference across %zu fingerprints\n",
                        pages.size());
        // part drift: relative to the figure as a whole (median part centre)
        auto centers = [](const SlwPage& r,
                          std::map<std::string, std::pair<double, double>>* out,
                          double* fx, double* fy) {
            std::vector<double> cx, cy;
            for (const auto& p : r.parts) {
                if (p.x1 < p.x0) continue;
                const double x = (p.x0 + p.x1) / 2, y = (p.y0 + p.y1) / 2;
                (*out)[slw_part_key(p)] = {x, y};
                cx.push_back(x);
                cy.push_back(y);
            }
            *fx = *fy = 0;
            if (cx.empty()) return;
            std::sort(cx.begin(), cx.end());
            std::sort(cy.begin(), cy.end());
            *fx = cx[cx.size() / 2];
            *fy = cy[cy.size() / 2];
        };
        std::map<std::string, std::pair<double, double>> c0;
        double fx0 = 0, fy0 = 0;
        centers(a, &c0, &fx0, &fy0);
        struct Hit {
            double d;
            std::string key;
            int shot;
            double px, py, fx, fy;
        };
        std::vector<Hit> hits;
        for (const SlwPage* r : pages) {
            std::map<std::string, std::pair<double, double>> c;
            double fx = 0, fy = 0;
            centers(*r, &c, &fx, &fy);
            for (const auto& [k, p] : c) {
                const auto i0 = c0.find(k);
                if (i0 == c0.end()) continue;
                const double dx = (p.first - fx) - (i0->second.first - fx0);
                const double dy = (p.second - fy) - (i0->second.second - fy0);
                hits.push_back(Hit{std::hypot(dx, dy), k, r->shot, p.first,
                                   p.second, fx, fy});
            }
        }
        std::sort(hits.begin(), hits.end(),
                  [](const Hit& x, const Hit& y) { return x.d > y.d; });
        std::printf("[slnywalk] PART DRIFT (part centre minus that page's "
                    "median part centre; ref shot=%d)\n",
                    a.shot);
        std::printf("[slnywalk]   parts per shot: ");
        for (const SlwPage* r : pages) std::printf("%d:%zu ", r->shot,
                                                   r->parts.size());
        std::printf("\n");
        size_t shown = 0;
        for (const Hit& h : hits) {
            if (h.d < 1.0 || shown >= 6) break;
            ++shown;
            std::printf("[slnywalk]   rel=%.2fpx shot%d key='%s' "
                        "part=(%.1f,%.1f) figC=(%.1f,%.1f)\n",
                        h.d, h.shot, h.key.c_str(), h.px, h.py, h.fx, h.fy);
        }
        if (!shown)
            std::printf("[slnywalk]   no part moved more than 1px relative "
                        "to the figure\n");
        // GPU-parts vs CPU-canvas silhouette agreement (same pose, two paths)
        int worst = 0, worst_shot = 0;
        for (const SlwPage* r : pages) {
            if (!r->canvas || r->px1 < r->px0 || r->bx1 < r->bx0) continue;
            const int d = std::max(std::abs(r->px0 - r->bx0),
                                   std::max(std::abs(r->py0 - r->by0),
                                            std::max(std::abs(r->px1 - r->bx1),
                                                     std::abs(r->py1 - r->by1))));
            if (d > worst) {
                worst = d;
                worst_shot = r->shot;
            }
        }
        std::printf("[slnywalk] GPU-parts vs CPU-canvas bbox: worst=%dpx "
                    "(shot%d)\n", worst, worst_shot);
    }
}
static bool ad_slnywalk_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    using K = oa::runtime::WaitReason::Kind;
    static int sub = 0;        // 0 boot / 1 story walk
    static int cap = -1, settle_len = -1, gap_len = -1, bootcap = -1;
    static int early_len = -1;
    static bool dump_png = true;
    static int lang_f = 0, story_f = 0, settle = 0;
    static int lines = 0, adv_clicks = 0, adv_wait = 0, done_lines = 0;
    static uint64_t last_key = 0;
    static bool consumed = true, early_done = false;
    static std::string last_park;
    if (cap < 0) {
        const char* v = std::getenv("OA_SLW_PAGES");
        cap = (v && *v) ? std::max(1, std::atoi(v)) : 24;
        v = std::getenv("OA_SLW_SETTLE");
        settle_len = (v && *v) ? std::max(1, std::atoi(v)) : 20;
        v = std::getenv("OA_SLW_EARLY");
        early_len = (v && *v) ? std::max(1, std::atoi(v)) : 2;
        v = std::getenv("OA_SLW_GAP");
        gap_len = (v && *v) ? std::max(1, std::atoi(v)) : 20;
        v = std::getenv("OA_SLW_BOOTCAP");
        bootcap = (v && *v) ? std::max(1000, std::atoi(v)) : 40000;
        dump_png = !((v = std::getenv("OA_SLW_CANVAS")) && *v &&
                     std::atoi(v) == 0);
        std::printf("[slnywalk] cap=%d settle=%d early=%d gap=%d bootcap=%d "
                    "canvas=%d\n",
                    cap, settle_len, early_len, gap_len, bootcap, (int)dump_png);
    }
    const oa::runtime::WaitReason* w = rt->current_wait();
    if (sub == 0) {
        // First-boot language page (bt_cn row over :ui/game/lang/bt_cn.png),
        // then the brand logos, then the title; bt_start enters the story.
        if (++lang_f > 30) {
            const oa::render::Layer* pick = ad_find_clickable(s, "bt_cn", "", false);
            if (!pick) pick = ad_find_clickable(s, "", "bt_cn", true);
            const oa::render::Layer* confirm =
                ad_find_clickable(s, "bt_cn_next", "", false);
            const oa::render::Layer* sel = confirm ? confirm : pick;
            if (sel) {
                ad_click_layer(s, sel);
                std::printf("[slnywalk] f=%llu lang click %s (%d,%d)\n",
                            (unsigned long long)s->frames,
                            confirm ? "confirm" : "pick-cn", s->ad_cx, s->ad_cy);
                lang_f = 0;
                return false;
            }
        }
        const oa::render::Layer* start = ad_find_clickable(s, "bt_start", "", false);
        if (start) {
            ad_click_layer(s, start);
            std::printf("[slnywalk] f=%llu title click bt_start (%d,%d)\n",
                        (unsigned long long)s->frames, s->ad_cx, s->ad_cy);
            sub = 1;
            return false;
        }
        if (s->frames > uint64_t(bootcap)) {
            std::printf("[slnywalk] title never reached; abort\n");
            s->quit = true;
        }
        return false;
    }
    // sub 1: walk the story one parked page at a time.
    std::string parked_id;
    uint64_t text_key = 1469598103934665603ULL;
    for (const std::string& id : rt->text().visible_content_layers()) {
        const oa::render::MessageLayer* ml = rt->text().layer(id);
        if (!ml || ml->page.empty() || ml->char_count == 0) continue;
        if (id.find("mw") == std::string::npos) continue;
        if (ml->reveal_pending || ml->reveal_index < ml->char_count) continue;
        bool has_text = false;
        for (const auto& u : ml->page)
            if (u.kind != oa::render::PageUnit::Kind::Newline && !u.data.empty()) {
                has_text = true;
                break;
            }
        if (!has_text) continue;
        if (parked_id.empty()) parked_id = id;
        for (char c : id) text_key = slw_mix(text_key, uint64_t(uint8_t(c)));
        for (const auto& u : ml->page) {
            text_key = slw_mix(text_key, uint64_t(u.kind));
            for (char c : u.data)
                text_key = slw_mix(text_key, uint64_t(uint8_t(c)));
        }
    }
    const bool parked =
        !parked_id.empty() &&
        (w && (w->kind == K::Generic || w->kind == K::Generic0 ||
               w->kind == K::Timed || w->kind == K::Se || w->kind == K::KeyWait));
    if (!parked) {
        story_f = 0;
        settle = 0;
        // nothing parked: run the ADV band so prologue clicks / choices that do
        // not park on a message layer cannot stall the walk.
        if (++adv_wait >= std::max(gap_len, 30)) {
            adv_wait = 0;
            const int sw = rt->project_.config.stage_width;
            const int sh = rt->project_.config.stage_height;
            s->ad_cx = sw / 2;
            s->ad_cy = int(sh * 0.87);
            s->ad_click = true;
            ++adv_clicks;
            std::printf("[slnywalk] f=%llu advance click %d (%d,%d) wait=%s\n",
                        (unsigned long long)s->frames, adv_clicks, s->ad_cx,
                        s->ad_cy, wait_desc(w).c_str());
        }
        if (s->frames > uint64_t(bootcap)) {
            std::printf("[slnywalk] story page never parked; abort "
                        "(clicks=%d)\n", adv_clicks);
            slw_report();
            s->quit = true;
        }
        return false;
    }
    if (text_key != last_key) {
        last_key = text_key;
        ++lines;
        settle = 0;
        consumed = false;
        last_park = parked_id;
        adv_wait = 0;
        early_done = false;
    }
    ++settle;
    // two fingerprints per line: an EARLY one right after the tag ran (the
    // expression entrance / a gesture in flight) and a SETTLED one (what the
    // player reads), so a mid-animation scramble cannot hide between stations.
    if (!early_done && settle >= early_len) {
        early_done = true;
        slw_capture(s, lines, 1, last_park, settle, dump_png);
    }
    if (!consumed && settle >= settle_len) {
        const size_t before = s_slw_pages.size();
        slw_capture(s, lines, 2, last_park, settle, dump_png);
        consumed = true;
        adv_wait = 0;
        if (s_slw_pages.size() > before && ++done_lines >= cap) {
            std::printf("[slnywalk] cap=%d emote lines reached (clicks=%d "
                        "shots=%zu)\n",
                        cap, adv_clicks, s_slw_pages.size());
            slw_report();
            s->quit = true;
            return true;
        }
        return false;
    }
    // consumed: keep nudging the ADV band until the line actually changes.
    if (consumed && ++adv_wait >= gap_len) {
        adv_wait = 0;
        const int sw = rt->project_.config.stage_width;
        const int sh = rt->project_.config.stage_height;
        s->ad_cx = sw / 2;
        s->ad_cy = int(sh * 0.87);
        s->ad_click = true;
        ++adv_clicks;
    }
    return false;
}

bool ad_drive(AppState* s) {
    oa::runtime::GameRuntime* rt = s->rt.get();
    ++s->ad_stage_f;
#if OA_TEST_BUILD
    if (s->ad_flow == "slnywalk") return ad_slnywalk_drive(s);
    if (s->ad_flow == "kdemote") return ad_kdemote_drive(s);
    if (s->ad_flow == "nmfg") return ad_nmfg_drive(s);
    if (s->ad_flow == "sn101mont") return ad_sn101mont_drive(s);
    if (s->ad_flow == "so90") return ad_so90_drive(s);
    if (s->ad_flow == "slnyface") return ad_slnyface_drive(s);
    if (s->ad_flow == "bt91blog") return ad_bt91blog_drive(s);
    if (s->ad_flow == "bt96snow") return ad_bt96snow_drive(s);
    if (s->ad_flow == "bt119cg") return ad_bt119_drive(s);
    if (s->ad_flow == "n2title") return ad_n2title_drive(s);
    if (s->ad_flow == "sakura") return ad_sakura_drive(s);
    if (s->ad_flow == "scale") return ad_scale_drive(s);
    if (s->ad_flow == "rr") return ad_rr_drive(s);
    if (s->ad_flow == "hctdig") return ad_hctdig_drive(s);
    if (s->ad_flow == "rrtext") return ad_rrtext_drive(s);
    if (s->ad_flow == "rrconf") return ad_rrconf_drive(s);
    if (s->ad_flow == "fontscan") return ad_fontscan_drive(s);
    if (s->ad_flow == "langidle") return ad_langidle_drive(s);
#endif
    switch (s->ad_stage) {
        case 0: { // wait for the parked title with bt_start, then enter story
            const oa::runtime::WaitReason* w = rt->current_wait();
            if (!(ad_wait_stop(w) && ad_has_handler(rt, "bt_start"))) break;
            ad_arm_click_key(s, "bt_start");
            s->ad_stage = 1;
            s->ad_stage_f = 0;
            std::printf("[ad] stage1 start game\n");
            break;
        }
        case 1: { // wait for a fully parked story page, then open the flow
            if (!ad_story_parked(rt)) break;
            s->ad_sample_page = [&] {
                const oa::render::MessageLayer* ml = rt->text().layer("1.80.mw.adv_adv");
                if (!ml) return std::string("-");
                for (const auto& u : ml->page)
                    if (!u.data.empty()) return u.data.substr(0, 16);
                return std::string("-");
            }();
            if (s->ad_flow == "help") {
                // M8 勘察:剧情停驻页的 click 按钮清单(key/层/世界矩形/over
                // 回调)——dock 快捷按钮的 uihelp hover 目标从此枚举挑选。
                std::map<std::string, std::string> seen;
                int n = 0;
                for (const oa::render::Layer* l : rt->scene().draw_order()) {
                    const auto* h = rt->scene().find_event_handler(l->id, "click");
                    if (!h) continue;
                    const auto k = h->params.find("key");
                    const std::string key =
                        k == h->params.end() ? std::string() : k->second;
                    if (key.empty() || seen.count(key)) continue;
                    seen[key] = l->id;
                    std::string rect;
                    if (const auto r = s->oaRender->layer_world_rect(*l)) {
                        char buf[96];
                        std::snprintf(buf, sizeof(buf), "(%.0f,%.0f %.0fx%.0f)",
                                      (*r)[0], (*r)[1], (*r)[2], (*r)[3]);
                        rect = buf;
                    }
                    if (n++ < 120)
                        std::printf("[help] dock key='%s' layer=%s rect=%s over=%s "
                                    "out=%s\n",
                                    key.c_str(), l->id.c_str(), rect.c_str(),
                                    h->params.count("over")
                                        ? h->params.at("over").c_str()
                                        : "-",
                                    h->params.count("out")
                                        ? h->params.at("out").c_str()
                                        : "-");
                }
                std::printf("[help] enum done keys=%zu (story '%s')\n", seen.size(),
                            s->ad_sample_page.c_str());
                // M8 复现:hover dock 的 config 快捷按钮(用户复现面
                // save/load/config;bt_conf 的 uihelp 走同一路径)。基线画面
                // 在 hover 生效前截获。
                s->ad_base_text_layers = rt->text().visible_content_layers();
                {
                    oa::media::Image img;
                    if (s->oaRender->snapshot_renderer(img) && img.w > 0 && img.h > 0) {
                        s->ad_help_base_rgba = img.rgba;
                        s->ad_help_base_w = img.w;
                        s->ad_help_base_h = img.h;
                        if (!s->ad_out.empty()) {
                            const std::vector<uint8_t> png = oa::runtime::encode_png(
                                uint32_t(img.w), uint32_t(img.h), img.rgba);
                            if (!png.empty()) {
                                char path[512];
                                std::snprintf(path, sizeof(path), "%s/help_base_%03llu.png",
                                              s->ad_out.c_str(),
                                              (unsigned long long)s->ad_ppm_count++);
                                FILE* f = std::fopen(path, "wb");
                                if (f) {
                                    std::fwrite(png.data(), 1, png.size(), f);
                                    std::fclose(f);
                                }
                            }
                        }
                    }
                    // 正文消息层对照参数(字体/几何——help 与正文走同引擎面)
                    const oa::render::MessageLayer* adv =
                        rt->text().layer("1.80.mw.adv_adv");
                    if (adv) {
                        std::printf("[help] adv-ml font_size=%.1f face='%s' left=%.1f "
                                    "top=%.1f bound=%s\n",
                                    adv->font.size(), adv->font.face().c_str(),
                                    adv->left, adv->top,
                                    rt->scene().bound_scene_id("1.80.mw.adv_adv").c_str());
                    }
                }
                const char* target = "bt_conf";
                bool found = false;
                for (const oa::render::Layer* l : rt->scene().draw_order()) {
                    const auto* h = rt->scene().find_event_handler(l->id, "click");
                    if (!h) continue;
                    const auto k = h->params.find("key");
                    if (k == h->params.end() || k->second != target) continue;
                    if (const auto r = s->oaRender->layer_world_rect(*l)) {
                        s->ad_cx = int((*r)[0] + (*r)[2] / 2);
                        s->ad_cy = int((*r)[1] + (*r)[3] / 2);
                        found = true;
                        std::printf("[help] hover target %s at layer %s -> (%d,%d)\n",
                                    target, l->id.c_str(), s->ad_cx, s->ad_cy);
                    }
                    break;
                }
                s->ad_hover = found;
                s->ad_stage = 2;
                s->ad_stage_f = 0;
                std::printf("[help] stage2 hold hover found=%d\n", (int)found);
                break;
            }
            if (s->ad_flow == "r10blog") {
                // backlog regression: first walk ~12 story pages so the
                // log has several backlog pages, then open F8.
                switch (s->ad_blog_sub) {
                    case 0: // park -> click through one page per cycle
                        if (ad_story_parked(rt)) {
                            s->input.mouse_x = 640;
                            s->input.mouse_y = 600;
                            s->ad_click = true;
                            s->ad_blog_click_done = true;
                            ++s->ad_blog_turns;
                            if (s->ad_blog_turns >= 12) s->ad_blog_sub = 1;
                        }
                        break;
                    case 1: { // wait for the last click to settle, open F8
                        if (s->ad_blog_f++ < 200) break;
                        s->ad_press_key = 119; // F8 backlog
                        s->ad_stage = 2;
                        s->ad_stage_f = 0;
                        s->ad_blog_sub = 10;
                        std::printf("[blog] stage2 open backlog (turns=%d)\n",
                                    s->ad_blog_turns);
                        break;
                    }
                }
                break;
            }
            if (s->ad_flow == "qld") {
                // phenomenon C: first make a quick save (F4 = adv QSAVE).
                s->ad_qld_sub = 0;
                s->ad_qld_armed = false;
                s->ad_qld_armf = -1;
                s->ad_press_key = 115; // F4 qsave
                s->ad_stage = 2;
                s->ad_stage_f = 0;
                std::printf("[qld] stage2 quick save (story '%s')\n",
                            s->ad_sample_page.c_str());
                break;
            }
            s->ad_press_key = (s->ad_flow == "exit" || s->ad_flow == "conf" ||
                               s->ad_flow == "r10" || s->ad_flow == "r10t")
                                  ? 121 /*F10 config*/
                                  : (s->ad_flow == "r10save")
                                        ? 117 /*F6 save*/
                                        : (s->ad_flow == "r10blog")
                                              ? 119 /*F8 backlog*/
                                              : (s->ad_flow == "d38")
                                                    ? -1 /*story park only*/
                                                    : 123 /*F12 title*/;
            s->ad_stage = 2;
            s->ad_stage_f = 0;
            std::printf("[ad] stage2 open '%s' (story page '%s')\n", s->ad_flow.c_str(),
                        s->ad_sample_page.c_str());
            break;
        }
        case 2: { // exit: config opened -> hit bt_end; title: dialog appears
            if (s->ad_flow == "qld") {
                // ---- quickload dock evidence ----
                // sub 0: qsave confirm dialog appeared -> Enter
                // sub 1: wait story park (qsave done) -> advance one page
                // sub 2: F5 quickload -> confirm dialog -> Enter
                // sub 3: wait restored story park; verify push rows; dock
                //        bt_save click must open the save screen
                // sub 4: Esc; bt_conf click must open config
                // sub 5: Esc; F6 key must open the save screen
                // sub 6: done
                const bool dlg = rt->scene().find("600.1.bt.1.0") != nullptr;
                // Windowed runs reveal text under real pacing; "settled story"
                // here means a story-family wait with the story layer count
                // and no transition in flight (ad_story_parked's reveal
                // predicate is stricter than this flow needs).
                const auto story_ok = [&]() {
                    const oa::runtime::WaitReason* w = rt->current_wait();
                    if (!w) return false;
                    switch (w->kind) {
                        case oa::runtime::WaitReason::Kind::Generic:
                        case oa::runtime::WaitReason::Kind::Generic0:
                        case oa::runtime::WaitReason::Kind::Stop:
                            break;
                        default:
                            return false;
                    }
                    return rt->scene().size() < 250 && rt->scene().size() > 50 &&
                           !rt->transition().is_in_progress(rt->now_ms());
                };
                switch (s->ad_qld_sub) {
                    case 0: // let the F4 quick save finish (no dialog on
                            // Windows: the qsave flow saves right away)
                        if (story_ok() && s->ad_stage_f > 600) {
                            s->ad_cx = 640; // advance one story page (click
                            s->ad_cy = 600; // the MW area)
                            s->ad_click = true;
                            s->ad_qld_sub = 1;
                            s->ad_stage_f = 0;
                            std::printf("[qld] qsave settled; advance page\n");
                        }
                        break;
                    case 1: // page turned -> press F5 (quickload)
                        if (story_ok() && s->ad_stage_f > 200) {
                            s->ad_press_key = 116; // F5 = adv QLOAD
                            s->ad_qld_sub = 2;
                            s->ad_stage_f = 0;
                            std::printf("[qld] F5 quickload pressed\n");
                        }
                        break;
                    case 2: // quickload confirm dialog -> Enter
                        if (dlg && s->ad_stage_f > 120) {
                            s->ad_press_key = 13; // confirm the quickload
                            s->ad_qld_sub = 3;
                            s->ad_stage_f = 0;
                            std::printf("[qld] qload confirm -> Enter\n");
                        } else if (s->ad_stage_f > 4000) {
                            std::printf("[qld] VERDICT FAIL: no qload dialog\n");
                            s->quit = true;
                        }
                        break;
                    case 3: // restored story: verify rows + dock save click
                        if (s->ad_qld_armed && s->ad_stage_f > size_t(s->ad_qld_armf + 240) &&
                            (rt->scene().size() > 250 ||
                             rt->scene().find("500.bt.1.bg.0") != nullptr)) {
                            ad_qld_capture(s, "save_open_after_qload");
                            std::printf("[qld] VERDICT save screen OPEN after "
                                        "quickload (layers=%zu)\n",
                                        rt->scene().size());
                            s->ad_press_key = 27; // Esc close
                            s->ad_qld_sub = 4;
                            s->ad_qld_armed = false;
                            s->ad_qld_armf = -1;
                            s->ad_stage_f = 0;
                            break;
                        }
                        if (!story_ok()) break;
                        if (!s->ad_qld_armed) {
                            const oa::render::MessageLayer* ml =
                                rt->text().layer("1.80.mw.adv_adv");
                            std::string cur = "-";
                            if (ml)
                                for (const auto& u : ml->page)
                                    if (!u.data.empty()) {
                                        cur = u.data.substr(0, 16);
                                        break;
                                    }
                            std::printf("[qld] restored sample='%s' (saved "
                                        "'%s') rows117=%d rows116=%d rows1=%d\n",
                                        cur.c_str(), s->ad_sample_page.c_str(),
                                        (int)(rt->scene().get_input_handler(
                                                  "push", "117") != nullptr),
                                        (int)(rt->scene().get_input_handler(
                                                  "push", "116") != nullptr),
                                        (int)(rt->scene().get_input_handler(
                                                  "push", "1") != nullptr));
                            // park the pointer over the dock save button
                            // (rollover arms the FPM button cursor), then click
                            bool found = false;
                            for (const oa::render::Layer* l : rt->scene().draw_order()) {
                                const auto* h = rt->scene().find_event_handler(l->id, "click");
                                if (!h) continue;
                                const auto k = h->params.find("key");
                                if (k == h->params.end() || k->second != "bt_save") continue;
                                if (const auto r = s->oaRender->layer_world_rect(*l)) {
                                    s->ad_cx = int((*r)[0] + (*r)[2] / 2);
                                    s->ad_cy = int((*r)[1] + (*r)[3] / 2);
                                    found = true;
                                }
                                break;
                            }
                            s->ad_hover = found;
                            s->ad_qld_armed = true;
                            s->ad_qld_armf = s->ad_stage_f;
                            std::printf("[qld] hover bt_save at (%d,%d) found=%d\n",
                                        s->ad_cx, s->ad_cy, (int)found);
                        }
                        if (s->ad_qld_armed && s->ad_hover &&
                            s->ad_stage_f == size_t(s->ad_qld_armf + 100)) {
                            s->ad_click = true; // click while hovered
                            std::printf("[qld] bt_save click\n");
                        }
                        if (s->ad_stage_f > size_t(s->ad_qld_armf + 3000)) {
                            std::printf("[qld] VERDICT FAIL: save screen did not "
                                        "open after quickload (layers=%zu)\n",
                                        rt->scene().size());
                            s->quit = true;
                        }
                        break;
                    case 4: // dock config after quickload
                        if (s->ad_qld_armed && s->ad_stage_f > size_t(s->ad_qld_armf + 200) &&
                            (rt->scene().size() > 250 ||
                             rt->scene().find("500") != nullptr)) {
                            ad_qld_capture(s, "conf_open_after_qload");
                            std::printf("[qld] VERDICT config screen OPEN after "
                                        "quickload (layers=%zu)\n",
                                        rt->scene().size());
                            s->ad_press_key = 27;
                            s->ad_qld_sub = 5;
                            s->ad_qld_armed = false;
                            s->ad_qld_armf = -1;
                            s->ad_stage_f = 0;
                            break;
                        }
                        if (!story_ok()) break;
                        if (!s->ad_qld_armed) {
                            bool found = false;
                            for (const oa::render::Layer* l : rt->scene().draw_order()) {
                                const auto* h = rt->scene().find_event_handler(l->id, "click");
                                if (!h) continue;
                                const auto k = h->params.find("key");
                                if (k == h->params.end() || k->second != "bt_conf") continue;
                                if (const auto r = s->oaRender->layer_world_rect(*l)) {
                                    s->ad_cx = int((*r)[0] + (*r)[2] / 2);
                                    s->ad_cy = int((*r)[1] + (*r)[3] / 2);
                                    found = true;
                                }
                                break;
                            }
                            s->ad_hover = found;
                            s->ad_qld_armed = true;
                            s->ad_qld_armf = s->ad_stage_f;
                            std::printf("[qld] hover bt_conf at (%d,%d) found=%d\n",
                                        s->ad_cx, s->ad_cy, (int)found);
                        }
                        if (s->ad_qld_armed && s->ad_hover &&
                            s->ad_stage_f == size_t(s->ad_qld_armf + 100)) {
                            s->ad_click = true;
                            std::printf("[qld] bt_conf click\n");
                        }
                        if (s->ad_stage_f > size_t(s->ad_qld_armf + 3000)) {
                            std::printf("[qld] VERDICT FAIL: config screen did not "
                                        "open after quickload\n");
                            s->quit = true;
                        }
                        break;
                    case 5: // F6 key after quickload
                        if (s->ad_qld_armed && s->ad_stage_f > size_t(s->ad_qld_armf + 200) &&
                            (rt->scene().size() > 250 ||
                             rt->scene().find("500.bt.1.bg.0") != nullptr)) {
                            ad_qld_capture(s, "f6_save_after_qload");
                            std::printf("[qld] VERDICT F6 SAVE opens the screen "
                                        "after quickload (layers=%zu)\n",
                                        rt->scene().size());
                            s->quit = true;
                            s->ad_stage_f = 0;
                            break;
                        }
                        if (!story_ok()) break;
                        if (!s->ad_qld_armed) {
                            s->ad_qld_armed = true;
                            s->ad_qld_armf = s->ad_stage_f;
                            s->ad_press_key = 117; // F6 SAVE
                            std::printf("[qld] F6 SAVE key\n");
                        }
                        if (s->ad_stage_f > size_t(s->ad_qld_armf + 3000)) {
                            std::printf("[qld] VERDICT FAIL: F6 did not open the "
                                        "save screen after quickload\n");
                            s->quit = true;
                        }
                        break;
                    case 6: // exit frame (the evidence PNG was queued)
                        if (s->ad_stage_f > 2) s->quit = true;
                        break;
                }
                break;
            }
            if (s->ad_flow == "help") {
                // M8 phases: 0 = hold bt_conf / 1 = pointer parked away /
                // 2 = hold bt_save. ad_stage_f counts within the phase.
                const int phase = s->ad_help_phase;
                if (phase == 0 && s->ad_stage_f == 120) {
                    // move the pointer off the dock (help should clear)
                    s->ad_hover = false;
                    s->input.mouse_x = 640;
                    s->input.mouse_y = 300;
                    s->ad_help_phase = 1;
                    s->ad_stage_f = 0;
                    std::printf("[help] phase1 pointer away\n");
                    ad_help_shot(s, "away");
                    break;
                }
                if (phase == 1 && s->ad_stage_f == 40) {
                    // hover the save button
                    bool found = false;
                    for (const oa::render::Layer* l : rt->scene().draw_order()) {
                        const auto* h = rt->scene().find_event_handler(l->id, "click");
                        if (!h) continue;
                        const auto k = h->params.find("key");
                        if (k == h->params.end() || k->second != "bt_save") continue;
                        if (const auto r = s->oaRender->layer_world_rect(*l)) {
                            s->ad_cx = int((*r)[0] + (*r)[2] / 2);
                            s->ad_cy = int((*r)[1] + (*r)[3] / 2);
                            found = true;
                            std::printf("[help] phase2 hover bt_save at %s -> "
                                        "(%d,%d)\n",
                                        l->id.c_str(), s->ad_cx, s->ad_cy);
                        }
                        break;
                    }
                    s->ad_hover = found;
                    s->ad_help_phase = 2;
                    s->ad_stage_f = 0;
                    break;
                }
                if (phase == 2 && s->ad_stage_f > 120) {
                    ad_help_shot(s, "save_final");
                    std::printf("[help] done\n");
                    s->quit = true;
                    break;
                }
                const auto now = rt->text().visible_content_layers();
                if (s->ad_help_layer.empty()) {
                    for (const std::string& id : now) {
                        if (std::find(s->ad_base_text_layers.begin(),
                                      s->ad_base_text_layers.end(),
                                      id) != s->ad_base_text_layers.end())
                            continue;
                        s->ad_help_layer = id;
                        s->ad_help_seen_at = s->ad_stage_f;
                        const oa::render::MessageLayer* ml = rt->text().layer(id);
                        std::string sample;
                        if (ml)
                            for (const auto& u : ml->page)
                                if (!u.data.empty()) {
                                    sample = u.data.substr(0, 24);
                                    break;
                                }
                        std::printf("[help] HELP-LAYER seen f=%zu id='%s' "
                                    "font_size=%.1f face='%s' left=%.1f top=%.1f "
                                    "bound=%s sample='%s'\n",
                                    s->ad_stage_f, id.c_str(),
                                    ml ? ml->font.size() : -1.0,
                                    ml ? ml->font.face().c_str() : "-",
                                    ml ? ml->left : -1.0, ml ? ml->top : -1.0,
                                    rt->scene().bound_scene_id(id).c_str(),
                                    sample.c_str());
                        const std::string sid = rt->scene().bound_scene_id(id);
                        const oa::render::Layer* bn = rt->scene().find(sid);
                        if (bn) {
                            std::printf("[help] HELP-NODE %s typed left=%.1f top=%.1f "
                                        "alpha=%.2f visible=%d\n",
                                        sid.c_str(), bn->left, bn->top, bn->alpha,
                                        bn->visible ? 1 : 0);
                            oa::render::Affine2 wt;
                            if (rt->scene().world_transform(sid, &wt)) {
                                std::printf("[help] HELP-NODE world=(%.1f,%.1f) "
                                            "text_world_top=%.1f (ml.top=%.1f) "
                                            "text_world_left=%.1f (ml.left=%.1f)\n",
                                            wt.e, wt.f, wt.f + ml->top,
                                            ml->top, wt.e + ml->left, ml->left);
                            }
                            for (const char* anc :
                                 {"1.80.mw.bt.dc", "1.80.mw", "1.80", "1"}) {
                                oa::render::Affine2 aw;
                                if (rt->scene().world_transform(anc, &aw))
                                    std::printf("[help] ANC %s world=(%.1f,%.1f)\n",
                                                anc, aw.e, aw.f);
                            }
                        }
                        ad_help_shot(s, "help_on");
                        std::printf("[help] frame glyphs=%zu (help should add ~5)\n",
                                    s->oaRender->last_frame_glyphs());
                        break;
                    }
                }
                if (!s->ad_help_layer.empty() && s->ad_stage_f % 25 == 0 &&
                    (s->ad_help_phase == 0 || s->ad_help_phase == 2)) {
                    ad_help_shot(s, s->ad_help_phase == 0 ? "hold" : "save_hold");
                }
                break;
            }
            if (s->ad_flow == "conf") {
                // L2 M10b probe: config page hover-help (500-z family) and the
                // text-speed-slider preview death. Sub states:
                //  0 settle / 1 hover bt_exit / 2 open page2 / 3 wait for the
                //  preview text / 4 hover sl0201 / 5 drag the thumb / 6 watch.
                switch (s->ad_conf_sub) {
                    case 0: { // config screen (page 1) open and settled
                        // NekoMiko config adds ~50 layers to a ~54-layer story
                        // scene (108 total < the FPM-oriented 150 gate), so
                        // the bt_exit row is the open-screen test that works
                        // for both games.
                        if (!ad_has_handler(rt, "bt_exit")) break;
                        if (s->ad_stage_f < 120) break;
                        s->ad_conf_ev0 = rt->text_events();
                        ad_conf_report(s, "conf_open");
                        s->ad_conf_sub = 1;
                        s->ad_stage_f = 0;
                        std::printf("[conf] sub1: hover bt_exit (plain button)\n");
                        break;
                    }
                    case 1: { // hover a plain page-1 button; its uihelp should
                               // print (text_events delta) yet end blank.
                        if (s->ad_stage_f == 1) { // first frame inside this sub
                            bool found = false;
                            for (const oa::render::Layer* l :
                                 rt->scene().draw_order()) {
                                const auto* h =
                                    rt->scene().find_event_handler(l->id, "click");
                                if (!h) continue;
                                const auto k = h->params.find("key");
                                if (k == h->params.end() || k->second != "bt_exit")
                                    continue;
                                if (const auto r =
                                        s->oaRender->layer_world_rect(*l)) {
                                    s->ad_cx = int((*r)[0] + (*r)[2] / 2);
                                    s->ad_cy = int((*r)[1] + (*r)[3] / 2);
                                    found = true;
                                    std::printf("[conf] hover bt_exit at %s -> "
                                                "(%d,%d)\n",
                                                l->id.c_str(), s->ad_cx, s->ad_cy);
                                }
                                break;
                            }
                            s->ad_hover = found;
                            s->ad_conf_ev0 = rt->text_events();
                            s->ad_conf_pd0 = rt->pointer_dispatch_count();
                        }
                        if (s->ad_stage_f == 100) {
                            ad_conf_report(s, "help_bt_exit");
                            s->ad_hover = false;
                            s->input.mouse_x = 640;
                            s->input.mouse_y = 300;
                            s->ad_conf_sub = 2;
                            s->ad_stage_f = 0;
                            std::printf("[conf] sub2: open the text page\n");
                        }
                        break;
                    }
                    case 2: { // click the left page-2 tab -> text settings
                        if (s->ad_stage_f == 20) ad_arm_click_key(s, "page2");
                        if (s->ad_stage_f == 240) {
                            if (!ad_has_handler(rt, "sl0201")) {
                                std::printf("[conf] page2 (sl0201) never appeared\n");
                                s->quit = true;
                                break;
                            }
                            s->ad_conf_sub = 3;
                            s->ad_stage_f = 0;
                            std::printf("[conf] sub3: wait for the preview text\n");
                        }
                        break;
                    }
                    case 3: { // wait until the preview page shows text
                        const oa::render::MessageLayer* ml =
                            rt->text().layer("500.z.sample");
                        const bool alive =
                            ml && ml->char_count > 0 && !ml->reveal_pending &&
                            ml->reveal_index >= ml->char_count &&
                            rt->scene().is_message_layer_visible("500.z.sample");
                        if (alive) {
                            s->ad_conf_ev0 = rt->text_events();
                            ad_conf_report(s, "preview_before");
                            s->ad_conf_sub = 4;
                            s->ad_stage_f = 0;
                            std::printf("[conf] preview alive; sub4 hover sl0201\n");
                        } else if (s->ad_stage_f > 900) {
                            std::printf("[conf] preview never became visible\n");
                            s->quit = true;
                        }
                        break;
                    }
                    case 4: { // hover the sl0201 track (2/3 along, off the
                               // thumb) — the same uihelp chain as bt_exit.
                        if (s->ad_stage_f == 1) { // first frame inside this sub
                            bool found = false;
                            for (const oa::render::Layer* l :
                                 rt->scene().draw_order()) {
                                const auto* h =
                                    rt->scene().find_event_handler(l->id, "click");
                                if (!h) continue;
                                const auto k = h->params.find("key");
                                if (k == h->params.end() || k->second != "sl0201")
                                    continue;
                                if (const auto r =
                                        s->oaRender->layer_world_rect(*l)) {
                                    s->ad_cx = int((*r)[0] + (*r)[2] / 3);
                                    s->ad_cy = int((*r)[1] + (*r)[3] / 2);
                                    found = true;
                                    std::printf("[conf] hover sl0201 at %s -> "
                                                "(%d,%d)\n",
                                                l->id.c_str(), s->ad_cx, s->ad_cy);
                                }
                                break;
                            }
                            s->ad_hover = found;
                            s->ad_conf_ev0 = rt->text_events();
                            s->ad_conf_pd0 = rt->pointer_dispatch_count();
                        }
                        if (s->ad_stage_f == 120) {
                            ad_conf_report(s, "help_slider");
                            s->ad_hover = false;
                            s->input.mouse_x = 640;
                            s->input.mouse_y = 300;
                            s->ad_conf_sub = 5;
                            s->ad_stage_f = 0;
                            std::printf("[conf] sub5: drag the thumb left\n");
                        }
                        break;
                    }
                    case 5: { // drag the sl0201 thumb left (each frame of the
                               // drag calls config_textex through p4).
                        if (s->ad_stage_f == 1) { // first frame inside this sub
                            int sx = 0, sy = 0;
                            if (ad_find_drag_layer(s, "sl0201", &sx, &sy)) {
                                ad_arm_drag(s, sx, sy, sx - 90, sy, 20);
                                s->ad_conf_ev0 = rt->text_events();
                                s->ad_conf_sub = 6;
                                s->ad_stage_f = 0;
                                std::printf("[conf] sub6: watch the preview\n");
                            } else {
                                std::printf("[conf] sl0201 thumb layer not found\n");
                                s->quit = true;
                            }
                        }
                        break;
                    }
                    case 6: { // post-drag: a live loop reprints within ~2-4 s;
                               // a dead one stays blank forever.
                        if (s->ad_stage_f == 90) ad_conf_report(s, "post_drag_1s");
                        if (s->ad_stage_f == 360) {
                            ad_conf_report(s, "post_drag_6s");
                            const oa::render::MessageLayer* ml =
                                rt->text().layer("500.z.sample");
                            const bool alive =
                                ml && !ml->page.empty() && ml->char_count > 0 &&
                                !ml->reveal_pending &&
                                ml->reveal_index >= ml->char_count;
                            std::printf("[conf] VERDICT preview %s after the "
                                        "speed-slider change\n",
                                        alive ? "ALIVE" : "DEAD/BLANK");
                            s->quit = true;
                        }
                        break;
                    }
                }
                break;
            }
            if (s->ad_flow == "d38") {
                // story dock probe — dock-click vs F6 open path
                // (media/SE/transition timing), pin/hover, edge tabs.
                auto d38_where = [&](const char* tag) {
                    const auto& au = rt->audio();
                    std::string se_ids, sv_ids;
                    for (const auto& [k, ch] : au.state().se_channels)
                        if (ch.playing) {
                            if (!se_ids.empty()) se_ids += ",";
                            se_ids += k;
                        }
                    for (const auto& [k, ch] : au.state().voice_channels)
                        if (ch.playing) {
                            if (!sv_ids.empty()) sv_ids += ",";
                            sv_ids += k;
                        }
                    const auto* w = rt->current_wait();
                    std::printf(
                        "[d38] %s f=%llu layers=%zu wait=%d text_ev=%zu "
                        "media_ev=%zu pd=%zu se[%s] svo[%s] tr=%d "
                        "save01=%d voice=%d lock=%d\n",
                        tag, (unsigned long long)s->frames, rt->scene().size(),
                        w ? (int)w->kind : -1, rt->text_events(),
                        rt->media_events(), rt->pointer_dispatch_count(),
                        se_ids.c_str(), sv_ids.c_str(),
                        rt->transition().is_in_progress(rt->now_ms()) ? 1 : 0,
                        ad_has_handler(rt, "bt_save01") ? 1 : 0,
                        ad_has_handler(rt, "bt_voice") ? 1 : 0,
                        ad_has_handler(rt, "bt_lock") ? 1 : 0);
                };
                switch (s->ad_d38_sub) {
                    case 0: { // settle the parked story
                        if (s->ad_d38_f++ < 300) break;
                        s->ad_d38_f = 0;
                        s->ad_d38_sub = 1;
                        d38_where("baseline");
                        std::printf("[d38] P1 dock bt_save path\n");
                        break;
                    }
                    case 1: { // hover + click the dock bt_save; trace
                        if (ad_has_handler(rt, "bt_save01") && s->ad_d38_ons1 < 0) {
                            s->ad_d38_ons1 = s->ad_d38_f;
                            std::printf("[d38] save01 at f=%d (dock save open)\n",
                                        s->ad_d38_f);
                        }
                        if (s->ad_d38_f == 4) ad_png_shot(s, "s1dock");
                        if (s->ad_d38_ons1 >= 0) {
                            const int d = s->ad_d38_f - s->ad_d38_ons1;
                            if (d >= 0 && d <= 28 && d % 4 == 0)
                                ad_png_shot(s, "s1tr");
                        }
                        if (ad_has_handler(rt, "bt_save")) {
                            for (const oa::render::Layer* l :
                                 rt->scene().draw_order()) {
                                const auto* h =
                                    rt->scene().find_event_handler(l->id, "click");
                                if (!h) continue;
                                const auto kk = h->params.find("key");
                                if (kk == h->params.end() || kk->second != "bt_save")
                                    continue;
                                if (const auto r =
                                        s->oaRender->layer_world_rect(*l)) {
                                    s->ad_cx = int((*r)[0] + (*r)[2] / 2);
                                    s->ad_cy = int((*r)[1] + (*r)[3] / 2);
                                    s->ad_hover = true;
                                }
                                break;
                            }
                            if (s->ad_d38_f == 10) {
                                s->ad_click = true;
                                std::printf("[d38] dock bt_save clicked "
                                            "(%d,%d)\n",
                                            s->ad_cx, s->ad_cy);
                            }
                            if (s->ad_d38_f < 60 && s->ad_d38_f % 2 == 0)
                                d38_where("dock");
                            if (s->ad_d38_f > 200) {
                                s->ad_hover = false;
                                s->ad_d38_sub = 2;
                                s->ad_d38_f = 0;
                                std::printf("[d38] dock path done; closing\n");
                            }
                        } else if (s->ad_d38_f++ > 2000) {
                            std::printf("[d38] no dock bt_save\n");
                            s->quit = true;
                        }
                        ++s->ad_d38_f;
                        break;
                    }
                    case 2: { // close the save screen (bt_close), back to story
                        if (s->ad_d38_f == 30) ad_arm_click_key(s, "bt_close");
                        if (ad_story_parked(rt) && s->ad_d38_f > 200) {
                            s->ad_d38_sub = 3;
                            s->ad_d38_f = 0;
                            std::printf("[d38] story back; F6 path\n");
                        }
                        if (s->ad_d38_f++ > 6000) {
                            std::printf("[d38] close stalled\n");
                            s->quit = true;
                        }
                        break;
                    }
                    case 3: { // F6 key path with the same trace
                        if (ad_has_handler(rt, "bt_save01") && s->ad_d38_ons3 < 0) {
                            s->ad_d38_ons3 = s->ad_d38_f;
                            std::printf("[d38] save01 at f=%d (F6 open)\n",
                                        s->ad_d38_f);
                        }
                        if (s->ad_d38_ons3 >= 0) {
                            const int d = s->ad_d38_f - s->ad_d38_ons3;
                            if (d >= 0 && d <= 28 && d % 4 == 0)
                                ad_png_shot(s, "s3tr");
                        }
                        if (s->ad_d38_f == 20) {
                            s->ad_press_key = 117; // F6
                            std::printf("[d38] F6 pressed\n");
                        }
                        if (s->ad_d38_f < 60 && s->ad_d38_f % 2 == 0)
                            d38_where("key");
                        if (s->ad_d38_f > 200) {
                            s->ad_d38_sub = 4;
                            s->ad_d38_f = 0;
                            std::printf("[d38] F6 path done; closing\n");
                        }
                        ++s->ad_d38_f;
                        break;
                    }
                    case 4: { // close, then unpin + hover strip
                        if (s->ad_d38_f == 30) ad_arm_click_key(s, "bt_close");
                        if (ad_story_parked(rt) && s->ad_d38_f > 200) {
                            s->ad_d38_sub = 5;
                            s->ad_d38_f = 0;
                            std::printf("[d38] P2 unpin (bt_lock)\n");
                        }
                        if (s->ad_d38_f++ > 6000) {
                            std::printf("[d38] close2 stalled\n");
                            s->quit = true;
                        }
                        break;
                    }
                    case 5: { // click bt_lock (unpin), observe dock
                        if (s->ad_d38_f == 40) {
                            ad_arm_click_key(s, "bt_lock");
                            std::printf("[d38] bt_lock clicked (unpin)\n");
                        }
                        if (s->ad_d38_f == 120) d38_where("unpinned");
                        if (s->ad_d38_f == 140) {
                            s->ad_d38_sub = 6;
                            s->ad_d38_f = 0;
                            std::printf("[d38] hover bottom strip x=200\n");
                        }
                        ++s->ad_d38_f;
                        break;
                    }
                    case 6: { // hover the bottom strip (dockarea zone)
                        const int xs[] = {200, 640, 1050, 640};
                        const int stage = s->ad_d38_f / 90;
                        const int x = xs[stage < 4 ? stage : 3];
                        s->input.mouse_x = x;
                        s->input.mouse_y = 695;
                        if (s->ad_d38_f % 15 == 0)
                            d38_where("strip");
                        ++s->ad_d38_f;
                        if (s->ad_d38_f >= 360) {
                            s->ad_d38_sub = 7;
                            s->ad_d38_f = 0;
                            std::printf("[d38] P2 strip hover done\n");
                        }
                        break;
                    }
                    case 7: { // tblt/tbrt toggle state machine probe.
                        // Stage every 90 frames: record geometry, then click
                        // one containment; cycle tblt,tblt,tbrt,tbrt,tblt,tbrt
                        // to walk right-park -> visible -> left-park ->
                        // visible -> right-park -> ...
                        const int stage = s->ad_d38_f / 90;
                        const char* seq[] = {"tblt", "tblt", "tbrt",
                                             "tbrt", "tblt", "tbrt"};
                        const int at = s->ad_d38_f % 90;
                        if (at == 5 && stage < 6) {
                            // geometry: container/bg left edge + containment
                            // centers (may be offscreen)
                            auto rect = [&](const char* id) -> std::string {
                                const oa::render::Layer* l =
                                    rt->scene().find(id);
                                if (!l) return "absent";
                                const auto r =
                                    s->oaRender->layer_world_rect(*l);
                                if (!r) return "nosize";
                                char buf[96];
                                std::snprintf(buf, sizeof(buf), "x0=%.0f cx=%.0f",
                                              (*r)[0], (*r)[0] + (*r)[2] / 2);
                                return buf;
                            };
                            std::printf("[d38] tab7 s%d %s=%s tb=%s zmask=%s "
                                        "t17=%s t18=%s\n",
                                        stage, seq[stage], seq[stage],
                                        rect("1.80.mw.tb.0").c_str(),
                                        rect("1.80.mw.zmask").c_str(),
                                        rect("1.80.mw.tb.17").c_str(),
                                        rect("1.80.mw.tb.18").c_str());
                        }
                        if (at == 10 && stage < 6) {
                            ad_arm_click_key(s, seq[stage]);
                            std::printf("[d38] tab7 click %s\n", seq[stage]);
                        }
                        if (s->ad_d38_f % 15 == 0) {
                            d38_where("tab");
                            if (const oa::render::Layer* l =
                                    rt->scene().find("1.80.mw.tb.0")) {
                                if (const auto r =
                                        s->oaRender->layer_world_rect(*l))
                                    std::printf("[d38] tb.x0=%.0f\n", (*r)[0]);
                            }
                        }
                        if (s->ad_d38_f >= 6 * 90 + 60) {
                            std::printf("[d38] DONE\n");
                            s->quit = true;
                        }
                        ++s->ad_d38_f;
                        break;
                    }
                }
                break;
            }
            if (s->ad_flow == "r10t") {
                // R10d: title round trips under the VM-kept [reset] (boot
                // re-run): story -> config bt_title -> title (round N return)
                // -> config from the title -> dock tip geometry dump + PNG.
                switch (s->ad_t_sub) {
                    case 0: { // walk 2 story pages, then open the config
                        if (s->ad_t_f == 0) {
                            s->input.mouse_x = 640;
                            s->input.mouse_y = 600;
                            s->ad_click = true;
                        }
                        if (s->ad_t_f++ > 4000) {
                            std::printf("[r10t] story walk stalled\n");
                            s->quit = true;
                        }
                        if (s->ad_t_round == 0 && ad_story_parked(rt)) {
                            s->ad_t_round = 1; // walked once
                        }
                        if (s->ad_t_round >= 1 && ad_story_parked(rt)) {
                            s->ad_press_key = 121; // F10 config
                            s->ad_t_sub = 1;
                            s->ad_t_f = 0;
                            std::printf("[r10t] round%d F10 config\n",
                                        s->ad_t_round);
                        }
                        break;
                    }
                    case 1: { // wait in-game config, click bt_title
                        if (rt->scene().size() <= 150 ||
                            !ad_has_handler(rt, "bt_title"))
                            break;
                        if (s->ad_t_f++ < 240) break;
                        ad_arm_click_key(s, "bt_title");
                        s->ad_t_sub = 2;
                        s->ad_t_f = 0;
                        std::printf("[r10t] round%d clicked bt_title\n",
                                    s->ad_t_round);
                        break;
                    }
                    case 2: { // dialog confirm -> title
                        if (rt->scene().find("600.1.bt.1.0") != nullptr) {
                            if (s->ad_t_dialog_at < 0) {
                                s->ad_t_dialog_at = s->ad_t_f;
                            } else if (s->ad_t_f - s->ad_t_dialog_at > 240) {
                                s->ad_press_key = 13;
                                s->ad_t_dialog_at = -2;
                                std::printf("[r10t] confirm Enter\n");
                            }
                        }
                        const auto* w = rt->current_wait();
                        if (ad_wait_stop(w) && ad_has_handler(rt, "bt_start")) {
                            s->ad_t_sub = 3;
                            s->ad_t_f = 0;
                            s->ad_t_dialog_at = -1;
                            std::printf("[r10t] round%d at title (reset %d)\n",
                                        s->ad_t_round, s->ad_t_round);
                        }
                        if (s->ad_t_f++ > 20000) {
                            std::printf("[r10t] never reached title\n");
                            s->quit = true;
                        }
                        break;
                    }
                    case 3: { // settle, open the config from the title
                        if (s->ad_t_f++ < 600) break;
                        ad_arm_click_key(s, "bt_config");
                        s->ad_t_sub = 4;
                        s->ad_t_f = 0;
                        std::printf("[r10t] round%d title -> bt_config\n",
                                    s->ad_t_round);
                        break;
                    }
                    case 4: { // measure the config-from-title tip
                        if (!ad_has_handler(rt, "bt1011")) {
                            if (s->ad_t_f++ > 9000) {
                                std::printf("[r10t] config never opened\n");
                                s->quit = true;
                            }
                            break;
                        }
                        if (s->ad_t_f == 120) {
                            s->ad_hover = true;
                            for (const oa::render::Layer* l :
                                 rt->scene().draw_order()) {
                                const auto* h =
                                    rt->scene().find_event_handler(l->id, "click");
                                if (!h) continue;
                                const auto k = h->params.find("key");
                                if (k == h->params.end() || k->second != "bt1011")
                                    continue;
                                if (const auto r =
                                        s->oaRender->layer_world_rect(*l)) {
                                    s->ad_cx = int((*r)[0] + (*r)[2] / 2);
                                    s->ad_cy = int((*r)[1] + (*r)[3] / 2);
                                }
                                break;
                            }
                            std::printf("[r10t] round%d hover bt1011\n",
                                        s->ad_t_round);
                        }
                        if (s->ad_t_f == 320) {
                            ad_tip_dump(s, rt, s->ad_t_round == 1 ? "r1" : "r2");
                        }
                        if (s->ad_t_f > 420) {
                            s->ad_hover = false;
                            s->input.mouse_x = 640;
                            s->input.mouse_y = 400;
                            s->ad_t_sub = 5;
                            s->ad_t_f = 0;
                            std::printf("[r10t] round%d measured; closing\n",
                                        s->ad_t_round);
                        }
                        ++s->ad_t_f;
                        break;
                    }
                    case 5: { // close (bt_close or right button) -> title
                        if (s->ad_t_f == 60 && !ad_has_handler(rt, "bt_close")) {
                            s->input.key_down_edges.push_back(2); // rclick
                        } else if (s->ad_t_f == 60) {
                            ad_arm_click_key(s, "bt_close");
                        }
                        const auto* w = rt->current_wait();
                        if (ad_wait_stop(w) && ad_has_handler(rt, "bt_start")) {
                            if (s->ad_t_round >= 2) {
                                std::printf("[r10t] DONE rounds=2\n");
                                s->quit = true;
                                break;
                            }
                            // round 2: start the game again, then return
                            s->ad_t_round = 2;
                            s->ad_t_f = 0;
                            s->ad_t_sub = 6;
                            std::printf("[r10t] round 2 start\n");
                            break;
                        }
                        if (s->ad_t_f++ > 12000) {
                            std::printf("[r10t] close stalled\n");
                            s->quit = true;
                        }
                        break;
                    }
                    case 6: { // round 2: はじめから -> story -> config bt_title
                        if (s->ad_t_f == 120) {
                            s->input.mouse_x = 640;
                            s->input.mouse_y = 600;
                            s->ad_click = true; // click bt_start area first
                        }
                        if (s->ad_t_f == 180) ad_arm_click_key(s, "bt_start");
                        if (ad_story_parked(rt) && s->ad_t_f > 400) {
                            s->ad_press_key = 121;
                            s->ad_t_sub = 1;
                            s->ad_t_f = 0;
                            std::printf("[r10t] round2 story; F10\n");
                            break;
                        }
                        if (s->ad_t_f++ > 20000) {
                            std::printf("[r10t] round2 start stalled\n");
                            s->quit = true;
                        }
                        break;
                    }
                }
                break;
            }
            if (s->ad_flow == "r10blog") {
                // Wait for the backlog content page, dump rows per page, and
                // flip pages with the UI's own bt_pageup button. The .2 sub
                // rows' top = the Lua get_message_layer_height outcome (the
                // barrier discriminator).
                switch (s->ad_blog_sub) {
                    case 10: { // wait for the backlog page content
                        const oa::render::MessageLayer* ml =
                            rt->text().layer("500.z.bt.tx.1.1");
                        if (++s->ad_blog_f < 300) break;
                        if (!ml || ml->page.empty()) {
                            if (s->ad_blog_f > 9000) {
                                std::printf("[blog] backlog never opened\n");
                                s->quit = true;
                            }
                            break;
                        }
                        s->ad_blog_sub = 11;
                        s->ad_blog_f = 0;
                        std::printf("[blog] backlog page up\n");
                        break;
                    }
                    case 11: { // dump this page, then flip to the next
                        ++s->ad_blog_f;
                        if (s->ad_blog_f == 90) {
                            ad_blog_dump(s, rt, s->ad_blog_page);
                            ++s->ad_blog_page;
                            if (s->ad_blog_page >= 3) {
                                std::printf("[blog] DONE pages=%d\n",
                                            s->ad_blog_page);
                                s->quit = true;
                                break;
                            }
                            ad_arm_click_key(s, "bt_pageup");
                            s->ad_blog_f = 0;
                            break;
                        }
                        if (s->ad_blog_f < 90) break;
                        s->ad_blog_f = 0;
                        break;
                    }
                }
                break;
            }
            if (s->ad_flow == "r10" || s->ad_flow == "r10save") {
                // stress: wait for the config/save screen, learn each click
                // target's resting help position, then alternate the pointer
                // between targets at OA_R10_DWELL frames per hover and flag
                // any frame where the dock help text top leaves its band.
                const bool r10_save_ui = s->ad_flow == "r10save";
                s->r10_help = r10_save_ui ? "500.help" : "500.z.help";
                const bool r10_open_ok = r10_save_ui
                                             ? (rt->scene().size() > 150 &&
                                                ad_has_handler(rt, "bt_save01"))
                                             : (rt->scene().size() > 150 &&
                                                ad_has_handler(rt, "bt_exit"));
                if (!r10_open_ok) break; // screen still opening
                auto r10_sample = [&]() -> bool {
                    const oa::render::MessageLayer* ml =
                        rt->text().layer(s->r10_help.c_str());
                    if (!ml) return false;
                    s->r10_units = ml->page.size();
                    if (s->r10_units == 0) return false;
                    const std::string sid =
                        rt->scene().bound_scene_id(s->r10_help);
                    const oa::render::Layer* bn = rt->scene().find(sid);
                    if (!bn) return false;
                    oa::render::Affine2 wt;
                    if (!rt->scene().world_transform(sid, &wt)) return false;
                    s->r10_hid = "500.z.help";
                    s->r10_ml_top = ml->top;
                    s->r10_node_top = bn->top;
                    s->r10_world_f = wt.f;
                    s->r10_font = ml->font.size();
                    s->r10_spacetop = ml->font.spacetop();
                    s->r10_face = ml->font.face();
                    return true;
                };
                switch (s->r10_sub) {
                    case 0: { // settle the screen entrance animation
                        if (!s->r10_seed_key.empty()) {
                            const std::string key = s->r10_seed_key;
                            s->r10_seed_key.clear();
                            for (const oa::render::Layer* l :
                                 rt->scene().draw_order()) {
                                const auto* h = rt->scene().find_event_handler(
                                    l->id, "click");
                                if (!h) continue;
                                const auto k = h->params.find("key");
                                if (k == h->params.end() || k->second != key)
                                    continue;
                                if (const auto r =
                                        s->oaRender->layer_world_rect(*l)) {
                                    s->ad_cx = int((*r)[0] + (*r)[2] / 2);
                                    s->ad_cy = int((*r)[1] + (*r)[3] / 2);
                                    s->ad_click = true;
                                    std::printf("[r10] seed click %s @"
                                                "(%d,%d)\n",
                                                key.c_str(), s->ad_cx,
                                                s->ad_cy);
                                }
                                break;
                            }
                            break;
                        }
                        if (s->r10_phase++ < 240) break;
                        if (r10_save_ui && !s->r10_save_seed1) {
                            // Hover help on the save screen exists only over
                            // FILLED slots (uihelp_saveload prints slot data).
                            // Seed two slots through the real UI chain.
                            s->r10_save_seed1 = true;
                            s->r10_phase = 0;
                            s->r10_seed_key = "bt_save01";
                            break;
                        }
                        if (r10_save_ui && s->r10_save_seed1 &&
                            !s->r10_save_seed2) {
                            s->r10_save_seed2 = true;
                            s->r10_phase = 0;
                            s->r10_seed_key = "bt_save02";
                            break;
                        }
                        if (r10_save_ui && s->r10_save_seed2 &&
                            !s->r10_seed_key.empty()) {
                            s->r10_phase = 0;
                            break; // seed click armed; wait for the save flow
                        }
                        std::map<std::string, std::string> seen;
                        for (const oa::render::Layer* l : rt->scene().draw_order()) {
                            const auto* h =
                                rt->scene().find_event_handler(l->id, "click");
                            if (!h) continue;
                            const auto k = h->params.find("key");
                            const std::string key = k == h->params.end()
                                                        ? std::string()
                                                        : k->second;
                            if (key.empty() || seen.count(key)) continue;
                            seen[key] = l->id;
                            if (const auto r = s->oaRender->layer_world_rect(*l)) {
                                AppState::R10Target t;
                                t.key = key;
                                t.layer = l->id;
                                t.cx = int((*r)[0] + (*r)[2] / 2);
                                t.cy = int((*r)[1] + (*r)[3] / 2);
                                s->r10_t.push_back(t);
                            }
                        }
                        std::printf("[r10] targets=%zu (%s screen)\n",
                                    s->r10_t.size(),
                                    r10_save_ui ? "save" : "config");
                        s->r10_sub = 1;
                        s->r10_phase = 0;
                        s->r10_learn_i = 0;
                        s->r10_learn_f = 0;
                        s->r10_learn_acc = 0;
                        s->r10_learn_n = 0;
                        break;
                    }
                    case 1: { // learn each target's resting text top T
                        if (s->r10_learn_i >= (int)s->r10_t.size()) {
                            for (int i = 0; i < (int)s->r10_t.size(); ++i)
                                if (s->r10_t[i].ref > 100.0)
                                    s->r10_stable.push_back(i);
                            if (!s->r10_pair.empty()) {
                                const std::string a = s->r10_pair.substr(
                                    0, s->r10_pair.find(','));
                                const std::string b = s->r10_pair.substr(
                                    s->r10_pair.find(',') + 1);
                                std::vector<int> keep;
                                for (int i : s->r10_stable) {
                                    const std::string& k = s->r10_t[i].key;
                                    if (k == a || k == b) keep.push_back(i);
                                }
                                std::printf("[r10] pair filter '%s','%s' -> %zu\n",
                                            a.c_str(), b.c_str(), keep.size());
                                s->r10_stable = keep;
                            }
                            std::printf("[r10] stable=%zu\n",
                                        s->r10_stable.size());
                            if (s->r10_stable.size() < 2) {
                                std::printf("[r10] too few stable targets\n");
                                s->quit = true;
                                break;
                            }
                            s->r10_sub = 2;
                            s->r10_phase = 0;
                            s->input.mouse_x = 640;
                            s->input.mouse_y = 300;
                            break;
                        }
                        AppState::R10Target& tg = s->r10_t[s->r10_learn_i];
                        s->input.mouse_x = tg.cx;
                        s->input.mouse_y = tg.cy;
                        ++s->r10_learn_f;
                        if (r10_sample()) {
                            const double T = s->r10_world_f + s->r10_ml_top;
                            if (s->r10_learn_f > 7) {
                                s->r10_learn_acc += T;
                                ++s->r10_learn_n;
                            }
                        }
                        if (s->r10_learn_f >= 12) {
                            if (s->r10_learn_n >= 3)
                                tg.ref = s->r10_learn_acc / double(s->r10_learn_n);
                            std::printf("[r10] ref %-12s layer=%-28s @(%d,%d) "
                                        "T=%.1f ml_top=%.1f world=%.1f "
                                        "spacetop=%.1f font=%.1f units=%zu\n",
                                        tg.key.c_str(), tg.layer.c_str(), tg.cx,
                                        tg.cy, tg.ref, s->r10_ml_top,
                                        s->r10_world_f, s->r10_spacetop,
                                        s->r10_font, s->r10_units);
                            s->r10_learn_acc = 0;
                            s->r10_learn_n = 0;
                            s->r10_learn_f = 0;
                            ++s->r10_learn_i;
                            if (s->r10_learn_i > 45) {
                                std::printf("[r10] learn truncated at 45\n");
                                s->r10_learn_i = (int)s->r10_t.size();
                            }
                        }
                        break;
                    }
                    case 2: { // stress: rapid alternation + per-frame check
                        if (s->r10_dwell <= 0 && s->r10_cur >= 0 &&
                            s->r10_alternations >= s->r10_alt_max) {
                            std::printf("[r10] DONE alternations=%d anomalies=%d "
                                        "units=%zu T=%.1f\n",
                                        s->r10_alternations, s->r10_anomalies,
                                        s->r10_units,
                                        s->r10_world_f + s->r10_ml_top);
                            s->quit = true;
                            break;
                        }
                        if (s->r10_dwell <= 0) {
                            // switch to the next random target
                            const int n = (int)s->r10_stable.size();
                            const int idx =
                                s->r10_stable[s->r10_seed % (unsigned)n];
                            s->r10_seed =
                                s->r10_seed * 1103515245u + 12345u;
                            const AppState::R10Target& tg = s->r10_t[idx];
                            s->r10_cur = idx;
                            s->r10_refT = tg.ref;
                            s->r10_dwell = s->r10_dwell_fixed;
                            s->r10_grace = 8;
                            ++s->r10_alternations;
                            if (s->r10_alternations % 100 == 1)
                                std::printf("[r10] alt %d -> %s @(%d,%d)\n",
                                            s->r10_alternations,
                                            tg.key.c_str(), tg.cx, tg.cy);
                        }
                        const AppState::R10Target& tg =
                            s->r10_t[s->r10_cur];
                        s->input.mouse_x = tg.cx;
                        s->input.mouse_y = tg.cy;
                        --s->r10_dwell;
                        ++s->r10_phase;
                        if (!r10_sample()) {
                            s->r10_grace = 8; // slot empty mid-transition
                            break;
                        }
                        const double T = s->r10_world_f + s->r10_ml_top;
                        // trace: first 120 sampled frames verbatim, then
                        // only value changes (state-jitter characterization).
                        if (s->r10_trace_left > 0) {
                            --s->r10_trace_left;
                            std::printf("[r10] T alt=%d key=%-10s T=%.1f "
                                        "world=%.1f units=%zu spacetop=%.1f\n",
                                        s->r10_alternations, tg.key.c_str(), T,
                                        s->r10_world_f, s->r10_units,
                                        s->r10_spacetop);
                        } else if (std::fabs(T - s->r10_lastT) > 4.0 ||
                                   s->r10_units != s->r10_last_units) {
                            std::printf("[r10] T! alt=%d key=%-10s T=%.1f "
                                        "world=%.1f units=%zu\n",
                                        s->r10_alternations, tg.key.c_str(), T,
                                        s->r10_world_f, s->r10_units);
                        }
                        s->r10_lastT = T;
                        s->r10_last_units = s->r10_units;
                        bool bad = false;
                        if (s->r10_grace > 0) {
                            --s->r10_grace;
                        } else if (T < 200.0 && s->r10_refT > 200.0) {
                            bad = true; // far up the screen (visible symptom)
                        } else if (std::fabs(T - s->r10_refT) >
                                   std::max(4.0, s->r10_refT * 0.008)) {
                            // any >~6px vertical excursion of the dock
                            // help text is the reported symptom class.
                            bad = true;
                        }
                        if (bad) {
                            ++s->r10_anomalies;
                            std::printf("[r10] ANOM alt=%d key=%s T=%.1f "
                                        "ref=%.1f node_top=%.1f world_f=%.1f "
                                        "ml_top=%.1f spacetop=%.1f font=%.1f "
                                        "face='%s' units=%zu\n",
                                        s->r10_alternations, tg.key.c_str(), T,
                                        s->r10_refT, s->r10_node_top,
                                        s->r10_world_f, s->r10_ml_top,
                                        s->r10_spacetop, s->r10_font,
                                        s->r10_face.c_str(), s->r10_units);
                            if (s->r10_png && !s->ad_out.empty()) {
                                oa::media::Image img;
                                if (s->oaRender->snapshot_renderer(img) &&
                                    img.w > 0 && img.h > 0) {
                                    char path[512];
                                    std::snprintf(path, sizeof(path),
                                                  "%s/r10_anom_%02d_alt%05d.png",
                                                  s->ad_out.c_str(),
                                                  s->r10_anomalies,
                                                  s->r10_alternations);
                                    const std::vector<uint8_t> png =
                                        oa::runtime::encode_png(
                                            uint32_t(img.w), uint32_t(img.h),
                                            img.rgba);
                                    if (!png.empty()) {
                                        FILE* f = std::fopen(path, "wb");
                                        if (f) {
                                            std::fwrite(png.data(), 1,
                                                        png.size(), f);
                                            std::fclose(f);
                                        }
                                    }
                                }
                            }
                            if (s->r10_anomalies > 12) {
                                std::printf("[r10] too many anomalies; stop\n");
                                s->quit = true;
                            }
                        }
                        if (s->r10_png && !s->ad_out.empty() &&
                            s->r10_phase % 600 == 250) {
                            oa::media::Image img;
                            if (s->oaRender->snapshot_renderer(img) &&
                                img.w > 0 && img.h > 0) {
                                char path[512];
                                std::snprintf(path, sizeof(path),
                                              "%s/r10_hold_%05d.png",
                                              s->ad_out.c_str(),
                                              s->r10_phase);
                                const std::vector<uint8_t> png =
                                    oa::runtime::encode_png(
                                        uint32_t(img.w), uint32_t(img.h),
                                        img.rgba);
                                if (!png.empty()) {
                                    FILE* f = std::fopen(path, "wb");
                                    if (f) {
                                        std::fwrite(png.data(), 1, png.size(),
                                                    f);
                                        std::fclose(f);
                                    }
                                }
                            }
                        }
                        break;
                    }
                }
                break;
            }
            if (s->ad_flow == "exit") {
                if (rt->scene().size() <= 150) break;
                if (rt->scene().find("600.1.bt.1.0") != nullptr) {
                    // the exit confirmation dialog opened directly
                    s->ad_stage = 3;
                    s->ad_stage_f = 0;
                    break;
                }
                for (const oa::render::Layer* l : rt->scene().draw_order()) {
                    const auto* h = rt->scene().find_event_handler(l->id, "click");
                    if (!h) continue;
                    const auto k = h->params.find("key");
                    if (k == h->params.end() || k->second != "bt_end") continue;
                    ad_arm_click_key(s, "bt_end");
                    s->ad_stage = 3;
                    s->ad_stage_f = 0;
                    std::printf("[ad] stage3 bt_end clicked\n");
                    break;
                }
                break;
            }
            // title flow: F12 dialog
            if (rt->scene().find("600.1.bt.1.0") == nullptr) break;
            s->ad_stage = 3;
            s->ad_stage_f = 0;
            break;
        }
        case 3: { // dialog up: settle the entrance, then confirm (Enter)
            const oa::runtime::WaitReason* w = rt->current_wait();
            const bool parked = w && w->kind == oa::runtime::WaitReason::Kind::Stop;
            const bool dlg = rt->scene().find("600.1.bt.1.0") != nullptr;
            if (!dlg || !parked) break;
            if (s->ad_stage_f < 240) break; // dialog open anime + settle
            // Mouse-autocursor evidence (exit/title confirm dialog): Lua's
            // mouse_autocursor (dialog.lua yesno_active -> adv.lua) dispatches
            // its 10-step [mouse] pointer-warp requests while the dialog
            // opens; the host warps the OS cursor (SDL motion feedback) and,
            // where the host cannot warp (hidden window / headless), the
            // runtime emulates the move — the engine pointer therefore rests
            // exactly on the last requested warp target. Verify: >=10 warps
            // requested and the engine pointer on that target (+/-1px).
            // Hard-fail only under OA_AD_MOUSE_ASSERT=1.
            if (s->ad_flow == "exit" || s->ad_flow == "title") {
                const size_t warp_n = rt->pointer_warp_requests();
                if (warp_n < 10 && s->ad_stage_f < 240 + 360) break;
                if (!s->ad_dlg_ev) {
                    s->ad_dlg_ev = true;
                    const auto mp = rt->mouse_point();
                    const bool on_target =
                        std::abs(mp.first - s->ad_warp_x) <= 1 &&
                        std::abs(mp.second - s->ad_warp_y) <= 1;
                    std::printf("[ad] dlg-mouse warps=%zu ptr=(%d,%d) "
                                "warp_target=(%d,%d) on_target=%d\n",
                                warp_n, mp.first, mp.second, s->ad_warp_x,
                                s->ad_warp_y, on_target ? 1 : 0);
                    if (std::getenv("OA_AD_MOUSE_ASSERT") &&
                        !(warp_n >= 10 && on_target)) {
                        std::fprintf(stderr,
                                     "[ad] FAIL: confirm-dialog mouse warp "
                                     "missing (warps=%zu on_target=%d)\n",
                                     warp_n, on_target ? 1 : 0);
                        std::exit(1);
                    }
                }
            }
            s->ad_press_key = 13; // Enter = CLICK on the auto-cursor YES
            s->ad_stage = 4;
            s->ad_stage_f = 0;
            s->ad_dlg_frame = s->frames;
            std::printf("[ad] stage4 confirm Enter\n");
            break;
        }
        case 4: { // tail: measure every rendered frame; end on flow outcome
            const bool fresh = s->ad_rendered;
            s->ad_rendered = false;
            if (fresh) {
                const double mean = s->ad_mean_luma;
                const uint64_t bright = s->ad_bright_pixels;
                const bool residue = mean < 18.0 && bright > 200;
                if (residue) {
                    ++s->ad_dark_bright_frames;
                    if (s->ad_dark_bright_frames <= 4) {
                        // diagnostics: which message layers would draw, where
                        // their bound node sits, and the draw tail
                        const auto order = s->rt->scene().draw_order();
                        std::string diag;
                        for (const std::string& mid :
                             s->rt->text().visible_content_layers()) {
                            const std::string bid = s->rt->scene().bound_scene_id(mid);
                            const oa::render::Layer* bn = s->rt->scene().find(bid);
                            std::string pos = "no-node";
                            if (bn) {
                                for (size_t i = 0; i < order.size(); ++i) {
                                    if (order[i]->id == bid) {
                                        char buf[32];
                                        std::snprintf(buf, sizeof(buf), "#%zu", i);
                                        pos = buf;
                                        break;
                                    }
                                }
                            }
                            // M7: 槽位判定是结构性的(is_message_slot),不再查
                            // 消息前缀字符串。
                            const bool ov = s->rt->scene().is_message_slot(bid);
                            char b2[64];
                            std::snprintf(b2, sizeof(b2), "%svis=%d", pos.c_str(),
                                          (int)s->rt->scene().is_message_layer_visible(mid));
                            if (!diag.empty()) diag += " ; ";
                            diag += mid + "@" + bid + (ov ? "(ov)" : "") + "[" +
                                    std::string(b2) + "]";
                        }
                        std::string tail;
                        for (size_t i = order.size() > 5 ? order.size() - 5 : 0;
                             i < order.size(); ++i) {
                            if (!tail.empty()) tail += " | ";
                            tail += order[i]->id;
                        }
                        // anchor ancestry of the story message at this frame
                        std::string anc;
                        for (const char* p : {"1", "1.80", "1.80.mw", "1.80.mw.adv_adv"})
                            anc += std::string(p) + "=" +
                                   (s->rt->scene().find(p) ? "Y" : "n") + " ";
                        std::string first12;
                        for (size_t i = 0; i < order.size() && i < 12; ++i) {
                            if (!first12.empty()) first12 += ",";
                            first12 += order[i]->id;
                        }
                        std::printf("[ad] RESIDUE f=%llu mean=%.1f bright=%llu "
                                    "msgs=[%s] anc[%s] first=[%s] tail=[%s]\n",
                                    (unsigned long long)s->frames, mean,
                                    (unsigned long long)bright, diag.c_str(), anc.c_str(),
                                    first12.c_str(), tail.c_str());
                    }
                    if (s->ad_dark_bright_frames <= 12 && !s->ad_out.empty()) {
                        char path[512];
                        std::snprintf(path, sizeof(path), "%s/res_%03llu.ppm",
                                      s->ad_out.c_str(),
                                      (unsigned long long)s->ad_dark_bright_frames);
                        oa::media::Image img;
                        if (s->oaRender->snapshot_renderer(img))
                            write_ppm(path, img);
                    }
                }
            }
            const bool done =
                s->ad_flow == "exit"
                    ? rt->exit_requested()
                    : (ad_wait_stop(rt->current_wait()) && ad_has_handler(rt, "bt_start"));
            if (s->ad_flow == "title" && done) std::printf("[ad] title restored\n");
            if (!done && s->ad_stage_f < 9000) break;
            std::printf(
                "[ad] %s DONE sampled=%llu dark_bright_frames=%llu "
                "mean=%.1f bright=%llu page='%s'\n",
                s->ad_flow.c_str(), (unsigned long long)s->ad_sampled_frames,
                (unsigned long long)s->ad_dark_bright_frames, s->ad_mean_luma,
                (unsigned long long)s->ad_bright_pixels, s->ad_sample_page.c_str());
            s->quit = true;
            break;
        }
        default:
            break;
    }
    return s->quit;
}
