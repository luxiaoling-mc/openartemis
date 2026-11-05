#pragma once
// E-mote animation data model.
// Autonomous C++ model of the win/common-spec PSB layout the survey pinned
// down: spec/metadata/object{motion{node/frame}}/source{icon}/timeline/
// variableList/controls. The krkr emotefile parser was the behavioural
// reference for field meanings; nothing is copied from it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
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
    double division = 0; // transToTick = division * clamp01((v-rangeBegin)/(rangeEnd-rangeBegin))
    double trans_to_tick(double v) const {
        const double span = rangeEnd - rangeBegin;
        if (!std::isfinite(v) || !std::isfinite(span) || span == 0.0 ||
            !std::isfinite(division))
            return 0.0;
        const double pos = std::clamp((v - rangeBegin) / span, 0.0, 1.0);
        const double axis = division > 0.0 ? division : 0.0;
        return pos * axis;
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
    /// Mesh-sync selector. Measured against emotedriver with single-byte
    uint32_t meshSyncChildMask = 0;
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

/// The ONE atlas mapping shared by both raster paths: the icon rect [left,top,width,height] is sampled **in place**: tu = (left + u*width) /...
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
// physics rules (metadata.bustControl / hairControl / partsControl)
// ---------------------------------------------------------------------------
/// One point-mass rule (bust): the authored `param` block is also the REST
/// state, so a player that just loaded has the oscillation at its equilibrium.
struct EmoteBustRule {
    std::string label, baseLayer, varLR, varUD;
    bool enabled = true;
    double friction = 0.125, gravity = 0.3, spring = 0.03125;
    double scaleX = 1, scaleY = 1;
    double ofs = 0;
    double op[3] = {0, 0, 0};  // anchor offset in the rule's own space
    double p[3] = {0, 0, 0};   // rest position / initial state
    double pv[3] = {0, 0, 0};  // initial velocity
};

/// One segment of a chain rule (hair strand / dangling part).
struct EmoteChainSegment {
    double bp[3] = {0, 0, 0};  // bind pose position (own space)
    double p[3] = {0, 0, 0};   // rest position
    double pv[3] = {0, 0, 0};  // rest velocity
    double scaleX = 1, scaleY = 1;
    double length = 0;
};

/// Chain rule: a 2+-segment spring chain that outputs LR / LR_M / UD.
struct EmoteChainRule {
    std::string label, baseLayer, varLR, varLRM, varUD;
    bool enabled = true;
    double bRate = 0.00390625, gravity = 0.5;
    double frictionX = 0.03125, frictionY = 0.09375;
    double bendSpd = 0, bendVol = 0, bendR = 0, bendS = 0, ofs = 0;
    double udEft = 0, vBound = 1;
    double op[3] = {0, 0, 0};
    std::vector<EmoteChainSegment> segments;
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
    /// Standard-variable fallback for "差分なし" models: the model never
    /// parameterises `body_UD`, yet the authored idle timeline drives nothing
    /// but `body_UD`, so the figure would stand perfectly still. When the model
    /// does own a whole-body translation axis (`move_UD`), the authored body
    /// offset is folded onto that axis (EvalCtx::var_value). Data-driven: stays
    /// off for every model that consumes `body_UD` itself. See
    /// OA_EMOTE_BODY_FALLBACK.
    bool body_ud_to_move_ud = false;
    std::vector<EyeControl> eyeControls;
    std::vector<SelectorControl> selectors;
    /// Physics rules (metadata.bustControl / hairControl / partsControl).
    /// Each rule anchors on a NODE LABEL (baseLayer) and WRITES the axis
    /// variables it names — the hair/bust quake is not a node animation but a
    /// per-frame writer into the variable domain (see emote_physics.h).
    std::vector<EmoteBustRule> bustControls;
    std::vector<EmoteChainRule> hairControls;
    std::vector<EmoteChainRule> partsControls;

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

// --------------------------------------------------------------------------- static (tick-0) render to an RGBA canvas.
struct StaticRenderOptions {
    double scale = 1.0;
    double dx = 0, dy = 0;
    bool fitToCanvas = true; // provisional mapping: fit
                             // the whole figure into the canvas before scale
    double headMargin = 0;   // extra canvas px below the top after fitting
};
/// Timeline track sample at `t` authored frames. A content-less (type 0) frame only marks the END of a segment, so the sample walks back to...
bool emote_sample_track(const TimeVar& track, double t, double* out);

/// Loop-timeline sample (待機 idles): the reference model, where a key's value is the TARGET to reach just before the next key (`prev -> cur`...
bool emote_sample_track_loop(const TimeVar& track, double t, double initial, double* out);

bool render_static_frame(const EmoteFile& file, const std::map<std::string, double>& vars,
                         int canvasW, int canvasH, const StaticRenderOptions& opt,
                         std::vector<uint8_t>* rgba, std::string* err = nullptr);

/// World position (model px, cumulative translation) of the named nodes — the
/// anchors the physics rules hang off (metadata.*Control[].baseLayer). Sampled
/// from the pose evaluation itself, so a label that never appears in a drawn
/// chain is simply absent from `out` (its rule then stays unanchored).
bool emote_collect_anchors(const EmoteFile& file,
                           const std::map<std::string, double>& vars,
                           const std::set<std::string>& labels,
                           std::map<std::string, std::pair<double, double>>* out,
                           std::string* err = nullptr);

/// Morph-warp patch sample. `bp` is a 4x4 grid of (u,v) control pairs covering
/// the warp region's [0,1]x[0,1]; the field is defined there, but a part may
/// reach past it. `bezier_patch_ext` keeps such samples on the field by
/// continuing linearly from the border (clamping them folded whole parts onto
/// the border — see emote_render.cpp). Exposed for the asset-free regression.
void bezier_patch_ext(const float bp[32], double u, double v, double* bx, double* by);

/// Content bounds of the static frame in *psb* coordinates (after motion
/// evaluation, before the canvas mapping) — diagnostics/calibration.
bool static_content_bounds(const EmoteFile& file,
                           const std::map<std::string, double>& vars, double* minX,
                           double* minY, double* maxX, double* maxY);

// --------------------------------------------------------------------------- GPU compositing path. The host rasterises the pose with SDL_R...
struct EmotePartVertex {
    double x = 0, y = 0; // canvas px (surface mapping applied)
    double u = 0, v = 0; // normalized *icon* uv (0..1 across the icon rect)
};
struct EmoteDrawPart {
    int source = -1; // index into EmoteFile::sources
    int icon = -1;   // index into source->icons
    double alpha = 1.0; // part opacity (frame opa * chain opa)
    // mesh diagnostics. `authoredMeshDivision` is the icon
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
// 网格细分（唯一规则 = node-adaptive 混合）
// ---------------------------------------------------------------------------
constexpr int kEmoteMeshDivMin = 2;
constexpr int kEmoteMeshDivMax = 64; // safety bound (data max is 20)
constexpr int kEmoteMeshDivFallback = 8; // krkr default when meshDivision < 2

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

} // namespace oa::emote
