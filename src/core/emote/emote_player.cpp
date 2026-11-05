// E-mote playback state machine.
// See emote_player.h for the semantics rulings. Autonomous implementation
// (krkr emoteplayer consulted as the behavioural reference; nothing copied).
#include "core/emote/emote_player.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace oa::emote {

// --------------------------------------------------------------------------- Host-compositing default for NEW players. The windowed app co...
std::atomic<bool> g_default_external_pose{false};
void emote_set_default_external_pose(bool on) {
    g_default_external_pose.store(on, std::memory_order_relaxed);
}
bool emote_default_external_pose() {
    return g_default_external_pose.load(std::memory_order_relaxed);
}

EmotePlayer::EmotePlayer() {
    external_pose_ = emote_default_external_pose();
}

EmotePlayer::~EmotePlayer() = default;

bool EmotePlayer::load(const uint8_t* data, size_t size, int canvasW, int canvasH,
                       std::string* err) {
    if (!file_.load(data, size, err)) return false;
    int w = std::clamp(canvasW > 0 ? canvasW : 1920, 64, 4096);
    int h = std::clamp(canvasH > 0 ? canvasH : 1620, 64, 4096);
    const double scale = 1.0;
    width_ = std::max(64, int(std::lround(w * scale)));
    height_ = std::max(64, int(std::lround(h * scale)));
    req_width_ = w;
    req_height_ = h;
    init_defaults();
    physics_.reset(file_); // hair/bust/part rules hang off node labels
    physics_vars_.clear();
    // initial static pose (evidence; the game fades the idle timeline in
    // right after creation, so the static pose only shows for a frame or two)
    dirty_ = true;
    render();
    return true;
}

bool EmotePlayer::load(const std::vector<uint8_t>& data, int canvasW, int canvasH,
                       std::string* err) {
    return load(data.data(), data.size(), canvasW, canvasH, err);
}

// --------------------------------------------------------------------------- defaults: 0 for every variable, then the selector option-0 in...
void EmotePlayer::init_defaults() {
    defaults_.clear();
    explicit_.clear();
    for (const auto& name : file_.variableNames) defaults_[name] = 0.0;
    // collect every motion parameter id too (timeline tracks may write them)
    for (const auto& m : file_.motions)
        for (const auto& p : m.parameter) defaults_.try_emplace(p.id, 0.0);
    // selector option-label variables are derived per frame from the
    // selector value (official SDK semantics); initialise them to the
    // option-0 selection so a fresh layer matches the authored default pose.
    for (const auto& sel : file_.selectors) {
        int i = 0;
        for (const auto& opt : sel.options) {
            defaults_[opt.label] = (i == 0) ? opt.onValue : opt.offValue;
            ++i;
        }
    }
    base_ = defaults_;
    vars_ = defaults_;
    // eyeControl (目パチ): the metadata entry names the eye-open variable and
    // the authored blink shape. Initialised idle with a random wait, seeded
    // per file so two layers created on the same frame (the two heroines)
    // never blink in lockstep.
    blink_label_.clear();
    blink_value_ = 0;
    blink_t_ = -1;
    blink_wait_ = 0;
    blink_frames_ = 0;
    blink_rng_ ^= uint32_t(file_.uid() * 2654435761u);
    if (!file_.eyeControls.empty()) {
        const EyeControl& ec = file_.eyeControls.front();
        if (ec.blinkFrameCount > 0 && !ec.label.empty()) {
            blink_label_ = ec.label;
            blink_begin_ = ec.beginFrame;
            blink_end_ = ec.endFrame;
            blink_frames_ = ec.blinkFrameCount;
            blink_interval_min_ = std::max(1.0, ec.blinkIntervalMin);
            blink_interval_max_ = std::max(blink_interval_min_, ec.blinkIntervalMax);
            blink_wait_ = blink_interval_min_ +
                          blink_rand01() * (blink_interval_max_ - blink_interval_min_);
        }
    }
    // the reference filters timeline-track writes
    known_labels_.clear();
    for (const auto& name : file_.variableNames) known_labels_.insert(name);
    for (const auto& m : file_.motions)
        for (const auto& p : m.parameter) known_labels_.insert(p.id);
    selector_option_labels_.clear();
    for (const auto& sel : file_.selectors)
        for (const auto& opt : sel.options) {
            selector_option_labels_.insert(opt.label);
            known_labels_.insert(opt.label);
        }
}

// whether a timeline track may write this label. * selector option labels are skipped outright (the reference skips them; the per-frame sel...
bool EmotePlayer::track_label_writable(const std::string& label) const {
    if (label.empty()) return false;
    if (selector_option_labels_.count(label)) return false;
    return known_labels_.count(label) != 0;
}

const EmoteTimeline* EmotePlayer::find_timeline(const std::string& label) const {
    for (const auto& t : file_.timelines)
        if (t.label == label) return &t;
    return nullptr;
}

// --------------------------------------------------------------------------- track evaluation at frame time t. A track with no frames neve...
bool emote_sample_track(const TimeVar& track, double t, double* out) {
    if (track.frames.empty()) return false;
    int cur = -1;
    for (size_t i = 0; i < track.frames.size(); ++i) {
        if (track.frames[i].time <= t) cur = int(i);
        else break;
    }
    if (cur < 0) return false;
    // A type-0 frame only marks the END of a segment; the previous content key
    while (cur >= 0 && !track.frames[size_t(cur)].hasContent) --cur;
    if (cur < 0) return false;
    const TimeVarFrame& f = track.frames[size_t(cur)];
    double v = f.value;
    if (cur + 1 < int(track.frames.size())) {
        const TimeVarFrame& g = track.frames[size_t(cur) + 1];
        // 参考实现：当前帧非 type-2，或下一帧是 type-2 时插值。
        const bool interp = (f.type != 2 || g.type == 2);
        if (g.hasContent && interp && g.time > f.time) {
            v = f.value + (g.value - f.value) * (t - f.time) / (g.time - f.time);
        }
    }
    *out = v;
    return true;
}

bool emote_sample_track_loop(const TimeVar& track, double t, double initial, double* out) {
    const auto& fr = track.frames;
    if (fr.empty()) return false;
    int cur = -1;
    int next = -1;
    double prev = initial;
    for (size_t i = 0; i < fr.size(); ++i) {
        if (fr[i].time > t) {
            next = int(i);
            break;
        }
        if (cur >= 0 && fr[size_t(cur)].hasContent) prev = fr[size_t(cur)].value;
        cur = int(i);
    }
    if (cur < 0) return false;
    // Terminal empty key only delimits the duration: the last content key's
    // value is what the loop rests on (reference timelineValue).
    if (!fr[size_t(cur)].hasContent) {
        if (cur + 1 != int(fr.size())) return false;
        for (int i = cur; i >= 0; --i)
            if (fr[size_t(i)].hasContent) {
                *out = fr[size_t(i)].value;
                return true;
            }
        return false;
    }
    double v = fr[size_t(cur)].value;
    // A timeline key names the value to REACH just before the next key, so its
    if (next >= 0 && fr[size_t(next)].time > fr[size_t(cur)].time + 1.0) {
        const double span = fr[size_t(next)].time - fr[size_t(cur)].time - 1.0;
        double r = std::clamp((t - fr[size_t(cur)].time) / span, 0.0, 1.0);
        const double e = fr[size_t(cur)].easing;
        if (e != 0.0) r = std::pow(r, e >= 0 ? 1 + e : 1 / (1 - e));
        v = prev + (v - prev) * r;
    }
    *out = v;
    return true;
}

bool EmotePlayer::track_value(const TimeVar& track, double t, double* out) const {
    return emote_sample_track(track, t, out);
}

// --------------------------------------------------------------------------- fold_end_pose: the natural-end pose of a one-shot timeline. A...
void EmotePlayer::fold_end_pose(const EmoteTimeline* tl) {
    for (const auto& track : tl->variables) {
        // same write filter as compose (a folded sample is a write).
        if (!track_label_writable(track.label)) continue;
        for (auto it = track.frames.rbegin(); it != track.frames.rend(); ++it) {
            if (!it->hasContent) continue;
            base_[track.label] = it->value;
            break;
        }
    }
}

// --------------------------------------------------------------------------- settle_slot. The reference runner wraps a timeline iff its au...
void EmotePlayer::settle_slot(Slot& slot, const EmoteTimeline* tl) {
    if (tl->loopEnd > 0) { // authored loop: period = loopEnd - loopBegin
        const double lo = tl->loopBegin >= 0 ? double(tl->loopBegin) : 0.0;
        const double hi = double(tl->loopEnd);
        const double period = hi - lo;
        if (period > 0) {
            // reference: `local >= loopEnd` wraps, so t == loopEnd lands on
            // loopBegin rather than lingering on the duration marker.
            while (slot.t >= hi) slot.t -= period;
            if (slot.t < lo) slot.t = lo;
        } else {
            slot.active = false; // degenerate [loopBegin..loopEnd]
        }
        return;
    }
    // one-shot (diff=0 expression/voice, and the diff=1 loopEnd<=0 gesture
    // tracks of tg3/slny): ends at lastTime when authored, otherwise at the
    // last keyframe time across the timeline's tracks.
    double end = tl->lastTime > 0 ? double(tl->lastTime) : 0.0;
    for (const auto& track : tl->variables) {
        if (!track.frames.empty()) {
            const double t = track.frames.back().time;
            if (t > end) end = t;
        }
    }
    if (end > 0 && slot.t >= end) {
        slot.t = end;
        slot.active = false;
    }
}

bool EmotePlayer::timeline_loops(const EmoteTimeline* tl) const {
    return tl->loopEnd > 0;
}

double EmotePlayer::start_phase(const EmoteTimeline* tl) const {
    // the start phase is the authored loopBegin (the reference's
    // startTimeline(-10000) collapses to loopBegin on the first update for
    // every loopEnd>0 timeline). loopBegin is -1 on every non-looping
    // timeline (the "no loop" sentinel) — clamp that to 0, the legacy entry
    // phase, so a one-shot still writes its t=0 key immediately.
    if (tl->loopEnd > 0 && tl->loopBegin > 0) return double(tl->loopBegin);
    return 0.0;
}

// Step the idle slot and every foreground entry by `frames` authored frames,
// dropping the foreground entries that ended. Each entry keeps its own clock
// (the reference's engine-level currStartTick resets every parallel track's
// phase and is deliberately not copied).
void EmotePlayer::step_slots(double frames) {
    // Authored fades advance with the same clock step (reference:
    // updateTimelineControl writes baseVariables from every live tween first).
    advance_tweens(frames);
    auto step_one = [&](Slot& slot) {
        if (!slot.active) return false;
        const EmoteTimeline* tl = find_timeline(slot.label);
        if (!tl) return false;
        slot.t += frames;
        settle_slot(slot, tl);
        return slot.active;
    };
    step_one(idle_);
    // landed Pass()es: the stage-end samples apply with this clock step, in
    // the order the entries were passed (later wins for a shared variable)
    for (const PendingPass& pp : pending_pass_) {
        const EmoteTimeline* tl = find_timeline(pp.label);
        if (!tl) continue;
        for (const auto& track : tl->variables) {
            if (!track_label_writable(track.label)) continue;
            double v = 0;
            if (!track_value(track, pp.t, &v)) continue;
            base_[track.label] = v;
        }
    }
    pending_pass_.clear();
    for (auto it = fg_list_.begin(); it != fg_list_.end();) {
        if (step_one(*it)) ++it;
        else it = fg_list_.erase(it);
    }
    blink_advance(frames);
}

// The automatic blink clock (metadata.eyeControl). Authored shape: over `blink_frame_count` frames the eye-open variable travels begin -> e...
void EmotePlayer::blink_advance(double frames) {
    if (blink_label_.empty() || blink_frames_ <= 0 || frames <= 0) return;
    double left = frames;
    while (left > 0) {
        if (blink_t_ < 0) {
            if (blink_wait_ > left) {
                blink_wait_ -= left;
                left = 0;
            } else {
                left -= blink_wait_;
                blink_wait_ = 0;
                blink_t_ = 0;
            }
        } else {
            const double rem = blink_frames_ - blink_t_;
            if (left < rem) {
                blink_t_ += left;
                left = 0;
            } else {
                left -= rem;
                blink_t_ = -1;
                blink_wait_ = blink_interval_min_ +
                              blink_rand01() * (blink_interval_max_ - blink_interval_min_);
            }
        }
    }
    if (blink_t_ < 0) {
        blink_value_ = blink_begin_;
        return;
    }
    const double p = std::clamp(blink_t_ / blink_frames_, 0.0, 1.0);
    double tri = p < 0.45 ? p / 0.45 : (1.0 - p) / 0.55;
    tri = std::clamp(tri, 0.0, 1.0);
    tri = tri * tri * (3.0 - 2.0 * tri); // smoothstep
    blink_value_ = blink_begin_ + (blink_end_ - blink_begin_) * tri;
}

void EmotePlayer::progress(double frames) {
    // game-driven clock — the E-mote SDK Progress() contract.
    if (frames > 0) {
        // keep the render-throttle time base moving with the game-driven
        // clock (last_advance_ms_ would otherwise stay 0 and schedule_render
        // would never pass its cadence gate)
        last_advance_ms_ += uint64_t(frames * 1000.0 / double(kFps) + 0.5);
    }
    step_slots(frames);
    tick_physics(frames);
    compose();
    schedule_render(false);
}

void EmotePlayer::compose() {
    // persistent absolute domain — initialised to defaults; the foreground
    std::map<std::string, double> total = base_;
    for (const Slot& slot : fg_list_) {
        if (!slot.active) continue;
        const EmoteTimeline* tl = find_timeline(slot.label);
        if (!tl) continue;
        for (const auto& track : tl->variables) {
            // selector option-name tracks are
            // skipped and unknown labels discarded (reference
            // E:emoterunner.cpp:1428-1442/1483-1487).
            if (!track_label_writable(track.label)) continue;
            double v = 0;
            if (!track_value(track, slot.t, &v)) continue;
            base_[track.label] = v;
            total[track.label] = v;
        }
    }
    if (idle_.active) {
        const EmoteTimeline* tl = find_timeline(idle_.label);
        if (tl) {
            for (const auto& track : tl->variables) {
                if (!track_label_writable(track.label)) continue;
                double v = 0;
                const bool ok = emote_sample_track_loop(track, idle_.t, 0.0, &v);
                if (!ok) continue;
                total[track.label] += v; // delta layer; never folded into base_
            }
        }
    }
    for (const auto& [k, v] : explicit_) {
        total[k] = v;
        base_[k] = v;
    }
    // Physics output (hair_LR/hair_UD/bust_*/quake_*): engine-owned axis
    // variables, written every tick. They override anything a timeline or
    // setVariable put there — the driver treats these as read-only outputs.
    for (const auto& [k, v] : physics_vars_) total[k] = v;
    for (const auto& sel : file_.selectors) {
        if (sel.options.empty()) continue;
        const auto it = total.find(sel.label);
        const double selv = it == total.end()
                                ? 0.0
                                : std::clamp(it->second, 0.0,
                                             double(sel.options.size() - 1));
        for (size_t i = 0; i < sel.options.size(); ++i) {
            const auto& opt = sel.options[i];
            const double d = std::min(1.0, std::fabs(selv - double(i)));
            total[opt.label] = opt.onValue + (opt.offValue - opt.onValue) * d;
        }
    }
    vars_ = std::move(total);
    // SetVariableDiff (差分) offsets: added LAST so they survive every other
    // writer for the frame (they are a separate additive layer, not a domain
    // write — see set_variable_diff). Empty map for frameworks that never
    // call it, so the composed frame is bit-identical to before.
    for (const auto& [k, v] : diff_) vars_[k] += v;
    // eyeControl blink: a runtime lid overlay on the eye-open variable. MAX,
    // not add — an expression that already closed the eye (笑顔/照れ/悲しみ all
    // sit above the blink's end value on the lid axis) keeps its own lid; a
    // 通常 eye at 0 still blinks through the authored range.
    if (!blink_label_.empty() && blink_value_ > 0.0) {
        auto bit = vars_.find(blink_label_);
        if (bit == vars_.end()) vars_[blink_label_] = blink_value_;
        else bit->second = std::max(bit->second, blink_value_);
    }
    // change detection vs the last rendered snapshot
    bool changed = vars_.size() != rendered_vars_.size();
    if (!changed) {
        for (const auto& [k, v] : vars_) {
            const auto it = rendered_vars_.find(k);
            double prev = it == rendered_vars_.end() ? 0.0 : it->second;
            if (std::fabs(v - prev) > 1e-6) { changed = true; break; }
        }
    }
    if (changed) dirty_ = true;
}

void EmotePlayer::copy_state_from(const EmotePlayer& other) {
    if (&other == this) return;
    base_ = other.base_;
    explicit_ = other.explicit_;
    diff_ = other.diff_;
    fg_list_ = other.fg_list_;
    // The copied slots carry the *old* player's domain snapshot. Rebase them
    // on the state we just inherited: Pass() rolls a fresh play back to this
    // snapshot, and a stale one (the all-zero domain from when 照れ first
    // started) is exactly what zeroed the expression again right after a
    // scene rebuild.
    for (Slot& s : fg_list_) {
        s.base_before = base_;
        s.start_t = s.t;
    }
    idle_ = other.idle_;
    blink_value_ = other.blink_value_;
    blink_t_ = other.blink_t_;
    compose();
    dirty_ = true;
}

bool EmotePlayer::has_playback_state() const {
    for (const auto& kv : base_)
        if (std::fabs(kv.second) > 1e-6) return true;
    for (const auto& s : fg_list_)
        if (s.active) return true;
    return false;
}

void EmotePlayer::advance_ms(uint64_t delta_ms) {
    if (delta_ms == 0) return;
    last_advance_ms_ += delta_ms;
    const double frames = double(delta_ms) * double(kFps) / 1000.0;
    step_slots(frames);
    tick_physics(frames);
    compose();
    if (dirty_) schedule_render(false);
}

// One physics step: sample the anchors of the pose the last compose produced
// (the reference resolves the anchor<->output loop with the same one-frame
// lag), then integrate and keep the written variables for compose().
void EmotePlayer::tick_physics(double frames) {
    if (physics_.empty()) return;
    if (!(frames > 0)) return;
    std::set<std::string> want(physics_.anchor_labels().begin(),
                               physics_.anchor_labels().end());
    std::map<std::string, std::pair<double, double>> anchors;
    std::string err;
    emote_collect_anchors(file_, vars_, want, &anchors, &err);
    physics_.begin_frame();
    for (const auto& [label, pos] : anchors) physics_.set_anchor(label, pos.first, pos.second);
    physics_.update(frames, [&](const std::string& name, double v) { physics_vars_[name] = v; });
}

void EmotePlayer::schedule_render(bool force) {
    if (!dirty_ && !force) return;
    const uint64_t now = last_advance_ms_;
    // the host-composited (GPU) mode evaluates every
    // pose at the configured cadence (default 30 fps — krkr's runner drives
    // the full emote per display frame; aliasing fast authored segments is
    // what made single poses look like abrupt head/body pops). The CPU
    // raster keeps its conservative throttle.
    const uint64_t iv = external_pose_
                            ? uint64_t(1000.0 / double(std::clamp(pose_fps_, 2, 60)))
                            : render_interval_ms_;
    if (!force && now - last_render_ms_ < iv) return;
    render();
}

void EmotePlayer::render_now() {
    compose();
    dirty_ = true;
    render();
}

oa::emote::StaticRenderOptions EmotePlayer::view_options() const {
    oa::emote::StaticRenderOptions opt;
    const double s = (view_scale_ > 0 ? view_scale_ : 0.6);
    const double reqW = req_width_ > 0 ? double(req_width_) : double(width_);
    const double reqH = req_height_ > 0 ? double(req_height_) : double(height_);
    const double fw = double(width_) / reqW;
    const double fh = double(height_) / reqH;
    const double f = std::fabs(fw - fh) < 0.001 ? fw : 1.0;
    opt.fitToCanvas = false;
    opt.scale = s * f;
    opt.dx = f * (reqW / 2.0 + view_x_ - s * scale_origin_x_);
    opt.dy = f * (reqH / 2.0 + view_y_ - s * scale_origin_y_);
    return opt;
}

void EmotePlayer::render() {
    oa::emote::StaticRenderOptions opt = view_options();
    if (external_pose_) {
        // the host composites the pose on the GPU — the
        // player only snapshots the composed variables and marks the pose.
        rendered_vars_ = vars_;
        dirty_ = false;
        last_render_ms_ = last_advance_ms_;
        ++revision_;
        push_frame_now();
        return;
    }
    std::string err;
    if (!render_static_frame(file_, vars_, width_, height_, opt, &rgba_, &err)) {
        return;
    }
    rendered_vars_ = vars_;
    dirty_ = false;
    last_render_ms_ = last_advance_ms_;
    ++revision_;
    push_frame_now();
}

void EmotePlayer::push_frame_now() {
    if (external_pose_) {
        if (!pose_push_) return;
        std::vector<EmoteDrawPart> parts;
        std::string err;
        if (collect_pose_parts(&parts, &err))
            pose_push_(file_, parts, width_, height_);
        return;
    }
    if (cpu_push_) cpu_push_(width_, height_, rgba_.data());
}

// ---------------------------------------------------------------------------
// layer API
// ---------------------------------------------------------------------------
void EmotePlayer::play_timeline(const std::string& label, int flags) {
    const EmoteTimeline* tl = find_timeline(label);
    if (!tl) {
        return;
    }
    // the foreground is an ordered play list. The
    const bool parallel = (flags & 1) != 0;
    if (!parallel) fg_list_.clear();
    if (fg_list_.size() >= kMaxForegroundSlots) fg_list_.erase(fg_list_.begin());
    Slot slot;
    slot.label = label;
    slot.t = start_phase(tl);
    slot.start_t = slot.t;
    slot.base_before = base_;
    slot.active = true;
    slot.flags = flags;
    fg_list_.push_back(std::move(slot));
    // Do NOT fold the t=0 sample into the persistent domain here. The game
    dirty_ = true;
    render_now();
}

void EmotePlayer::fade_in_timeline(const std::string& label) {
    if (!find_timeline(label)) {
        return;
    }
    if (idle_.label != label) idle_.t = 0;
    idle_.label = label;
    idle_.active = true;
    compose();
    render_now();
}

void EmotePlayer::pass() {
    // Pass() advances the playhead to the end of the timeline's current stage
    if (fg_list_.empty()) return;
    for (const Slot& slot : fg_list_) {
        if (!slot.active) continue;
        const EmoteTimeline* tl = find_timeline(slot.label);
        if (!tl) continue;
        double end = tl->lastTime > 0 ? double(tl->lastTime) : 0.0;
        for (const auto& track : tl->variables)
            if (!track.frames.empty() && track.frames.back().time > end)
                end = track.frames.back().time;
        pending_pass_.push_back({slot.label, end > 0 ? end : slot.t});
    }
    fg_list_.clear();
    dirty_ = true; // the pose lands with the next clock step
}

// Fold every foreground entry's authored END pose into the persistent domain
// (step()/skip()): a wrapping entry is sampled at its loop end, a one-shot
// entry folds its per-track last content sample (fold_end_pose). Entries
// fold in list order, so a later entry wins for a shared variable — the same
// order compose() writes them.
void EmotePlayer::fold_foreground_end() {
    for (Slot& slot : fg_list_) {
        if (!slot.active) continue;
        const EmoteTimeline* tl = find_timeline(slot.label);
        if (!tl) continue;
        if (timeline_loops(tl)) {
            // the loopEnd frame is reachable and IS the authored loop seam
            // sample
            slot.t = double(tl->loopEnd);
            compose(); // folds that sample into base_
        } else {
            fold_end_pose(tl); // authored end pose straight into base_
        }
    }
}

void EmotePlayer::step() {
    // (api_skip.html): Step() skips to the stage where the
    if (fg_list_.empty()) return;
    fold_foreground_end();
    fg_list_.clear();
    compose();
    render_now();
}

void EmotePlayer::skip() {
    // exskip: land every slot at its end state (api_skip.html:
    if (!fg_list_.empty()) {
        fold_foreground_end();
        fg_list_.clear();
    }
    if (idle_.active) {
        idle_.t = 0;
        idle_.active = false;
        idle_.label.clear();
    }
    compose();
    render_now();
}

void EmotePlayer::stop() {
    // both slots end internally; the persistent domain keeps the current
    // pose (header contract: "both slots off at the current pose")
    idle_.active = false;
    idle_.label.clear();
    idle_.t = 0;
    fg_list_.clear();
    compose();
    render_now();
}

void EmotePlayer::stop_idle_timeline() {
    // the named idle (diff/待機) timeline ended — stopTimeline
    // / fadeOutTimeline on an idle match. The delta layer stops applying;
    // the persistent domain keeps its pose and the foreground slot keeps
    // playing (a still-running expression/voice timeline is unaffected).
    if (!idle_.active) return;
    idle_.active = false;
    idle_.label.clear();
    idle_.t = 0;
    compose();
    render_now();
}

void EmotePlayer::set_scale(double s, double origin_x, double origin_y) {
    if (s > 0.01 && s <= 4.0) view_scale_ = s;
    scale_origin_x_ = origin_x;
    scale_origin_y_ = origin_y;
    render_now();
}

void EmotePlayer::set_coord(double x, double y) {
    view_x_ = x;
    view_y_ = y;
    render_now();
}

void EmotePlayer::set_variable(const std::string& name, double value) {
    if (name.empty()) return;
    tweens_.erase(name); // an instant write cancels the variable's fade
    explicit_[name] = value;
    compose();
    schedule_render(false);
}

/// Reference easing (emotephysics.h emoteEase): exponent = 1+ease for ease>=0,
/// 1/(1-ease) for negative ease; t clamped to [0,1].
static double emote_ease(double t, double easing) {
    t = std::clamp(t, 0.0, 1.0);
    if (!std::isfinite(easing)) return t;
    const double exponent = easing >= 0 ? 1.0 + easing : 1.0 / (1.0 - easing);
    return std::pow(t, exponent);
}

void EmotePlayer::set_variable_faded(const std::string& name, double value, double time,
                                     double easing) {
    if (name.empty()) return;
    if (!(time > 0) || !std::isfinite(time)) {
        set_variable(name, value);
        return;
    }
    // Start from the composed value the current frame shows (the reference's
    // emoteVariable::set(current, ...)) and drive the persistent domain from
    // the tween until it lands.
    const double current = get_variable(name);
    explicit_.erase(name);
    VarTween tw;
    tw.from = current;
    tw.to = value;
    tw.start = tween_clock_;
    tw.duration = time;
    tw.easing = easing;
    tweens_[name] = tw;
    base_[name] = current;
    compose();
    schedule_render(false);
}

void EmotePlayer::advance_tweens(double frames) {
    if (!(frames > 0)) return;
    tween_clock_ += frames;
    if (tweens_.empty()) return;
    for (auto it = tweens_.begin(); it != tweens_.end();) {
        const VarTween& tw = it->second;
        const double t = tween_clock_ - tw.start;
        const bool landed = tw.duration <= 0 || t >= tw.duration;
        base_[it->first] =
            landed ? tw.to
                   : tw.from + (tw.to - tw.from) * emote_ease(t / tw.duration, tw.easing);
        if (landed) {
            explicit_.erase(it->first);
            it = tweens_.erase(it);
        } else {
            ++it;
        }
    }
}

void EmotePlayer::set_variable_diff(const std::string& name, double value) {
    if (name.empty()) return;
    // 0 = "差分変数をリセット" (emote.lua:186-187): a true clear, so the offset
    // layer disappears from the composed frame entirely.
    if (value == 0.0) diff_.erase(name);
    else diff_[name] = value;
    compose();
    schedule_render(false);
}

double EmotePlayer::get_variable(const std::string& name) const {
    const auto it = vars_.find(name);
    if (it != vars_.end()) {
        // Report the value WITHOUT the SetVariableDiff offset: the offset is a
        const auto dd = diff_.find(name);
        return dd == diff_.end() ? it->second : it->second - dd->second;
    }
    const auto b = explicit_.find(name);
    if (b != explicit_.end()) return b->second;
    const auto d = defaults_.find(name);
    return d == defaults_.end() ? 0.0 : d->second;
}

int EmotePlayer::variable_count() const { return int(file_.variableNames.size()); }

std::string EmotePlayer::variable_label_at(int index) const {
    if (index < 0 || size_t(index) >= file_.variableNames.size()) return std::string();
    return file_.variableNames[size_t(index)];
}

bool EmotePlayer::stop_timeline(const std::string& label) {
    // stopTimeline(label) ends the NAMED timeline wherever it
    bool found = false;
    for (auto it = fg_list_.begin(); it != fg_list_.end();) {
        if (it->label == label) {
            it = fg_list_.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    if (idle_.active && idle_.label == label) {
        idle_.active = false;
        idle_.label.clear();
        idle_.t = 0;
        found = true;
    }
    if (found) {
        compose();
        render_now();
    }
    return found;
}

bool EmotePlayer::is_timeline_playing(const std::string& label) const {
    if (label.empty()) return false;
    if (idle_.active && idle_.label == label) return true;
    for (const Slot& slot : fg_list_)
        if (slot.active && slot.label == label) return true;
    return false;
}

double EmotePlayer::idle_time() const { return idle_.active ? idle_.t : -1.0; }

std::vector<EmotePlayer::ForegroundSlot> EmotePlayer::foreground_slots() const {
    std::vector<ForegroundSlot> out;
    out.reserve(fg_list_.size());
    for (const Slot& slot : fg_list_) {
        if (!slot.active) continue;
        out.push_back(ForegroundSlot{slot.label, slot.t, slot.flags});
    }
    return out;
}

std::string EmotePlayer::foreground_timeline() const {
    // list top = the most recently started entry (the legacy single slot is
    // the list's only entry, so this stays its exact meaning)
    for (auto it = fg_list_.rbegin(); it != fg_list_.rend(); ++it)
        if (it->active) return it->label;
    return "";
}

std::string EmotePlayer::idle_timeline() const {
    return idle_.active ? idle_.label : "";
}

std::map<std::string, double> EmotePlayer::variables() const { return vars_; }

bool EmotePlayer::collect_pose_parts(std::vector<EmoteDrawPart>* parts,
                                     std::string* err) const {
    const oa::emote::StaticRenderOptions opt = view_options();
    return emote_collect_parts(file_, vars_, width_, height_, opt, parts, err);
}

} // namespace oa::emote
