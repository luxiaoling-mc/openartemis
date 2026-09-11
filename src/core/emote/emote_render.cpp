// Static E-mote evaluation + CPU raster with bezier mesh warp.
// Evaluates the file's base motion
// (metadata.base: object all_parts / motion タイムライン構造 -> 全体構造) at
// the given variable values and paints icon quads into an RGBA canvas.
//
// Node semantics mirror the krkr emoteplayer reference (read-only scheme
// reference, no code copied):
//  * a node picks the last frame with time <= tick; a frame without content
//    makes the node itself invisible, but its children are still walked;
//  * a parameter node substitutes its own axis tick (var value -> tick via
//    range/division) for *its* frame selection; children/sub-motions keep the
//    incoming motion tick (timeOffset adds for sub-motions);
//  * frame content is one of: source icon (draw), sub-motion reference
//    (src=object, icon=motion, motion.timeOffset), blank region + bezier
//    mesh (morph carrier), or pure layout coords.
//
// Bezier mesh semantics (own autonomous interpretation,
// cross-checked against the krkr reference surface chain):
//  * a morph (blank) frame carries a region (icon "W:H:OX:OY" with OX=W/2,
//    OY=H/2 — a box centred on the part origin) and a 16-point bicubic
//    Bezier control grid stored row-major as (u,v) pairs with rows on v and
//    columns on u (identity grid = (c/3, r/3)). The identity grid leaves the
//    subtree geometry untouched; deviations warp it like a rubber sheet:
//        uv = (p + (OX,OY)) / (W,H);  p' = bezier(uv) * (W,H) - (OX,OY)
//  * an icon frame may carry its own grid: the warp is applied in icon uv
//    space before the quad geometry (face parts; shape keyframes like the
//    mouth 0003/0009 pair are per-frame grids, not deviations).
//  * grids interpolate linearly between two interpolatable keyframes (same
//    icon for icon grids), matching krkr's linear keyframe blend.
// Raster: warped entries are forward-evaluated on a per-icon UV grid and
// painted triangle-by-triangle with bilinear atlas sampling; pure-affine
// entries keep the fast inverse-UV quad path. The
// grid is the subdivision the ICON NODE authorises (node.meshDivision, <2 ->
// 8, krkr E:emoterunner.cpp:853-854) on both raster paths — the adaptive
// clamp(round(diag/90),6,18) survives only behind OA_EMOTE_MESHDIV.
// The chain map is prepared once per entry. The atlas
// mapping has exactly one definition, emote_icon_uv_to_atlas (no trim term).
// Documented deviations that remain: type-12 stencil/mask
// composites are ignored (census: 6 stencil nodes/file, all with a
// non-empty mask-layer list; 3/64/93 draws sit under one at the default pose
// — 0.46%/1.55%/1.82% of the drawn triangle area);
// clip nodes are ignored; icon nodes with children are walked without an
// added icon level; the psb->canvas mapping stays provisional fit-to-canvas
// until the GPU path lands.
#include "core/emote/emote_file.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <map>
#include <string>
#include <vector>

namespace oa::emote {

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// mesh subdivision policy (API in emote_file.h).
// The environment is latched once at static-init time;
// emote_set_mesh_div_policy re-points the policy in-process for same-process
// REF/NEW A/B (no cross-process frame comparison).
// ---------------------------------------------------------------------------
struct MeshDivEnv {
    int mode = kEmoteMeshDivNodeAdaptive; // default
    int fixed = 0;
    bool debug = false;
    MeshDivEnv() {
        if (const char* v = std::getenv("OA_EMOTE_MESHDIV")) {
            if (std::strcmp(v, "adaptive") == 0 || std::strcmp(v, "legacy") == 0) {
                mode = kEmoteMeshDivAdaptive;
            } else if (std::strcmp(v, "node") == 0) {
                mode = kEmoteMeshDivNode; // strict krkr fallback (<2 -> 8)
            } else if (*v) {
                const int n = std::atoi(v);
                if (n > 0) {
                    mode = kEmoteMeshDivFixed;
                    fixed = n;
                }
            }
        }
        const char* d = std::getenv("OA_EMOTE_MESHDBG");
        debug = d && *d && d[0] != '0';
    }
};
const MeshDivEnv& meshdiv_env() {
    static const MeshDivEnv env; // lazy: no cross-TU static-init-order hazard
    return env;
}
std::atomic<int> g_meshdiv_mode{-1}; // -1 = environment latch
std::atomic<int> g_meshdiv_fixed{0};

// ---------------------------------------------------------------------------
// numeric-robustness switches. Same latch/re-point scheme
// as the mesh policy so a single process can A/B a REF and a NEW arm
// (no cross-process frame comparison).
//   OA_EMOTE_ANGLEWRAP=0      disable the +-180 degree interpolation wrap
//   OA_EMOTE_TAILFALLBACK=1   enable the content-less-tail fallback
//   OA_EMOTE_PARAMOOB=1       enable "parameter index out of range -> not
//                             drawn"
//   OA_EMOTE_NONFINITE=0      disable the NaN/Inf guard
// Defaults: the angle wrap and the non-finite
// guard follow the reference and are provably inert on the three works
// (census: 0 non-finite frames); the two behaviour changes default OFF.
// ---------------------------------------------------------------------------
bool env_flag_on(const char* name, bool dflt) {
    const char* v = std::getenv(name);
    if (!v || !*v) return dflt;
    if (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 ||
        std::strcmp(v, "false") == 0 || std::strcmp(v, "no") == 0)
        return false;
    return true;
}

struct S2Env {
    int angleWrap = 1;
    int tailFallback = 0;
    int paramOob = 0;
    int nonfinite = 1;
    S2Env() {
        angleWrap = env_flag_on("OA_EMOTE_ANGLEWRAP", 1);
        tailFallback = env_flag_on("OA_EMOTE_TAILFALLBACK", 0);
        paramOob = env_flag_on("OA_EMOTE_PARAMOOB", 0);
        nonfinite = env_flag_on("OA_EMOTE_NONFINITE", 1);
    }
};
const S2Env& s2_env() {
    static const S2Env env;
    return env;
}
std::atomic<int> g_s2_anglewrap{-1}, g_s2_tailfallback{-1}, g_s2_paramoob{-1},
    g_s2_nonfinite{-1};

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

void bezier_patch(const float bp[32], double u, double v, double* bx, double* by) {
    const double bu[4] = {bez_b0(u), bez_b1(u), bez_b2(u), bez_b3(u)};
    const double bv[4] = {bez_b0(v), bez_b1(v), bez_b2(v), bez_b3(v)};
    double rx = 0, ry = 0;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const double wgt = bu[c] * bv[r]; // columns -> u, rows -> v
            rx += double(bp[(r * 4 + c) * 2 + 0]) * wgt;
            ry += double(bp[(r * 4 + c) * 2 + 1]) * wgt;
        }
    }
    *bx = rx;
    *by = ry;
}

// ---------------------------------------------------------------------------
// one evaluated frame op along a node path (px -> px, y-down). A level is
// either a plain affine op (layout coords / rotations / scales) or a morph
// warp: a centred region (OX,OY,W,H) + Bezier grid that deforms the subtree
// geometry (identity grid -> no-op).
// ---------------------------------------------------------------------------
struct FrameVals; // interpolated frame values (defined below)
struct Level {
    double cx = 0, cy = 0, cz = 0;
    double angle = 0;
    double sx = 0, sy = 0; // shear
    double zx = 1, zy = 1; // scale
    bool any = false;      // has any effect at all

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
    // values (defined below after FrameVals). Keyframes between two sample
    // points (krkr rule: any cur type except a type-2 key whose follower is
    // not type 2) contribute their linear blend — the old code rebuilt the
    // level from the raw current key, so parameter axes (e.g. head_slant's
    // 首傾き設定 ±8° angle keys) held the full endpoint pose across the whole
    // half range: small variable values (-5, +6, -14.4...) rendered the
    // full-extent pose (stair-step jumps).
    static Level from_frame_vals(const EmoteFrame& f, const EmoteFrame& g,
                                 const FrameVals& v);
    // per-level local-frame op (krkr matrix order T·R·S·H — point order
    // shear, scale, rotate about the node's own origin, then translate).
    // translation is outermost, so a rotation pivots on the
    // node's own origin (its coord point in the parent frame) — rotating the
    // subtree about the authored pivot (首傾き設定 = the neck,
    // 胴体回転中心 = the body sway centre) instead of about the psb origin.
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
    // Morph warp fields (hasWarp/hasBp/region/bp) are *not* applied here —
    // the warp is sampled separately at the icon level in the morph node's
    // own frame (the earlier in-chain application treated
    // the accumulated chain origin as the region centre, which displaced the
    // wrong parts — e.g. body_UD breathing warped the legs instead of the
    // torso/head — and windowed the field to zero outside the region box;
    // the reference semantics recentre each morph field on the node's own
    // frame origin and clamp the sample to the region border).
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
    // transient index into emote_collect_parts' per-entry
    // grid slices; it survives the z sort so the geometry pass can look up the
    // grid points the bounds pass already evaluated.
    int slot = -1;
    std::vector<std::string> debugPath;
};

struct EvalCtx {
    const EmoteFile& file;
    const std::map<std::string, double>& vars;
    std::vector<DrawEntry>* out = nullptr;
    std::string* err = nullptr;
    bool debug = false;
    // policy snapshot for this evaluation (read once).
    EmoteS2Policy s2;
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

    double var_value(const std::string& name) const {
        auto it = vars.find(name);
        if (it != vars.end()) return it->second;
        auto d = file.variableDefaults.find(name);
        if (d != file.variableDefaults.end()) return d->second;
        return 0.0;
    }
};

// node tick: parameter nodes map their variable to the axis; others keep the
// incoming (motion) tick. Mirrors krkr (emotemotionref::getTickByIdx -> emoteVar
// transToTick with the current var value; default 0).
double node_tick(const EmoteMotion& m, const EmoteNode& n, double incoming,
                 const EvalCtx& ctx) {
    if (!n.isParameterized) return incoming;
    if (n.parameterIndex < 0 || n.parameterIndex >= int(m.parameter.size())) return incoming;
    const EmoteVar& v = m.parameter[n.parameterIndex];
    return v.trans_to_tick(ctx.var_value(v.id));
}

// picks the last frame with time <= tick; `contentNext` gets the immediately
// following content frame (krkr uses only cur+1) when it exists.
// with `tailFallback` the reference's
// content-less-tail rule applies (E:emoterunner.cpp:355-374): when the tick
// reaches the LAST frame and that frame carries no content, the node holds the
// previous content frame ("static layer keeps its geometry") with no further
// interpolation (nextframe = nullptr). Default OFF — the three works are
// censused unreachable there.
int pick_frame(const std::vector<EmoteFrame>& frames, double tick, int* contentNext,
               bool tailFallback) {
    *contentNext = -1;
    int cur = -1;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].time <= tick) cur = int(i);
        else break;
    }
    if (cur < 0) return -1;
    if (tailFallback && !frames[size_t(cur)].hasContent &&
        cur == int(frames.size()) - 1) {
        for (int i = cur - 1; i >= 0; --i) {
            if (frames[size_t(i)].hasContent) return i; // *contentNext stays -1
        }
        return cur; // no content anywhere: the caller hides the node
    }
    if (cur + 1 < int(frames.size()) && frames[cur + 1].hasContent)
        *contentNext = cur + 1;
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
                     double tick, const EmoteS2Policy& pol, const double* lim) {
    FrameVals v;
    if (cur < 0) return v;
    const EmoteFrame& f = frames[cur];
    v.frame = &f;
    // the reference repairs NaN/Inf coords BEFORE
    // interpolating (E:emoterunner.cpp:559-583, both the current and the next
    // frame): NaN -> -lim.origin, Inf -> lim.size - lim.origin. The copies
    // below leave every finite value bit-identical.
    double fcx = f.coordX, fcy = f.coordY;
    if (pol.nonfiniteGuard && lim) {
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
        const bool sameIcon = f.icon == g.icon && f.src == g.src;
        if (interp_allowed(f, g)) {
            const double ta = f.time, tb = g.time;
            double gcx = g.coordX, gcy = g.coordY;
            if (pol.nonfiniteGuard && lim) {
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
            if (pol.angleWrap) {
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
            // bezier grids blend only across the same content (icon shapes
            // keyframe on their own grid; blank morphs share the empty icon)
            if ((f.hasBp || g.hasBp) && sameIcon) {
                for (int i = 0; i < 32; ++i) v.bp[i] = float(lerp(f.hasBp ? f.bp[i] : grid_identity(i), g.hasBp ? g.bp[i] : grid_identity(i), ta, tb, tick));
                v.hasBp = true;
            }
        }
    }
    if (pol.nonfiniteGuard) {
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

// ---------------------------------------------------------------------------
// the per-entry forward map, prepared ONCE per draw entry
// and applied per vertex. The chain fold (level ops, morph registration) is
// entry-invariant, but the earlier code redid it for every mesh vertex — a
// 64-slot WarpRef array plus 2-3 sin/cos per level per vertex, which is what
// made a div=20 mesh cost ~1.2 us per vertex.
// The arithmetic is the SAME sequence as before: the fold is hoisted
// verbatim, and the point application keeps Level::apply_affine's operation
// order with only the sin/cos pair precomputed — so the emitted geometry is
// bit-identical (a same-pose vertex checksum before/after the refactor
// matches).
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

struct WarpRef {
    double ox = 0, oy = 0;   // region centre anchor: morph origin in world px
    double w = 1, h = 1, cx = 0, cy = 0; // region box (dims + centre offsets)
    // angle fold: ancestor linear part folded at this morph
    // (its content frame -> world), and its inverse (world -> content frame).
    // `folded` is false for axis-aligned (translation only) ancestors: those
    // keep the legacy code path bit-identical.
    double la = 1, lb = 0, lc = 0, ld = 1;
    double ia = 1, ib = 0, ic = 0, id = 1;
    bool folded = false;
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
    m->probe = std::getenv("OA_DEBUG_ICON") != nullptr && !en.debugPath.empty() &&
               en.debugPath.back() == std::getenv("OA_DEBUG_ICON");
    // ---- pass 1 (root -> leaf), verbatim from the earlier function ---------
    double la = 1, lb = 0, lc = 0, ld = 1; // cumulative linear (content -> world)
    double le = 0, lf = 0;                 // cumulative translation
    for (const Level& l : en.levels) {
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
    m->opsRev.reserve(en.levels.size());
    for (size_t ri = en.levels.size(); ri-- > 0;) {
        const Level& l = en.levels[ri];
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
        su = std::clamp((qx + wr.cx) / wr.w, 0.0, 1.0);
        sv = std::clamp((qy + wr.cy) / wr.h, 0.0, 1.0);
        bezier_patch(wr.bp, su, sv, &bx, &by);
        if (!wr.folded) {
            px = wr.ox + bx * wr.w - wr.cx;
            py = wr.oy + by * wr.h - wr.cy;
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
                 int type12Depth, std::vector<std::string>& path);

/// the cell count one entry's mesh is built with — the
/// node-authorised value under the active policy (see emote_file.h).
static int entry_mesh_div(const DrawEntry& en) {
    return emote_effective_mesh_division(en.authoredMeshDivision,
                                         en.icon ? en.icon->width : 0.0,
                                         en.icon ? en.icon->height : 0.0);
}

void eval_node(EvalCtx& ctx, const EmoteMotion& motion, int nodeIdx, double incomingTick,
               const std::vector<Level>& levels, double opaMul, int depth,
               int type12Depth, std::vector<std::string>& path) {
    if (depth > 96) return;
    if (nodeIdx < 0 || nodeIdx >= int(motion.nodes.size())) return;
    const EmoteNode& node = motion.nodes[nodeIdx];
    if (node.removed || node.frames.empty()) return;
    path.push_back(node.label);

    // --- tick & frame selection --------------------------------------------
    // the reference's getTickByIdx returns -1 for
    // an out-of-range parameterIndex and the node is then NOT drawn
    // (E:emoterunner.cpp:1004-1005 -> isNeedDraw = false, 530-534); its
    // children are still walked. Default OFF (data: 0/3466 parameter nodes).
    const bool paramOob = node.isParameterized &&
                          (node.parameterIndex < 0 ||
                           node.parameterIndex >= int(motion.parameter.size()));
    if (paramOob && ctx.s2.paramOobSkip) {
        for (int ch : node.children)
            eval_node(ctx, motion, ch, incomingTick, levels, opaMul, depth + 1,
                      type12Depth + (node.type == 12 ? 1 : 0), path);
        path.pop_back();
        return;
    }
    const double tick = node_tick(motion, node, incomingTick, ctx);
    int next = -1;
    const int cur = pick_frame(node.frames, tick, &next, ctx.s2.tailFallback);
    const EmoteFrame* curFrame = (cur >= 0) ? &node.frames[cur] : nullptr;
    const bool hasContent = curFrame && curFrame->hasContent;

    // Node whose current frame is content-less contributes nothing itself but
    // its subtree is still evaluated (krkr reference: children recursion is
    // unconditional; the node only stopped drawing).
    if (!hasContent) {
        for (int ch : node.children)
            eval_node(ctx, motion, ch, incomingTick, levels, opaMul, depth + 1,
                      type12Depth + (node.type == 12 ? 1 : 0), path);
        path.pop_back();
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

    const FrameVals fv = frame_vals(node.frames, cur, next, tick, ctx.s2, lim);
    const EmoteFrame& f = *fv.frame;
    if (ctx.debug) {
        // per-node frame selection state at the current vars.
        double mn = 9, mx = -9;
        if (fv.hasBp)
            for (int i = 0; i < 32; ++i) { mn = std::min(mn, double(fv.bp[i])); mx = std::max(mx, double(fv.bp[i])); }
        const std::string par = node.isParameterized && node.parameterIndex >= 0 &&
                                        node.parameterIndex < int(motion.parameter.size())
                                    ? motion.parameter[size_t(node.parameterIndex)].id
                                    : std::string();
        std::printf("[eval] node='%s' par='%s' tick=%.0f cur=%d next=%d bp=%s%s\n",
                    node.label.c_str(), par.c_str(), tick, cur, next, fv.hasBp ? "y" : "-",
                    fv.hasBp ? "" : "");
        if (fv.hasBp)
            std::printf("  bp range [%+.4f..%+.4f]\n", mn, mx);
    }

    // --- classify content --------------------------------------------------
    // sub-motion reference (win: content.motion + src=object/icon=motion)
    if (f.isSubMotion) {
        const int midx = ctx.file.find_motion(f.subObject, f.subMotion);
        if (midx >= 0) {
            // affine level from the interpolated values when a following
            // content key exists (type-3 keys interpolate; only a type-2 key
            // followed by a non-type-2 key holds — krkr rule).
            const Level lv =
                (next >= 0) ? Level::from_frame_vals(f, node.frames[size_t(next)], fv)
                            : Level::from_frame(f, false);
            std::vector<Level> childLevels = levels;
            if (lv.any) childLevels.push_back(lv);
            const double subTick = incomingTick + fv.timeOffset;
            eval_motion(ctx, ctx.file.motions[midx], subTick, childLevels, opaMul, depth + 1,
                        type12Depth + (node.type == 12 ? 1 : 0), path);
        }
        for (int ch : node.children)
            eval_node(ctx, motion, ch, incomingTick, levels, opaMul, depth + 1,
                      type12Depth + (node.type == 12 ? 1 : 0), path);
        path.pop_back();
        return;
    }
    // source icon (resolved above for the non-finite `lim`)
    const EmoteSource* src = srcPre;
    const EmoteIcon* ic = icPre;
    Level lv = (next >= 0) ? Level::from_frame_vals(f, node.frames[size_t(next)], fv)
                           : Level::from_frame(f, true);
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
            std::memcpy(lv.bp, fv.bp, sizeof(lv.bp));
            lv.any = true;
        }
    }
    std::vector<Level> childLevels = levels;
    if (lv.any) childLevels.push_back(lv);
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
        if (ctx.debug || emote_mesh_debug()) {
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
            // grid point (the earlier code re-folded the chain per point).
            EntryMap emap;
            prepare_entry_map(en, &emap);
            if (ctx.debug) {
                // per-icon world bbox after the full
                // chain, sampled on the mesh grid actually drawn (the node's
                // own subdivision, not a fixed 8x8).
                const int dbgDiv = entryDiv;
                double nx = 1e300, ny = 1e300, xx = -1e300, xy = -1e300;
                for (int gy = 0; gy <= dbgDiv; ++gy) {
                    for (int gx = 0; gx <= dbgDiv; ++gx) {
                        double x = 0, y = 0;
                        map_entry_uv(emap, double(gx) / dbgDiv, double(gy) / dbgDiv, &x, &y);
                        nx = std::min(nx, x);
                        xx = std::max(xx, x);
                        ny = std::min(ny, y);
                        xy = std::max(xy, y);
                    }
                }
                std::string label = path.empty() ? "?" : path.back();
                std::printf("[eval] icon='%s' bbox=(%.1f,%.1f)-(%.1f,%.1f) src=%s "
                            "tex='%s' warped=%d div=%d\n",
                            label.c_str(), nx, ny, xx, xy, f.src.c_str(), f.icon.c_str(),
                            en.warped ? 1 : 0, entryDiv);
            }
            if (!en.warped) {
                const double corners[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
                for (const auto& c : corners) {
                    double x = 0, y = 0;
                    map_entry_uv(emap, c[0], c[1], &x, &y);
                    add_pt(x, y);
                }
            } else {
                // sample on the mesh grid actually drawn (node subdivision)
                const int div = entryDiv;
                for (int gy = 0; gy <= div; ++gy) {
                    for (int gx = 0; gx <= div; ++gx) {
                        double x = 0, y = 0;
                        map_entry_uv(emap, double(gx) / div, double(gy) / div, &x, &y);
                        add_pt(x, y);
                    }
                }
            }
        }
        // win trees put at most tiny accessory subtrees below icon leaves;
        // walk them without adding an icon level.
        for (int ch : node.children)
            eval_node(ctx, motion, ch, incomingTick, levels, opaMul, depth + 1,
                      type12Depth + (node.type == 12 ? 1 : 0), path);
        path.pop_back();
        return;
    }

    // transform carrier (blank / coord / mask frames)
    for (int ch : node.children)
        eval_node(ctx, motion, ch, incomingTick, childLevels, nodeOpa, depth + 1,
                  type12Depth + (node.type == 12 ? 1 : 0), path);
    path.pop_back();
}

void eval_motion(EvalCtx& ctx, const EmoteMotion& motion, double tick,
                 const std::vector<Level>& levels, double opaMul, int depth,
                 int type12Depth, std::vector<std::string>& path) {
    for (int root : motion.layer)
        eval_node(ctx, motion, root, tick, levels, opaMul, depth, type12Depth, path);
}

double sample_atlas(const std::vector<uint8_t>& rgba, int texW, int texH, double u, double v,
                    int c) {
    if (texW <= 0 || texH <= 0) return 0;
    double x = u * texW - 0.5;
    double y = v * texH - 0.5;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    auto px = [&](int xx, int yy) -> double {
        xx = std::clamp(xx, 0, texW - 1);
        yy = std::clamp(yy, 0, texH - 1);
        return rgba[(size_t(yy) * texW + xx) * 4 + c];
    };
    const double p00 = px(x0, y0), p10 = px(x0 + 1, y0);
    const double p01 = px(x0, y0 + 1), p11 = px(x0 + 1, y0 + 1);
    const double top = p00 + (p10 - p00) * fx;
    const double bot = p01 + (p11 - p01) * fx;
    return top + (bot - top) * fy;
}

struct MeshVert {
    double x = 0, y = 0, u = 0, v = 0;
};

static void blend_pixel(int canvasW, int x, int y, double r, double g, double b, double a,
                        std::vector<uint8_t>* canvas) {
    uint8_t* dst = canvas->data() + (size_t(y) * canvasW + x) * 4;
    const double da = dst[3] / 255.0;
    const double outa = a + da * (1.0 - a);
    if (outa <= 0.0001) return;
    dst[0] = uint8_t(std::clamp((r * a + dst[0] * da * (1.0 - a)) / outa, 0.0, 255.0));
    dst[1] = uint8_t(std::clamp((g * a + dst[1] * da * (1.0 - a)) / outa, 0.0, 255.0));
    dst[2] = uint8_t(std::clamp((b * a + dst[2] * da * (1.0 - a)) / outa, 0.0, 255.0));
    dst[3] = uint8_t(std::clamp(outa * 255.0, 0.0, 255.0));
}

// paint one entry: warped entries go through the forward UV mesh; pure
// affine entries keep the inverse-UV quad path (same result, much faster).
// full-resolution poses (~1920x1620) are parallelised
// across disjoint row bands (pixel blending is per-row sequential over
// triangles/entries, so splitting rows is output-identical); thread count
// capped 1..8 (OA_EMOTE_THREADS override).
static int raster_threads() {
    if (const char* v = std::getenv("OA_EMOTE_THREADS")) {
        const int n = std::atoi(v);
        if (n >= 1 && n <= 16) return n;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    return int(std::clamp(hw ? hw : 2u, 1u, 8u));
}

static void paint_entry(const DrawEntry& en, const Mat& fit, int canvasW, int canvasH,
                        std::vector<uint8_t>* canvas) {
    const std::vector<uint8_t>& rgba = en.source->rgba;
    const int texW = en.source->textureWidth, texH = en.source->textureHeight;
    if (texW <= 0 || texH <= 0) return;
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
        auto scan_band = [&](int r0, int r1) {
            for (int py = r0; py <= r1; ++py) {
                for (int px = x0; px <= x1; ++px) {
                    const double u = ia * (px + 0.5) + ic * (py + 0.5) + ie;
                    const double v = ib * (px + 0.5) + id * (py + 0.5) + inf;
                    if (u < 0 || u > 1 || v < 0 || v > 1) continue;
                    double au = 0, av = 0;
                    // the one atlas mapping (no trim term)
                    emote_icon_uv_to_atlas(*en.icon, texW, texH, u, v, &au, &av);
                    const double alpha = sample_atlas(rgba, texW, texH, au, av, 3) / 255.0;
                    if (alpha <= 0.001) continue;
                    const double opa = alpha * en.opa;
                    const double r = sample_atlas(rgba, texW, texH, au, av, 0);
                    const double g = sample_atlas(rgba, texW, texH, au, av, 1);
                    const double b = sample_atlas(rgba, texW, texH, au, av, 2);
                    blend_pixel(canvasW, px, py, r, g, b, opa, canvas);
                }
            }
        };
        const int rows = y1 - y0 + 1;
        const int band = std::max(1, (rows + nThreads - 1) / nThreads);
        if (nThreads <= 1 || rows < 64) {
            scan_band(y0, y1);
        } else {
            std::vector<std::thread> th;
            th.reserve(size_t(nThreads));
            for (int t = 0; t < nThreads && y0 + t * band <= y1; ++t) {
                const int b0 = y0 + t * band;
                const int b1 = std::min(y1, b0 + band - 1);
                th.emplace_back(scan_band, b0, b1);
            }
            for (auto& j : th) j.join();
        }
        return;
    }
    // warped: forward mesh at the subdivision the ICON NODE authorises
    // (krkr E:emoterunner.cpp:853-854; the adaptive
    // clamp(round(diag/90),6,18) stays reachable via OA_EMOTE_MESHDIV).
    const int div = entry_mesh_div(en);
    const int n = div + 1;
    // vertex-buffer reuse: grid + triangle-soup scratch live in
    // thread-local storage and only ever grow, so a steady-state pose costs
    // zero mesh allocations per entry/frame (div=20 => 441 grid + 2400 soup
    // vertices, ~90 KB, reused across every entry of every frame). Bound as
    // references: the row-band workers below run this lambda on OTHER threads
    // and must see the calling thread's buffers, not their own empty TLS.
    static thread_local std::vector<MeshVert> verts_tls;
    static thread_local std::vector<MeshVert> tris_tls;
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
    auto scan_band = [&](int r0_, int r1_) {
        for (size_t ti = 0; ti + 2 < tris.size(); ti += 3) {
            const MeshVert& amv = tris[ti];
            const MeshVert& bmv = tris[ti + 1];
            const MeshVert& cmv = tris[ti + 2];
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
                    const double u = (r1 * amv.u + r2 * bmv.u + r3 * cmv.u) * inv;
                    const double v = (r1 * amv.v + r2 * bmv.v + r3 * cmv.v) * inv;
                    double au = 0, av = 0;
                    // the one atlas mapping (no trim term)
                    emote_icon_uv_to_atlas(*en.icon, texW, texH, u, v, &au, &av);
                    const double alpha = sample_atlas(rgba, texW, texH, au, av, 3) / 255.0;
                    if (alpha <= 0.001) continue;
                    const double opa = alpha * en.opa;
                    const double r = sample_atlas(rgba, texW, texH, au, av, 0);
                    const double g = sample_atlas(rgba, texW, texH, au, av, 1);
                    const double b = sample_atlas(rgba, texW, texH, au, av, 2);
                    blend_pixel(canvasW, px, py, r, g, b, opa, canvas);
                }
            }
        }
    };
    const int rows = canvasH;
    const int band = std::max(1, (rows + nThreads - 1) / nThreads);
    if (nThreads <= 1 || rows < 256) {
        scan_band(0, canvasH - 1);
    } else {
        std::vector<std::thread> th;
        th.reserve(size_t(nThreads));
        for (int t = 0; t < nThreads && t * band < canvasH; ++t) {
            const int b0 = t * band;
            const int b1 = std::min(canvasH - 1, b0 + band - 1);
            th.emplace_back(scan_band, b0, b1);
        }
        for (auto& j : th) j.join();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// mesh subdivision policy — see emote_file.h.
// ---------------------------------------------------------------------------
EmoteMeshDivPolicy emote_mesh_div_policy() {
    EmoteMeshDivPolicy p;
    const int m = g_meshdiv_mode.load(std::memory_order_relaxed);
    if (m < 0) {
        p.mode = meshdiv_env().mode;
        p.fixed = meshdiv_env().fixed;
    } else {
        p.mode = m;
        p.fixed = g_meshdiv_fixed.load(std::memory_order_relaxed);
    }
    return p;
}

void emote_set_mesh_div_policy(int mode, int fixed) {
    g_meshdiv_mode.store(mode, std::memory_order_relaxed);
    g_meshdiv_fixed.store(fixed, std::memory_order_relaxed);
}

int emote_node_mesh_division(int authored) {
    // krkr: int div = node ? node->meshDivision : 0; if (div < 2) div = 8;
    return clamp_mesh_div(authored < kEmoteMeshDivMin ? kEmoteMeshDivFallback : authored);
}

int emote_adaptive_mesh_division(double iconW, double iconH) {
    return adaptive_mesh_div(iconW, iconH); // adaptive rule, verbatim
}

int emote_effective_mesh_division(int authored, double iconW, double iconH) {
    const EmoteMeshDivPolicy p = emote_mesh_div_policy();
    if (p.mode == kEmoteMeshDivFixed) return clamp_mesh_div(p.fixed);
    if (p.mode == kEmoteMeshDivAdaptive) return adaptive_mesh_div(iconW, iconH);
    // hybrid (default): the authored value wins; a node with no (or an
    // invalid) authored key falls back to the adaptive grid instead of
    // krkr's flat 8, so the fallback can never be coarser than that grid.
    if (p.mode == kEmoteMeshDivNodeAdaptive && authored < kEmoteMeshDivMin)
        return adaptive_mesh_div(iconW, iconH);
    return emote_node_mesh_division(authored);
}

bool emote_mesh_debug() { return meshdiv_env().debug; }

// ---------------------------------------------------------------------------
// numeric-robustness policy (see the env block above).
// ---------------------------------------------------------------------------
EmoteS2Policy emote_s2_policy() {
    EmoteS2Policy p;
    const int aw = g_s2_anglewrap.load(std::memory_order_relaxed);
    const int tf = g_s2_tailfallback.load(std::memory_order_relaxed);
    const int po = g_s2_paramoob.load(std::memory_order_relaxed);
    const int nf = g_s2_nonfinite.load(std::memory_order_relaxed);
    p.angleWrap = aw < 0 ? s2_env().angleWrap != 0 : aw != 0;
    p.tailFallback = tf < 0 ? s2_env().tailFallback != 0 : tf != 0;
    p.paramOobSkip = po < 0 ? s2_env().paramOob != 0 : po != 0;
    p.nonfiniteGuard = nf < 0 ? s2_env().nonfinite != 0 : nf != 0;
    return p;
}

void emote_set_s2_policy(int angleWrap, int tailFallback, int paramOobSkip,
                         int nonfiniteGuard) {
    g_s2_anglewrap.store(angleWrap, std::memory_order_relaxed);
    g_s2_tailfallback.store(tailFallback, std::memory_order_relaxed);
    g_s2_paramoob.store(paramOobSkip, std::memory_order_relaxed);
    g_s2_nonfinite.store(nonfiniteGuard, std::memory_order_relaxed);
}

bool static_content_bounds(const EmoteFile& file, const std::map<std::string, double>& vars,
                           double* minX, double* minY, double* maxX, double* maxY) {
    EvalCtx ctx{file, vars, nullptr, nullptr};
    ctx.s2 = emote_s2_policy();
    ctx.wantBounds = true; // this is the one consumer of the eval-time bounds
    const int midx = file.find_motion(file.baseChara, file.baseMotion);
    if (midx < 0) return false;
    std::vector<Level> empty;
    std::vector<std::string> p;
    eval_motion(ctx, file.motions[midx], 0, empty, 1.0, 0, 0, p);
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
                             std::vector<DrawEntry>* entries, std::string* err) {
    EvalCtx ctx{file, vars, entries, err};
    ctx.s2 = emote_s2_policy(); // read the policy once
    ctx.debug = std::getenv("OA_DEBUG_EVAL") != nullptr;
    // the render/collect paths compute their own content
    // bounds from the grid they sample anyway — only the debug trace wants the
    // eval-time bounds here (static_content_bounds builds its own context).
    ctx.wantBounds = ctx.debug;
    const int midx = file.find_motion(file.baseChara, file.baseMotion);
    if (midx < 0) {
        if (err) *err = "emote: base motion not found";
        return false;
    }
    std::vector<Level> empty;
    std::vector<std::string> p;
    eval_motion(ctx, file.motions[midx], 0, empty, 1.0, 0, 0, p);
    if (ctx.debug) {
        std::fprintf(stderr, "[eval] entries=%zu bounds=%.0f..%.0f x %.0f..%.0f\n",
                     entries->size(), ctx.minX, ctx.maxX, ctx.minY, ctx.maxY);
    }
    if (entries->empty()) return true; // nothing drawn; canvas stays transparent
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

bool render_static_frame(const EmoteFile& file, const std::map<std::string, double>& vars,
                         int canvasW, int canvasH, const StaticRenderOptions& opt,
                         std::vector<uint8_t>* rgba, std::string* err) {
    if (canvasW <= 0 || canvasH <= 0) return false;
    std::vector<DrawEntry> entries;
    if (!evaluate_entries(file, vars, &entries, err)) return false;
    if (entries.empty()) return true; // nothing drawn; canvas stays transparent

    // ---- provisional psb -> canvas mapping --------------------------------
    // Whole-figure fit with a head margin; scale/dx/dy multiply after the
    // fit. A later pass will calibrate against the official runtime.
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    EntryMap boundsMap;
    for (const DrawEntry& en : entries) {
        const int divb = en.warped ? entry_mesh_div(en) : 1;
        prepare_entry_map(en, &boundsMap);
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

    // stable draw order: z (accumulated frame coordZ), then insertion order
    std::stable_sort(entries.begin(), entries.end(), [](const DrawEntry& a, const DrawEntry& b) {
        if (a.z != b.z) return a.z < b.z;
        return false;
    });

    rgba->assign(size_t(canvasW) * canvasH * 4, 0);
    Mat fit = mul(translate(offX, offY), scale(k, k));
    for (const DrawEntry& en : entries) paint_entry(en, fit, canvasW, canvasH, rgba);
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
    //
    // the bounds pass and the geometry pass need the SAME
    // mesh grid, so it is evaluated ONCE per entry here and both passes read it
    // (the grid previously ran twice in this function and a third time in the
    // evaluator's bounds sampling; map_entry_uv + that sampling held ~77% of the
    // collect profile). Same doubles, same order => bit-identical geometry.
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
    // vector destroys every part's `verts` allocation each frame; with a
    // stable entry count (the normal case when only variable values change —
    // div=20 => 2400 soup vertices per warped part) the reused capacity makes
    // a steady-state collect allocation-free. Only the tail beyond the new
    // part count is popped.
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
            for (size_t k = 0; k < verts.size(); ++k) {
                double x = gridpts[sl.first + k].x;
                double y = gridpts[sl.first + k].y;
                to_canvas(&x, &y);
                verts[k] = {x, y, gridpts[sl.first + k].u, gridpts[sl.first + k].v};
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
