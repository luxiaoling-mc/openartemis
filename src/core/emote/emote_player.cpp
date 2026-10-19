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

// ---------------------------------------------------------------------------
// play-model switches. Read once at construction so a single
// process can hold a REF player (legacy env set at construction) and a NEW
// player side by side — the same-process A-B used by the tests. They are not
// per-frame knobs.
//   OA_EMOTE_LEGACY_SLOTS=1  legacy model: single foreground slot + the
//                            diff==1-based wrap rule (REF arm)
//   OA_EMOTE_WRAP=diff       old wrap rule only (diff==1 wraps, loopEnd<=0
//                            falls back to lastTime/lo+1 — the freeze)
//   OA_EMOTE_PARALLEL=0      playTimeline always replaces the whole
//                            foreground list (single-slot behaviour)
// ---------------------------------------------------------------------------
static bool env_flag_on(const char* name, bool dflt) {
    const char* v = std::getenv(name);
    if (!v || !*v) return dflt;
    if (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 ||
        std::strcmp(v, "false") == 0 || std::strcmp(v, "no") == 0)
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// Host-compositing default for NEW players. The windowed app composites the
// pose on the GPU (SDL_RenderGeometry) and only needs the player's own CPU
// raster as a fallback — but without this latch every freshly loaded player
// still paid one full CPU pose raster inside load() (43+ ms on the large
// PSBs) before the host's first pump flipped external-pose on. The host
// latches this once at startup (renderer exists -> on); headless /
// OA_EMOTE_GPU=0 hosts keep it off so rgba() keeps its CPU truth.
// ---------------------------------------------------------------------------
std::atomic<bool> g_default_external_pose{false};
void emote_set_default_external_pose(bool on) {
    g_default_external_pose.store(on, std::memory_order_relaxed);
}
bool emote_default_external_pose() {
    return g_default_external_pose.load(std::memory_order_relaxed);
}

EmotePlayer::EmotePlayer() {
    if (const char* fps = std::getenv("OA_EMOTE_FPS")) {
        const int v = std::atoi(fps);
        if (v >= 2 && v <= 60) {
            kFps = v;
            pose_fps_ = v;
            render_interval_ms_ = uint64_t(1000 / (v < 8 ? v : 8));
        }
    }
    legacy_slots_ = env_flag_on("OA_EMOTE_LEGACY_SLOTS", false);
    const char* wrap = std::getenv("OA_EMOTE_WRAP");
    wrap_legacy_ = wrap != nullptr && std::strcmp(wrap, "diff") == 0;
    parallel_off_ = !env_flag_on("OA_EMOTE_PARALLEL", true);
    // the legacy write surface (the `next.type==2`
    // interpolation constraint and the unfiltered track writes). Latched at
    // construction so one process can hold both arms side by side.
    s2_legacy_ = env_flag_on("OA_EMOTE_S2_LEGACY", false);
    // GPU-compositing hosts latch this before boot (see
    // emote_set_default_external_pose): new players skip their load-time CPU
    // raster and the host composites the pose geometry instead.
    external_pose_ = emote_default_external_pose();
}

EmotePlayer::~EmotePlayer() = default;

bool EmotePlayer::load(const uint8_t* data, size_t size, int canvasW, int canvasH,
                       std::string* err) {
    if (!file_.load(data, size, err)) return false;
    int w = std::clamp(canvasW > 0 ? canvasW : 1920, 64, 4096);
    int h = std::clamp(canvasH > 0 ? canvasH : 1620, 64, 4096);
    // full resolution by default — the half-size canvas
    // was stretched 2x into the 1920x1620 layer box, visibly blurring the
    // figure (user report). The CPU raster cost is paid per pose (~4 fps
    // throttle); OA_EMOTE_HALFRES=1 restores the half-size fallback and
    // OA_EMOTE_FULLRES stays accepted as an explicit 1:1 request.
    const double scale = (std::getenv("OA_EMOTE_HALFRES") && !std::getenv("OA_EMOTE_FULLRES"))
                             ? 0.5
                             : 1.0;
    width_ = std::max(64, int(std::lround(w * scale)));
    height_ = std::max(64, int(std::lround(h * scale)));
    req_width_ = w;
    req_height_ = h;
    if (std::getenv("OA_EMOTE_DEBUG"))
        std::fprintf(stderr, "[emote] load req=%dx%d buf=%dx%d scale=%.2f\n", w, h,
                     width_, height_, scale);
    if (std::getenv("OA_EMOTE_PRIO_LOG")) {
        size_t reordered = 0;
        for (const auto& m : file_.motions) {
            if (m.drawOrder.empty() || m.drawOrder == m.nodeListDfs) continue;
            ++reordered;
            if (reordered <= 3)
                std::fprintf(stderr, "[prio] motion '%s' order differs (%zu nodes)\n",
                             m.name.c_str(), m.drawOrder.size());
        }
        std::fprintf(stderr, "[prio] motions=%zu with-priority-reorder=%zu\n",
                     file_.motions.size(), reordered);
    }
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

// ---------------------------------------------------------------------------
// defaults: 0 for every variable, then the selector option-0 init (the
// metadata selectorControl groups apply their first option's onValue to its
// labelled variable and offValue to the group's other variables — e.g.
// arm_type option 0 "fade_a": fade_a=0 visible, fade_b..e=1 hidden).
// ---------------------------------------------------------------------------
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
    // against the file's variable domain (E:emoterunner.cpp:1483-1487) and
    // skips selector option labels explicitly (1428-1442). `known_labels_` is
    // the O(1) membership set for that filter, built once per file: the
    // variable list, every motion parameter id (tracks drive the parameter
    // axes directly) and the selector option labels.
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

// whether a timeline track may write this label.
//   * selector option labels are skipped outright (the reference skips them;
//     the per-frame selector derivation owns those variables — the legacy
//     track sample was written into the persistent base_ domain first and then
//     overwritten in the composed snapshot every frame, i.e. observationally
//     equivalent but polluting);
//   * a label outside the known variable domain is discarded (reference
//     `_varList.find(label)` miss).
// OA_EMOTE_S2_LEGACY=1 restores the unfiltered write surface.
bool EmotePlayer::track_label_writable(const std::string& label) const {
    if (label.empty()) return false;
    if (s2_legacy_) return true;
    if (selector_option_labels_.count(label)) {
        ++stats_.selectorTracksSkipped;
        return false;
    }
    if (!known_labels_.count(label)) {
        ++stats_.unknownLabelWrites;
        return false;
    }
    return true;
}

const EmoteTimeline* EmotePlayer::find_timeline(const std::string& label) const {
    for (const auto& t : file_.timelines)
        if (t.label == label) return &t;
    return nullptr;
}

// ---------------------------------------------------------------------------
// track evaluation at frame time t. A track with no frames never writes; the
// current frame is the last content frame with time <= t; a type-0/empty
// current frame means the track does NOT write this frame (the variable
// keeps its current domain value — official SDK variable-domain semantics).
// Linear interpolation between two type-2 keyframes.
// ---------------------------------------------------------------------------
bool emote_sample_track(const TimeVar& track, double t, bool legacy, double* out) {
    if (track.frames.empty()) return false;
    int cur = -1;
    for (size_t i = 0; i < track.frames.size(); ++i) {
        if (track.frames[i].time <= t) cur = int(i);
        else break;
    }
    if (cur < 0) return false;
    // A type-0 frame only marks the END of a segment; the previous content key
    // keeps holding (the Rust sampler does the same walk-back). Sampling must
    // skip those tails instead of reporting "no write": a fractional clock can
    // otherwise jump straight over a short content window. NekoMiko's 照れ
    // timeline keeps arm_type=1 only in authored frames [3,4); at 60 fps with
    // fractional deltas the samples landed at 0, 2.2, 4.3 ... and the key was
    // never read — the shy pose kept the default arm_type=0 (hands down) while
    // the t=0 face keys still applied, which is exactly the reported
    // "pose/eyes/mouth wrong" frame.
    while (cur >= 0 && !track.frames[size_t(cur)].hasContent) --cur;
    if (cur < 0) return false;
    const TimeVarFrame& f = track.frames[size_t(cur)];
    double v = f.value;
    if (cur + 1 < int(track.frames.size())) {
        const TimeVarFrame& g = track.frames[size_t(cur) + 1];
        // the reference interpolates whenever the
        // follower has content and the pair is not a type-2 key held by a
        // non-type-2 successor (E:emoterunner.cpp:1465-1481):
        //   next != null && (cur.type != 2 || next.type == 2)
        // The legacy predicate also required cur.type == 2, i.e. it held any
        // OTHER content key flat until the next one. Census over six real PSBs:
        // zero such pairs — every non-type-2 key followed
        // by a content frame is a content-LESS gap key (type 0) that never
        // writes, so the aligned predicate is inert on the shipped data.
        // `OA_EMOTE_S2_LEGACY=1` (s2_legacy_) restores the old predicate.
        const bool interp = legacy ? (f.type == 2 && g.type == 2)
                                   : (f.type != 2 || g.type == 2);
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
    // segment runs previous -> own over [cur.time, next.time - 1]. `next` may
    // be an EMPTY key (the terminal duration marker): the reference still uses
    // its time as the segment end — that is precisely what makes the tail ease
    // into the wrap value instead of holding the last key and snapping. Two
    // consequences that the earlier revision got wrong:
    //   * at tick == cur.time the sampled value is `prev`, not `v` (r starts
    //     at 0), so the curve is continuous across every key, and
    //   * an empty `next` must NOT suppress interpolation.
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
    return emote_sample_track(track, t, s2_legacy_, out);
}

// ---------------------------------------------------------------------------
// fold_end_pose: the natural-end pose of a one-shot timeline. A track that
// played to the end leaves its LAST CONTENT sample in the persistent domain
// (from the final content key it holds until its type-0 tail stops writes,
// and the tail value is never sampled while active). So the fold writes each
// track's last content key value — one shared clock sample cannot reproduce
// this (at the timeline end every track sits on its own type-0 tail and does
// not write; tracks whose tails end earlier stop even sooner).
// step()/skip() used to sample t = lastTime, falling back
// to t = 0 because every NekoMiko one-shot carries lastTime = -1 — that
// folded the START pose (arm_type t0 = 0 -> arm group A) into the domain.
// ---------------------------------------------------------------------------
void EmotePlayer::fold_end_pose(const EmoteTimeline* tl) {
    for (const auto& track : tl->variables) {
        // same write filter as compose (a folded sample is a write).
        if (!track_label_writable(track.label)) continue;
        for (auto it = track.frames.rbegin(); it != track.frames.rend(); ++it) {
            if (!it->hasContent) continue;
            base_[track.label] = it->value;
            if (std::getenv("OA_DEBUG_FOLD"))
                std::fprintf(stderr, "[fold] %-20s <- %.3f (t=%.0f, last=%zu)\n",
                             track.label.c_str(), it->value, it->time, track.frames.size());
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// settle_slot. The reference runner wraps a timeline
// iff its authored loopEnd > 0, independently of `diff`:
//   E:emoterunner.cpp:1411-1417
//     if (loopEnd > 0 && tick - startTick + loopBegin > loopEnd) startTick = tick;
//     currRelTime = tick - startTick + loopBegin;
// i.e. the relative clock lives in [loopBegin, loopEnd] (the loopEnd frame is
// REACHABLE: the reset fires only once it is exceeded, landing back on
// loopBegin) with period = loopEnd - loopBegin. The old approximation here was
// `diff == 1 => loop` with a fallback end of lo+1 when loopEnd<=0, which pinned
// every diff==1 loopEnd<=0 timeline (tg3/slny gesture tracks, 441 sampled) at
// t == fmod(t, 1) == 0 — the freeze.
// Everything else is a one-shot: it plays through and deactivates at its
// authored end (lastTime when positive, otherwise the largest keyframe time
// across its tracks), keeping the last written samples in the domain — a
// finished timeline never returns to t0. The reference never removes the
// timeline (it keeps re-sampling its tail); with the persistent variable
// domain both models leave the identical pose, and ours keeps
// isTimelinePlaying truthful (our safety rule is kept).
// ---------------------------------------------------------------------------
void EmotePlayer::settle_slot(Slot& slot, const EmoteTimeline* tl) {
    if (!wrap_is_legacy()) {
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
        // one-shot (diff=0 expression/voice, and the diff=1 loopEnd<=0
        // gesture tracks of tg3/slny): ends at lastTime when authored,
        // otherwise at the last keyframe time across the timeline's tracks
        // (tracks carry their own type-0 tails, e.g. 47 frames for
        // 笑顔_ボイス再生用).
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
        return;
    }
    // --- legacy A-B arm: diff==1 wraps, everything else one-shot ---
    if (tl->diff == 1) { // loop (待機): wrap in [loopBegin, loopEnd)
        const double lo = tl->loopBegin >= 0 ? double(tl->loopBegin) : 0.0;
        const double hi = tl->loopEnd > 0 ? double(tl->loopEnd)
                                          : (tl->lastTime > 0 ? double(tl->lastTime) : lo + 1);
        const double period = hi - lo;
        if (period > 0) slot.t = lo + std::fmod(slot.t - lo, period);
        else slot.active = false;
    } else {
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
}

bool EmotePlayer::timeline_loops(const EmoteTimeline* tl) const {
    return wrap_is_legacy() ? tl->diff == 1 : tl->loopEnd > 0;
}

double EmotePlayer::start_phase(const EmoteTimeline* tl) const {
    // the start phase is the authored loopBegin (the reference's
    // startTimeline(-10000) collapses to loopBegin on the first update for
    // every loopEnd>0 timeline). loopBegin is -1 on every non-looping
    // timeline (the "no loop" sentinel) — clamp that to 0, the legacy entry
    // phase, so a one-shot still writes its t=0 key immediately.
    if (wrap_is_legacy()) return 0.0;
    if (tl->loopEnd > 0 && tl->loopBegin > 0) return double(tl->loopBegin);
    return 0.0;
}

// Step the idle slot and every foreground entry by `frames` authored frames,
// dropping the foreground entries that ended. Each entry keeps its own clock
// (the reference's engine-level currStartTick resets every parallel track's
// phase and is deliberately not copied).
void EmotePlayer::step_slots(double frames) {
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

// The automatic blink clock (metadata.eyeControl). Authored shape: over
// `blink_frame_count` frames the eye-open variable travels begin -> end ->
// begin; the controller then idles for a random interval inside
// [blinkIntervalMin .. blinkIntervalMax]. The closing half is slightly faster
// than the opening half, like a real lid; a smoothstep keeps the endpoints
// quiet so the lid never pops.
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
    // The host Lua framework computes the elapsed 60-fps frame count from
    // its own clock and calls em:progress(fr) every vsync (甜蜜女友3
    // e-mote.lua ex.progress / emote.vsync); the slots must advance by
    // exactly those frames (fractional values accumulate on slot.t) and
    // settle like any other clock step. A zero/negative frame count only
    // re-composes (no-op when nothing changed).
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

// ---------------------------------------------------------------------------
// compose: persistent absolute domain (base_) + per-frame idle delta layer
// + explicit + selectors; diff against the last rendered snapshot.
// ---------------------------------------------------------------------------
// OA_EMOTE_CTRL_FIX=1 — corrected transport semantics for the Artemis e-mote
// wrapper (system/adv/emote.lua):
//   layer:pass()                 before/after playTimeline  -> NO-OP (Pass()
//                                only passes a timeline stage; NekoMiko's
//                                expression tracks have none, and ending the
//                                slot here would kill the expression the game
//                                just started)
//   layer:step()                 after a save load          -> NO-OP (Step()
//                                settles the fade blend, not the timeline)
//   expression end                                          -> release
// Until now pass() cleared the whole foreground list and step() folded the
// timeline's END pose into the persistent domain, which is why a finished
// expression stayed on screen forever (eyes 特殊4 / brows 特殊1 instead of the
// 通常 state the reference capture shows).
static bool ctrl_fix() {
    static const bool on = [] {
        const char* e = std::getenv("OA_EMOTE_CTRL_FIX");
        return e && *e == '1';
    }();
    return on;
}

// See compose(): expression timelines are released at their end instead of
// being folded into the persistent domain. Opt-in for A/B verification.
static bool expr_release() {
    static const bool on = [] {
        const char* e = std::getenv("OA_EMOTE_EXPR_RELEASE");
        return e && *e == '1';
    }();
    return on;
}

void EmotePlayer::compose() {
    // persistent absolute domain — initialised to defaults; the foreground
    // (absolute) timelines write their samples INTO the domain while active,
    // and a timeline that ends (auto-end / Pass / Step / Skip / Stop) simply
    // STOPS WRITING: the domain keeps its last values, so expression poses
    // persist between lines and no snap back to the neutral base occurs
    // (matches the manual's persistent-variable model and the krkr v1-3
    // domain; per-frame rebuild from defaults was the release-on-end snap).
    // The foreground is an ORDERED list (the reference's
    // currTimeline): entries compose in list order and a later entry's write
    // overwrites an earlier one for a shared variable (the reference's
    // insert-order overwrite). The idle loop (通常待機, diff=1) is a per-frame
    // DELTA layer ADDED on top of the domain while active and never folded
    // into base_ (breathing under any expression; no accumulation). Explicit
    // setVariable writes both the frame and the domain (it wins over the
    // delta for the frame, and its value stays in the domain). Selector
    // controls derive the option-label variables (fade_a..e) from the
    // selector value every frame (selector control apply semantics: per
    // option value = on + (off - on) * min(1, |selector - index|)).
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
            // OA_EMOTE_EXPR_RELEASE=1: an absolute (expression) timeline only
            // contributes while it plays. Without the fold the pose returns to
            // the base/default state when the timeline ends — the behaviour the
            // reference capture shows after 照れ finishes (wide eyes + thin
            // brows) and the behaviour `pass()`/natural end should produce.
            // Kept opt-in until the remaining screens are re-verified: the
            // persistent domain is what the arm/selector poses were tuned on.
            if (!expr_release()) base_[track.label] = v;
            total[track.label] = v;
        }
    }
    if (idle_.active) {
        const EmoteTimeline* tl = find_timeline(idle_.label);
        if (tl) {
            // The idle is the one LOOPING timeline in this format, and it uses
            // the reference's target model (emote_sample_track_loop): a key is
            // the value to REACH just before the next key, so the 300-frame
            // tail eases into the wrap value instead of holding the last
            // authored key and snapping at t=loopEnd. Measured on the driver:
            // the centroid tracks the official render within 0.6 px across the
            // whole loop, and the wrap step is 0.02 px (the hold model popped
            // 9.3 px at the wrap and lagged the official curve by one frame
            // everywhere).
            // OA_EMOTE_IDLE_TARGET=0 restores the old hold model (A/B arm).
            static const bool target_env = [] {
                const char* e = std::getenv("OA_EMOTE_IDLE_TARGET");
                return !(e && *e && std::strcmp(e, "0") == 0);
            }();
            // Note: NOT tied to wrap_is_legacy() — the legacy arm exists to A/B
            // the SLOT model, and keeping the sampler constant is what lets
            // that comparison isolate slot semantics.
            const bool target_model = target_env;
            for (const auto& track : tl->variables) {
                if (!track_label_writable(track.label)) continue;
                double v = 0;
                const bool ok = target_model ? emote_sample_track_loop(track, idle_.t, 0.0, &v)
                                             : track_value(track, idle_.t, &v);
                if (!ok) continue;
                if (std::getenv("OA_DEBUG_IDLE") && track.label == "body_slant")
                    std::fprintf(stderr,
                                 "[idle] '%s' n=%zu t=%.1f sample=%.3f first=(%.1f,%.3f) base=%.3f\n",
                                 tl->label.c_str(), track.frames.size(), idle_.t, v,
                                 track.frames.empty() ? -1 : track.frames.front().time,
                                 track.frames.empty() ? 0 : track.frames.front().value,
                                 base_.count("body_slant") ? base_["body_slant"] : 0.0);
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
    if (std::getenv("OA_DUMP_VARS")) {
        static long long dumpTick = 0;
        if ((dumpTick++ % 30) == 0) {
            std::fprintf(stdout, "[vars] #%lld f=%.0f", dumpTick,
                         double(last_advance_ms_) * double(kFps) / 1000.0);
            for (const auto& kv : vars_)
                std::fprintf(stdout, " %s=%.3f", kv.first.c_str(), kv.second);
            std::fprintf(stdout, "\n");
        }
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
    // OA_EMOTE_PHYS=0 disables the whole system (A/B arm, and a safety valve if
    // a future PSB authors something the integrator handles badly).
    static const bool enabled = [] {
        const char* v = std::getenv("OA_EMOTE_PHYS");
        return !(v && *v && std::strcmp(v, "0") == 0);
    }();
    if (!enabled) return;
    if (physics_.empty()) return;
    if (!(frames > 0)) return;
    std::set<std::string> want(physics_.anchor_labels().begin(),
                               physics_.anchor_labels().end());
    std::map<std::string, std::pair<double, double>> anchors;
    std::string err;
    emote_collect_anchors(file_, vars_, want, &anchors, &err);
    if (std::getenv("OA_DEBUG_PHYS")) {
        static long long n = 0;
        if ((n++ % 30) == 0) {
            std::fprintf(stderr, "[phys] frames=%.2f anchors=%zu", frames, anchors.size());
            for (const auto& [k, v] : anchors)
                std::fprintf(stderr, " %s=(%.1f,%.1f)", k.c_str(), v.first, v.second);
            std::fprintf(stderr, "\n");
        }
    }
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
    // OA_EMOTE_SCALE_MUL: A/B multiplier on the view scale (the report's
    // characters render ~15% larger than the original engine's).
    static const double scale_mul = [] {
        const char* v = std::getenv("OA_EMOTE_SCALE_MUL");
        return (v && *v) ? std::atof(v) : 1.0;
    }();
    const double s = (view_scale_ > 0 ? view_scale_ : 0.6) * scale_mul;
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
    if (std::getenv("OA_EMOTE_DEBUG"))
        std::fprintf(stderr, "[emote] view s=%.3f dx=%.1f dy=%.1f coord=(%.1f,%.1f) "
                             "origin=(%.1f,%.1f) canvas=%dx%d\n",
                     opt.scale, opt.dx, opt.dy, view_x_, view_y_, scale_origin_x_,
                     scale_origin_y_, width_, height_);
    if (external_pose_) {
        // the host composites the pose on the GPU — the
        // player only snapshots the composed variables and marks the pose.
        rendered_vars_ = vars_;
        dirty_ = false;
        last_render_ms_ = last_advance_ms_;
        ++revision_;
        return;
    }
    std::string err;
    if (!render_static_frame(file_, vars_, width_, height_, opt, &rgba_, &err)) {
        if (std::getenv("OA_EMOTE_DEBUG"))
            std::fprintf(stderr, "[emote] pose render failed: %s\n", err.c_str());
        return;
    }
    rendered_vars_ = vars_;
    dirty_ = false;
    last_render_ms_ = last_advance_ms_;
    ++revision_;
}

// ---------------------------------------------------------------------------
// layer API
// ---------------------------------------------------------------------------
void EmotePlayer::play_timeline(const std::string& label, int flags) {
    const EmoteTimeline* tl = find_timeline(label);
    if (!tl) {
        if (std::getenv("OA_EMOTE_DEBUG"))
            std::fprintf(stderr, "[emote] playTimeline: unknown timeline '%s'\n",
                         label.c_str());
        return;
    }
    // the foreground is an ordered play list. The
    // official SDK PlayTimeline(label, flags) contract — flags &
    // TIMELINE_PLAY_PARALLEL (1) plays IN PARALLEL with what is already
    // playing, otherwise the current playback is stopped first — and the
    // reference implementation's currTimeline push (E:emoterunner.cpp:1354).
    // OA_EMOTE_PARALLEL=0 / OA_EMOTE_LEGACY_SLOTS=1 restore the legacy
    // single-slot replacement (A-B arm).
    const bool parallel = parallel_enabled() && (flags & 1) != 0;
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
    // calls playTimeline and Pass() in the same beat (emote.lua), and the
    // reference applies nothing in between: playing 通常 over an existing
    // 照れ pose leaves the pose untouched. An eager compose wrote 通常's
    // t0=0 before Pass() could skip the timeline (the face snapped to 通常
    // at a scene reset). The next advance_ms() evaluates the slot normally.
    dirty_ = true;
    render_now();
}

void EmotePlayer::fade_in_timeline(const std::string& label) {
    if (!find_timeline(label)) {
        if (std::getenv("OA_EMOTE_DEBUG"))
            std::fprintf(stderr, "[emote] fadeInTimeline: unknown timeline '%s'\n",
                         label.c_str());
        return;
    }
    if (idle_.label != label) idle_.t = 0;
    idle_.label = label;
    idle_.active = true;
    compose();
    render_now();
}

void EmotePlayer::pass() {
    if (ctrl_fix()) {
        compose();
        render_now();
        return;
    }
    // Pass() advances the playhead to the end of the timeline's current stage
    // and ENDS the entry — but the stage-end sample lands on the NEXT clock
    // step, not at the call. Re-verified against the official driver
    // (emote_official --seq):
    //   play 照れ,1 ; Pass                    -> values unchanged at Pass,
    //                                            Progress(1) leaves 36.5/50/40
    //   ... over that pose: play 通常,1 ; Pass -> still 36.5/50/40 at Pass,
    //                                            Progress(1) writes 0/0/0
    //   (通常's tracks DO have content — all zeros — at their end, so the
    //    official write is a real write. The earlier "only land non-zero ends,
    //    roll the rest back to the pre-play snapshot" rule left the previous
    //    expression's mouth/head/arm axes on the face, which is the reported
    //    "mouth and nose sit somewhere else".)
    if (fg_list_.empty()) return;
    for (const Slot& slot : fg_list_) {
        if (!slot.active) continue;
        const EmoteTimeline* tl = find_timeline(slot.label);
        if (!tl) continue;
        double end = tl->lastTime > 0 ? double(tl->lastTime) : 0.0;
        for (const auto& track : tl->variables)
            if (!track.frames.empty() && track.frames.back().time > end)
                end = track.frames.back().time;
        if (std::getenv("OA_DEBUG_PASS"))
            std::fprintf(stderr, "[pass] slot '%s' -> t=%.0f (slots=%zu)\n",
                         slot.label.c_str(), end, fg_list_.size());
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
            if (wrap_is_legacy()) {
                // legacy arm: the sample just before the loop end
                const double hi = tl->loopEnd > 0 ? double(tl->loopEnd)
                                                  : double(tl->lastTime);
                slot.t = hi > 1 ? hi - 1 : 0;
                compose(); // folds that sample into base_
            } else {
                // the loopEnd frame is reachable and IS the authored loop
                // seam sample
                slot.t = double(tl->loopEnd);
                compose(); // folds that sample into base_
            }
        } else {
            fold_end_pose(tl); // authored end pose straight into base_
        }
    }
}

void EmotePlayer::step() {
    if (ctrl_fix()) {
        // Step() settles the fade-in blend, not the timeline itself.
        compose();
        render_now();
        return;
    }
    // (api_skip.html): Step() skips to the stage where the
    // transitions have settled. With the persistent domain the foreground's
    // END pose is folded into the domain (fold_foreground_end — the
    // natural-end pose), then the list ends internally and
    // the domain keeps that final state. (The earlier fold sampled the
    // timeline at lastTime, falling back to t = 0 since every NekoMiko
    // one-shot has lastTime = -1: a stepped expression reverted to its START
    // pose — arm_type snapped back to the t0 arm group A — instead of the
    // authored tail pose a natural end leaves in the domain.)
    if (fg_list_.empty()) return;
    fold_foreground_end();
    fg_list_.clear();
    compose();
    render_now();
}

void EmotePlayer::skip() {
    // exskip: land every slot at its end state (api_skip.html:
    // Skip() advances everything instantly to the final state). The
    // foreground's authored END pose is folded into the persistent domain
    // (fold_foreground_end — same natural-end pose as step()),
    // then the list ends; the idle loop is a delta layer and simply stops
    // (variables keep their domain values).
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
    if (std::getenv("OA_DEBUG_PASS"))
        std::fprintf(stderr, "[pass] after rollback eye=%.1f brow=%.1f\n",
                     vars_["face_eye_open"], vars_["face_eyebrow"]);
    render_now();
}

void EmotePlayer::set_scale(double s, double origin_x, double origin_y) {
    if (s > 0.01 && s <= 4.0) view_scale_ = s;
    scale_origin_x_ = origin_x;
    scale_origin_y_ = origin_y;
    if (std::getenv("OA_EMOTE_DEBUG"))
        std::fprintf(stderr, "[emote] set_scale s=%.3f origin=(%.1f,%.1f) "
                             "req=%dx%d buf=%dx%d\n",
                     view_scale_, scale_origin_x_, scale_origin_y_, req_width_,
                     req_height_, width_, height_);
    render_now();
}

void EmotePlayer::set_coord(double x, double y) {
    view_x_ = x;
    view_y_ = y;
    if (std::getenv("OA_EMOTE_DEBUG"))
        std::fprintf(stderr, "[emote] set_coord (%.1f,%.1f)\n", view_x_, view_y_);
    render_now();
}

void EmotePlayer::set_variable(const std::string& name, double value) {
    if (name.empty()) return;
    explicit_[name] = value;
    compose();
    schedule_render(false);
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
        // composer-side layer only, and the framework's own re-adjust logic
        // (emote.lua vsync: `var = getVariable("face_cheek"); if var ~= param -
        // diff then setVariableDiff(param - var)`) reads back the underlying
        // timeline/domain value here. Subtracting recovers it exactly; the
        // lookup is skipped for every variable with no offset.
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
    // plays. With the foreground list this must not map to pass() — that would
    // end unrelated parallel entries too (the single-slot fg match maps to
    // pass; the per-entry erase keeps that meaning for a
    // one-entry list). Stopping an entry only stops its writes: the domain
    // keeps the pose (persistent-variable model).
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
