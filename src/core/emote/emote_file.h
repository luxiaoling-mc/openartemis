#pragma once
// E-mote animation data model.
// Autonomous C++ model of the win/common-spec PSB layout the survey pinned
// down: spec/metadata/object{motion{node/frame}}/source{icon}/timeline/
// variableList/controls. The krkr emotefile parser was the behavioural
// reference for field meanings; nothing is copied from it.
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/emote/psb_reader.h"

namespace oa::emote {

enum class SpecKind { Unknown, Krkr, Win, Common };

// ---------------------------------------------------------------------------
// motion parameter (variable morph range)
// ---------------------------------------------------------------------------
struct EmoteVar {
    std::string id;
    double rangeBegin = 0;
    double rangeEnd = 1;
    double division = 0; // transToTick = division * (v-rangeBegin)/(rangeEnd-rangeBegin)
    double trans_to_tick(double v) const {
        if (rangeEnd <= rangeBegin) return 0;
        return division * (v - rangeBegin) / (rangeEnd - rangeBegin);
    }
};

// ---------------------------------------------------------------------------
// frame (a state sample on the node timeline / parameter axis)
// ---------------------------------------------------------------------------
struct EmoteFrame {
    double time = 0;
    int type = 0; // 0 = empty/terminal, 2 = linear-interp keyframe, 3 = hold
    bool hasContent = false;

    // content (win spec): src+icon reference a source icon or a
    // sub-motion; mesh bp is the 4x4 bezier control grid (16 x/y pairs).
    std::string src;
    std::string icon;
    int64_t mask = 0;
    bool hasCoord = false;
    double coordX = 0, coordY = 0, coordZ = 0;
    bool hasAngle = false;
    double angle = 0;
    double sx = 0, sy = 0; // shear
    double zx = 1, zy = 1; // scale
    double ox = 0, oy = 0;
    double opa = 1.0; // win/common: stored 0..255 int, normalised at parse
    double timeOffset = 0; // sub-motion reference shift
    bool isSubMotion = false;   // src/icon resolved to object+motion
    std::string subObject;      // resolved sub-motion target
    std::string subMotion;
    bool hasBp = false;
    float bp[32]; // identity grid set at parse when absent
    int bm = 0;
    int64_t color = 0;
};

// ---------------------------------------------------------------------------
// node
// ---------------------------------------------------------------------------
struct EmoteNode {
    std::string label;
    int type = 0; // 0 part, 3 layout, 12 mask-composite
    std::vector<int> children; // indices into EmoteMotion.nodes
    std::vector<EmoteFrame> frames;
    bool isParameterized = false;
    int parameterIndex = -1; // index into motion.parameter
    uint32_t inheritMask = 0x20007FC;
    int meshCombine = 0, meshDivision = 0, meshTransform = 0;
    bool removed = false;
    bool isMaskNode = false; // type 12 (stencil composite)
    // stencilCompositeMaskLayerList — node LABELS whose drawn alpha masks
    // this stencil's subtree (the eye: content = the pupil subtree, mask =
    // the eye-white shape). Labels resolve within the stencil's parent
    // subtree (ancestor scope): both eyes carry a node labeled 'shirome'
    // and each stencil binds the one inside its own 目L/目R branch.
    std::vector<std::string> stencil_mask_layers;
    int stencil_type = 0;
};

// ---------------------------------------------------------------------------
// motion (a tree of nodes)
// ---------------------------------------------------------------------------
struct EmoteMotion {
    std::string name;
    double lastTime = 0;
    double loopTime = -1; // >=0 loops; survey: win files use -1
    std::vector<int> layer;      // root node indices
    std::vector<int> drawOrder;  // all nodes, priority-reordered (krkr parity)
    std::vector<int> nodeListDfs; // creation DFS order
    std::vector<EmoteNode> nodes;
    std::vector<EmoteVar> parameter;
    bool isParameterized = false;
    int parameterIndex = -1;
    double selfSyncTime = 0;
};

struct EmoteObject {
    std::string name;
    int type = 0;
    std::vector<std::pair<std::string, int>> motions; // name -> index
};

// ---------------------------------------------------------------------------
// timeline (named variable animation; label-driven like 通常待機)
// ---------------------------------------------------------------------------
struct TimeVarFrame {
    double time = 0;
    int type = 0;
    bool hasContent = false;
    double easing = 0;
    double value = 0;
};
struct TimeVar {
    std::string label;
    std::vector<TimeVarFrame> frames;
};
struct EmoteTimeline {
    std::string label;
    int diff = 0;
    int lastTime = -1;
    int loopBegin = -1;
    int loopEnd = -1;
    std::vector<TimeVar> variables;
};

// ---------------------------------------------------------------------------
// sources/icons (win spec: DXT5 atlases embedded as chunk resources)
// ---------------------------------------------------------------------------
struct EmoteIcon {
    std::string name;
    double left = 0, top = 0, width = 0, height = 0;
    double originX = 0, originY = 0;
    int attr = 0;
    int zorder = 0;
};
struct EmoteSource {
    std::string name;
    std::string textureType; // "DXT5" / "RGBA8" ...
    int textureWidth = 0, textureHeight = 0;
    int32_t pixelChunk = -1;   // chunk index of the atlas (regular chunks)
    bool pixelExtra = false;
    std::vector<EmoteIcon> icons;
    // lazily decoded RGBA atlas (one per source)
    std::vector<uint8_t> rgba;
    bool rgbaDecoded = false;
};

/// The ONE atlas mapping shared by both raster paths:
/// the icon rect [left,top,width,height] is sampled **in place**:
///     tu = (left + u*width) / texW      tv = (top + v*height) / texH
/// The format carries no crop/offset/padding
/// field (49 real PSBs / 8461 icons: exactly 8 icon keys, 0 `clip`, 0
/// overlaps, 0 out-of-atlas rects), so this deliberately has **no trim
/// compensation term**. `emote_axis_dump mesh` asserts this formula verbatim
/// as a regression guard against ever adding one.
inline void emote_icon_uv_to_atlas(const EmoteIcon& ic, int texW, int texH, double u,
                                   double v, double* tu, double* tv) {
    *tu = (ic.left + u * ic.width) / double(texW);
    *tv = (ic.top + v * ic.height) / double(texH);
}

// ---------------------------------------------------------------------------
// controls (kept minimal: read + exposed; the raster paths use them)
// ---------------------------------------------------------------------------
struct EyeControl {
    std::string label;
    double beginFrame = 0, endFrame = 0;
    double blinkFrameCount = 0;
    double blinkIntervalMin = 0, blinkIntervalMax = 0;
};
struct SelectorOption {
    std::string label;
    double offValue = 1, onValue = 0;
};
struct SelectorControl {
    std::string label;
    std::vector<SelectorOption> options;
};

struct PixelMarker {
    double top = 0, bottom = 0, left = 0, right = 0;
    double eye = 0, mouth = 0, bust = 0;
};

// ---------------------------------------------------------------------------
// whole emote file
// ---------------------------------------------------------------------------
class EmoteFile {
public:
    ~EmoteFile();
    EmoteFile(const EmoteFile&) = delete;
    EmoteFile& operator=(const EmoteFile&) = delete;
    EmoteFile();

    bool load(const uint8_t* data, size_t size, std::string* err = nullptr);
    bool load(const std::vector<uint8_t>& data, std::string* err = nullptr);

    /// Identity of this parsed PSB, assigned on every successful load.
    /// Renderer-side caches that OUTLIVE one EmotePlayer must key on it: a
    /// game reuses one layer id while swapping the character's file
    /// ([fg] file="mit_0" -> "mit_1", costume changes run_0 -> run_1), so a
    /// cache keyed on the layer alone hands the GPU the PREVIOUS file's atlas
    /// to sample under the NEW file's icon rectangles — the on-screen portrait
    /// then shows another file's pixels at this file's part positions
    /// ("parts scrambled"; research/132). 0 = not loaded.
    uint64_t uid() const { return uid_; }

    SpecKind spec = SpecKind::Unknown;
    double version = 0; // psb version field "3.03" style double-ish (string)

    // screenSize (authoring viewport) & chara bounds.
    int screenX = 0, screenY = 0, screenWidth = 0, screenHeight = 0;
    PixelMarker marker;
    double charaHeight = 0; // charaProfile.height (160 in samples)
    double scale = 1.0;

    // metadata.base
    std::string baseChara;  // e.g. "all_parts"
    std::string baseMotion; // e.g. "タイムライン構造"

    std::vector<EmoteObject> objects;
    std::vector<EmoteMotion> motions; // flat motion store
    std::vector<EmoteTimeline> timelines;
    std::vector<std::string> variableNames; // metadata.variableList order
    std::map<std::string, double> variableDefaults; // label -> 0
    std::vector<EyeControl> eyeControls;
    std::vector<SelectorControl> selectors;

    std::vector<std::string> sourceOrder; // source names (metadata order)
    std::vector<std::unique_ptr<EmoteSource>> sources;

    // convenience: find motion index by object name + motion name.
    int find_motion(const std::string& object, const std::string& motion) const;
    int find_object(const std::string& name) const;
    int find_timeline(const std::string& label) const;
    const EmoteSource* find_source(const std::string& name) const;
    /// Icon within a source; returns null when absent.
    const EmoteIcon* find_icon(const EmoteSource& src, const std::string& icon) const;

    /// Decode (once) the RGBA atlas of `src`.
    bool ensure_atlas(EmoteSource* src, std::string* err = nullptr) const;

    const PsbReader& psb() const { return psb_; }

private:
    bool parse_root(const PsbReader& r, std::string* err);
    bool parse_metadata(uint32_t off, std::string* err);
    bool parse_motion(uint32_t off, const std::string& oname, const std::string& mname,
                      std::string* err);
    bool parse_node(EmoteMotion& m, int nodeIndex, uint32_t off, std::string* err);
    bool parse_frame(uint32_t off, EmoteFrame* out, std::string* err);
    bool parse_timeline(uint32_t off, std::string* err);
    bool parse_source(uint32_t off, const std::string& name, std::string* err);

    PsbReader psb_;
    uint64_t uid_ = 0; // assigned by load(); see uid()
};

// ---------------------------------------------------------------------------
// static (tick-0) render to an RGBA canvas.
// `vars` overrides variable defaults (face_talk etc.); `scale` multiplies
// the whole drawing, `dy`/`dx` shift it in canvas pixels. The canvas is
// w x h RGBA, cleared to transparent. Returns false on structural errors.
// ---------------------------------------------------------------------------
struct StaticRenderOptions {
    double scale = 1.0;
    double dx = 0, dy = 0;
    bool fitToCanvas = true; // provisional mapping: fit
                             // the whole figure into the canvas before scale
    double headMargin = 0;   // extra canvas px below the top after fitting
};
bool render_static_frame(const EmoteFile& file, const std::map<std::string, double>& vars,
                         int canvasW, int canvasH, const StaticRenderOptions& opt,
                         std::vector<uint8_t>* rgba, std::string* err = nullptr);

/// Content bounds of the static frame in *psb* coordinates (after motion
/// evaluation, before the canvas mapping) — diagnostics/calibration.
bool static_content_bounds(const EmoteFile& file,
                           const std::map<std::string, double>& vars, double* minX,
                           double* minY, double* maxX, double* maxY);

// ---------------------------------------------------------------------------
// GPU compositing path. The host rasterises the pose
// with SDL_RenderGeometry instead of the CPU pixel fill: the CPU side only
// evaluates the pose and subdivides the bezier-mesh icons into triangles
// (light). One part = one textured triangle soup of one atlas icon.
// ---------------------------------------------------------------------------
struct EmotePartVertex {
    double x = 0, y = 0; // canvas px (surface mapping applied)
    double u = 0, v = 0; // normalized *icon* uv (0..1 across the icon rect)
};
struct EmoteDrawPart {
    int source = -1; // index into EmoteFile::sources
    int icon = -1;   // index into source->icons
    double alpha = 1.0; // part opacity (frame opa * chain opa)
    // mesh diagnostics. `authoredMeshDivision` is the icon
    // node's raw meshDivision (0 = key absent in the PSB); `meshDivision` is
    // the cell count this geometry was actually built with (1 = affine quad
    // path, i.e. no mesh anywhere in the chain — krkr's buildRectMesh).
    // Exposed so the mesh dump can assert "div == node.meshDivision" without
    // re-walking the motion tree.
    int authoredMeshDivision = 0;
    int meshDivision = 1;
    // how many type-12 stencil-composite nodes sit
    // above this draw in the node chain. 0 = the plain path; >0 = the
    // reference would composite it through a mask target built from that
    // node's stencilCompositeMaskLayerList (implemented: see stencil_group).
    int type12Depth = 0;
    // Stencil composite (type 12) tagging, mirroring the CPU raster:
    //   mask_role 0 + stencil_group >= 0 — content drawn under that stencil
    //   group, composited through the group's mask shapes;
    //   mask_role 1 — this part IS a mask shape of mask_group (it also
    //   draws normally; the renderer consumes both roles).
    int stencil_group = -1;
    int mask_role = 0;
    int mask_group = -1;
    // Draw path ("/"-joined node labels; nodes with type==12 are tagged
    // "<label>#12"). Only filled when OA_EMOTE_MESHDBG=1 — the production
    // hot path stays allocation-free.
    std::string nodePath;
    std::vector<EmotePartVertex> verts; // triangle soup, 3 verts per triangle
};
/// Evaluate the pose and emit its geometry as parts (no pixel work). Uses
/// the same evaluation, view mapping and draw order as render_static_frame.
bool emote_collect_parts(const EmoteFile& file,
                         const std::map<std::string, double>& vars, int canvasW,
                         int canvasH, const StaticRenderOptions& opt,
                         std::vector<EmoteDrawPart>* parts, std::string* err = nullptr);

// ---------------------------------------------------------------------------
// mesh subdivision policy. krkr takes the subdivision straight from the icon
// node (E:emoterunner.cpp:853-854):  div = node->meshDivision; if (div < 2) div = 8;
// and only subdivides when the render chain actually carries a mesh (else
// buildRectMesh = our affine quad path). The data authorises 16/20; the
// engine used to apply a per-icon adaptive clamp(round(diag/90), 6, 18) on
// both raster paths.
//
// The DEFAULT is the hybrid `node-adaptive` rule.
// krkr's `<2 -> 8` fallback is real data (13/129 icon nodes in NekoMiko tay_0,
// 33/110 in slny kir_2 carry no meshDivision key) but 8 is COARSER than the
// adaptive grid those nodes used to get (adaptive picks 13..18 for the
// large icons involved), so a strict krkr fallback made those parts less
// faithful (worst part 112px vs 57px, 90/1303 pose-parts). The hybrid keeps
// the authored value where the data has one and falls back to the adaptive
// rule where it does not, so no part is ever coarser than the adaptive grid
// would have been. `node` (strict krkr) stays reachable.
//
//   OA_EMOTE_MESHDIV               unset / "node-adaptive" -> hybrid (DEFAULT)
//   OA_EMOTE_MESHDIV=node          -> strict krkr: authored, <2 -> 8
//   OA_EMOTE_MESHDIV=adaptive      -> adaptive clamp(round(diag/90),6,18)
//   OA_EMOTE_MESHDIV=<n>           -> fixed n cells for every warped part
//                                     (clamped to kMin..kMax below)
// The policy is latched from the environment at first use and can be
// re-pointed in-process (same-process REF/NEW A/B, tests; mode < 0 restores
// the environment latch).
enum EmoteMeshDivMode {
    kEmoteMeshDivNode = 0,     // node.meshDivision (<2 -> 8), krkr parity
    kEmoteMeshDivAdaptive = 1, // adaptive clamp(round(diag/90),6,18)
    kEmoteMeshDivFixed = 2,    // one fixed cell count for every warped part
    kEmoteMeshDivNodeAdaptive = 3, // authored value, missing key -> adaptive
};
constexpr int kEmoteMeshDivMin = 2;
constexpr int kEmoteMeshDivMax = 64; // safety bound (data max is 20)
constexpr int kEmoteMeshDivFallback = 8; // krkr default when meshDivision < 2

struct EmoteMeshDivPolicy {
    int mode = kEmoteMeshDivNodeAdaptive; // default: the hybrid rule
    int fixed = 0;
};
/// Active policy (env-latched unless overridden by emote_set_mesh_div_policy).
EmoteMeshDivPolicy emote_mesh_div_policy();
/// mode < 0 restores the environment-latched policy.
void emote_set_mesh_div_policy(int mode, int fixed = 0);
/// krkr rule: authored < 2 -> 8, clamped to [kEmoteMeshDivMin, kEmoteMeshDivMax].
int emote_node_mesh_division(int authored);
/// adaptive rule: clamp(round(hypot(iconW,iconH)/90), 6, 18).
int emote_adaptive_mesh_division(double iconW, double iconH);
/// Cell count for one icon under the active policy (`iconW/H` only matter for
/// the adaptive / node-adaptive modes). Always >= kEmoteMeshDivMin; callers
/// that walk the affine quad path keep their own 4-corner geometry.
int emote_effective_mesh_division(int authored, double iconW, double iconH);
/// OA_EMOTE_MESHDBG=1: fill EmoteDrawPart::nodePath (diagnostics only).
bool emote_mesh_debug();

// ---------------------------------------------------------------------------
// numeric-robustness switches — see emote_render.cpp for
// the exact reference rules and the default rationale.
//   angleWrap        +-180 degree interpolation wrap (reference
//                    E:emoterunner.cpp:604-620). Default ON (reference).
//   tailFallback     a content-less LAST frame falls back to the last
//                    content frame (E:emoterunner.cpp:355-374). Default OFF.
//   paramOobSkip     a parameter node whose parameterIndex is out of range
//                    is not drawn (E:emoterunner.cpp:1004-1005, 530-534).
//                    Default OFF.
//   nonfiniteGuard   NaN/Inf frame values are repaired instead of
//                    propagating (E:emoterunner.cpp:559-583). Default ON.
// ---------------------------------------------------------------------------
struct EmoteS2Policy {
    bool angleWrap = true;
    bool tailFallback = false;
    bool paramOobSkip = false;
    bool nonfiniteGuard = true;
};
/// Active policy (env-latched unless overridden by emote_set_s2_policy).
EmoteS2Policy emote_s2_policy();
/// Each argument: -1 restores the environment latch, 0/1 forces off/on.
void emote_set_s2_policy(int angleWrap, int tailFallback, int paramOobSkip,
                         int nonfiniteGuard);
/// Stencil-composite (type-12 eye mask) A/B arm: -1 restores the environment
/// latch (OA_EMOTE_STENCIL, default on), 0/1 forces off/on.
bool emote_stencil_enabled();
void emote_set_stencil_enabled(int on);

} // namespace oa::emote
