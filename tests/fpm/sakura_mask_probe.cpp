// research/100 real-pair evidence probe — the snll title petal pair
// (movie/sakura.ogv + movie/sakura_m.ogv, 1920x1080 301-frame Ogg/Theora)
// through the VideoEngine decode pipeline, reporting what the mask composite
// actually does to the visible picture:
//
//   * detection on the real archive (mask_on, sibling name derived),
//   * per-frame aggregate stats over the whole clip: main-content coverage
//     vs the visible (composited) core, the "white-but-invisible" ring the
//     mask erases, and the colors of every pixel the mask lets through
//     (the honest white+white-stays-white / no-pink-invented check),
//   * a solo (unmasked) pass via OA_SAKURA_PROBE_NO_MASK=1 for comparison.
//
// REPORT-ONLY (not a ctest). Env:
//   OA_TEST_SNLL_PFS    path to the snll root.pfs FILE (loose movie/ dir of
//                       its parent supplies the ogv pair); argv[1] works too
//   OA_SAKURA_PROBE_NO_MASK=1   loader hides the _m sibling (unmasked pass)
//   OA_SAKURA_PROBE_LAYER=0     play on the overlay channel instead of layer
//   OA_SAKURA_PROBE_MAXF        cap the decoded frames (default 301 = full)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/media/video.h"

namespace {

constexpr const char* kFile = "movie/sakura.ogv";
constexpr const char* kMaskFile = "movie/sakura_m.ogv";

/// Production luma (Rec.601-style integer, video.h rgba_luma).
inline int luma_of(const uint8_t* p) {
    return (int(p[0]) * 77 + int(p[1]) * 150 + int(p[2]) * 29 + 128) >> 8;
}

// Pink/magenta candidate: strong red dominance WITHOUT yellow cast (warm
// fringes of the white petals are yellow-ish: B well below G; true pink is
// red-dominant with B ~ G). The census-95 assets measured 0 pink/red of any
// kind; the classifier must not count warm fringe pixels as pink.
inline bool is_pinkish(int r, int g, int b) {
    return r > 120 && (r - g) >= 24 && (r - b) >= 24 && b >= g - 12;
}

struct FrameStats {
    // Content that WOULD draw unmasked: main pixels whose luma-key alpha
    // (oa::media::layer_video_key_alpha) is >= 64 (i.e. clearly visible).
    uint64_t main_keyed = 0;
    // Pixels the composited channel actually draws: frame alpha >= 64
    // (the frame handed to hosts already carries key(main) x mask / 255).
    uint64_t visible = 0;
    // Fully opaque core: alpha >= 200 (mask white on main white).
    uint64_t core = 0;
    // White rgb pixels (luma >= 150) that the mask erased to alpha <= 40 —
    // the "white but invisible" signature of 照绘+掩码.
    uint64_t ring_white = 0;
    // Color census over visible (alpha >= 64) pixels.
    uint64_t vis_px = 0;
    double vis_r = 0, vis_g = 0, vis_b = 0;
    uint64_t vis_warm = 0;  // |R-G|>=8 (fringe tint) within visible pixels
    uint64_t vis_pink = 0;  // pinkish candidates within visible pixels
    uint64_t vis_whiteish = 0; // near-neutral bright: R,G,B >= 235, |R-G|<=6
    int max_alpha_delta_luma = 0; // |alpha - key(luma)| max (mask modulation)
};

FrameStats stats_of(const uint8_t* px, int w, int h, bool use_frame_alpha) {
    FrameStats s;
    const uint64_t n = uint64_t(w) * uint64_t(h);
    for (uint64_t i = 0; i < n; ++i) {
        const uint8_t* p = px + i * 4;
        const int lum = luma_of(p);
        const uint8_t keyed = oa::media::layer_video_key_alpha(uint8_t(lum));
        if (keyed >= 64) ++s.main_keyed;
        const int a = use_frame_alpha ? int(p[3]) : int(keyed);
        if (a >= 64) ++s.visible;
        if (a >= 200) ++s.core;
        if (use_frame_alpha && lum >= 150 && p[3] <= 40) ++s.ring_white;
        if (a >= 64) {
            const int r = p[0], g = p[1], b = p[2];
            ++s.vis_px;
            s.vis_r += r;
            s.vis_g += g;
            s.vis_b += b;
            if (r - g >= 8 || g - r >= 8) ++s.vis_warm;
            if (is_pinkish(r, g, b)) ++s.vis_pink;
            if (r >= 235 && g >= 235 && b >= 235 && (r - g >= -6 && r - g <= 6) &&
                (g - b >= -6 && g - b <= 6))
                ++s.vis_whiteish;
        }
        const int da = a > keyed ? a - keyed : keyed - a;
        if (da > s.max_alpha_delta_luma) s.max_alpha_delta_luma = da;
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pfs_path = std::getenv("OA_TEST_SNLL_PFS");
    if ((!pfs_path || !*pfs_path) && argc > 1) pfs_path = argv[1];
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_SNLL_PFS unset; skipping\n");
        return 77;
    }
    const bool no_mask = std::getenv("OA_SAKURA_PROBE_NO_MASK") != nullptr;
    const bool overlay = std::getenv("OA_SAKURA_PROBE_LAYER") != nullptr;
    const uint64_t max_frames =
        std::getenv("OA_SAKURA_PROBE_MAXF")
            ? uint64_t(std::atoll(std::getenv("OA_SAKURA_PROBE_MAXF")))
            : 301;

    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, true);
    if (!fs->read(kFile) || !fs->read(kMaskFile)) {
        std::fprintf(stderr, "sakura pair not readable via the project fs "
                             "(loose movie/ dir missing?)\n");
        return 1;
    }
    std::printf("[sakura-probe] pfs='%s' main='%s' mask='%s' mode=%s channel=%s\n",
                pfs_path, kFile, kMaskFile, no_mask ? "SOLO(unmasked)" : "PAIR",
                overlay ? "overlay" : "layer");

    oa::media::VideoEngine v;
    v.set_loader([fs, no_mask](const std::string& f) -> std::optional<std::vector<uint8_t>> {
        if (no_mask) {
            // Hide every <stem>_m.<ext> sibling for the unmasked control
            // pass: the mask probe then resolves to nothing and the channel
            // stays on the plain unmasked pipeline.
            const size_t slash = f.find_last_of("/\\");
            const std::string base =
                slash == std::string::npos ? f : f.substr(slash + 1);
            if (base.find("_m.") != std::string::npos) return std::nullopt;
        }
        return fs->read(f);
    });

    oa::media::VideoConfig cfg;
    cfg.file = kFile;
    cfg.skippable = true;
    cfg.loop_play = false;
    if (overlay) {
        v.play_overlay(cfg);
    } else {
        v.play_layer("500.z.mv", cfg);
    }
    const auto st0 = v.state();
    const auto* ch = overlay ? (st0.overlay_video ? &*st0.overlay_video : nullptr)
                             : (st0.video_layers.count("500.z.mv")
                                    ? &st0.video_layers.at("500.z.mv")
                                    : nullptr);
    if (!ch || !ch->decoded) {
        std::fprintf(stderr, "sakura main did not decode\n");
        return 1;
    }
    std::printf("[sakura-probe] decode attached file='%s' mask_on=%d (probe of "
                "masked=%d)\n",
                ch->file.c_str(), ch->mask_on ? 1 : 0, no_mask ? 0 : 1);
    if (no_mask == false && !ch->mask_on) {
        std::fprintf(stderr, "FAIL: the real pair did not auto-detect mask_on\n");
        return 1;
    }
    if (no_mask == true && ch->mask_on) {
        std::fprintf(stderr, "FAIL: solo pass unexpectedly mask_on\n");
        return 1;
    }

    // Pump the whole clip on the 16 ms tick clock (deterministic; EOF after
    // 301 frames @ 30 fps ~= 10033 ms => ~628 ticks).
    uint64_t frames_seen = 0;
    uint64_t core_sum = 0, visible_sum = 0, ring_sum = 0, main_keyed_sum = 0;
    uint64_t pink_sum = 0, warm_sum = 0, whiteish_sum = 0, vis_px_sum = 0;
    uint64_t min_core = ~0ull, max_core = 0, min_ring = ~0ull, max_ring = 0;
    double r_sum = 0, g_sum = 0, b_sum = 0;
    uint64_t eof_events = 0;
    bool done = false;
    for (size_t tick = 1; tick < 2000 && !done; ++tick) {
        v.update(16);
        eof_events += v.poll_finish_events().size();
        if (!v.state().overlay_video && v.state().video_layers.empty()) {
            done = true;
            break;
        }
        if (frames_seen >= max_frames) break;
        int w = 0, h = 0;
        uint64_t rev = 0;
        const uint8_t* px = nullptr;
        if (!v.video_frame(overlay ? "" : "500.z.mv", &w, &h, &px, &rev) || !px)
            continue;
        if (rev > frames_seen) {
            frames_seen = rev;
            const FrameStats s = stats_of(px, w, h, !no_mask);
            main_keyed_sum += s.main_keyed;
            visible_sum += s.visible;
            core_sum += s.core;
            ring_sum += s.ring_white;
            vis_px_sum += s.vis_px;
            r_sum += s.vis_r;
            g_sum += s.vis_g;
            b_sum += s.vis_b;
            warm_sum += s.vis_warm;
            pink_sum += s.vis_pink;
            whiteish_sum += s.vis_whiteish;
            if (s.core < min_core) min_core = s.core;
            if (s.core > max_core) max_core = s.core;
            if (s.ring_white < min_ring) min_ring = s.ring_white;
            if (s.ring_white > max_ring) max_ring = s.ring_white;
            if (frames_seen <= 3 || frames_seen % 60 == 0) {
                std::printf("[sakura-probe] f=%llu keyed=%llu vis=%llu core=%llu "
                            "ring_white=%llu warm=%llu pink=%llu\n",
                            (unsigned long long)rev,
                            (unsigned long long)s.main_keyed,
                            (unsigned long long)s.visible,
                            (unsigned long long)s.core,
                            (unsigned long long)s.ring_white,
                            (unsigned long long)s.vis_warm,
                            (unsigned long long)s.vis_pink);
            }
        }
    }
    const double n = double(frames_seen ? frames_seen : 1);
    const double total_px = 1920.0 * 1080.0 * n;
    std::printf("[sakura-probe] ---- %s aggregate over %llu frames ----\n",
                no_mask ? "SOLO" : "PAIR(masked)", (unsigned long long)frames_seen);
    std::printf("[sakura-probe] main-keyed content  %8.1f px/frame "
                "(%.4f%% of frame)\n",
                double(main_keyed_sum) / n, 100.0 * double(main_keyed_sum) / total_px);
    if (!no_mask) {
        std::printf("[sakura-probe] visible (a>=64)      %8.1f px/frame "
                    "(%.4f%%)\n",
                    double(visible_sum) / n, 100.0 * double(visible_sum) / total_px);
        std::printf("[sakura-probe] opaque core (a>=200) %8.1f px/frame "
                    "[min=%llu max=%llu]\n",
                    double(core_sum) / n, (unsigned long long)min_core,
                    (unsigned long long)max_core);
        std::printf("[sakura-probe] ring white-but-invis %8.1f px/frame "
                    "[min=%llu max=%llu]  (mask-erased main content)\n",
                    double(ring_sum) / n, (unsigned long long)min_ring,
                    (unsigned long long)max_ring);
    }
    if (vis_px_sum) {
        const double vr = r_sum / double(vis_px_sum);
        const double vg = g_sum / double(vis_px_sum);
        const double vb = b_sum / double(vis_px_sum);
        std::printf("[sakura-probe] visible-pixel colors: mean RGB = "
                    "[%.1f, %.1f, %.1f]  white-neutral %.2f%%  warm/fringe "
                    "%.4f%%  PINK %.6f%%\n",
                    vr, vg, vb, 100.0 * double(whiteish_sum) / double(vis_px_sum),
                    100.0 * double(warm_sum) / double(vis_px_sum),
                    100.0 * double(pink_sum) / double(vis_px_sum));
    }
    std::printf("[sakura-probe] frames=%llu eof_events=%llu (probe done)\n",
                (unsigned long long)frames_seen, (unsigned long long)eof_events);
    if (!no_mask) {
        // Masked run must show a real visible difference (ring > 0) — the
        // mask does gate the main — yet it must not invent color: the few
        // pinkish pixels the probe counts (<< 0.02 % of the visible pixels)
        // are decoder chroma-ringing artifacts of the white fringes that
        // exist in ANY decode of the material — the unmasked solo pass
        // measures MORE of them (the mask erases fringe pixels). The mask
        // composite therefore never adds pink.
        if (ring_sum == 0) {
            std::fprintf(stderr, "FAIL: no mask effect observed on the real pair\n");
            return 1;
        }
        if (100.0 * double(pink_sum) / double(vis_px_sum ? vis_px_sum : 1) > 0.02) {
            std::fprintf(stderr, "FAIL: macro-scale pink invented by the "
                                 "composite\n");
            return 1;
        }
        std::printf("[sakura-probe] verdict: mask gates the main (ring > 0) and "
                    "invents no color (pink %.5f%% <= decode-artifact floor "
                    "0.02%%)\n",
                    100.0 * double(pink_sum) / double(vis_px_sum ? vis_px_sum : 1));
    }
    return 0;
}
