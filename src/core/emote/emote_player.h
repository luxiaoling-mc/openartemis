#pragma once
// E-mote playback state machine.
// One EmotePlayer per createEmoteLayer layer: holds the parsed PSB, the
// current engine variable values (defaults 0 + selector option-0 init;
// metadata.instantVariableList is name-only authoring data — no runtime
// effect in the reference: krkr ignores it, the other stores an unused
// flag), one idle slot (idle loop = 通常待機-style) plus an ORDERED
// foreground play list (the reference runner's
// currTimeline list — a timeline played with TIMELINE_PLAY_PARALLEL
// appends, a plain playTimeline replaces the whole list) and the rendered
// RGBA canvas.
//
// Variable-hub model (v4 semantics):
// timelines are time-driven WRITERS of the shared variable domain; part /
// mesh / icon state is a pure function of the composed variable snapshot
// (variable value -> parameter-axis tick via trans_to_tick -> node frames).
// The pose evaluator receives no clock at all (base-motion tree tick is the
// constant 0; only parameterized axes move with the variables), so the
// structural invariant holds by construction: same variable snapshot =>
// same pose regardless of slot clock positions (regression "varhub
// invariant"). Data audit: no non-parameterized node in the real files
// carries time-keyed content, so the constant tree tick is complete.
//
// Semantics rulings (official SDK manual):
//  * persistent variable domain: base_ holds the absolute
//    variable values (init = defaults incl. selector option-0). The
//    foreground (absolute) timeline writes its samples INTO the domain while
//    active; a timeline that ends — natural end, pass(), step(), skip(),
//    stop() — simply STOPS WRITING and the domain keeps its last values
//    (expressions persist between lines; no snap back to neutral; matches
//    the manual's variable model and the krkr domain). The idle loop
//    (通常待機, diff=1) is a per-frame DELTA layer ADDED on top of the
//    domain while active and never folded into base_ (breathing/sway keeps
//    running under any expression; no accumulation).
//  * per-frame compose: base_ -> foreground list samples in list order
//    (absolute, into base_; a later entry overwrites an earlier one for a
//    shared variable — the reference's insert-order overwrite) -> idle slot
//    samples ADD -> explicit setVariable (frame + domain) -> selector
//    controls derive the option-label variables (fade_a..e) from the
//    selector value (selector apply semantics).
//  * frame clock: timeline var-track times are authoring frames at 60 fps;
//    the engine advances slots by delta_ms * 60 / 1000. A track whose
//    current frame is the content-less type-0 tail does not write that
//    frame (the variable keeps its domain value).
//  * wrap/settle rule (reference E:emoterunner.cpp:
//    1411-1417): a timeline wraps iff its authored loopEnd > 0 — the loop
//    period is loopEnd - loopBegin and the clock lives in
//    [loopBegin, loopEnd] with the loopEnd frame itself reachable (the
//    reference's reset lands on loopBegin when currRelTime > loopEnd). This
//    is DECOUPLED from diff: a diff==1 timeline with loopBegin/loopEnd<=0 is
//    a ONE-SHOT (tg3/slny mark their gesture/action timelines that way) and
//    plays through to its end instead of being frozen at t==0 by the old
//    `diff==1 => loop` approximation; a timeline that ends keeps its last
//    authored sample in the domain and never returns to t0. Start phase =
//    loopBegin (0 when the sentinel is negative). One-shots still deactivate
//    at their authored end (our safety rule, kept: the reference never
//    removes them, but the domain is identical).
//    Env switches: OA_EMOTE_WRAP=diff restores the old diff-based wrap,
//    OA_EMOTE_PARALLEL=0 restores single-slot replacement,
//    OA_EMOTE_LEGACY_SLOTS=1 restores both (same-process REF/NEW A-B).
//  * pose re-render is throttled (OA_EMOTE_FPS override, default ~4 fps)
//    with immediate renders on play/setVariable/force changes; large
//    canvases render at full resolution by default (the
//    half-size canvas blurred the stretched figure); OA_EMOTE_HALFRES=1
//    restores the half-size fallback for CPU-constrained hosts.
//  * playTimeline(label, flags): start the foreground list — flags &
//    TIMELINE_PLAY_PARALLEL (1) appends to the list, otherwise the list is
//    cleared first (the reference startTimeline list, one entry per call);
//    fadeInTimeline(label, fade, wait): (re)start the idle slot (fade ticks
//    are accepted but no crossfade is applied; game calls use 0,0);
//    pass(): every foreground entry ends internally (timeline no longer
//    playing); the domain keeps the pose — the display does not jump
//    (official SDK Pass semantics, api_skip.html);
//    step(): the foreground's authored END pose is folded into the domain
//    then the list ends (settled end state, api_skip.html). The fold lands
//    every track at its last content sample — the same values a natural
//    playback leaves in the domain (sampling at lastTime, which
//    is -1 on every NekoMiko one-shot and fell back to t=0, reverted a
//    stepped/skipped expression to its START pose);
//    skip(): both slots end at their end states (fg end pose into the
//    domain, idle delta layer dropped);
//    stop(): both slots off; the domain keeps the current pose.
//    stopTimeline(label): ends only the named entry (fg or idle).
//  * setVariable(name, v): writes the domain and the composed frame (the
//    value persists until a timeline/explicit writer changes it).
// Pose-rotation axes (head_slant/body_slant — the only parameter axes whose
// parameterized nodes carry angle keyframes in the real files) are ordinary
// variables: the idle diff loop (通常待機) drives them per its authored
// curves exactly like every other axis. Ruling correction
// (user, 2026-09): an earlier adjudication froze these axes under
// idle because the standing pose appeared to sweep sideways every 5 s loop —
// that observation was made while the slant geometry was still broken
// (stair-step full-extent holds + wrong pivot) and
// misattributed the authored idle excursion to the engine. The idle layer
// writes ONLY the delta (never folds into the domain), so expressions still
// own the absolute pose and return it to 0 at their tail; parameterized
// nodes remain pure functions of the composed variable snapshot.
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/emote/emote_file.h"

namespace oa::emote {

class EmotePlayer {
public:
    EmotePlayer();
    ~EmotePlayer();

    EmotePlayer(const EmotePlayer&) = delete;
    EmotePlayer& operator=(const EmotePlayer&) = delete;

    /// Parse the PSB and render the initial static pose at the canvas size.
    /// Canvas > 4096 is clamped. Canvases render at full resolution by
    /// default; OA_EMOTE_HALFRES=1 restores the half-size
    /// fallback (OA_EMOTE_FULLRES remains an explicit 1:1 request).
    /// width()/height() report the actual render buffer size.
    bool load(const uint8_t* data, size_t size, int canvasW, int canvasH,
              std::string* err = nullptr);
    bool load(const std::vector<uint8_t>& data, int canvasW, int canvasH,
              std::string* err = nullptr);

    const EmoteFile& file() const { return file_; }

    /// Advance the timeline clocks by real time and re-render when the pose
    /// changed enough (throttled).
    void advance_ms(uint64_t delta_ms);
    /// Host-driven clock: advance both timeline slots by an
    /// authored-frame count at 60 fps — the E-mote SDK Progress() contract
    /// (the game Lua layer calls em:progress(elapsed_ms*60/1000) once per
    /// vsync on frameworks that drive the player themselves, e.g. 甜蜜女友3
    /// e-mote.lua ex.progress/emote.vsync). Same end/wrap semantics as
    /// advance_ms; fractional frames accumulate on the slot clocks.
    void progress(double frames);
    /// Force an immediate re-render (expression switch, explicit pose ops).
    void render_now();

    // -- GPU compositing mode -----------------------------------------------
    /// When enabled the player stops rasterising: render() only snapshots
    /// the composed variables and bumps the revision; the host then asks
    /// for the pose geometry (collect_pose_parts) and fills the canvas on
    /// the GPU (SDL_RenderGeometry). Headless use keeps the CPU raster.
    void set_external_pose(bool on) { external_pose_ = on; }
    /// Emit the current pose as textured triangles (no pixel work) in the
    /// canvas mapping the CPU raster would use.
    bool collect_pose_parts(std::vector<EmoteDrawPart>* parts,
                            std::string* err = nullptr) const;

    // -- view: surface mapping semantics ------------------------------------
    /// setScale(scale, origin_x, origin_y) / setCoord(x, y) per the
    /// E-Mote layer API (scale zoom about its origin in model px; coord is
    /// a surface-px offset added after centering — surface mapping semantics).
    void set_scale(double s, double origin_x = 0.0, double origin_y = 0.0);
    void set_coord(double x, double y);
    double view_scale() const { return view_scale_; }

    // -- E-mote layer API ---------------------------------------------------
    /// Play a foreground timeline. flags & TIMELINE_PLAY_PARALLEL (1)
    /// appends to the foreground list (parallel tracks compose in list order,
    /// later entries overwrite earlier ones); without it the list is cleared
    /// first (the new timeline replaces every foreground entry).
    void play_timeline(const std::string& label, int flags = 0); // foreground list
    void fade_in_timeline(const std::string& label);   // idle slot
    void pass();  // foreground list off
    void step();  // foreground evaluated at end, then off
    void skip();  // both slots at their end/rest pose
    void stop();  // both slots off at the current pose (no idle loop)
    /// Stop only the idle (diff/待機) slot — the named idle timeline ended
    /// (stopTimeline/fadeOutTimeline on an idle match): the
    /// delta layer stops applying; the persistent domain and the foreground
    /// slot are untouched.
    void stop_idle_timeline();
    /// stopTimeline(label): end the named timeline wherever it
    /// plays — every matching foreground-list entry stops writing (the
    /// domain keeps its pose, like pass()) and/or the idle slot ends. Returns
    /// true when something matched. With a foreground LIST this must not be
    /// pass(): that would end unrelated parallel tracks too.
    bool stop_timeline(const std::string& label);
    /// isTimelinePlaying(label): the named timeline plays in any slot.
    bool is_timeline_playing(const std::string& label) const;
    void set_variable(const std::string& name, double value);
    double get_variable(const std::string& name) const;
    /// SetVariableDiff(srcLabel, dstLabel, value) — the 差分 (difference) write
    /// the KukkoroDays framework uses to keep a script-driven face parameter
    /// (cheek/tears) pinned to its [fg] tag value while the expression
    /// timelines keep running (emote.lua playTimeline/vsync: the game measures
    /// the observed variable, computes `param - observed` and applies it here).
    /// The offset is a COMPOSER-SIDE layer only (it is added on top of
    /// base_/foreground/idle/explicit in compose()): it is never folded into
    /// the persistent domain, and get_variable() reports the value WITHOUT it,
    /// so the game's own re-adjust arithmetic — which re-reads the variable
    /// every frame and re-applies the delta whenever the timeline moves the
    /// variable underneath — converges instead of accumulating.
    /// value == 0 clears the offset (emote.lua's "差分変数をリセット").
    /// The srcLabel argument carries no engine-visible semantics on the paths
    /// the game uses (only dstLabel + value are consumed); see research/127.
    void set_variable_diff(const std::string& name, double value);
    /// countVariables(): size of the file's variable domain
    /// (metadata.variableList order — EmoteFile::variableNames).
    int variable_count() const;
    /// getVariableLabelAt(index): the label at that domain index. Out-of-range
    /// yields "" instead of nil: the framework's enumeration is the inclusive
    /// `for i = 0, layer:countVariables() do` (emote.lua:128/144), so the last
    /// probe is always one past the domain and must stay a usable table key.
    std::string variable_label_at(int index) const;
    /// Currently playing foreground timeline ("" when none). With a parallel
    /// list this is the most recently started entry (list top).
    std::string foreground_timeline() const;
    /// Current idle (fade) timeline ("" when none).
    std::string idle_timeline() const;
    /// Composed variable snapshot (diagnostics/tests).
    std::map<std::string, double> variables() const;
    /// One playing foreground entry (diagnostics/tests): label, authored-frame
    /// clock, play flags. List order = compose order (later overwrites).
    struct ForegroundSlot {
        std::string label;
        double t = 0; // frames (60 fps units)
        int flags = 0;
    };
    std::vector<ForegroundSlot> foreground_slots() const;
    /// Idle slot clock in authored frames (-1 when no idle timeline plays).
    double idle_time() const;

    /// Counters for the two write-surface filters
    /// — how many track writes were skipped as selector option labels and how
    /// many were discarded as unknown labels. Diagnostics/tests only (they
    /// double as the lazy-evidence check on real data).
    struct S2Stats {
        size_t selectorTracksSkipped = 0;
        size_t unknownLabelWrites = 0;
    };
    S2Stats s2_stats() const { return stats_; }
    /// The label universe a timeline track may write (variableList + motion
    /// parameter ids + selector option labels; diagnostics/tests).
    const std::set<std::string>& known_labels() const { return known_labels_; }

    // -- rendered frame -----------------------------------------------------
    uint64_t revision() const { return revision_; }
    int width() const { return width_; }
    int height() const { return height_; }
    const std::vector<uint8_t>& rgba() const { return rgba_; }

private:
    struct Slot {
        std::string label;
        double t = 0; // frames (60 fps units)
        bool active = false;
        int flags = 0; // playTimeline flags (PARALLEL bit)
    };

    const EmoteTimeline* find_timeline(const std::string& label) const;
    void init_defaults();
    /// Compose base_+foreground list+idle+explicit+selectors into vars_.
    void compose();
    /// Evaluate one track at frame time t; writes through when active.
    /// Returns false when the track currently does not write (empty/terminal).
    bool track_value(const TimeVar& track, double t, double* out) const;
    /// whether a timeline track may write `label`
    /// (selector option labels are skipped, unknown labels discarded; both
    /// counted in stats_). OA_EMOTE_S2_LEGACY=1 admits every label.
    bool track_label_writable(const std::string& label) const;
    /// Fold a one-shot timeline's authored END pose into the persistent
    /// domain: every track's last content sample (natural-end pose).
    void fold_end_pose(const EmoteTimeline* tl);
    /// Step the idle slot and every foreground entry by `frames` authored
    /// frames (shared by advance_ms/progress) and drop the entries that ended.
    void step_slots(double frames);
    /// Apply the shared slot end/wrap rules after a clock step: a timeline
    /// with an authored loopEnd>0 wraps in [loopBegin, loopEnd]; everything
    /// else deactivates at its authored end (see the
    /// header; the legacy diff-based rule is the OA_EMOTE_WRAP=diff A-B arm).
    void settle_slot(Slot& slot, const EmoteTimeline* tl);
    /// Whether this timeline wraps (new rule: loopEnd>0; legacy: diff==1).
    bool timeline_loops(const EmoteTimeline* tl) const;
    /// Start phase of a freshly played timeline (loopBegin when authored).
    double start_phase(const EmoteTimeline* tl) const;
    /// Fold the end pose of every foreground entry into base_, then clear.
    void fold_foreground_end();
    void schedule_render(bool force);
    void render();
    StaticRenderOptions view_options() const;

    EmoteFile file_;
    int width_ = 0, height_ = 0;
    int req_width_ = 0, req_height_ = 0; // requested surface size at load
    int kFps = 60;
    // pose cadence — krkr's runner evaluates and draws
    // the whole emote every display frame; our CPU raster forced a ~8 fps
    // throttle that aliased authored fast segments (expression entrance
    // spikes etc.) into single-frame pose jumps. With GPU compositing
    // (external-pose mode) evaluation is cheap, so the cadence defaults to
    // 30 fps there (OA_EMOTE_FPS override, up to 60); the CPU path keeps
    // its 125 ms throttle.
    int pose_fps_ = 30;

    // play-model switches (read once, like kFps; they are
    // A-B arm selectors, not per-frame knobs):
    //  * legacy_slots_ (OA_EMOTE_LEGACY_SLOTS=1) = the legacy model: single
    //    foreground slot + the diff==1-based wrap. Same-process REF arm.
    //  * wrap_legacy_ (OA_EMOTE_WRAP=diff) = old wrap rule only.
    //  * parallel_off_ (OA_EMOTE_PARALLEL=0) = a playTimeline always replaces
    //    the foreground list (single-slot behaviour).
    bool legacy_slots_ = false;
    bool wrap_legacy_ = false;
    bool parallel_off_ = false;
    bool wrap_is_legacy() const { return legacy_slots_ || wrap_legacy_; }
    bool parallel_enabled() const { return !legacy_slots_ && !parallel_off_; }
    static constexpr size_t kMaxForegroundSlots = 8; // parallel-list cap

    // write-surface switches, latched at
    // construction like the play-model ones:
    //  * s2_legacy_ (OA_EMOTE_S2_LEGACY=1) = the legacy surface: the
    //    `f.type==2 && g.type==2` interpolation predicate and unfiltered
    //    track writes. Same-process REF arm.
    bool s2_legacy_ = false;
    // Label domain for the write filter (built once in init_defaults).
    std::set<std::string> known_labels_;
    std::set<std::string> selector_option_labels_;
    mutable S2Stats stats_; // track_label_writable is const (read path)

    Slot idle_;
    // Ordered foreground play list (reference currTimeline): compose order =
    // list order, a later entry overwrites an earlier one for a shared
    // variable. Each entry keeps its own clock (the reference's shared
    // currStartTick is a defect and is NOT copied).
    std::vector<Slot> fg_list_;
    // every known variable at 0 (defaults incl. motion parameter ids)
    std::map<std::string, double> defaults_;
    // explicit setVariable writes (write the frame and the domain)
    std::map<std::string, double> explicit_;
    // SetVariableDiff offsets: a composer-side additive layer, never folded
    // into the domain and subtracted again by get_variable (see
    // set_variable_diff). Empty for every framework that never calls it.
    std::map<std::string, double> diff_;
    // persistent absolute variable domain (init = defaults_;
    // the foreground writes samples into it; stopping timelines leave it
    // untouched). The idle diff loop is a per-frame delta on top and never
    // stored here.
    std::map<std::string, double> base_;
    // composed per-frame values fed to the pose evaluation
    std::map<std::string, double> vars_;
    std::map<std::string, double> rendered_vars_; // snapshot at last render

    uint64_t revision_ = 1;
    uint64_t last_advance_ms_ = 0;
    uint64_t last_render_ms_ = 0;
    uint64_t render_interval_ms_ = 125; // throttle; OA_EMOTE_FPS override
    bool dirty_ = true;
    bool external_pose_ = false;

    double view_scale_ = 0.6; // game standard "no"; setScale overrides
    double view_x_ = 0, view_y_ = 0;
    double scale_origin_x_ = 0, scale_origin_y_ = 0;

    std::vector<uint8_t> rgba_;
};

} // namespace oa::emote
