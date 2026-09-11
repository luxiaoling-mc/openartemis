// research/100 — engine-level tests for the `_m` mask-partner semantics:
// when a playing video file has a same-directory `<stem>_m.<ext>` sibling,
// the engine plays the pair as "main picture + mask": the main's RGB is
// drawn verbatim while its visibility (luma-key alpha) is modulated by the
// sibling's gray — the video-domain equivalent of the image-mask convention
// (texture_for_masked: out.a = file.a * mask-gray / 255).
//
// Asset-free: two tiny synthetic Ogg/Theora clips (test-time encode with
// libtheoraenc, tests/theora_fixture.h) stand in for the data class —
// white-content-on-black-canvas strips, like the snll sakura pair the
// research/95 census documented (there: 1920x1080 white petals; the _m is
// the frame-locked hard-core version).
//
// Geometry (32x32 frames, white blocks on black canvas):
//   main clip: 12x12 white square at phase offset (ox,oy): p0=(6,6),
//              p1=(8,6), p2=(8,8) — 3 frames, 30 fps.
//   mask clip:  6x6 white square inset by +3 on each side of the main's,
//              same phases (a "core" mask ⊂ main, like sakura_m ⊂ sakura).
//   => composited frame: opaque white core (main & mask overlap), white RGB
//      with ~zero alpha in the ring band (main only), black canvas keyed to
//      alpha 0.
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/media/decode_pool.h"
#include "core/media/video.h"
#include "theora_fixture.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

constexpr int kW = 32;
constexpr int kH = 32;
constexpr int kPhases = 3;
constexpr int kOx[kPhases] = {6, 8, 8};
constexpr int kOy[kPhases] = {6, 6, 8};
constexpr int kMainSize = 12; // main white square edge
constexpr int kMaskInset = 3; // mask is the main's square inset by 3

/// One synthetic clip: `phase` blocks of white (255,255,255) at the phase
/// geometry on black; frames are pure black/white (chroma neutral).
std::vector<uint8_t> white_block_frames(int size, int inset, int frames) {
    std::vector<uint8_t> all;
    all.reserve(size_t(kW) * kH * 4 * size_t(frames));
    for (int f = 0; f < frames; ++f) {
        std::vector<uint8_t> rgba(size_t(kW) * kH * 4, 0);
        const int ox = kOx[f] + inset;
        const int oy = kOy[f] + inset;
        for (int y = oy; y < oy + size && y < kH; ++y) {
            for (int x = ox; x < ox + size && x < kW; ++x) {
                uint8_t* p = &rgba[(size_t(y) * kW + size_t(x)) * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = 255;
            }
        }
        all.insert(all.end(), rgba.begin(), rgba.end());
    }
    return all;
}

using Bytes = std::optional<std::vector<uint8_t>>;

/// Loader host: returns the mapped bytes for a logical name; records every
/// name the engine asked for (detection probe evidence). VideoEngine copies
/// the loader (std::function), so the log is a shared_ptr kept across the
/// copies.
struct AssetHost {
    std::map<std::string, std::vector<uint8_t>> assets;
    std::shared_ptr<std::vector<std::string>> probe_log =
        std::make_shared<std::vector<std::string>>();
    Bytes operator()(const std::string& f) {
        probe_log->push_back(f);
        const auto it = assets.find(f);
        if (it == assets.end()) return std::nullopt;
        return it->second;
    }
};

bool probe_was_asked(const AssetHost& host, const std::string& name) {
    for (const auto& n : *host.probe_log)
        if (n == name) return true;
    return false;
}

const uint8_t* frame_px(const oa::media::VideoEngine& v, const std::string& id,
                        int* rev_out = nullptr) {
    int w = 0, h = 0;
    const uint8_t* px = nullptr;
    uint64_t rev = 0;
    if (!v.video_frame(id, &w, &h, &px, &rev) || !px) return nullptr;
    if (rev_out) *rev_out = int(rev);
    return px;
}

/// Aggregate per-frame mask evidence for one channel frame: counts of
/// opaque-core pixels (alpha >= 200), visible pixels (alpha >= 128), the
/// bboxes of the visible core and of the white (luma >= 150) main content.
struct Stats {
    int core = 0;        // alpha >= 200
    int vis = 0;         // alpha >= 128
    int ring_white = 0;  // white rgb pixels (luma >= 150) with alpha <= 40
    int core_l = -1, core_t = -1, core_r = -1, core_b = -1;
    int white_l = -1, white_t = -1, white_r = -1, white_b = -1;
    int canvas_alpha_max = 0; // max alpha outside any white content
    int white_rgb_min_alpha = 255; // min alpha over white rgb pixels
};
Stats stats_of(const uint8_t* px, int w, int h) {
    Stats s;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = px + (size_t(y) * w + size_t(x)) * 4;
            const int lum = (int(p[0]) * 77 + int(p[1]) * 150 + int(p[2]) * 29 + 128) >> 8;
            const bool white = lum >= 150;
            if (p[3] >= 200) {
                ++s.core;
                if (s.core_l < 0) { s.core_l = s.core_r = x; s.core_t = s.core_b = y; }
                if (x < s.core_l) s.core_l = x;
                if (x > s.core_r) s.core_r = x;
                if (y < s.core_t) s.core_t = y;
                if (y > s.core_b) s.core_b = y;
            }
            if (p[3] >= 128) ++s.vis;
            if (white) {
                if (s.white_l < 0) { s.white_l = s.white_r = x; s.white_t = s.white_b = y; }
                if (x < s.white_l) s.white_l = x;
                if (x > s.white_r) s.white_r = x;
                if (y < s.white_t) s.white_t = y;
                if (y > s.white_b) s.white_b = y;
                if (p[3] < s.white_rgb_min_alpha) s.white_rgb_min_alpha = p[3];
                if (p[3] <= 40) ++s.ring_white;
            } else if (p[3] > s.canvas_alpha_max) {
                s.canvas_alpha_max = p[3];
            }
        }
    }
    return s;
}

void expect_phase_stats(const char* tag, const Stats& s, int phase) {
    // Expected geometry of this phase: main square [ox, ox+12), mask square
    // [ox+3, ox+9) — the visible core is the mask square (alpha >= 200 over
    // its interior), the white rgb region is the main square, and the ring
    // band (main-only white) must be white-but-invisible.
    const int ox = kOx[phase], oy = kOy[phase];
    std::printf("[%s] phase=%d core_bbox=(%d,%d)-(%d,%d) n=%d vis=%d "
                "white_bbox=(%d,%d)-(%d,%d) ring_white=%d white_min_a=%d "
                "canvas_a=%d\n",
                tag, phase, s.core_l, s.core_t, s.core_r, s.core_b, s.core,
                s.vis, s.white_l, s.white_t, s.white_r, s.white_b, s.ring_white,
                s.white_rgb_min_alpha, s.canvas_alpha_max);
    // Core is the mask square (6x6) — allow ±1 px for codec edge softening.
    check(s.core_l >= ox + 2 && s.core_t >= oy + 2 &&
              s.core_r <= ox + 10 && s.core_b <= oy + 10,
          "core region inside the mask square");
    check(s.core >= 6 * 4, "core region at least 4x6 px (solid center)");
    // The white rgb region is the main square (12x12), drawn verbatim.
    check(s.white_l >= ox - 1 && s.white_t >= oy - 1 &&
              s.white_r <= ox + kMainSize && s.white_b <= oy + kMainSize,
          "white region inside the main square");
    check(s.white_r - s.white_l + 1 >= kMainSize - 2 &&
              s.white_b - s.white_t + 1 >= kMainSize - 2,
          "white region spans ~the main square");
    // Ring band: main-white pixels outside the mask are drawn at full white
    // but gated to ~transparent (rgb 照绘 + mask off) — the decisive
    // signature of the composite.
    check(s.ring_white >= 8, "ring band: white rgb pixels with alpha<=40");
    check(s.white_rgb_min_alpha >= 0, "white pixels keep some alpha");
    // Canvas: black keyed transparent.
    check(s.canvas_alpha_max <= 20, "canvas alpha <= 20 (black keyed out)");
}

void build_pair(AssetHost& host, const std::string& main_name,
                const std::string& mask_name) {
    std::vector<std::vector<uint8_t>> main_frames, mask_frames;
    for (int f = 0; f < kPhases; ++f) {
        std::vector<uint8_t> fm(size_t(kW) * kH * 4, 0);
        for (int y = kOy[f]; y < kOy[f] + kMainSize; ++y)
            for (int x = kOx[f]; x < kOx[f] + kMainSize; ++x) {
                uint8_t* p = &fm[(size_t(y) * kW + size_t(x)) * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = 255;
            }
        main_frames.push_back(std::move(fm));
        std::vector<uint8_t> fx(size_t(kW) * kH * 4, 0);
        for (int y = kOy[f] + kMaskInset; y < kOy[f] + kMaskInset + (kMainSize - 2 * kMaskInset);
             ++y)
            for (int x = kOx[f] + kMaskInset;
                 x < kOx[f] + kMaskInset + (kMainSize - 2 * kMaskInset); ++x) {
                uint8_t* p = &fx[(size_t(y) * kW + size_t(x)) * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = 255;
            }
        mask_frames.push_back(std::move(fx));
    }
    host.assets[main_name] = oafix::encode_theora_ogv(kW, kH, main_frames);
    host.assets[mask_name] = oafix::encode_theora_ogv(kW, kH, mask_frames);
}

} // namespace

int main() {
    try {
        // ------------------------------------------------------------------
        // 0. zero-impact baseline: no `_m` sibling — the channel stays on
        //    the unmasked pipeline (mask_on false, frames alpha 255).
        // ------------------------------------------------------------------
        {
            AssetHost host;
            std::vector<std::vector<uint8_t>> frames;
            for (int f = 0; f < kPhases; ++f) {
                std::vector<uint8_t> fm(size_t(kW) * kH * 4, 0);
                for (int y = kOy[f]; y < kOy[f] + kMainSize; ++y)
                    for (int x = kOx[f]; x < kOx[f] + kMainSize; ++x) {
                        uint8_t* p = &fm[(size_t(y) * kW + size_t(x)) * 4];
                        p[0] = p[1] = p[2] = 255;
                        p[3] = 255;
                    }
                frames.push_back(std::move(fm));
            }
            host.assets["movie/plain.ogv"] = oafix::encode_theora_ogv(kW, kH, frames);
            oa::media::VideoEngine v;
            v.set_loader(host);
            oa::media::VideoConfig cfg;
            cfg.file = "movie/plain.ogv";
            v.play_layer("mv", cfg);
            const auto st = v.state();
            check(st.video_layers.at("mv").decoded, "0: decoded");
            check(!st.video_layers.at("mv").mask_on, "0: no mask without sibling");
            check(probe_was_asked(host, "movie/plain_m.ogv"),
                  "0: the sibling name was probed once");
            int rev = 0;
            const uint8_t* px = frame_px(v, "mv", &rev);
            check(px && rev == 1, "0: frame 0 current");
            int alpha_non255 = 0;
            if (px) {
                for (int i = 3; i < kW * kH * 4; i += 4)
                    if (px[i] != 255) ++alpha_non255;
            }
            check(alpha_non255 == 0, "0: unmasked frame alpha stays 255 (byte-identical)");
        }

        // ------------------------------------------------------------------
        // 1. layer path: pair plays with composite alpha; frame stepping
        //    keeps the 1:1 index lock through all three phases; EOF finish.
        // ------------------------------------------------------------------
        {
            AssetHost host;
            build_pair(host, "movie/sakura.ogv", "movie/sakura_m.ogv");
            oa::media::VideoEngine v;
            v.set_loader(host);
            oa::media::VideoConfig cfg;
            cfg.file = "movie/sakura.ogv";
            cfg.loop_play = false;
            v.play_layer("500.z.mv", cfg);
            check(v.state().video_layers.at("500.z.mv").mask_on,
                  "1: mask partner detected on the layer channel");
            check(probe_was_asked(host, "movie/sakura_m.ogv"),
                  "1: sibling name derived as movie/sakura_m.ogv");

            int rev = 0;
            const uint8_t* px = frame_px(v, "500.z.mv", &rev);
            check(px && rev == 1, "1: frame 0 composited at play");
            if (px) expect_phase_stats("layer", stats_of(px, kW, kH), 0);

            // Two 40 ms ticks pull frames 1 and 2 (30 fps => 33.3 ms).
            for (int tick = 0; tick < 2; ++tick) {
                v.update(40);
                const auto evs = v.poll_finish_events();
                check(evs.empty(), "1: no finish before EOF");
            }
            px = frame_px(v, "500.z.mv", &rev);
            check(px && rev == 3, "1: frame 2 (third frame) current");
            if (px) expect_phase_stats("layer", stats_of(px, kW, kH), 2);

            // EOF on the next tick: one finish event, channel retired.
            v.update(40);
            const auto evs = v.poll_finish_events();
            check(!v.is_layer_playing("500.z.mv"), "1: EOF ended the channel");
            check(evs.size() == 1 && evs[0].id == "500.z.mv",
                  "1: one layer finish event at EOF");
        }

        // ------------------------------------------------------------------
        // 2. overlay path: the same pair through play_overlay shares the
        //    composite (both routes run one engine-level formula).
        // ------------------------------------------------------------------
        {
            AssetHost host;
            build_pair(host, "movie/sakura.ogv", "movie/sakura_m.ogv");
            oa::media::VideoEngine v;
            v.set_loader(host);
            oa::media::VideoConfig cfg;
            cfg.file = "movie/sakura.ogv";
            cfg.loop_play = false;
            v.play_overlay(cfg);
            check(v.state().overlay_video && v.state().overlay_video->mask_on,
                  "2: mask partner detected on the overlay channel");
            int rev = 0;
            const uint8_t* px = frame_px(v, "", &rev);
            check(px && rev == 1, "2: overlay frame 0 composited");
            if (px) expect_phase_stats("overlay", stats_of(px, kW, kH), 0);
            v.stop_overlay();
        }

        // ------------------------------------------------------------------
        // 3. loop semantics: an equal-length pair wraps together — after the
        //    main's EOF restart the mask restarts with it, so phase 0 comes
        //    back exactly (the core again sits at the phase-0 geometry).
        // ------------------------------------------------------------------
        {
            AssetHost host;
            build_pair(host, "loop.ogv", "loop_m.ogv");
            oa::media::VideoEngine v;
            v.set_loader(host);
            oa::media::VideoConfig cfg;
            cfg.file = "loop.ogv";
            cfg.loop_play = true;
            v.play_layer("mv", cfg);
            int rev0 = 0;
            const uint8_t* p0 = frame_px(v, "mv", &rev0);
            const Stats s0 = stats_of(p0, kW, kH);
            expect_phase_stats("loop", s0, 0);
            // Tick past EOF (3 frames @30 fps: EOF due at ~100 ms) and two
            // frames into the second lap: 4 x 40 ms = 160 ms => phase 1 of
            // lap 2 (rev 5).
            for (int tick = 0; tick < 4; ++tick) {
                v.update(40);
                v.poll_finish_events();
                check(v.is_layer_playing("mv"), "3: loop keeps playing");
            }
            int rev = 0;
            const uint8_t* px = frame_px(v, "mv", &rev);
            check(px && rev == 5, "3: loop wrapped; second-lap frame 1");
            if (px) {
                const Stats s = stats_of(px, kW, kH);
                expect_phase_stats("loop", s, 1); // phase-1 geometry again
            }
            // A third lap boundary must land phase 0 again with the exact
            // phase-0 core bbox (mask restarted with the main).
            for (int tick = 0; tick < 2; ++tick) {
                v.update(40);
                v.poll_finish_events();
            }
            px = frame_px(v, "mv", &rev);
            check(px && rev == 7, "3: third lap frame 0");
            if (px) {
                const Stats s = stats_of(px, kW, kH);
                check(s.core_l == s0.core_l && s.core_t == s0.core_t &&
                          s.core_r == s0.core_r && s.core_b == s0.core_b,
                      "3: mask core bbox after the wrap equals phase 0");
            }
            v.stop_layer("mv");
        }

        // ------------------------------------------------------------------
        // 4. decode-pool pipe path: the worker decodes the main ahead (and
        //    restarts its loop silently); the caller still steps the mask
        //    one frame per delivered frame. Real-time smoke: sample over
        //    several loop periods and check the alignment invariant — the
        //    visible core must sit exactly at main-white inset by 3 (mask
        //    offset), i.e. mask phase == main phase at every sample.
        // ------------------------------------------------------------------
        {
            AssetHost host;
            build_pair(host, "loop.ogv", "loop_m.ogv");
            oa::media::DecodePool pool(1);
            oa::media::VideoEngine v;
            v.set_loader(host);
            v.set_decode_pool(&pool);
            oa::media::VideoConfig cfg;
            cfg.file = "loop.ogv";
            cfg.loop_play = true;
            v.play_layer("mv", cfg);
            check(v.state().video_layers.at("mv").mask_on,
                  "4: pipe path detects the mask partner");
            for (int i = 0; i < 100 && !v.driver_active(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            check(v.driver_active(), "4: driver running (pool present)");
            bool saw_ok = false;
            bool bad = false;
            int samples = 0;
            int last_rev = 0;
            for (int i = 0; i < 60 && !bad; ++i) { // ~6 s of loop at most
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                int rev = 0;
                const uint8_t* px = frame_px(v, "mv", &rev);
                if (!px) continue;
                if (rev > last_rev) {
                    last_rev = rev;
                    const Stats s = stats_of(px, kW, kH);
                    if (s.core_l >= 0 && s.white_l >= 0) {
                        ++samples;
                        const int off_l = s.core_l - s.white_l;
                        const int off_t = s.core_t - s.white_t;
                        if (off_l < 2 || off_l > 4 || off_t < 2 || off_t > 4) {
                            bad = true;
                            std::fprintf(stderr,
                                         "FAIL: 4: mask/main phase drift: core "
                                         "offset (%d,%d) (sample rev=%d)\n",
                                         off_l, off_t, rev);
                        }
                        if (s.canvas_alpha_max <= 20 && s.ring_white >= 4) saw_ok = true;
                    }
                }
                if (last_rev >= 12) break; // >= 3 full laps through the pipe
            }
            check(!bad, "4: mask/main stay phase-locked through the pipe path");
            check(last_rev >= 8, "4: pipe delivered past two loop wraps");
            check(saw_ok, "4: composite signatures seen in pipe-delivered frames");
            v.stop_all_videos();
            pool.stop();
        }

        // ------------------------------------------------------------------
        // 5. size-mismatched mask is ignored (image-mask convention).
        // ------------------------------------------------------------------
        {
            AssetHost host;
            build_pair(host, "a.ogv", "a_m.ogv");
            // Overwrite the sibling with a different-size clip.
            std::vector<std::vector<uint8_t>> tiny;
            for (int f = 0; f < kPhases; ++f) {
                std::vector<uint8_t> mf(16 * 16 * 4, 0);
                for (int i = 3; i < 16 * 16 * 4; i += 4) mf[size_t(i)] = 255;
                tiny.push_back(std::move(mf));
            }
            host.assets["a_m.ogv"] = oafix::encode_theora_ogv(16, 16, tiny);
            oa::media::VideoEngine v;
            v.set_loader(host);
            oa::media::VideoConfig cfg;
            cfg.file = "a.ogv";
            v.play_layer("mv", cfg);
            check(!v.state().video_layers.at("mv").mask_on,
                  "5: mismatched mask ignored (mask_on false)");
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "EXC: %s\n", ex.what());
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "video_mask_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("video_mask_test: all ok\n");
    return 0;
}
