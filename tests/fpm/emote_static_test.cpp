// P1 media test (research/42): E-mote PSB parse parity + static pose raster
// against the real NekoMiko archive. Reads image\fhd\fg\aya\tay_0.psb from
// OA_TEST_NEKOMIKO_PFS and asserts
//   * the win-spec container stats pinned in research/40 §3.2/§3.3 (objects,
//     motions, timelines incl. exact labels, variables, sources, icons,
//     nodes, frames) — the "sample stats test" from the P1 scope;
//   * metadata charaProfile markers and the base motion pointer;
//   * the evaluated static pose bounds stay in the charaProfile envelope
//     (design px), i.e. the layout chain really composes to the authored
//     figure instead of exploding;
//   * rasterizing onto a 480x400 canvas yields plausible coverage with
//     colored (non-black) opaque pixels in the middle band.
// DXT5 atlas decode correctness is implicitly covered by the colour
// assertions (a decode that only fills alpha would fail them).
// Env: OA_TEST_NEKOMIKO_PFS (skip 77 unset).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/emote/emote_file.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
void check_eq(size_t got, size_t want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s: got %zu want %zu\n", what, got, want);
        ++failures;
    }
}
void check_dbl(double got, double want, double eps, const char* what) {
    if (std::fabs(got - want) > eps) {
        std::fprintf(stderr, "FAIL: %s: got %g want %g\n", what, got, want);
        ++failures;
    }
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_NEKOMIKO_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_NEKOMIKO_PFS unset; skipping\n");
        return 77;
    }
    std::string err;
    oa::fs::PhysFileSystem fs_phys(pfs_path, false);    oa::fs::IFileSystem& fs = fs_phys;
    const char* entry = "image\\fhd\\fg\\aya\\tay_0.psb";
    auto bytes = fs.read(entry);
    if (!bytes) {
        std::fprintf(stderr, "FAIL: read %s failed\n", entry);
        return 1;
    }

    oa::emote::EmoteFile f;
    if (!f.load(*bytes, &err)) {
        std::fprintf(stderr, "FAIL: emote load: %s\n", err.c_str());
        return 1;
    }
    check(f.spec == oa::emote::SpecKind::Win, "spec == win");
    check_eq(f.screenWidth, 800, "screenWidth");
    check_eq(f.screenHeight, 1080, "screenHeight");
    check(f.baseChara == "all_parts", "base chara == all_parts");
    check(f.baseMotion == "タイムライン構造", "base motion == タイムライン構造");

    // research/40 §3 parity numbers
    check_eq(f.objects.size(), 21, "objects");
    check_eq(f.motions.size(), 49, "motions");
    check_eq(f.timelines.size(), 15, "timelines");
    check_eq(f.variableNames.size(), 56, "variables");
    check_eq(f.sources.size(), 8, "sources");
    size_t icons = 0;
    for (const auto& s : f.sources) icons += s->icons.size();
    check_eq(icons, 207, "icons");
    size_t nodes = 0, frames = 0;
    for (const auto& m : f.motions) {
        nodes += m.nodes.size();
        for (const auto& n : m.nodes) frames += n.frames.size();
    }
    check_eq(nodes, 1806, "nodes");
    check_eq(frames, 7395, "frames");

    static const char* const kTimelines[] = {
        "通常待機", "通常", "通常_ボイス再生用", "笑顔", "笑顔_ボイス再生用", "悲しみ",
        "悲しみ_ボイス再生用", "怒り", "怒り_ボイス再生用", "照れ", "照れ_ボイス再生用",
        "呆れ", "呆れ_ボイス再生用", "驚き", "驚き_ボイス再生用",
    };
    for (int i = 0; i < 15; ++i) {
        check(i < int(f.timelines.size()) && f.timelines[size_t(i)].label == kTimelines[i],
              "timeline label order");
    }

    // charaProfile markers (design px)
    check_dbl(f.marker.top, -3679, 0.5, "marker top");
    check_dbl(f.marker.bottom, 220, 0.5, "marker bottom");
    check_dbl(f.marker.left, -737, 0.5, "marker left");
    check_dbl(f.marker.right, 689, 0.5, "marker right");
    check_dbl(f.marker.eye, -3201.5, 0.5, "marker eye");
    check_dbl(f.marker.mouth, -3053, 0.5, "marker mouth");
    check_dbl(f.marker.bust, -2677, 0.5, "marker bust");
    check_dbl(f.charaHeight, 160, 0.5, "chara height");

    // static pose bounds: close to the charaProfile envelope (icon quads may
    // stick out a bit beyond the tight markers); a broken layout chain
    // exploded to x~61710/y~-22750 in development (research/42 §5).
    double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    if (!oa::emote::static_content_bounds(f, {}, &bx0, &by0, &bx1, &by1)) {
        check(false, "static_content_bounds");
    } else {
        std::printf("content bounds x[%.0f..%.0f] y[%.0f..%.0f]\n", bx0, bx1, by0, by1);
        check(bx0 > -1500 && bx0 < 0, "bounds minX sane");
        check(bx1 > 0 && bx1 < 1500, "bounds maxX sane");
        check(by0 > -4500 && by0 < -3000, "bounds minY sane");
        check(by1 > -50 && by1 < 500, "bounds maxY sane");
        check(bx1 - bx0 < 3000 && by1 - by0 < 4500, "bounds extent sane");
    }

    // raster smoke: 480x400 fit-to-canvas
    std::vector<uint8_t> rgba;
    oa::emote::StaticRenderOptions opt;
    if (!oa::emote::render_static_frame(f, {}, 480, 400, opt, &rgba, &err)) {
        std::fprintf(stderr, "FAIL: render: %s\n", err.c_str());
        return 1;
    }
    size_t opaque = 0, colored = 0, midColored = 0;
    for (int y = 0; y < 400; ++y) {
        for (int x = 0; x < 480; ++x) {
            const uint8_t* p = &rgba[(size_t(y) * 480 + x) * 4];
            if (p[3] > 64) {
                ++opaque;
                if (p[0] > 12 || p[1] > 12 || p[2] > 12) {
                    ++colored;
                    if (y > 60 && y < 380) ++midColored;
                }
            }
        }
    }
    const double cov = 100.0 * opaque / (480.0 * 400.0);
    std::printf("canvas: opaque %.1f%% colored %zu midColored %zu\n", cov, colored, midColored);
    check(cov > 4.0 && cov < 70.0, "opaque coverage band");
    check(colored > 4000, "colored pixels present (atlas decode colours)");
    check(midColored > 2000, "coloured pixels in the central band (figure body)");

    std::printf("%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
