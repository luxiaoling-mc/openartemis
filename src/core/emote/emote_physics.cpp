#include "core/emote/emote_physics.h"

#include <algorithm>
#include <cmath>

namespace oa::emote {
namespace {

inline double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

} // namespace

// Maps a model-pixel displacement onto the authored ±30 axis domain. The angle
// constant is the reference's 2.25°: displacement*1.15 radians, divided back
// out, so ~26 px reads as ~30 axis units and larger swings saturate smoothly.
double EmotePhysics::response(double displacement) {
    constexpr double angle = 0.039269909262657166;
    return std::atan(displacement * 1.149999976158142 * angle) / angle;
}

void EmotePhysics::integrate(V3& p, V3& v, V3 force, double fx, double fy, double dt) {
    v.x += force.x * dt;
    v.y += force.y * dt;
    v.z += force.z * dt;
    v.x *= std::max(0.0, 1 - fx * dt);
    v.y *= std::max(0.0, 1 - fy * dt);
    p.x += v.x * dt;
    p.y += v.y * dt;
    p.z += v.z * dt;
}

void EmotePhysics::reset(const EmoteFile& file) {
    busts_.clear();
    chains_.clear();
    labels_.clear();
    remainder_ = 0;
    for (const auto& r : file.bustControls) {
        if (!r.enabled) continue;
        Bust b;
        b.rule = r;
        b.p = V3{r.p[0], r.p[1], r.p[2]};
        b.v = V3{r.pv[0], r.pv[1], r.pv[2]};
        busts_.push_back(std::move(b));
    }
    auto add = [&](const std::vector<EmoteChainRule>& rules, bool hair) {
        for (const auto& r : rules) {
            if (!r.enabled || r.segments.empty()) continue;
            Chain c;
            c.rule = r;
            c.hair = hair;
            c.phase = r.bendR;
            c.bend = r.bendS;
            c.p.resize(r.segments.size());
            c.v.resize(r.segments.size());
            for (size_t i = 0; i < r.segments.size(); ++i) {
                c.p[i] = V3{r.segments[i].p[0], r.segments[i].p[1], r.segments[i].p[2]};
                c.v[i] = V3{r.segments[i].pv[0], r.segments[i].pv[1], r.segments[i].pv[2]};
            }
            chains_.push_back(std::move(c));
        }
    };
    add(file.hairControls, true);
    add(file.partsControls, false);
    for (const auto& b : busts_) labels_.push_back(b.rule.baseLayer);
    for (const auto& c : chains_) labels_.push_back(c.rule.baseLayer);
    std::sort(labels_.begin(), labels_.end());
    labels_.erase(std::unique(labels_.begin(), labels_.end()), labels_.end());
}

bool EmotePhysics::needs_anchor(const std::string& label) const {
    for (const auto& b : busts_)
        if (b.rule.baseLayer == label) return true;
    for (const auto& c : chains_)
        if (c.rule.baseLayer == label) return true;
    return false;
}

void EmotePhysics::begin_frame() {
    for (auto& b : busts_) b.anchor.available = false;
    for (auto& c : chains_) c.anchor.available = false;
}

void EmotePhysics::set_anchor(const std::string& label, double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    for (auto& b : busts_)
        if (b.rule.baseLayer == label) {
            b.anchor.current = V3{x, y, 0};
            b.anchor.available = true;
        }
    for (auto& c : chains_)
        if (c.rule.baseLayer == label) {
            c.anchor.current = V3{x, y, 0};
            c.anchor.available = true;
        }
}

void EmotePhysics::update(double frames, const WriteFn& write) {
    if (!std::isfinite(frames) || frames <= 0) return;
    if (busts_.empty() && chains_.empty()) return;
    // Fixed 1/60 s steps so the authored damping holds at any frame rate;
    // bound a suspended interval so resuming cannot run a huge backlog.
    remainder_ += std::min(frames, 15.0);
    const int steps = int(std::floor(remainder_ + 1e-9));
    remainder_ -= steps;
    if (steps <= 0) return;

    for (auto& b : busts_)
        if (b.anchor.available && !b.anchor.initialized) {
            b.anchor.origin = b.anchor.previous = b.anchor.current;
            b.anchor.initialized = true;
            b.p = V3{b.rule.p[0], b.rule.p[1], b.rule.p[2]};
            b.v = V3{b.rule.pv[0], b.rule.pv[1], b.rule.pv[2]};
        }
    for (auto& c : chains_)
        if (c.anchor.available && !c.anchor.initialized) {
            c.anchor.origin = c.anchor.previous = c.anchor.current;
            c.anchor.initialized = true;
            for (size_t i = 0; i < c.p.size(); ++i) {
                c.p[i] = V3{c.rule.segments[i].p[0], c.rule.segments[i].p[1],
                            c.rule.segments[i].p[2]};
                c.v[i] = V3{c.rule.segments[i].pv[0], c.rule.segments[i].pv[1],
                            c.rule.segments[i].pv[2]};
            }
        }

    const double cs = std::cos(rotation_), sn = std::sin(rotation_);
    auto local = [&](V3 f) {
        return V3{cs * f.x + sn * f.y, -sn * f.x + cs * f.y, f.z};
    };

    for (int step = 0; step < steps; ++step) {
        const double fraction = (step + 1.0) / steps;
        for (auto& b : busts_) {
            if (!b.anchor.available) continue;
            const V3 anchor{b.rule.op[0] + b.anchor.previous.x - b.anchor.origin.x +
                                (b.anchor.current.x - b.anchor.previous.x) * fraction,
                            b.rule.op[1] + b.anchor.previous.y - b.anchor.origin.y +
                                (b.anchor.current.y - b.anchor.previous.y) * fraction,
                            0};
            const V3 force{local(bust_force_).x,
                           local(bust_force_).y + b.rule.gravity,
                           0};
            integrate(b.p, b.v,
                      V3{force.x + (anchor.x - b.p.x) * b.rule.spring,
                         force.y + (anchor.y - b.p.y) * b.rule.spring, 0},
                      b.rule.friction, b.rule.friction, 1);
        }
        for (auto& c : chains_) {
            if (!c.anchor.available) continue;
            const V3 anchor{c.rule.op[0] + c.anchor.previous.x - c.anchor.origin.x +
                                (c.anchor.current.x - c.anchor.previous.x) * fraction,
                            c.rule.op[1] + c.anchor.previous.y - c.anchor.origin.y +
                                (c.anchor.current.y - c.anchor.previous.y) * fraction,
                            0};
            const V3 ext = c.hair ? hair_force_ : parts_force_;
            const V3 external{local(ext).x, local(ext).y + c.rule.gravity, 0};
            for (size_t i = 0; i < c.p.size(); ++i) {
                const auto& seg = c.rule.segments[i];
                const V3 parent = i ? c.p[i - 1] : anchor;
                V3 dir{parent.x - c.p[i].x, parent.y - c.p[i].y, 0};
                const double dist = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (dist > seg.length && dist > 1e-9) {
                    dir.x /= dist;
                    dir.y /= dist;
                    const double stretch = dist - seg.length;
                    if (i == 0) {
                        c.v[i].x += dir.x * (stretch * c.rule.bRate);
                        c.v[i].y += dir.y * (stretch * c.rule.bRate);
                    } else {
                        c.p[i].x += dir.x * stretch;
                        c.p[i].y += dir.y * stretch;
                        const double along = c.v[i].x * dir.x + c.v[i].y * dir.y;
                        c.v[i].x += dir.x * (-along * c.rule.vBound);
                        c.v[i].y += dir.y * (-along * c.rule.vBound);
                    }
                }
                integrate(c.p[i], c.v[i], external, c.rule.frictionX, c.rule.frictionY, 1);
            }
            const double gain = c.hair ? hair_scale_ : parts_scale_;
            const double primary =
                response((c.p.front().x - anchor.x) * c.rule.segments.front().scaleX * gain);
            // Extra bend is excited by a strong swing and must decay at rest,
            // otherwise it drives a perpetual oscillation.
            c.bend = std::clamp(c.bend + (std::abs(primary) > 28 ? 1 : -1) / 32.0, 0.0, 1.0);
            c.phase = std::fmod(c.phase + c.rule.bendSpd * c.bend, 6.283185307179586);
        }
    }

    for (auto& b : busts_) {
        if (!b.anchor.available) continue;
        const V3 anchor{b.rule.op[0] + b.anchor.current.x - b.anchor.origin.x,
                        b.rule.op[1] + b.anchor.current.y - b.anchor.origin.y, 0};
        const double dx = (b.p.x - anchor.x) * bust_scale_;
        const double dy = (b.p.y - anchor.y) * bust_scale_;
        if (!b.rule.varLR.empty()) write(b.rule.varLR, response(dx * b.rule.scaleX));
        if (!b.rule.varUD.empty())
            write(b.rule.varUD, response((dy - b.rule.ofs) * b.rule.scaleY));
        b.anchor.previous = b.anchor.current;
    }
    for (auto& c : chains_) {
        if (!c.anchor.available) continue;
        const double scale = c.hair ? hair_scale_ : parts_scale_;
        const V3 anchor{c.rule.op[0] + c.anchor.current.x - c.anchor.origin.x,
                        c.rule.op[1] + c.anchor.current.y - c.anchor.origin.y, 0};
        const double wave = std::sin(c.phase) * c.bend * c.rule.bendVol;
        if (!c.rule.varLRM.empty())
            write(c.rule.varLRM,
                  response((c.p.front().x - anchor.x) * c.rule.segments.front().scaleX * scale) +
                      wave);
        if (!c.rule.varLR.empty())
            write(c.rule.varLR,
                  response((c.p.back().x - anchor.x) * c.rule.segments.back().scaleX * scale) -
                      wave);
        if (!c.rule.varUD.empty()) {
            const size_t vertical = size_t(
                std::clamp(c.rule.udEft, 0.0, double(c.p.size() - 1)));
            double restY = anchor.y;
            for (size_t i = 0; i <= vertical; ++i) restY += c.rule.segments[i].length;
            write(c.rule.varUD,
                  response((c.p[vertical].y - restY + c.rule.ofs) *
                           c.rule.segments[vertical].scaleY * scale));
        }
        c.anchor.previous = c.anchor.current;
    }
}

std::vector<EmotePhysics::State> EmotePhysics::snapshot() const {
    std::vector<State> out;
    for (const auto& b : busts_) {
        State s;
        s.label = b.rule.baseLayer;
        s.px = b.p.x;
        s.py = b.p.y;
        s.ax = b.anchor.current.x;
        s.ay = b.anchor.current.y;
        s.anchored = b.anchor.available;
        out.push_back(std::move(s));
    }
    for (const auto& c : chains_) {
        State s;
        s.label = c.rule.baseLayer;
        if (!c.p.empty()) {
            s.px = c.p.back().x;
            s.py = c.p.back().y;
        }
        s.ax = c.anchor.current.x;
        s.ay = c.anchor.current.y;
        s.anchored = c.anchor.available;
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace oa::emote
