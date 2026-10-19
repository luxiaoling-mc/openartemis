#pragma once
// E-mote physics (hair / bust / dangling-part sway).
//
// The PSB does not animate these: metadata.bustControl / hairControl /
// partsControl each describe a small spring system that is ANCHORED on a node
// label (baseLayer) and WRITES axis variables every frame (hair_LR, bust_UD,
// quake_LR_M, ...). Those variables are exactly the parameter axes the pose
// evaluator already consumes, so the whole feature is "one more writer into
// the variable domain", not a new render path.
//
// Model (matching the reference implementation that krkrsdl3 ships and the
// official driver's behaviour):
//   * bust: one point mass per control, pulled toward the anchor by `spring`,
//     damped by `friction`, biased by `gravity`; output = response(offset).
//   * chain: N point masses in series, each constrained to the segment
//     `length` behind its parent; the tail lags the anchor and the first
//     mass's lateral offset becomes hair_LR_M / the last one hair_LR, with
//     the vertical sag becoming hair_UD.
//   * `response()` is the atan curve that maps a pixel displacement onto the
//     authored ±30 axis domain; without it a 200 px hair swing would saturate
//     every parameter node.
//   * coefficients are authored for a 60 Hz Euler step, so the caller feeds
//     FRAMES (not ms) and the integrator keeps a fractional remainder.
//
// Anchors are model-pixel positions of the baseLayer node, sampled once per
// frame by the caller (they depend on the pose, which depends on the variables
// this system writes — the reference resolves that with a one-frame lag too).
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/emote/emote_file.h"

namespace oa::emote {

class EmotePhysics {
public:
    using WriteFn = std::function<void(const std::string&, double)>;

    /// Rebuild the runtime state from the file's parsed rules.
    void reset(const EmoteFile& file);
    bool empty() const { return busts_.empty() && chains_.empty(); }

    /// Labels the host must sample this frame (the rules' baseLayer nodes).
    const std::vector<std::string>& anchor_labels() const { return labels_; }
    bool needs_anchor(const std::string& label) const;

    /// Drop last frame's anchors before feeding this frame's.
    void begin_frame();
    void set_anchor(const std::string& label, double x, double y);

    /// Advance `frames` at 60 Hz and emit every variable the rules write.
    void update(double frames, const WriteFn& write);

    /// Diagnostic: the state the next update would start from.
    struct State {
        std::string label;
        double px = 0, py = 0;
        double ax = 0, ay = 0;
        bool anchored = false;
    };
    std::vector<State> snapshot() const;

private:
    struct V3 {
        double x = 0, y = 0, z = 0;
    };
    struct Anchor {
        V3 previous, current, origin;
        bool initialized = false, available = false;
    };
    struct Bust {
        EmoteBustRule rule;
        Anchor anchor;
        V3 p, v;
    };
    struct Chain {
        EmoteChainRule rule;
        Anchor anchor;
        std::vector<V3> p, v;
        double phase = 0, bend = 0;
        bool hair = true;
    };

    static double response(double displacement);
    static void integrate(V3& p, V3& v, V3 force, double fx, double fy, double dt);

    std::vector<Bust> busts_;
    std::vector<Chain> chains_;
    std::vector<std::string> labels_;
    double remainder_ = 0;
    // host-set gains (the SDK exposes bust/hair/parts scale)
    double bust_scale_ = 1, hair_scale_ = 1, parts_scale_ = 1;
    // host-set external forces (SetWind / SetForce)
    V3 bust_force_, hair_force_, parts_force_;
    double rotation_ = 0;
};

} // namespace oa::emote
