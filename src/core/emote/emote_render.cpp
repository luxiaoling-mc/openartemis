// Static E-mote evaluation + CPU raster with bezier mesh warp. Evaluates the file's base motion (metadata.base: object all_parts / motion タ...
#include "core/emote/emote_file.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <map>
#include <string>
#include <vector>

namespace oa::emote {

namespace {

constexpr double kPi = 3.14159265358979323846;

// --------------------------------------------------------------------------- mesh subdivision policy (API in emote_file.h).
struct MeshDivEnv {
    bool debug = false; // OA_EMOTE_MESHDBG：填充 nodePath 诊断
};
const MeshDivEnv& meshdiv_env() {
    static const MeshDivEnv env; // lazy: no cross-TU static-init-order hazard
    return env;
}
int clamp_mesh_div(int d) { return std::clamp(d, kEmoteMeshDivMin, kEmoteMeshDivMax); }

// adaptive rule, kept verbatim as one arm of the hybrid fallback / the A-B arm
int adaptive_mesh_div(double iconW, double iconH) {
    const double diag = std::sqrt(iconW * iconW + iconH * iconH);
    return std::clamp(int(std::lround(diag / 90.0)), 6, 18);
}

// 2D affine (column convention; x' = a*x + c*y + e, y' = b*x + d*y + f).
struct Mat {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};
Mat mul(const Mat& m, const Mat& n) { // m * n
    Mat r;
    r.a = m.a * n.a + m.c * n.b;
    r.b = m.b * n.a + m.d * n.b;
    r.c = m.a * n.c + m.c * n.d;
    r.d = m.b * n.c + m.d * n.d;
    r.e = m.a * n.e + m.c * n.f + m.e;
    r.f = m.b * n.e + m.d * n.f + m.f;
    return r;
}
Mat translate(double x, double y) { return Mat{1, 0, 0, 1, x, y}; }
Mat scale(double x, double y) { return Mat{x, 0, 0, y, 0, 0}; }
Mat rotate(double deg) {
    const double r = deg * kPi / 180.0;
    return Mat{std::cos(r), std::sin(r), -std::sin(r), std::cos(r), 0, 0};
}
Mat shear(double sx, double sy) { return Mat{1, sy, sx, 1, 0, 0}; }

// ---------------------------------------------------------------------------
// cubic Bezier patch helpers. Grid stored row-major: control[r*4+c] = (u,v)
// pair; rows run along v, columns along u (identity = (c/3, r/3)), so the
// patch evaluates to its input when the grid is the identity grid.
// ---------------------------------------------------------------------------
inline double bez_b0(double t) { return (1 - t) * (1 - t) * (1 - t); }
inline double bez_b1(double t) { return 3 * t * (1 - t) * (1 - t); }
inline double bez_b2(double t) { return 3 * t * t * (1 - t); }
inline double bez_b3(double t) { return t * t * t; }

// Diagnostic per-node switches set by eval_node (see OA_* knobs there).
thread_local bool g_bezier_transpose = false;
// OA_TICK_OFFSET=<n> + OA_TICK_OFFSET_LABEL=<label>: per-node tick nudge.
thread_local double g_tick_offset = 0.0;

void bezier_patch(const float bp[32], double u, double v, double* bx, double* by) {
    const double bu[4] = {bez_b0(u), bez_b1(u), bez_b2(u), bez_b3(u)};
    const double bv[4] = {bez_b0(v), bez_b1(v), bez_b2(v), bez_b3(v)};
    double rx = 0, ry = 0;
    // OA_BEZIER_UV=transpose swaps the u/v axis assignment for an A/B test of
    // the control-grid orientation (reference: basis = bu[row]*bv[col]).
    const bool transpose = g_bezier_transpose;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const double wgt = transpose ? bu[r] * bv[c] : bu[c] * bv[r];
            rx += double(bp[(r * 4 + c) * 2 + 0]) * wgt;
            ry += double(bp[(r * 4 + c) * 2 + 1]) * wgt;
        }
    }
    *bx = rx;
    *by = ry;
}

// Patch sample for a point outside the warp region. The region only bounds where the field is authored — it is not a clip. Clamping the sam...
} // namespace
void bezier_patch_ext(const float bp[32], double u, double v, double* bx, double* by) {
    constexpr double kEps = 1e-3;
    const double cu = std::clamp(u, 0.0, 1.0);
    const double cv = std::clamp(v, 0.0, 1.0);
    double px = 0, py = 0;
    bezier_patch(bp, cu, cv, &px, &py);
    const double du = u - cu;
    if (du != 0.0) {
        // Slope taken inside the region: forward at u=0, backward at u=1.
        const double at = cu < 1.0 ? cu : cu - kEps;
        double ax = 0, ay = 0, bx1 = 0, by1 = 0;
        bezier_patch(bp, at, cv, &ax, &ay);
        bezier_patch(bp, std::min(at + kEps, 1.0), cv, &bx1, &by1);
        px += du * (bx1 - ax) / kEps;
        py += du * (by1 - ay) / kEps;
    }
    const double dv = v - cv;
    if (dv != 0.0) {
        const double at = cv < 1.0 ? cv : cv - kEps;
        double ax = 0, ay = 0, bx1 = 0, by1 = 0;
        bezier_patch(bp, cu, at, &ax, &ay);
        bezier_patch(bp, cu, std::min(at + kEps, 1.0), &bx1, &by1);
        px += dv * (bx1 - ax) / kEps;
        py += dv * (by1 - ay) / kEps;
    }
    *bx = px;
    *by = py;
}

namespace {

// --------------------------------------------------------------------------- one evaluated frame op along a node path (px -> px, y-down)....
struct FrameVals; // interpolated frame values (defined below)
struct Level {
    double cx = 0, cy = 0, cz = 0;
    double angle = 0;
    double sx = 0, sy = 0; // shear
    double zx = 1, zy = 1; // scale
    bool any = false;      // has any effect at all
    // which of this node's LINEAR components its descendants receive
    uint32_t inheritMask = 0x20007FC;
    /// node.meshCombine — 0 for a carrier whose patch only offsets the frame
    /// (rigid subtree translation), 1 for one that warps the children per
    /// vertex. See warp_is_rigid().
    int meshCombine = 0;
    /// which of this node's children receive its patch (node.meshSyncChildMask,
    /// bit = child index). A child that is NOT selected only inherits the
    /// node's frame, i.e. the patch acts as a rigid offset for it.
    uint32_t meshSyncChildMask = 0;
    int childIndex = -1; // branch taken from this node (set while recursing)
    const std::string* region = nullptr; // blank region "W:H:OX:OY" (borrowed)
    /// the owning node's label (points at the EmoteNode in the file; stable
    /// for the file's lifetime) — used by the OA_SKIP_WARP_LABEL bisect and
    /// by the OA_DEBUG_WARP trace.
    const std::string* name = nullptr;

    // morph warp (blank + bezier frame)
    bool hasWarp = false;
    bool hasBp = false;
    double wox = 0, woy = 0; // region centre (= W/2, H/2 in the win files)
    double ww = 1, wh = 1;
    float bp[32]; // identity grid filled at parse when absent

    static Level from_frame(const EmoteFrame& f, bool withOpa) {
        Level l;
        if (f.hasCoord) { l.cx = f.coordX; l.cy = f.coordY; l.cz = f.coordZ; l.any = true; }
        if (f.hasAngle && f.angle != 0) { l.angle = f.angle; l.any = true; }
        if (f.sx != 0 || f.sy != 0) { l.sx = f.sx; l.sy = f.sy; l.any = true; }
        if (f.zx != 1 || f.zy != 1) { l.zx = f.zx; l.zy = f.zy; l.any = true; }
        (void)withOpa; // frame opa is folded through the recursion parameter
        if (l.any) return l;
        return l;
    }
    // affine level built from the *interpolated* frame
    static Level from_frame_vals(const EmoteFrame& f, const EmoteFrame& g,
                                 const FrameVals& v);
    // per-level local-frame op (krkr matrix order T·R·S·H — point order
    Mat op() const {
        Mat m = translate(cx, cy);
        m = mul(m, rotate(angle));
        m = mul(m, scale(zx, zy));
        m = mul(m, shear(sx, sy));
        return m;
    }
    // px -> px point application of the same local-frame op (H, S, R, then T).
    void apply_affine(double* x, double* y) const {
        double px = *x, py = *y;
        if (sx != 0 || sy != 0) {
            const double tx = px + sx * py;
            py = sy * px + py;
            px = tx;
        }
        px *= zx;
        py *= zy;
        if (angle != 0) {
            const double r = angle * kPi / 180.0;
            const double cs = std::cos(r), sn = std::sin(r);
            const double tx = px * cs - py * sn;
            py = px * sn + py * cs;
            px = tx;
        }
        *x = px + cx;
        *y = py + cy;
    }
    // px -> px point application: the affine part of a node's frame content.
    void apply(double* x, double* y) const { apply_affine(x, y); }
};

// ---------------------------------------------------------------------------
// evaluation state
// ---------------------------------------------------------------------------
struct DrawEntry {
    const EmoteSource* source = nullptr;
    const EmoteIcon* icon = nullptr;
    // icon-local bezier grid (shape keyframes on the icon frame itself)
    bool iconWarp = false;
    float iconBp[32];
    double iconOx = 0, iconOy = 0; // frame-level offset added to the anchor
    std::vector<Level> levels;     // px -> px chain, outermost first
    double opa = 1.0;
    double z = 0;
    bool warped = false; // mesh path needed?
    // the icon node's authored meshDivision — krkr reads it
    // from exactly this node (E:emoterunner.cpp:853-854), <2 -> 8.
    int authoredMeshDivision = 0;
    int type12Depth = 0; // number of type-12 stencil nodes above this entry
    // Stencil composite (type 12) tagging. stencil_group = the innermost
    // stencil whose subtree this entry draws under (its alpha is masked by
    // the group's mask shapes); mask_for_group = the stencil group this
    // entry's shape masks (the entry itself still draws normally — the
    // eye-white is both visible content and the pupil's stencil).
    int stencil_group = -1;
    int mask_for_group = -1;
    int mask_scope_depth = -1; // transient: deepest matching scope depth
    // the motion this node lives in — the fallback mask-binding key (see the
    // same-motion rule in eval_node's mask tagging).
    const void* node_motion = nullptr;
    // transient index into emote_collect_parts' per-entry
    // grid slices; it survives the z sort so the geometry pass can look up the
    // grid points the bounds pass already evaluated.
    int slot = -1;
    std::vector<std::string> debugPath;
};

/// One type-12 stencil-composite node seen during evaluation. `labels` are
/// the stencilCompositeMaskLayerList node labels; `scope` is the stencil's
/// PARENT node index within `motion` — mask labels resolve among draw
/// entries whose node chain contains that scope (the ancestor rule binds
/// each 目L/目R stencil to the 'shirome' node of its own eye branch).
struct StencilFrame {
    const void* motion = nullptr; // EmoteMotion identity
    int node = -1;
};
struct StencilGroup {
    std::vector<std::string> labels;
    // the stencil's ancestor chain including its parent node — a draw entry
    // belongs to this group's mask when its own chain has this chain as a
    // prefix (binds each eye's 'shirome' to its own 目L/目R branch and lets
    // mask references cross sub-motion boundaries)
    std::vector<StencilFrame> scope_chain;
    const void* owner_motion = nullptr; // the stencil node itself
    int owner_node = -1;
    mutable bool mask_built = false; // CPU raster: mask bitmap ready
};

struct EvalCtx {
    const EmoteFile& file;
    const std::map<std::string, double>& vars;
    std::vector<DrawEntry>* out = nullptr;
    std::string* err = nullptr;
    std::vector<StencilGroup> stencil_groups;
    // the eval-time content bounds are only consumed by
    // static_content_bounds() and the OA_DEBUG_EVAL trace; render_static_frame
    // and emote_collect_parts compute their own (identical) bounds. Sampling
    // the mesh grid here for every entry a second/third time was ~20% of the
    // collect cost (perf profile: map_entry_uv 57% + this 20%).
    bool wantBounds = true;

    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = -std::numeric_limits<double>::max();
    double maxY = -std::numeric_limits<double>::max();

    double raw_value(const std::string& name) const {
        auto it = vars.find(name);
        if (it != vars.end()) return it->second;
        auto d = file.variableDefaults.find(name);
        if (d != file.variableDefaults.end()) return d->second;
        return 0.0;
    }
    double var_value(const std::string& name) const {
        const double v = raw_value(name);
        // 标准变量兜底: 模型不消费 body_UD 时，把作者写的体偏移折到整体位移轴
        // move_UD 上（同一套 [-30,30]/division 坐标，见 EmoteFile 的判定）。
        if (file.body_ud_to_move_ud && name == "move_UD") return v + raw_value("body_UD");
        return v;
    }
};

// node tick: parameter nodes map their variable to the axis; others keep the incoming (motion) tick. Mirrors krkr (emotemotionref::getTickB...
double clamp_static_tick(const EmoteMotion& m, const EmoteNode& n, double tick) {
    const size_t count = n.frames.size();
    if (count > 1 && n.frames[count - 1].type == 0) {
        if (count == 2 && n.frames[0].type == 2) return n.frames[0].time; // B
        if (n.frames[count - 2].type == 2 && tick > m.selfSyncTime)       // A
            return m.selfSyncTime;
        if (n.frames[count - 2].type == 3)                                // C
            return std::min(tick, n.frames[count - 2].time);
    }
    return tick;
}

double node_tick(const EmoteMotion& m, const EmoteNode& n, double incoming,
                 const EvalCtx& ctx) {
    if (!n.isParameterized) return clamp_static_tick(m, n, incoming);
    if (n.parameterIndex < 0 || n.parameterIndex >= int(m.parameter.size()))
        return clamp_static_tick(m, n, incoming);
    const EmoteVar& v = m.parameter[n.parameterIndex];
    return v.trans_to_tick(ctx.var_value(v.id));
}

// picks the last frame with time <= tick; `contentNext` gets the immediately
// following frame only when it carries content (the reference's nextframe is
// cleared when the follower is an empty key, so a gap holds instead of
// interpolating across it). The content-less tail rule matches
// selectEmoteNodeFrames (see the body).
int pick_frame(const std::vector<EmoteFrame>& frames, double tick, int* contentNext,
               bool shapeNode) {
    *contentNext = -1;
    int cur = -1;
    int next = -1;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].time > tick) {
            next = int(i);
            break;
        }
        cur = int(i);
    }
    if (cur < 0) return -1;
    // Reference selectEmoteNodeFrames (krkrsdl3 plugins/emoteplayer/
    if (!frames[size_t(cur)].hasContent && next < 0) {
        for (int i = cur - 1; i >= 0; --i) {
            if (!frames[size_t(i)].hasContent) continue;
            const std::string& src = frames[size_t(i)].src;
            if (shapeNode || src == "layout" || src == "clip" || src == "blank" ||
                src.rfind("blank/", 0) == 0 || src.rfind("src/blank/", 0) == 0 ||
                src.rfind("shape/", 0) == 0 || src.rfind("src/shape/", 0) == 0)
                cur = i;
            break;
        }
    }
    if (next >= 0 && frames[size_t(next)].hasContent) *contentNext = next;
    return cur;
}

// krkr interp rule (reference): interpolate unless the current frame is a
// type-2 keyframe whose follower is not type 2 (type 3 == hold).
bool interp_allowed(const EmoteFrame& a, const EmoteFrame& b) {
    if (a.type != 2) return true;
    return b.type == 2;
}

double lerp(double va, double vb, double ta, double tb, double tick) {
    if (tb <= ta) return va;
    return va + (vb - va) * (tick - ta) / (tb - ta);
}

void fill_identity_grid(float bp[32]);
// OA_BP_HOLD=<label substring>: hold the current key's grid instead of
// interpolating (diagnostic for "does the driver blend bp at all").
thread_local bool g_bp_hold_active = false;
// OA_BP_MIX=<r> + OA_BP_MIX_LABEL=<label substring>: force the bp blend ratio
// for one node (diagnostic).
thread_local double g_bp_mix = -1.0;
float grid_identity(int i);

// interpolated frame values at tick (krkr progress(): linear between cur/next
// when allowed, else exactly the current frame).
struct FrameVals {
    const EmoteFrame* frame = nullptr;
    double cx = 0, cy = 0, cz = 0;
    double angle = 0, sx = 0, sy = 0, zx = 1, zy = 1, ox = 0, oy = 0;
    double opa = 1.0;
    double timeOffset = 0;
    bool hasBp = false; // interpolated bezier grid (icon/morph keyframes)
    float bp[32];
};
Level Level::from_frame_vals(const EmoteFrame& f, const EmoteFrame& g,
                             const FrameVals& v) {
    Level l;
    if (f.hasCoord || g.hasCoord) { l.cx = v.cx; l.cy = v.cy; l.cz = v.cz; l.any = true; }
    if ((f.hasAngle || g.hasAngle) && v.angle != 0) { l.angle = v.angle; l.any = true; }
    if (f.sx != 0 || f.sy != 0 || g.sx != 0 || g.sy != 0) {
        l.sx = v.sx;
        l.sy = v.sy;
        l.any = true;
    }
    if (f.zx != 1 || f.zy != 1 || g.zx != 1 || g.zy != 1) {
        l.zx = v.zx;
        l.zy = v.zy;
        l.any = true;
    }
    return l;
}

// `lim` = the drawn icon's limit box
// {originX, originY, width, height} (the reference's emotelimit for this
// frame), used by the non-finite coord repair; nullptr disables that repair.
FrameVals frame_vals(const std::vector<EmoteFrame>& frames, int cur, int next,
                     double tick, const double* lim) {
    FrameVals v;
    if (cur < 0) return v;
    const EmoteFrame& f = frames[cur];
    v.frame = &f;
    // the reference repairs NaN/Inf coords BEFORE
    // interpolating (E:emoterunner.cpp:559-583, both the current and the next
    // frame): NaN -> -lim.origin, Inf -> lim.size - lim.origin. The copies
    // below leave every finite value bit-identical.
    double fcx = f.coordX, fcy = f.coordY;
    if (lim) {
        if (std::isnan(fcx)) fcx = -lim[0];
        if (std::isnan(fcy)) fcy = -lim[1];
        if (std::isinf(fcx)) fcx = lim[2] - lim[0];
        if (std::isinf(fcy)) fcy = lim[3] - lim[1];
    }
    v.cx = fcx;
    v.cy = fcy;
    v.cz = f.coordZ;
    v.angle = f.angle;
    v.sx = f.sx;
    v.sy = f.sy;
    v.zx = f.zx;
    v.zy = f.zy;
    v.ox = f.ox;
    v.oy = f.oy;
    v.opa = f.opa;
    v.timeOffset = f.timeOffset;
    v.hasBp = f.hasBp;
    if (f.hasBp) std::memcpy(v.bp, f.bp, sizeof(v.bp));
    else fill_identity_grid(v.bp);
    if (next >= 0) {
        const EmoteFrame& g = frames[next];
        if (interp_allowed(f, g)) {
            const double ta = f.time, tb = g.time;
            double gcx = g.coordX, gcy = g.coordY;
            if (lim) {
                if (std::isnan(gcx)) gcx = -lim[0];
                if (std::isnan(gcy)) gcy = -lim[1];
                if (std::isinf(gcx)) gcx = lim[2] - lim[0];
                if (std::isinf(gcy)) gcy = lim[3] - lim[1];
            }
            // the reference folds a +-180 crossing to
            // the short way round (E:emoterunner.cpp:604-620):
            //   next<180 && cur>180 -> lerp(cur-360, next)   ("small 360 -> big 0")
            //   next>180 && cur<180 -> lerp(cur, next-360)   ("big 0 -> small 360")
            double a0 = f.angle, a1 = g.angle;
            {
                if (a1 < 180 && a0 > 180) a0 -= 360;
                else if (a1 > 180 && a0 < 180) a1 -= 360;
            }
            v.cx = lerp(fcx, gcx, ta, tb, tick);
            v.cy = lerp(fcy, gcy, ta, tb, tick);
            v.cz = lerp(f.coordZ, g.coordZ, ta, tb, tick);
            v.angle = lerp(a0, a1, ta, tb, tick);
            v.sx = lerp(f.sx, g.sx, ta, tb, tick);
            v.sy = lerp(f.sy, g.sy, ta, tb, tick);
            v.zx = lerp(f.zx, g.zx, ta, tb, tick);
            v.zy = lerp(f.zy, g.zy, ta, tb, tick);
            v.ox = lerp(f.ox, g.ox, ta, tb, tick);
            v.oy = lerp(f.oy, g.oy, ta, tb, tick);
            v.opa = lerp(f.opa, g.opa, ta, tb, tick);
            v.timeOffset = lerp(f.timeOffset, g.timeOffset, ta, tb, tick);
            // bezier grids: blend when EITHER side carries a grid, exactly like
            if ((f.hasBp || g.hasBp) && !g_bp_hold_active) {
                double bpt = tick;
                if (g_bp_mix >= 0) bpt = ta + (tb - ta) * g_bp_mix;
                for (int i = 0; i < 32; ++i) v.bp[i] = float(lerp(f.hasBp ? f.bp[i] : grid_identity(i), g.hasBp ? g.bp[i] : grid_identity(i), ta, tb, bpt));
                v.hasBp = true;
            }
        }
    }
    {
        // The reference rule above covers the coords; this engine extends the
        // same "never let a non-finite value reach the geometry" principle to
        // the remaining fields (no reference counterpart —
        // this engine's own safety net, inert on the three works).
        const double neutral[11] = {0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1};
        double* fields[11] = {&v.cx, &v.cy,       &v.cz, &v.angle, &v.sx, &v.sy,
                              &v.zx, &v.zy,       &v.ox, &v.oy,    &v.opa};
        for (int i = 0; i < 11; ++i)
            if (!std::isfinite(*fields[i])) *fields[i] = neutral[i];
        if (!std::isfinite(v.timeOffset)) v.timeOffset = 0;
        if (v.hasBp)
            for (int i = 0; i < 32; ++i)
                if (!std::isfinite(double(v.bp[i]))) v.bp[i] = grid_identity(i);
    }
    return v;
}

void fill_identity_grid(float bp[32]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            bp[(r * 4 + c) * 2 + 0] = float(c) / 3.0f;
            bp[(r * 4 + c) * 2 + 1] = float(r) / 3.0f;
        }
}
float grid_identity(int i) {
    const int r = (i / 2) / 4, c = (i / 2) % 4;
    return float((i & 1) ? r : c) / 3.0f;
}

// parse the win blank-region string "W:H:OX:OY" (icon field of blank frames)
bool parse_region(const std::string& s, double* w, double* h, double* ox, double* oy) {
    double v[4] = {0, 0, 0, 0};
    int n = 0;
    size_t pos = 0;
    while (n < 4 && pos <= s.size()) {
        size_t p = s.find(':', pos);
        const std::string tok = s.substr(pos, p == std::string::npos ? p : p - pos);
        if (tok.empty()) return false;
        try {
            v[n] = std::stod(tok);
        } catch (...) {
            return false;
        }
        ++n;
        if (p == std::string::npos) break;
        pos = p + 1;
    }
    if (n != 4) return false;
    *w = v[0];
    *h = v[1];
    *ox = v[2];
    *oy = v[3];
    return *w > 0 && *h > 0;
}

// --------------------------------------------------------------------------- the per-entry forward map, prepared ONCE per draw entry and a...
struct LevelOp {
    double sx = 0, sy = 0, zx = 1, zy = 1, cx = 0, cy = 0, cs = 1, sn = 0;
    bool shear = false, angle = false;
};
inline void apply_level_op(const LevelOp& o, double* x, double* y) {
    double px = *x, py = *y;
    if (o.shear) {
        const double tx = px + o.sx * py;
        py = o.sy * px + py;
        px = tx;
    }
    px *= o.zx;
    py *= o.zy;
    if (o.angle) {
        const double tx = px * o.cs - py * o.sn;
        py = px * o.sn + py * o.cs;
        px = tx;
    }
    *x = px + o.cx;
    *y = py + o.cy;
}

bool warp_is_rigid(const std::string& label, int meshCombine,
                   const std::string* region, uint32_t syncMask = 0,
                   int childIndex = -1) {
    (void)label;
    (void)meshCombine;
    (void)region;
    (void)childIndex;
    // Measured against emotedriver (docs/TESTING.md §3.1): the field is a
    // mode selector, not a child bitmask — 0x8 = the patch deforms the
    // subtree per vertex, 0x1 = the subtree is only offset rigidly, 0x0
    // is a third, distinct mode. Of the real values in this work
    // (0x0/0x1/0x8) only 0x1 is proven rigid, so keep the rule narrow.
    return syncMask == 0x1u;
}

struct WarpRef {
    const std::string* name = nullptr; // owning node label
    double ox = 0, oy = 0;   // region centre anchor: morph origin in world px
    double w = 1, h = 1, cx = 0, cy = 0; // region box (dims + centre offsets)
    // angle fold: ancestor linear part folded at this morph
    // (its content frame -> world), and its inverse (world -> content frame).
    // `folded` is false for axis-aligned (translation only) ancestors: those
    // keep the legacy code path bit-identical.
    double la = 1, lb = 0, lc = 0, ld = 1;
    double ia = 1, ib = 0, ic = 0, id = 1;
    bool folded = false;
    /// A `blank` carrier (region-only frame) does NOT warp the vertices of its
    /// descendants: the driver reads the patch at the region CENTRE (u=v=0.5)
    /// and offsets the whole subtree by that vector. Only the artwork's own
    /// icon meshes deform per vertex.
    bool rigid = false;
    int meshCombine = 0;
    uint32_t meshSyncChildMask = 0;
    int childIndex = -1;
    const std::string* region = nullptr;
    float bp[32];
};

struct EntryMap {
    std::vector<LevelOp> opsRev; // leaf -> root (the pass-2 application order)
    WarpRef warps[64];
    int nWarps = 0;
    bool iconWarp = false;
    float iconBp[32] = {0};
    double iconOx = 0, iconOy = 0;
    double originX = 0, originY = 0, iconW = 1, iconH = 1;
    bool probe = false;
};

void prepare_entry_map(const DrawEntry& en, EntryMap* m) {
    m->nWarps = 0;
    m->iconWarp = en.iconWarp;
    std::memcpy(m->iconBp, en.iconBp, sizeof(m->iconBp));
    m->iconOx = en.iconOx;
    m->iconOy = en.iconOy;
    m->originX = en.icon->originX;
    m->originY = en.icon->originY;
    m->iconW = en.icon->width;
    m->iconH = en.icon->height;
    m->probe = false;
    // inheritMask (emotegeometry.h buildMotionLinear): a node's linear part
    // reaches its subtree only when EVERY level between it and the leaf still
    // inherits that component; coord translation is always passed down. The
    // accumulated mask is the AND of the inheritMask values of the levels
    // deeper than the one being applied, so walk leaf -> root and filter.
    static constexpr uint32_t kInhLinear = 0x1F0; // angle|zoomX|zoomY|slX|slY
    std::vector<Level> effLevels(en.levels);
    {
        uint32_t acc = kInhLinear;
        for (size_t i = effLevels.size(); i-- > 0;) {
            Level& l = effLevels[i];
            if (!(acc & 0x10)) l.angle = 0;
            if (!(acc & 0x20)) l.zx = 1;
            if (!(acc & 0x40)) l.zy = 1;
            if (!(acc & 0x80)) l.sx = 0;
            if (!(acc & 0x100)) l.sy = 0;
            acc &= l.inheritMask;
        }
    }
    // ---- pass 1 (root -> leaf), verbatim from the earlier function ---------
    double la = 1, lb = 0, lc = 0, ld = 1; // cumulative linear (content -> world)
    double le = 0, lf = 0;                 // cumulative translation
    for (const Level& l : effLevels) {
        if (l.angle == 0 && l.sx == 0 && l.sy == 0 && l.zx == 1 && l.zy == 1) {
            le += la * l.cx + lb * l.cy;
            lf += lc * l.cx + ld * l.cy;
        } else {
            const Mat op = l.op();
            const double nla = la * op.a + lb * op.b;
            const double nlb = la * op.c + lb * op.d;
            const double nlc = lc * op.a + ld * op.b;
            const double nld = lc * op.c + ld * op.d;
            const double nle = la * op.e + lb * op.f + le;
            const double nlf = lc * op.e + ld * op.f + lf;
            la = nla; lb = nlb; lc = nlc; ld = nld; le = nle; lf = nlf;
        }
        if (l.hasWarp && l.hasBp && m->nWarps < 64) {
            WarpRef& wr = m->warps[m->nWarps++];
            wr.name = l.name;
            wr.ox = le;
            wr.oy = lf;
            wr.w = l.ww;
            wr.h = l.wh;
            wr.cx = l.wox;
            wr.cy = l.woy;
            wr.la = la;
            wr.lb = lb;
            wr.lc = lc;
            wr.ld = ld;
            wr.folded = la != 1 || lb != 0 || lc != 0 || ld != 1;
            // `hasWarp` is only set for frames whose src is `blank` (a region
            wr.meshCombine = l.meshCombine;
            wr.meshSyncChildMask = l.meshSyncChildMask;
            wr.childIndex = l.childIndex;
            wr.region = l.region;
            wr.rigid = warp_is_rigid(l.name ? *l.name : std::string(),
                                     l.meshCombine, l.region, l.meshSyncChildMask,
                                     l.childIndex);
            if (wr.folded) {
                const double det = la * ld - lb * lc;
                if (det > 1e-12 || det < -1e-12) {
                    wr.ia = ld / det;
                    wr.ib = -lb / det;
                    wr.ic = -lc / det;
                    wr.id = la / det;
                }
            }
            std::memcpy(wr.bp, l.bp, sizeof(wr.bp));
        }
    }
    // ---- level ops in the pass-2 (leaf -> root) order ---------------------
    m->opsRev.clear();
    m->opsRev.reserve(effLevels.size());
    for (size_t ri = effLevels.size(); ri-- > 0;) {
        const Level& l = effLevels[ri];
        LevelOp o;
        o.sx = l.sx;
        o.sy = l.sy;
        o.zx = l.zx;
        o.zy = l.zy;
        o.cx = l.cx;
        o.cy = l.cy;
        o.shear = (l.sx != 0 || l.sy != 0);
        o.angle = (l.angle != 0);
        if (o.angle) {
            const double r = l.angle * kPi / 180.0;
            o.cs = std::cos(r);
            o.sn = std::sin(r);
        }
        m->opsRev.push_back(o);
    }
}

// icon-local uv -> world px through the prepared chain (forward).
void map_entry_uv(const EntryMap& m, double u, double v, double* x, double* y) {
    double uu = u, vv = v;
    if (m.iconWarp) {
        double bx = 0, by = 0;
        bezier_patch(m.iconBp, std::clamp(uu, 0.0, 1.0), std::clamp(vv, 0.0, 1.0), &bx, &by);
        uu = bx;
        vv = by;
    }
    double px = uu * m.iconW - m.originX - m.iconOx;
    double py = vv * m.iconH - m.originY - m.iconOy;
    for (const LevelOp& o : m.opsRev) apply_level_op(o, &px, &py);
    for (int i = m.nWarps - 1; i >= 0; --i) {
        if (i < 32 && (0x0 & (1 << i))) continue;
        const WarpRef& wr = m.warps[i];
        double qx, qy, su, sv, bx = 0, by = 0;
        if (!wr.folded) {
            qx = px - wr.ox;
            qy = py - wr.oy;
        } else {
            const double dx0 = px - wr.ox, dy0 = py - wr.oy;
            qx = wr.ia * dx0 + wr.ib * dy0;
            qy = wr.ic * dx0 + wr.id * dy0;
        }
        su = (qx + wr.cx) / wr.w;
        sv = (qy + wr.cy) / wr.h;
        if (wr.rigid) {
            // rigid carrier: the patch is read at the region centre, so the
            // offsets below are (bx-0.5)*w / (by-0.5)*h — see WarpRef::rigid
            su = 0.5;
            sv = 0.5;
        }
        bezier_patch_ext(wr.bp, su, sv, &bx, &by);
        if (wr.rigid) {
            // constant offset of the whole subtree (see WarpRef::rigid)
            const double ddx = (bx - 0.5) * wr.w;
            const double ddy = (by - 0.5) * wr.h;
            px += wr.folded ? wr.la * ddx + wr.lb * ddy : ddx;
            py += wr.folded ? wr.lc * ddx + wr.ld * ddy : ddy;
        } else if (!wr.folded) {
            px = wr.ox + qx + (bx * wr.w - wr.cx - qx);
            py = wr.oy + qy + (by * wr.h - wr.cy - qy);
        } else {
            const double ddx = bx * wr.w - wr.cx - qx;
            const double ddy = by * wr.h - wr.cy - qy;
            px = px + wr.la * ddx + wr.lb * ddy;
            py = py + wr.lc * ddx + wr.ld * ddy;
        }
    }
    *x = px;
    *y = py;
}

void eval_motion(EvalCtx& ctx, const EmoteMotion& motion, double tick,
                 const std::vector<Level>& levels, double opaMul, int depth,
                 int type12Depth, std::vector<std::string>& path,
                 std::vector<StencilFrame>& chain, int stencil_group);

/// Pre-scan a motion tree for type-12 stencil nodes, recording one group
/// per node (mask labels + the stencil's ancestor chain). Runs before the
/// main walk so a mask node drawn BEFORE its stencil (the eye-white
/// precedes the eye stencil in tree order) can still be tagged as that
/// stencil's mask.
void scan_stencil_groups(EvalCtx& ctx, const EmoteMotion& motion,
                         int nodeIdx, std::vector<StencilFrame> chain);

/// the cell count one entry's mesh is built with — the
/// node-authorised value under the active policy (see emote_file.h).
static int entry_mesh_div(const DrawEntry& en) {
    return emote_effective_mesh_division(en.authoredMeshDivision,
                                         en.icon ? en.icon->width : 0.0,
                                         en.icon ? en.icon->height : 0.0);
}

void scan_stencil_groups(EvalCtx& ctx, const EmoteMotion& motion,
                         int nodeIdx, std::vector<StencilFrame> chain) {
    if (nodeIdx < 0 || nodeIdx >= int(motion.nodes.size())) return;
    const EmoteNode& node = motion.nodes[nodeIdx];
    if (node.removed) return;
    if (node.isMaskNode && !node.stencil_mask_layers.empty()) {
        StencilGroup g;
        g.labels = node.stencil_mask_layers;
        // scope = the stencil's ancestor chain (the chain here EXCLUDES the
        // stencil itself): the mask shapes are drawn in sibling branches of
        // that chain (eye-white under mabuta), so a mask node matches when
        // its own chain extends this scope chain
        g.scope_chain = chain;
        g.owner_motion = (const void*)&motion;
        g.owner_node = nodeIdx;
        ctx.stencil_groups.push_back(std::move(g));
    }
    chain.push_back({(const void*)&motion, nodeIdx});
    for (int ch : node.children)
        scan_stencil_groups(ctx, motion, ch, chain);
    // stencil/mask pairs across a sub-motion reference: scan the referenced
    // motion too (the ancestor chain continues through the dive)
    for (const auto& f : node.frames) {
        if (!f.isSubMotion) continue;
        const int midx = ctx.file.find_motion(f.subObject, f.subMotion);
        if (midx >= 0 && !ctx.file.motions[midx].nodes.empty())
            for (int root : ctx.file.motions[midx].layer)
                scan_stencil_groups(ctx, ctx.file.motions[midx], root, chain);
    }
}

void eval_node(EvalCtx& ctx, const EmoteMotion& motion, int nodeIdx, double incomingTick,
               const std::vector<Level>& levels, double opaMul, int depth,
               int type12Depth, std::vector<std::string>& path,
               std::vector<StencilFrame>& chain, int stencil_group) {
    if (depth > 96) return;
    if (nodeIdx < 0 || nodeIdx >= int(motion.nodes.size())) return;
    const EmoteNode& node = motion.nodes[nodeIdx];
    if (node.removed || node.frames.empty()) return;
    path.push_back(node.label);
    chain.push_back({(const void*)&motion, nodeIdx});
    if (node.isMaskNode && !node.stencil_mask_layers.empty()) {
        // a stencil node: its subtree's draws clip against the mask labels.
        // The pre-scan recorded the group under this exact stencil node.
        for (const auto& g : ctx.stencil_groups) {
            if (g.owner_motion == (const void*)&motion &&
                g.owner_node == nodeIdx) {
                stencil_group = int(&g - ctx.stencil_groups.data());
                break;
            }
        }
    }

    // --- tick & frame selection --------------------------------------------
    // the reference's getTickByIdx returns -1 for
    // an out-of-range parameterIndex and the node is then NOT drawn
    // (E:emoterunner.cpp:1004-1005 -> isNeedDraw = false, 530-534); its
    // children are still walked. Default OFF (data: 0/3466 parameter nodes).

    // per-node tick nudge (diagnostic: OA_TICK_OFFSET / _LABEL) — applied
    // BEFORE the key is picked, so it can move the selection across a key.
    const double tick0 = node_tick(motion, node, incomingTick, ctx);
    const bool tick_knob = false;
    const double tick = tick0 + (tick_knob ? g_tick_offset : 0.0);
    int next = -1;
    // shapeNode: the reference passes `type == 1` (emoterunner.cpp:167).
    const int cur = pick_frame(node.frames, tick, &next, node.type == 1);
    const EmoteFrame* curFrame = (cur >= 0) ? &node.frames[cur] : nullptr;
    const bool hasContent = curFrame && curFrame->hasContent;

    // Node whose current frame is content-less contributes nothing itself but
    // its subtree is still evaluated (krkr reference: children recursion is
    // unconditional; the node only stopped drawing).
    if (!hasContent) {
        for (int ch : node.children)
            eval_node(ctx, motion, ch, tick, levels, opaMul, depth + 1,
                      type12Depth + (node.type == 12 ? 1 : 0), path, chain,
                      stencil_group);
        path.pop_back();
        chain.pop_back();
        return;
    }

    // the reference's non-finite coord repair needs this frame's
    // "limit" (the drawn icon's origin/size). The content
    // lookup only depends on the raw current frame, so it is resolved before
    // the interpolation and reused below.
    const EmoteFrame& fraw = *curFrame;
    const EmoteSource* srcPre = nullptr;
    const EmoteIcon* icPre = nullptr;
    double lim4[4] = {0, 0, 0, 0};
    const double* lim = nullptr;
    if (!fraw.isSubMotion && !fraw.src.empty() && !fraw.icon.empty()) {
        srcPre = ctx.file.find_source(fraw.src);
        if (srcPre) icPre = ctx.file.find_icon(*srcPre, fraw.icon);
        if (icPre) {
            lim4[0] = icPre->originX;
            lim4[1] = icPre->originY;
            lim4[2] = icPre->width;
            lim4[3] = icPre->height;
            lim = lim4;
        } else if (fraw.src == "blank") {
            double w = 0, h = 0, ox = 0, oy = 0;
            if (parse_region(fraw.icon, &w, &h, &ox, &oy)) {
                lim4[0] = ox;
                lim4[1] = oy;
                lim4[2] = w;
                lim4[3] = h;
                lim = lim4;
            }
        }
    }

    const FrameVals fv = frame_vals(node.frames, cur, next, tick, lim);
    const EmoteFrame& f = *fv.frame;
    // --- classify content --------------------------------------------------
    // sub-motion reference (win: content.motion + src=object/icon=motion)
    if (f.isSubMotion) {
        const int midx = ctx.file.find_motion(f.subObject, f.subMotion);
        if (midx >= 0) {
            // affine level from the interpolated values when a following
            // content key exists (type-3 keys interpolate; only a type-2 key
            // followed by a non-type-2 key holds — krkr rule).
            Level lv =
                (next >= 0) ? Level::from_frame_vals(f, node.frames[size_t(next)], fv)
                            : Level::from_frame(f, false);
            lv.inheritMask = node.inheritMask;
            lv.name = &node.label;
            lv.meshCombine = node.meshCombine;
    lv.meshSyncChildMask = node.meshSyncChildMask;
            std::vector<Level> childLevels = levels;
            if (lv.any) childLevels.push_back(lv);
            // reference (emoterunner.cpp:320-331): a parameterised node's
            // derived tick BECOMES currTick and is what the subtree (including
            // sub-motion references) is evaluated at — that is how an axis
            // drives key-framed children like 目L/目R's mabuta/shirome.
            const double subTick = tick + fv.timeOffset;
            eval_motion(ctx, ctx.file.motions[midx], subTick, childLevels, opaMul, depth + 1,
                        type12Depth + (node.type == 12 ? 1 : 0), path, chain,
                        stencil_group);
        }
        for (int ch : node.children)
            eval_node(ctx, motion, ch, tick, levels, opaMul, depth + 1,
                      type12Depth + (node.type == 12 ? 1 : 0), path, chain,
                      stencil_group);
        path.pop_back();
        chain.pop_back();
        return;
    }
    // source icon (resolved above for the non-finite `lim`)
    const EmoteSource* src = srcPre;
    const EmoteIcon* ic = icPre;
    Level lv = (next >= 0) ? Level::from_frame_vals(f, node.frames[size_t(next)], fv)
                           : Level::from_frame(f, true);
    lv.inheritMask = node.inheritMask;
    lv.name = &node.label;
    lv.meshCombine = node.meshCombine;
    lv.meshSyncChildMask = node.meshSyncChildMask;
    if (f.src == "blank") {
        // morph carrier: blank + bezier frame (region "W:H:OX:OY" centred on
        // the part origin). Only pushed when it actually carries a grid; a
        // plain blank is an identity no-op.
        double w = 0, h = 0, ox = 0, oy = 0;
        if (fv.hasBp && parse_region(f.icon, &w, &h, &ox, &oy)) {
            lv.hasWarp = true;
            lv.hasBp = true;
            lv.ww = w;
            lv.wh = h;
            lv.wox = ox;
            lv.woy = oy;
            lv.region = &f.icon;
            std::memcpy(lv.bp, fv.bp, sizeof(lv.bp));
            lv.any = true;
        }
    } else if (fv.hasBp && srcPre && icPre) {
        // A TEXTURED node's patch deforms its own icon AND its subtree: the
        lv.hasWarp = true;
        lv.hasBp = true;
        lv.ww = icPre->width;
        lv.wh = icPre->height;
        lv.wox = icPre->originX;
        lv.woy = icPre->originY;
        lv.region = &f.icon;
        std::memcpy(lv.bp, fv.bp, sizeof(lv.bp));
        lv.any = true;
    }
    const double nodeOpa = opaMul * std::clamp(fv.opa, 0.0, 1.0);

    if (src && ic) {
        DrawEntry en;
        en.source = src;
        en.icon = ic;
        en.levels.assign(levels.begin(), levels.end()); // chain above this node
        en.iconOx = fv.ox;
        en.iconOy = fv.oy;
        if (fv.hasBp && !f.src.empty()) {
            // icon shape keyframe (face parts); applied in icon uv space
            en.iconWarp = true;
            std::memcpy(en.iconBp, fv.bp, sizeof(en.iconBp));
        }
        en.opa = nodeOpa;
        double z = 0;
        for (const Level& l : levels) z += l.cz;
        en.z = z + fv.cz;
        en.warped = en.iconWarp;
        for (const Level& l : levels)
            if (l.hasWarp) { en.warped = true; break; }
        // the subdivision input comes from THIS node
        // (krkr E:emoterunner.cpp:853-854), independent of where the mesh
        // itself sits in the chain.
        en.authoredMeshDivision = node.meshDivision;
        en.type12Depth = type12Depth + (node.type == 12 ? 1 : 0);
        en.stencil_group = stencil_group;
        en.node_motion = &motion;
        // mask-shape tagging, two rules in priority order:
        for (int gi = 0; gi < int(ctx.stencil_groups.size()); ++gi) {
            const StencilGroup& g = ctx.stencil_groups[gi];
            if (g.scope_chain.size() > chain.size()) continue;
            bool prefix = true;
            for (size_t ci = 0; ci < g.scope_chain.size(); ++ci) {
                if (g.scope_chain[ci].motion != chain[ci].motion ||
                    g.scope_chain[ci].node != chain[ci].node) {
                    prefix = false;
                    break;
                }
            }
            if (!prefix) continue;
            if (std::find(g.labels.begin(), g.labels.end(), node.label) ==
                g.labels.end())
                continue;
            if (int(g.scope_chain.size()) > en.mask_scope_depth) {
                en.mask_scope_depth = int(g.scope_chain.size());
                en.mask_for_group = gi; // deepest scope wins (L/R eyes)
            }
        }
        if (en.mask_for_group < 0) {
            for (int gi = 0; gi < int(ctx.stencil_groups.size()); ++gi) {
                const StencilGroup& g = ctx.stencil_groups[gi];
                if (g.owner_motion != (const void*)&motion) continue;
                if (std::find(g.labels.begin(), g.labels.end(), node.label) ==
                    g.labels.end())
                    continue;
                // several stencils in one motion: prefer the one sharing the
                // longest ancestor prefix with this node (the head stencil is
                // the nearest one to the face outline as well).
                size_t common = 0;
                while (common < g.scope_chain.size() && common < chain.size() &&
                       g.scope_chain[common].motion == chain[common].motion &&
                       g.scope_chain[common].node == chain[common].node)
                    ++common;
                if (int(common) > en.mask_scope_depth) {
                    en.mask_scope_depth = int(common);
                    en.mask_for_group = gi;
                }
            }
        }
        if (emote_mesh_debug()) {
            en.debugPath = path;
            if (node.type == 12 && !en.debugPath.empty()) en.debugPath.back() += "#12";
        }
        if (ctx.out) ctx.out->push_back(en);
        // bounds: affine corners or the mesh grid extremes
        auto add_pt = [&](double x, double y) {
            ctx.minX = std::min(ctx.minX, x);
            ctx.maxX = std::max(ctx.maxX, x);
            ctx.minY = std::min(ctx.minY, y);
            ctx.maxY = std::max(ctx.maxY, y);
        };
        const int entryDiv = en.warped ? entry_mesh_div(en) : 1;
        // the eval-time bounds only matter when the
        // caller asked for them (static_content_bounds / OA_DEBUG_EVAL); the
        // render and collect paths recompute the very same bounds from the
        // grid they already sample, so sampling it here again was pure waste
        // (perf: this pass held ~20% of the collect cost).
        if (ctx.wantBounds) {
            // the chain map is prepared once per entry, then sampled per
            std::unique_ptr<EntryMap> emap = std::make_unique<EntryMap>();
            prepare_entry_map(en, emap.get());
            if (!en.warped) {
                const double corners[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
                for (const auto& c : corners) {
                    double x = 0, y = 0;
                    map_entry_uv(*emap, c[0], c[1], &x, &y);
                    add_pt(x, y);
                }
            } else {
                // sample on the mesh grid actually drawn (node subdivision)
                const int div = entryDiv;
                for (int gy = 0; gy <= div; ++gy) {
                    for (int gx = 0; gx <= div; ++gx) {
                        double x = 0, y = 0;
                        map_entry_uv(*emap, double(gx) / div, double(gy) / div, &x, &y);
                        add_pt(x, y);
                    }
                }
            }
        }
        // win trees put at most tiny accessory subtrees below icon leaves;
        // walk them without adding an icon level. EXCEPT when this icon node
        // carries a mesh patch: the reference applies a chain's bezier
        // surfaces to the whole subtree (emotegeometry.h evaluate), so the
        // patch must ride along for the children.
        if (lv.hasWarp && lv.hasBp) {
            std::vector<Level> childLevels = levels;
            childLevels.push_back(lv);
            for (size_t ci = 0; ci < node.children.size(); ++ci) {
                childLevels.back().childIndex = int(ci);
                eval_node(ctx, motion, node.children[ci], tick, childLevels, opaMul,
                          depth + 1, type12Depth + (node.type == 12 ? 1 : 0), path,
                          chain, stencil_group);
            }
        } else {
            for (int ch : node.children)
                eval_node(ctx, motion, ch, tick, levels, opaMul, depth + 1,
                          type12Depth + (node.type == 12 ? 1 : 0), path, chain,
                          stencil_group);
        }
        path.pop_back();
        chain.pop_back();
        return;
    }

    // transform carrier (blank / coord / mask frames). The chain copy only
    // happens when this level actually carries an affine/morph op; a no-op
    // level (common on layout shells) recurses with the incoming chain.
    if (lv.any) {
        std::vector<Level> childLevels = levels;
        childLevels.push_back(lv);
        for (size_t ci = 0; ci < node.children.size(); ++ci) {
            childLevels.back().childIndex = int(ci);
            eval_node(ctx, motion, node.children[ci], tick, childLevels, nodeOpa,
                      depth + 1, type12Depth + (node.type == 12 ? 1 : 0), path,
                      chain, stencil_group);
        }
    } else {
        for (int ch : node.children)
            eval_node(ctx, motion, ch, tick, levels, nodeOpa, depth + 1,
                      type12Depth + (node.type == 12 ? 1 : 0), path, chain,
                      stencil_group);
    }
    path.pop_back();
    chain.pop_back();
}

void eval_motion(EvalCtx& ctx, const EmoteMotion& motion, double tick,
                 const std::vector<Level>& levels, double opaMul, int depth,
                 int type12Depth, std::vector<std::string>& path,
                 std::vector<StencilFrame>& chain, int stencil_group) {
    // a sub-motion dive starts a fresh node-index space but the ancestor
    // CHAIN continues (mask references cross sub-motion boundaries) and
    // the enclosing stencil group is kept: the eye stencil's content
    // lives under a UD sub-motion inside the stencil subtree
    for (int root : motion.layer)
        eval_node(ctx, motion, root, tick, levels, opaMul, depth,
                  type12Depth, path, chain, stencil_group);
}

// --------------------------------------------------------------------------- bilinear atlas sampling, specialized for the scan loops. One...
struct MeshVert {
    double x = 0, y = 0, u = 0, v = 0;
};

struct AtlasSampler {
    const uint8_t* rgba;
    int texW, texH;
    // atlas mapping precomputed from the icon rect (the ONE mapping,
    // emote_icon_uv_to_atlas, folded to affine form):
    //   au = u * aw + bx ; av = v * ah + by
    float aw, ah, bx, by;
};

struct BilerpTexels {
    const uint8_t* t00;
    const uint8_t* t10;
    const uint8_t* t01;
    const uint8_t* t11;
    float fx, fy;
};

// u/v (icon space 0..1) -> the 4 surrounding atlas texels. Same texels and
// weights as sample_atlas's independent per-texel clamping: after clamping
// the base texel the fractional part clamps too, so a point that fell
// outside the atlas resolves to its border texel exactly like the old
// per-channel clamp (sample_atlas) did.
inline bool bilerp_setup(const AtlasSampler& s, float u, float v, BilerpTexels* b) {
    const float au = u * s.aw + s.bx;
    const float av = v * s.ah + s.by;
    const float x = au * float(s.texW) - 0.5f;
    const float y = av * float(s.texH) - 0.5f;
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    float fx = x - float(x0), fy = y - float(y0);
    if (x0 < 0) {
        x0 = 0;
        fx = 0;
    }
    if (x0 > s.texW - 2) {
        x0 = s.texW - 2;
        fx = 1;
    }
    if (y0 < 0) {
        y0 = 0;
        fy = 0;
    }
    if (y0 > s.texH - 2) {
        y0 = s.texH - 2;
        fy = 1;
    }
    b->fx = fx;
    b->fy = fy;
    const size_t row = size_t(s.texW);
    const uint8_t* p = s.rgba + (size_t(y0) * row + size_t(x0)) * 4;
    b->t00 = p;
    b->t10 = p + 4;
    b->t01 = p + row * 4;
    b->t11 = p + row * 4 + 4;
    return true;
}

inline float bilerp_c(const BilerpTexels& b, int c) {
    const float p00 = float(b.t00[c]);
    const float p10 = float(b.t10[c]);
    const float p01 = float(b.t01[c]);
    const float p11 = float(b.t11[c]);
    const float top = p00 + (p10 - p00) * b.fx;
    const float bot = p01 + (p11 - p01) * b.fx;
    return top + (bot - top) * b.fy;
}

// paint one entry: warped entries go through the forward UV mesh; pure affine entries keep the inverse-UV quad path (same result, much fast...
static int raster_threads() {
    const unsigned hw = std::thread::hardware_concurrency();
    return int(std::clamp(hw ? hw : 2u, 1u, 16u));
}

// --------------------------------------------------------------------------- persistent raster worker pool. The per-entry fan-outs used to...
struct RasterPool {
    const int n;
    std::mutex m;
    std::condition_variable cv, done_cv;
    const std::function<void(int)>* job = nullptr;
    uint64_t gen = 0;
    int done_count = 0;   // workers finished for the current generation
    int expected = 0;     // workers expected to finish that generation
    bool done = false;
    std::vector<std::thread> th;

    explicit RasterPool(int threads) : n(threads > 1 ? threads : 1) {
        th.reserve(size_t(n - 1));
        for (int i = 1; i < n; ++i) th.emplace_back([this, i] { worker(i); });
    }
    static RasterPool& get() {
        const int threads = raster_threads();
        static RasterPool* pool = new RasterPool(threads); // leaky by design
        return *pool;
    }
    void run(const std::function<void(int)>& j, int bands) {
        if (n <= 1 || bands <= 1) {
            j(0);
            return;
        }
        {
            std::lock_guard<std::mutex> g(m);
            job = &j;
            ++gen;
            expected = bands - 1; // band 0 runs on the calling thread
            done_count = 0;
            done = false;
        }
        cv.notify_all();
        j(0);
        std::unique_lock<std::mutex> g(m);
        done_cv.wait(g, [this] { return done; });
        job = nullptr;
    }
    void worker(int idx) {
        uint64_t seen = 0;
        for (;;) {
            std::unique_lock<std::mutex> g(m);
            cv.wait(g, [&] { return (job && gen != seen); });
            seen = gen;
            const int my_gen = int(gen);
            const std::function<void(int)>* j = job;
            const int my_expected = expected;
            g.unlock();
            (*j)(idx);
            g.lock();
            // only the completion of the CURRENT generation counts
            if (int(gen) == my_gen) {
                if (++done_count == my_expected) {
                    done = true;
                    done_cv.notify_one();
                }
            }
        }
    }
};

// mask_in (when non-null): a canvasW*canvasH alpha map multiplying this
// entry's coverage (the stencil composite: content only where the mask
// shape covers). mask_out (when non-null): the entry paints its coverage
// ALPHA only into this 1-byte map (max-blended union — the mask shapes'
// own rasterization); color sampling is skipped.
static void paint_entry(const DrawEntry& en, const Mat& fit, int canvasW, int canvasH,
                        std::vector<uint8_t>* canvas, const uint8_t* mask_in = nullptr,
                        uint8_t* mask_out = nullptr) {
    const std::vector<uint8_t>& rgba = en.source->rgba;
    const int texW = en.source->textureWidth, texH = en.source->textureHeight;
    if (texW <= 0 || texH <= 0 || rgba.empty()) return;
    // the one atlas mapping folded to affine form (identical to
    // emote_icon_uv_to_atlas): au = u*aw + bx, av = v*ah + by
    const AtlasSampler samp{rgba.data(), texW, texH,
                            float(en.icon->width / double(texW)),
                            float(en.icon->height / double(texH)),
                            float(en.icon->left / double(texW)),
                            float(en.icon->top / double(texH))};
    const int nThreads = canvasH >= 600 ? raster_threads() : 1;
    if (!en.warped) {
        // affine fast path: uv -> world is linear
        Mat g = mul(translate(-en.icon->originX - en.iconOx, -en.icon->originY - en.iconOy),
                    scale(en.icon->width, en.icon->height));
        Mat chain;
        for (const Level& l : en.levels) chain = mul(chain, l.op());
        const Mat m = mul(fit, mul(chain, g));
        const double detm = m.a * m.d - m.b * m.c;
        if (std::fabs(detm) < 1e-12) return;
        const double ia = m.d / detm, ib = -m.b / detm, ic = -m.c / detm, id = m.a / detm;
        const double ie = -(ia * m.e + ic * m.f), inf = -(ib * m.e + id * m.f);
        double cxs[4], cys[4];
        for (int i = 0; i < 4; ++i) {
            const double u = (i & 1) ? 1 : 0, v = (i & 2) ? 1 : 0;
            cxs[i] = m.a * u + m.c * v + m.e;
            cys[i] = m.b * u + m.d * v + m.f;
        }
        double minX = cxs[0], maxX = cxs[0], minY = cys[0], maxY = cys[0];
        for (int i = 1; i < 4; ++i) {
            minX = std::min(minX, cxs[i]);
            maxX = std::max(maxX, cxs[i]);
            minY = std::min(minY, cys[i]);
            maxY = std::max(maxY, cys[i]);
        }
        const int x0 = std::max(0, int(std::floor(minX)));
        const int x1 = std::min(canvasW - 1, int(std::ceil(maxX)));
        const int y0 = std::max(0, int(std::floor(minY)));
        const int y1 = std::min(canvasH - 1, int(std::ceil(maxY)));
        if (x0 > x1 || y0 > y1) return;
        // inverse-map increments: u,v are linear in (px,py), so the row
        // start value + a per-pixel step replaces the per-pixel 2x2 madd
        // (same line, float precision — sampling only).
        const float fu0 = float(ia), fu1 = float(ic), fv0 = float(ib), fv1 = float(id);
        const float fuo = float(ie), fvo = float(inf);
        auto scan_band = [&](int r0, int r1) {
            std::vector<uint8_t>& cnv = *canvas;
            for (int py = r0; py <= r1; ++py) {
                float u = fu0 * (float(x0) + 0.5f) + fu1 * (float(py) + 0.5f) + fuo;
                float v = fv0 * (float(x0) + 0.5f) + fv1 * (float(py) + 0.5f) + fvo;
                uint8_t* dst = cnv.data() + (size_t(py) * canvasW + size_t(x0)) * 4;
                for (int px = x0; px <= x1; ++px, dst += 4, u += fu0, v += fv0) {
                    if (u < 0 || u > 1 || v < 0 || v > 1) continue;
                    BilerpTexels bt;
                    bilerp_setup(samp, u, v, &bt);
                    float alpha = bilerp_c(bt, 3) / 255.0f;
                    if (alpha <= 0.001f) continue;
                    alpha *= float(en.opa);
                    if (mask_out) {
                        const uint8_t cov = uint8_t(std::min(255.0f, alpha * 255.0f + 0.5f));
                        uint8_t& slot = mask_out[size_t(py) * canvasW + size_t(px)];
                        if (cov > slot) slot = cov;
                        continue;
                    }
                    if (mask_in) alpha *= float(mask_in[size_t(py) * canvasW + size_t(px)]) / 255.0f;
                    if (alpha <= 0.001f) continue;
                    const float opa = alpha;
                    const float da = float(dst[3]) / 255.0f;
                    const float outa = opa + da * (1.0f - opa);
                    if (outa <= 0.0001f) continue;
                    const float r = bilerp_c(bt, 0);
                    const float g = bilerp_c(bt, 1);
                    const float b = bilerp_c(bt, 2);
                    dst[0] = uint8_t(std::clamp((r * opa + float(dst[0]) * da * (1.0f - opa)) / outa,
                                                0.0f, 255.0f));
                    dst[1] = uint8_t(std::clamp((g * opa + float(dst[1]) * da * (1.0f - opa)) / outa,
                                                0.0f, 255.0f));
                    dst[2] = uint8_t(std::clamp((b * opa + float(dst[2]) * da * (1.0f - opa)) / outa,
                                                0.0f, 255.0f));
                    dst[3] = uint8_t(std::clamp(outa * 255.0f, 0.0f, 255.0f));
                }
            }
        };
        const int rows = y1 - y0 + 1;
        const int band = std::max(1, (rows + nThreads - 1) / nThreads);
        if (nThreads <= 1 || rows < 64) {
            scan_band(y0, y1);
        } else {
            RasterPool::get().run([&](int t) {
                const int b0 = y0 + t * band;
                if (t >= nThreads || b0 > y1) return;
                scan_band(b0, std::min(y1, b0 + band - 1));
            }, nThreads);
        }
        return;
    }
    // warped: forward mesh at the subdivision the ICON NODE authorises
    // (krkr E:emoterunner.cpp:853-854; the adaptive
    // clamp(round(diag/90),6,18) stays reachable via OA_EMOTE_MESHDIV).
    const int div = entry_mesh_div(en);
    const int n = div + 1;
    // vertex-buffer reuse: grid + triangle-soup scratch live in
    static thread_local std::vector<MeshVert> verts_tls;
    static thread_local std::vector<MeshVert> tris_tls;
    static thread_local std::vector<uint32_t> tri_bucket_tls; // band bucketing
    std::vector<MeshVert>& verts = verts_tls;
    std::vector<MeshVert>& tris = tris_tls;
    verts.resize(size_t(n) * n);
    tris.resize(size_t(div) * div * 6);
    EntryMap emap; // chain prepared once per entry, sampled per grid point
    prepare_entry_map(en, &emap);
    for (int gy = 0; gy < n; ++gy) {
        for (int gx = 0; gx < n; ++gx) {
            double u = 0, v = 0;
            map_entry_uv(emap, double(gx) / div, double(gy) / div, &u, &v);
            MeshVert& mv = verts[size_t(gy) * n + gx];
            mv.x = fit.a * u + fit.c * v + fit.e;
            mv.y = fit.b * u + fit.d * v + fit.f;
            mv.u = double(gx) / div;
            mv.v = double(gy) / div;
        }
    }
    size_t ti = 0;
    for (int gy = 0; gy < div; ++gy) {
        for (int gx = 0; gx < div; ++gx) {
            const MeshVert& p0 = verts[size_t(gy) * n + gx];
            const MeshVert& p1 = verts[size_t(gy) * n + gx + 1];
            const MeshVert& p2 = verts[size_t(gy + 1) * n + gx];
            const MeshVert& p3 = verts[size_t(gy + 1) * n + gx + 1];
            tris[ti++] = p0;
            tris[ti++] = p1;
            tris[ti++] = p2;
            tris[ti++] = p1;
            tris[ti++] = p3;
            tris[ti++] = p2;
        }
    }
    // Row-band bucketing: each band worker used to re-walk EVERY triangle
    const int rows_total = canvasH;
    const int nBands = (nThreads > 1 && rows_total >= 256) ? nThreads : 1;
    const int bandH = std::max(1, (canvasH + nBands - 1) / nBands);
    std::vector<uint32_t>& bucket = tri_bucket_tls;
    static thread_local std::vector<uint32_t> band_start_tls;
    static thread_local std::vector<uint32_t> band_fill_tls;
    std::vector<uint32_t>& band_start = band_start_tls;
    {
        const size_t nTris = tris.size() / 3;
        static thread_local std::vector<int> tb0_tls, tb1_tls;
        std::vector<int>& tb0 = tb0_tls;
        std::vector<int>& tb1 = tb1_tls;
        tb0.resize(nTris);
        tb1.resize(nTris);
        band_start.assign(size_t(nBands) + 1, 0);
        for (size_t t = 0; t < nTris; ++t) {
            const MeshVert& a = tris[t * 3];
            const MeshVert& b = tris[t * 3 + 1];
            const MeshVert& c = tris[t * 3 + 2];
            const double ymin = std::min({a.y, b.y, c.y});
            const double ymax = std::max({a.y, b.y, c.y});
            const int b0 = std::clamp(int(std::floor(ymin)) / bandH, 0, nBands - 1);
            const int b1 = std::clamp(int(std::ceil(ymax)) / bandH, 0, nBands - 1);
            tb0[t] = b0;
            tb1[t] = b1;
            for (int bb = b0; bb <= b1; ++bb) ++band_start[size_t(bb) + 1];
        }
        for (int b = 0; b < nBands; ++b) band_start[size_t(b) + 1] += band_start[size_t(b)];
        bucket.assign(band_start[size_t(nBands)], 0);
        band_fill_tls.assign(size_t(nBands), 0);
        for (size_t t = 0; t < nTris; ++t)
            for (int bb = tb0[t]; bb <= tb1[t]; ++bb)
                bucket[band_start[size_t(bb)] + band_fill_tls[size_t(bb)]++] = uint32_t(t);
    }
    auto scan_band = [&](int band, int r0_, int r1_) {
        const uint32_t beg = band_start[size_t(band)];
        const uint32_t end = band_start[size_t(band) + 1];
        for (uint32_t bi = beg; bi < end; ++bi) {
            const size_t t = size_t(bucket[bi]) * 3;
            const MeshVert& amv = tris[t];
            const MeshVert& bmv = tris[t + 1];
            const MeshVert& cmv = tris[t + 2];
            const double minY = std::min({amv.y, bmv.y, cmv.y});
            const double maxY = std::max({amv.y, bmv.y, cmv.y});
            const int y0 = std::max(r0_, int(std::floor(minY)));
            const int y1 = std::min(r1_, int(std::ceil(maxY)));
            if (y0 > y1) continue;
            const double minX = std::min({amv.x, bmv.x, cmv.x});
            const double maxX = std::max({amv.x, bmv.x, cmv.x});
            const int x0 = std::max(0, int(std::floor(minX)));
            const int x1 = std::min(canvasW - 1, int(std::ceil(maxX)));
            if (x0 > x1) continue;
            const double denom = (bmv.x - amv.x) * (cmv.y - amv.y) - (cmv.x - amv.x) * (bmv.y - amv.y);
            if (std::fabs(denom) < 1e-12) continue;
            const double inv = 1.0 / denom;
            for (int py = y0; py <= y1; ++py) {
                for (int px = x0; px <= x1; ++px) {
                    const double wx = px + 0.5;
                    const double wy = py + 0.5;
                    const double r1 = (bmv.y - cmv.y) * (wx - cmv.x) + (cmv.x - bmv.x) * (wy - cmv.y);
                    const double r2 = (cmv.y - amv.y) * (wx - cmv.x) + (amv.x - cmv.x) * (wy - cmv.y);
                    const double r3 = denom - r1 - r2;
                    if (r1 < -0.5 || r2 < -0.5 || r3 < -0.5) continue;
                    const float u = float((r1 * amv.u + r2 * bmv.u + r3 * cmv.u) * inv);
                    const float v = float((r1 * amv.v + r2 * bmv.v + r3 * cmv.v) * inv);
                    BilerpTexels bt;
                    bilerp_setup(samp, u, v, &bt);
                    float alpha = bilerp_c(bt, 3) / 255.0f;
                    if (alpha <= 0.001f) continue;
                    alpha *= float(en.opa);
                    uint8_t* dst = canvas->data() + (size_t(py) * canvasW + size_t(px)) * 4;
                    if (mask_out) {
                        const uint8_t cov = uint8_t(std::min(255.0f, alpha * 255.0f + 0.5f));
                        uint8_t& slot = mask_out[size_t(py) * canvasW + size_t(px)];
                        if (cov > slot) slot = cov;
                        continue;
                    }
                    if (mask_in) alpha *= float(mask_in[size_t(py) * canvasW + size_t(px)]) / 255.0f;
                    if (alpha <= 0.001f) continue;
                    const float opa = alpha;
                    const float da = float(dst[3]) / 255.0f;
                    const float outa = opa + da * (1.0f - opa);
                    if (outa <= 0.0001f) continue;
                    const float r = bilerp_c(bt, 0);
                    const float g = bilerp_c(bt, 1);
                    const float b = bilerp_c(bt, 2);
                    dst[0] = uint8_t(std::clamp((r * opa + float(dst[0]) * da * (1.0f - opa)) / outa,
                                                0.0f, 255.0f));
                    dst[1] = uint8_t(std::clamp((g * opa + float(dst[1]) * da * (1.0f - opa)) / outa,
                                                0.0f, 255.0f));
                    dst[2] = uint8_t(std::clamp((b * opa + float(dst[2]) * da * (1.0f - opa)) / outa,
                                                0.0f, 255.0f));
                    dst[3] = uint8_t(std::clamp(outa * 255.0f, 0.0f, 255.0f));
                }
            }
        }
    };
    if (nBands <= 1) {
        scan_band(0, 0, canvasH - 1);
    } else {
        RasterPool::get().run([&](int t) {
            const int b0 = t * bandH;
            if (t >= nBands || b0 >= canvasH) return;
            scan_band(t, b0, std::min(canvasH - 1, b0 + bandH - 1));
        }, nBands);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// mesh subdivision policy — see emote_file.h.
// ---------------------------------------------------------------------------
int emote_node_mesh_division(int authored) {
    // krkr: int div = node ? node->meshDivision : 0; if (div < 2) div = 8;
    return clamp_mesh_div(authored < kEmoteMeshDivMin ? kEmoteMeshDivFallback : authored);
}

int emote_adaptive_mesh_division(double iconW, double iconH) {
    return adaptive_mesh_div(iconW, iconH); // adaptive rule, verbatim
}

int emote_effective_mesh_division(int authored, double iconW, double iconH) {
    // 唯一规则（node-adaptive 混合）：有作者值用作者值；没有/非法则退回
    // 自适应网格，而不是 krkr 的扁平 8 —— 回退永远不粗于自适应网格。
    if (authored < kEmoteMeshDivMin) return adaptive_mesh_div(iconW, iconH);
    return emote_node_mesh_division(authored);
}

bool emote_mesh_debug() { return meshdiv_env().debug; }

// ---------------------------------------------------------------------------
// numeric-robustness policy (see the env block above).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// stencil composite A/B arm (OA_EMOTE_STENCIL=0 disables the type-12 mask
// compositing — the pre-fix behaviour arm for same-process comparison).
// ---------------------------------------------------------------------------
bool static_content_bounds(const EmoteFile& file, const std::map<std::string, double>& vars,
                           double* minX, double* minY, double* maxX, double* maxY) {
    EvalCtx ctx{file, vars, nullptr, nullptr, {}};
    ctx.wantBounds = true; // this is the one consumer of the eval-time bounds
    const int midx = file.find_motion(file.baseChara, file.baseMotion);
    if (midx < 0) return false;
    std::vector<Level> empty;
    std::vector<std::string> p;
    std::vector<StencilFrame> chain;
    eval_motion(ctx, file.motions[midx], 0, empty, 1.0, 0, 0, p, chain, -1);
    if (ctx.minX > ctx.maxX) return false;
    *minX = ctx.minX;
    *minY = ctx.minY;
    *maxX = ctx.maxX;
    *maxY = ctx.maxY;
    return true;
}

// evaluate the base motion at the given vars into draw entries (shared by
// the CPU raster and the GPU part collector); decodes the used atlases.
static bool evaluate_entries(const EmoteFile& file,
                             const std::map<std::string, double>& vars,
                             std::vector<DrawEntry>* entries, std::string* err,
                             std::vector<StencilGroup>* stencil_groups = nullptr,
                             bool light = false) {
    // selectorControl: the option-label variables (fade_a..fade_e) are DERIVED
    std::map<std::string, double> resolvedVars;
    const std::map<std::string, double>* evalVars = &vars;
    if (!file.selectors.empty()) {
        resolvedVars = vars;
        for (const auto& [k, v] : file.variableDefaults)
            resolvedVars.emplace(k, v); // caller-provided values win
        for (const auto& sel : file.selectors) {
            if (sel.options.empty()) continue;
            const auto it = resolvedVars.find(sel.label);
            const double selv =
                std::clamp(it == resolvedVars.end() ? 0.0 : it->second, 0.0,
                           double(sel.options.size() - 1));
            for (size_t i = 0; i < sel.options.size(); ++i) {
                const auto& opt = sel.options[i];
                const double d = std::min(1.0, std::fabs(selv - double(i)));
                resolvedVars[opt.label] =
                    opt.onValue + (opt.offValue - opt.onValue) * d;
            }
        }
        evalVars = &resolvedVars;
    }
    EvalCtx ctx{file, *evalVars, entries, err, {}};
    // the render/collect paths compute their own content
    // bounds from the grid they sample anyway — only the debug trace wants the
    // eval-time bounds here (static_content_bounds builds its own context).
    ctx.wantBounds = false;
    const int midx = file.find_motion(file.baseChara, file.baseMotion);
    if (midx < 0) {
        if (err) *err = "emote: base motion not found";
        return false;
    }
    // pre-scan stencil groups so mask nodes drawn before their stencil can
    // still be tagged (the eye-white precedes the eye stencil in tree order)
    if (!light) {
        for (int root : file.motions[midx].layer)
            scan_stencil_groups(ctx, file.motions[midx], root,
                                std::vector<StencilFrame>{});
    }
    std::vector<Level> empty;
    std::vector<std::string> p;
    std::vector<StencilFrame> chain;
    eval_motion(ctx, file.motions[midx], 0, empty, 1.0, 0, 0, p, chain, -1);
    if (stencil_groups) *stencil_groups = ctx.stencil_groups;
    if (entries->empty()) return true; // nothing drawn; canvas stays transparent
    if (light) return true; // anchor sampling: geometry only, no atlas work
    // decode atlases for the used sources
    for (const DrawEntry& en : *entries) {
        for (auto& s : file.sources) {
            if (s.get() == en.source && !s->rgbaDecoded) {
                if (!file.ensure_atlas(s.get(), err)) return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// physics anchors: the world position of a few named nodes, taken from the same
// evaluation the renderer uses (each draw entry carries its ancestor Level
// chain, so a blank carrier that parents real artwork is always in there).
// ---------------------------------------------------------------------------
bool emote_collect_anchors(const EmoteFile& file,
                           const std::map<std::string, double>& vars,
                           const std::set<std::string>& labels,
                           std::map<std::string, std::pair<double, double>>* out,
                           std::string* err) {
    if (!out) return false;
    out->clear();
    if (labels.empty()) return true;
    EvalCtx ctx{file, vars, nullptr, err, {}};
    ctx.wantBounds = false;
    const int midx = file.find_motion(file.baseChara, file.baseMotion);
    if (midx < 0) {
        if (err) *err = "emote: base motion not found";
        return false;
    }
    // Anchor nodes are MARKERS (type-1 shape nodes with no artwork and no
    // children), so they never show up in a draw entry's Level chain — walk
    // the tree itself with the renderer's own tick/frame selection.
    std::vector<Level> levels;
    std::function<void(const EmoteMotion&, int, double, int)> walk =
        [&](const EmoteMotion& m, int idx, double parentTick, int depth) {
            if (idx < 0 || idx >= int(m.nodes.size()) || depth > 64) return;
            const EmoteNode& node = m.nodes[idx];
            if (node.removed || node.frames.empty()) return;
            const double tick = node_tick(m, node, parentTick, ctx);
            int next = -1;
            const int cur = pick_frame(node.frames, tick, &next, node.type == 1);
            if (cur < 0) return;
            const FrameVals fv = frame_vals(node.frames, cur, next, tick, nullptr);
            Level lv = (next >= 0)
                           ? Level::from_frame_vals(*fv.frame, node.frames[size_t(next)], fv)
                           : Level::from_frame(*fv.frame, true);
            lv.inheritMask = node.inheritMask;
            lv.name = &node.label;
            levels.push_back(lv);
            if (labels.count(node.label) != 0 && out->count(node.label) == 0) {
                double la = 1, lb = 0, lc = 0, ld = 1, le = 0, lf = 0;
                for (const Level& l : levels) {
                    if (l.angle == 0 && l.sx == 0 && l.sy == 0 && l.zx == 1 && l.zy == 1) {
                        le += la * l.cx + lb * l.cy;
                        lf += lc * l.cx + ld * l.cy;
                    } else {
                        const Mat op = l.op();
                        const double nla = la * op.a + lb * op.b;
                        const double nlb = la * op.c + lb * op.d;
                        const double nlc = lc * op.a + ld * op.b;
                        const double nld = lc * op.c + ld * op.d;
                        const double nle = la * op.e + lb * op.f + le;
                        const double nlf = lc * op.e + ld * op.f + lf;
                        la = nla; lb = nlb; lc = nlc; ld = nld; le = nle; lf = nlf;
                    }
                }
                (*out)[node.label] = {le, lf};
            }
            if (fv.frame->isSubMotion) {
                const int sm = file.find_motion(fv.frame->subObject, fv.frame->subMotion);
                if (sm >= 0) {
                    const EmoteMotion& sub = file.motions[size_t(sm)];
                    for (int root : sub.layer) walk(sub, root, tick, depth + 1);
                }
            }
            for (int ch : node.children) walk(m, ch, tick, depth + 1);
            levels.pop_back();
        };
    const EmoteMotion& base = file.motions[size_t(midx)];
    for (int root : base.layer) walk(base, root, 0, 0);
    return true;
}

bool render_static_frame(const EmoteFile& file, const std::map<std::string, double>& vars,
                         int canvasW, int canvasH, const StaticRenderOptions& opt,
                         std::vector<uint8_t>* rgba, std::string* err) {
    if (canvasW <= 0 || canvasH <= 0) return false;
    std::vector<DrawEntry> entries;
    std::vector<StencilGroup> stencil_groups;
    if (!evaluate_entries(file, vars, &entries, err, &stencil_groups)) return false;
    if (entries.empty()) return true; // nothing drawn; canvas stays transparent

    // ---- provisional psb -> canvas mapping --------------------------------
    // Whole-figure fit with a head margin; scale/dx/dy multiply after the
    // fit. A later pass will calibrate against the official runtime.
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    EntryMap boundsMap;
    for (const DrawEntry& en : entries) {
        const int divb = en.warped ? entry_mesh_div(en) : 1;
        prepare_entry_map(en, &boundsMap);
        // OA_DEBUG_WARP=<substr>: which morph carriers actually reach this
        // entry, and where the entry sits inside each carrier's region.
        for (int gy = 0; gy <= divb; ++gy)
            for (int gx = 0; gx <= divb; ++gx) {
                double x = 0, y = 0;
                map_entry_uv(boundsMap, double(gx) / divb, double(gy) / divb, &x, &y);
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
    }
    double k = 1.0, offX = 0, offY = 0;
    if (minX <= maxX && minY <= maxY) {
        const double cw = maxX - minX;
        const double ch = maxY - minY;
        if (cw > 0 && ch > 0) {
            double kx = canvasW / cw;
            double ky = canvasH / ch;
            if (opt.fitToCanvas) {
                k = std::min(kx, ky);
                if (k > 0) {
                    const double margin = opt.headMargin;
                    const double contentH = ch * k;
                    const double top = margin + (canvasH - margin - contentH) * 0.5;
                    offY = top - minY * k;
                    offX = (canvasW - cw * k) * 0.5 - minX * k;
                }
            }
        }
    }
    k *= opt.scale;
    offX += opt.dx;
    offY += opt.dy;
    std::stable_sort(entries.begin(), entries.end(), [](const DrawEntry& a, const DrawEntry& b) {
        if (a.z != b.z) return a.z < b.z;
        return false;
    });

    rgba->assign(size_t(canvasW) * canvasH * 4, 0);
    Mat fit = mul(translate(offX, offY), scale(k, k));

    // stencil composite: per group, the mask shapes' union alpha rasterized
    const size_t nGroups = stencil_groups.size();
    std::vector<char> mask_built(nGroups, 0);
    std::vector<std::vector<uint8_t>> mask_bits(nGroups);
    auto mask_bits_of = [&](int gi) -> uint8_t* {
        if (!mask_built[gi]) {
            mask_bits[gi].assign(size_t(canvasW) * canvasH, 0);
            mask_built[gi] = 1;
        }
        return mask_bits[gi].data();
    };

    const bool dbg_stencil = false;
    // pass 0: every mask shape feeds its group's bitmap. Nothing lands on the
    // canvas here — each shape is drawn again as normal artwork in the walk
    // below (it is both a clip source and a picture).
    for (const DrawEntry& en : entries)
        if (en.mask_for_group >= 0)
            paint_entry(en, fit, canvasW, canvasH, rgba, nullptr,
                        mask_bits_of(en.mask_for_group));

    for (const DrawEntry& en : entries) {
        if (en.mask_for_group >= 0) {
            // A stencil mask layer is REAL ARTWORK as well as a clip source:
            const uint8_t* own =
                en.stencil_group >= 0 ? mask_bits_of(en.stencil_group) : nullptr;
            paint_entry(en, fit, canvasW, canvasH, rgba, own, nullptr);
            continue;
        }
        const uint8_t* ma =
            en.stencil_group >= 0 ? mask_bits_of(en.stencil_group) : nullptr;
        paint_entry(en, fit, canvasW, canvasH, rgba, ma);
    }
    if (dbg_stencil) {
        for (size_t gi = 0; gi < stencil_groups.size(); ++gi) {
            if (!mask_built[gi]) continue;
            long nz = 0;
            for (uint8_t v : mask_bits[gi])
                if (v) ++nz;
            std::printf("[stencil-raster] group %zu mask bits nonzero=%ld\n", gi, nz);
        }
    }
    return true;
}

bool emote_collect_parts(const EmoteFile& file, const std::map<std::string, double>& vars,
                         int canvasW, int canvasH, const StaticRenderOptions& opt,
                         std::vector<EmoteDrawPart>* parts, std::string* err) {
    if (canvasW <= 0 || canvasH <= 0) return false;
    parts->clear();
    std::vector<DrawEntry> entries;
    if (!evaluate_entries(file, vars, &entries, err)) return false;
    if (entries.empty()) return true;

    // content bounds (identical rules to the eval-time ctx bounds)
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    auto add_pt = [&](double x, double y) {
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    };
    static thread_local std::vector<EmotePartVertex> gridpts;        // world px + icon uv
    static thread_local std::vector<std::pair<size_t, size_t>> slices; // (off, count)
    gridpts.clear();
    slices.assign(entries.size(), {0, 0});
    EntryMap boundsMap;
    for (size_t ei = 0; ei < entries.size(); ++ei) {
        DrawEntry& en = entries[ei];
        en.slot = int(ei); // survives the z sort below
        prepare_entry_map(en, &boundsMap);
        const size_t off = gridpts.size();
        if (!en.warped) {
            const double c[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int i = 0; i < 4; ++i) {
                double x = 0, y = 0;
                map_entry_uv(boundsMap, c[i][0], c[i][1], &x, &y);
                add_pt(x, y);
                gridpts.push_back({x, y, c[i][0], c[i][1]});
            }
            slices[ei] = {off, 4};
        } else {
            const int divb = entry_mesh_div(en);
            const int nb = divb + 1;
            for (int gy = 0; gy < nb; ++gy)
                for (int gx = 0; gx < nb; ++gx) {
                    const double u = double(gx) / divb, v = double(gy) / divb;
                    double x = 0, y = 0;
                    map_entry_uv(boundsMap, u, v, &x, &y);
                    add_pt(x, y);
                    gridpts.push_back({x, y, u, v});
                }
            slices[ei] = {off, size_t(nb) * size_t(nb)};
        }
    }
    // provisional psb -> canvas mapping (same as render_static_frame)
    double k = 1.0, offX = 0, offY = 0;
    if (minX <= maxX && minY <= maxY) {
        const double cw = maxX - minX;
        const double ch = maxY - minY;
        if (cw > 0 && ch > 0) {
            double kx = canvasW / cw;
            double ky = canvasH / ch;
            if (opt.fitToCanvas) {
                k = std::min(kx, ky);
                if (k > 0) {
                    const double margin = opt.headMargin;
                    const double contentH = ch * k;
                    const double top = margin + (canvasH - margin - contentH) * 0.5;
                    offY = top - minY * k;
                    offX = (canvasW - cw * k) * 0.5 - minX * k;
                }
            }
        }
    }
    k *= opt.scale;
    offX += opt.dx;
    offY += opt.dy;
    const Mat fit = mul(translate(offX, offY), scale(k, k));

    std::stable_sort(entries.begin(), entries.end(), [](const DrawEntry& a, const DrawEntry& b) {
        if (a.z != b.z) return a.z < b.z;
        return false;
    });
    auto to_canvas = [&](double* x, double* y) {
        const double nx = fit.a * *x + fit.c * *y + fit.e;
        const double ny = fit.b * *x + fit.d * *y + fit.f;
        *x = nx;
        *y = ny;
    };
    // Vertex-buffer reuse: refill the caller's parts IN PLACE. A cleared
    const bool meshdbg = emote_mesh_debug();
    parts->reserve(entries.size());
    size_t outIdx = 0;
    for (const DrawEntry& en : entries) {
        int srcIdx = -1;
        for (size_t i = 0; i < file.sources.size(); ++i)
            if (file.sources[i].get() == en.source) { srcIdx = int(i); break; }
        if (srcIdx < 0) continue;
        const EmoteSource& src = *file.sources[size_t(srcIdx)];
        int iconIdx = -1;
        for (size_t i = 0; i < src.icons.size(); ++i)
            if (&src.icons[i] == en.icon) { iconIdx = int(i); break; }
        if (iconIdx < 0) continue;
        if (outIdx >= parts->size()) parts->emplace_back();
        EmoteDrawPart& part = (*parts)[outIdx++];
        part.source = srcIdx;
        part.icon = iconIdx;
        part.alpha = en.opa;
        part.authoredMeshDivision = en.authoredMeshDivision;
        part.meshDivision = en.warped ? entry_mesh_div(en) : 1;
        part.type12Depth = en.type12Depth;
        part.stencil_group = en.stencil_group;
        part.mask_role = en.mask_for_group >= 0 ? 1 : 0;
        part.mask_group = en.mask_for_group;
        if (meshdbg) {
            part.nodePath.clear();
            for (size_t i = 0; i < en.debugPath.size(); ++i) {
                if (i) part.nodePath += '/';
                part.nodePath += en.debugPath[i];
            }
        } else if (!part.nodePath.empty()) {
            part.nodePath.clear();
        }
        // the grid points were computed once in the bounds pass above and
        // are reused here (indexed by the entry's slot, which survives the z
        // sort). Only the canvas transform and the triangle soup are done now.
        const std::pair<size_t, size_t> sl = slices[size_t(en.slot)];
        if (!en.warped) {
            double w[4][2];
            for (int i = 0; i < 4; ++i) {
                double x = gridpts[sl.first + size_t(i)].x;
                double y = gridpts[sl.first + size_t(i)].y;
                to_canvas(&x, &y);
                w[i][0] = x;
                w[i][1] = y;
            }
            const EmotePartVertex v0{w[0][0], w[0][1], 0, 0};
            const EmotePartVertex v1{w[1][0], w[1][1], 1, 0};
            const EmotePartVertex v2{w[2][0], w[2][1], 1, 1};
            const EmotePartVertex v3{w[3][0], w[3][1], 0, 1};
            part.verts = {v0, v1, v2, v0, v2, v3};
        } else {
            // node-authorised subdivision + a reused grid scratch; the
            // soup is written straight into the part's own buffer at final
            // size, so nothing reallocates once the capacity is warm.
            const int div = part.meshDivision;
            const int n = div + 1;
            static thread_local std::vector<EmotePartVertex> verts;
            verts.resize(size_t(n) * n);
            for (size_t ki = 0; ki < verts.size(); ++ki) {
                double x = gridpts[sl.first + ki].x;
                double y = gridpts[sl.first + ki].y;
                to_canvas(&x, &y);
                verts[ki] = {x, y, gridpts[sl.first + ki].u, gridpts[sl.first + ki].v};
            }
            part.verts.resize(size_t(div) * div * 6);
            size_t ti = 0;
            for (int gy = 0; gy < div; ++gy) {
                for (int gx = 0; gx < div; ++gx) {
                    const EmotePartVertex& p0 = verts[size_t(gy) * n + gx];
                    const EmotePartVertex& p1 = verts[size_t(gy) * n + gx + 1];
                    const EmotePartVertex& p2 = verts[size_t(gy + 1) * n + gx];
                    const EmotePartVertex& p3 = verts[size_t(gy + 1) * n + gx + 1];
                    part.verts[ti++] = p0;
                    part.verts[ti++] = p1;
                    part.verts[ti++] = p2;
                    part.verts[ti++] = p1;
                    part.verts[ti++] = p3;
                    part.verts[ti++] = p2;
                }
            }
        }
    }
    parts->resize(outIdx); // drop stale tail parts, keep their peers' buffers
    return true;
}

} // namespace oa::emote
