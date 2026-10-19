// Asset-free regression for the morph-warp sampler.
//
// A warp region only bounds where the field is authored; parts routinely reach
// past it (a raised hand above the torso region, a sleeve outside it). Clamping
// the sample folded every outside vertex onto the border curve, which flattened
// NekoMiko's fingertips into a line and cut the outer half of a sleeve away.
// `bezier_patch_ext` continues linearly from the border instead: identical
// inside, continuous on the border, and bounded far outside.
#include <cassert>
#include <cmath>
#include <cstdio>

#include "core/emote/emote_file.h"

using oa::emote::bezier_patch_ext;

int main() {
    // Linear patch: x follows the column (0..3), y follows the row (0,2,4,6),
    // so the patch evaluates to (3u, 6v) and the border slope is constant.
    float grid[32];
    for (int i = 0; i < 32; ++i) {
        const int k = i / 2;
        grid[i] = (i % 2 == 0) ? float(k % 4) : float((k / 4) * 2);
    }

    double x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bezier_patch_ext(grid, 1.0, 0.5, &x0, &y0);  // on the border
    bezier_patch_ext(grid, 1.2, 0.5, &x1, &y1);  // 20% past it
    bezier_patch_ext(grid, 1.4, 0.5, &x2, &y2);  // 40% past it
    assert(std::fabs(x0 - 3.0) < 1e-9 && std::fabs(y0 - 3.0) < 1e-9);
    // Clamping would give x1 == x0 here (the whole part folded onto the border).
    assert(x1 > x0 + 0.1);
    assert(std::fabs(y1 - y0) < 1e-9);
    // Linear continuation: equal steps. A cubic continuation would accelerate.
    assert(std::fabs((x2 - x1) - (x1 - x0)) < 1e-9);

    // Inside the region the helper is the plain patch.
    double ux = 0, uy = 0;
    bezier_patch_ext(grid, 0.25, 0.75, &ux, &uy);
    assert(std::fabs(ux - 0.75) < 1e-9 && std::fabs(uy - 4.5) < 1e-9);

    // Past the far corner both axes continue independently.
    double cx = 0, cy = 0;
    bezier_patch_ext(grid, 1.1, -0.1, &cx, &cy);
    assert(cx > 3.29 && cx < 3.31);
    assert(cy < -0.59 && cy > -0.61);

    std::printf("emote_warp_test: ok\n");
    return 0;
}
