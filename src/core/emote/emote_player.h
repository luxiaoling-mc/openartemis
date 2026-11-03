#pragma once
// E-mote playback state machine. Full semantics: docs/EMOTE.md.
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/emote/emote_file.h"
#include "core/emote/emote_physics.h"

namespace oa::emote {

/// Default external-pose latch for new players (set once at startup).
void emote_set_default_external_pose(bool on);
bool emote_default_external_pose();

class EmotePlayer;

/// Runtime-side state of one emote layer; render uploads frames by revision.
struct LayerState {
    int width = 0, height = 0;
    uint64_t revision = 0; // last composed render revision
    std::string file;      // first psb path
    bool host_clock = false; // game drives the player clock itself
    std::shared_ptr<EmotePlayer> player;
};

class EmotePlayer {
public:
    EmotePlayer();
    ~EmotePlayer();

    EmotePlayer(const EmotePlayer&) = delete;
    EmotePlayer& operator=(const EmotePlayer&) = delete;

    /// Parse the PSB and render the initial pose (canvas clamped to 4096).
    /// width()/height() report the actual render buffer size.
    bool load(const uint8_t* data, size_t size, int canvasW, int canvasH,
              std::string* err = nullptr);
    bool load(const std::vector<uint8_t>& data, int canvasW, int canvasH,
              std::string* err = nullptr);

    const EmoteFile& file() const { return file_; }

    /// Advance clocks by real time; re-render throttled pose changes.
    void advance_ms(uint64_t delta_ms);
    /// Host-driven clock (SDK Progress contract); same end/wrap rules.
    void progress(double frames);
    /// Force an immediate re-render (expression switch, explicit pose ops).
    void render_now();

    // -- GPU compositing mode -----------------------------------------------
    /// Stop CPU rasterisation; render() snapshots variables and bumps revision.
    void set_external_pose(bool on) { external_pose_ = on; }
    /// 帧推送（渲染侧注入）：CPU 帧 = 整幅 rgba；GPU 模式 = 姿态部件，
    /// 由注入方合成到画布。player 每次产出新帧时自己调用。
    using FramePush = std::function<bool(int w, int h, const uint8_t* rgba)>;
    using PosePush = std::function<bool(const EmoteFile&, const std::vector<EmoteDrawPart>&,
                                        int w, int h)>;
    void set_frame_push(FramePush cpu, PosePush pose) {
        cpu_push_ = std::move(cpu);
        pose_push_ = std::move(pose);
    }

    /// Emit the current pose as textured triangles (no pixel work) in the
    /// canvas mapping the CPU raster would use.
    bool collect_pose_parts(std::vector<EmoteDrawPart>* parts,
                            std::string* err = nullptr) const;

    // -- view: surface mapping semantics ------------------------------------
    /// E-mote setScale/setCoord view mapping.
    void set_scale(double s, double origin_x = 0.0, double origin_y = 0.0);
    void set_coord(double x, double y);
    double view_scale() const { return view_scale_; }
    /// Game-configured view; restorable across player rebuilds.
    struct ViewState {
        double scale = 0.6;
        double origin_x = 0, origin_y = 0;
        double x = 0, y = 0;
    };
    ViewState view_state() const {
        return ViewState{view_scale_, scale_origin_x_, scale_origin_y_, view_x_,
                         view_y_};
    }
    void set_view_state(const ViewState& v) {
        view_scale_ = v.scale;
        scale_origin_x_ = v.origin_x;
        scale_origin_y_ = v.origin_y;
        view_x_ = v.x;
        view_y_ = v.y;
        render_now();
    }
    /// Inherit the view on a same-id rebuild (avoids the 0.6 default reset).
    void copy_view_from(const EmotePlayer& other) {
        view_scale_ = other.view_scale_;
        view_x_ = other.view_x_;
        view_y_ = other.view_y_;
        scale_origin_x_ = other.scale_origin_x_;
        scale_origin_y_ = other.scale_origin_y_;
    }

    // -- E-mote layer API ---------------------------------------------------
    /// Play foreground; flag 1 appends, otherwise the list is replaced.
    void play_timeline(const std::string& label, int flags = 0); // foreground list
    void fade_in_timeline(const std::string& label);   // idle slot
    void pass();  // foreground list off
    void step();  // foreground evaluated at end, then off
    void skip();  // both slots at their end/rest pose
    void stop();  // both slots off at the current pose (no idle loop)
    /// End the named timeline in any slot; true when something matched.
    bool stop_timeline(const std::string& label);
    /// isTimelinePlaying(label): the named timeline plays in any slot.
    bool is_timeline_playing(const std::string& label) const;
    void set_variable(const std::string& name, double value);
    double get_variable(const std::string& name) const;
    /// Composer-side variable offset (setVariableDiff); value 0 clears it.
    /// Never folded into the domain; get_variable() excludes the offset.
    void set_variable_diff(const std::string& name, double value);
    /// countVariables(): size of the file's variable domain
    /// (metadata.variableList order — EmoteFile::variableNames).
    int variable_count() const;
    /// Variable label at index; out-of-range yields "" (usable table key).
    std::string variable_label_at(int index) const;
    /// Current foreground timeline ("" when none; list top if parallel).
    std::string foreground_timeline() const;
    /// Current idle (fade) timeline ("" when none).
    std::string idle_timeline() const;
    /// Composed variable snapshot (diagnostics/tests).
    std::map<std::string, double> variables() const;
    /// One foreground entry (diagnostics/tests); list order = compose order.
    struct ForegroundSlot {
        std::string label;
        double t = 0; // frames (60 fps units)
        int flags = 0;
    };
    std::vector<ForegroundSlot> foreground_slots() const;
    /// Idle slot clock in authored frames (-1 when no idle timeline plays).
    double idle_time() const;

    /// Label universe a timeline track may write (diagnostics/tests).
    const std::set<std::string>& known_labels() const { return known_labels_; }

    // -- rendered frame -----------------------------------------------------
    uint64_t revision() const { return revision_; }
    int width() const { return width_; }
    int height() const { return height_; }
    const std::vector<uint8_t>& rgba() const { return rgba_; }

private:
    /// Carry playback state across a same-PSB rebuild (expression persists).
    void copy_state_from(const EmotePlayer& other);
    /// True when this player carries a non-neutral domain (diagnostics).
    bool has_playback_state() const;
    /// Stop only the idle delta slot; domain/foreground untouched.
    void stop_idle_timeline();

    struct Slot {
        std::string label;
        double t = 0; // frames (60 fps units)
        bool active = false;
        int flags = 0; // playTimeline flags (PARALLEL bit)
        /// Pre-play snapshot; Pass() restores non-authored values from here.
        std::map<std::string, double> base_before;
        double start_t = 0;
    };

    const EmoteTimeline* find_timeline(const std::string& label) const;
    void init_defaults();
    /// Compose base_+foreground list+idle+explicit+selectors into vars_.
    void compose();
    /// Evaluate one track at frame time t; writes through when active.
    /// Returns false when the track currently does not write (empty/terminal).
    bool track_value(const TimeVar& track, double t, double* out) const;
    /// whether a timeline track may write `label`
    /// (selector option labels are skipped, unknown labels discarded).
    bool track_label_writable(const std::string& label) const;
    /// Fold a one-shot timeline's authored END pose into the persistent
    /// domain: every track's last content sample (natural-end pose).
    void fold_end_pose(const EmoteTimeline* tl);
    /// Step the idle slot and every foreground entry by `frames` authored
    /// frames (shared by advance_ms/progress) and drop the entries that ended.
    void step_slots(double frames);
    /// Apply the shared end/wrap rule after a clock step.
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
    int pose_fps_ = 30;

    static constexpr size_t kMaxForegroundSlots = 8; // parallel-list cap

    // Label domain for the write filter (built once in init_defaults).
    std::set<std::string> known_labels_;
    std::set<std::string> selector_option_labels_;

    Slot idle_;
    // Ordered foreground list; each entry keeps its own clock.
    std::vector<Slot> fg_list_;
    /// Two-phase Pass: entries end now, end samples land on the next step.
    struct PendingPass {
        std::string label;
        double t = 0;
    };
    std::vector<PendingPass> pending_pass_;
    // every known variable at 0 (defaults incl. motion parameter ids)
    std::map<std::string, double> defaults_;
    // explicit setVariable writes (write the frame and the domain)
    std::map<std::string, double> explicit_;
    // Composer-side SetVariableDiff offsets (never folded into base_).
    std::map<std::string, double> diff_;
    // Persistent absolute domain; idle only adds a per-frame delta.
    std::map<std::string, double> base_;
    // composed per-frame values fed to the pose evaluation
    std::map<std::string, double> vars_;
    // PSB physics variables, layered over the timeline domain.
    EmotePhysics physics_;
    std::map<std::string, double> physics_vars_;
    void tick_physics(double frames);
    std::map<std::string, double> rendered_vars_; // snapshot at last render

    void push_frame_now();

    uint64_t revision_ = 1;
    uint64_t last_advance_ms_ = 0;
    uint64_t last_render_ms_ = 0;
    uint64_t render_interval_ms_ = 125; // throttle; OA_EMOTE_FPS override
    bool dirty_ = true;
    bool external_pose_ = false;
    FramePush cpu_push_;
    PosePush pose_push_;

    // --- eyeControl (目パチ): authored automatic blink ----------------------
    std::string blink_label_;   // eye-open variable ("" = no eyeControl)
    double blink_begin_ = 0, blink_end_ = 0;
    double blink_frames_ = 0;   // authored length of one blink
    double blink_wait_ = 0;     // frames until the next blink starts
    double blink_t_ = -1;       // frames into the current blink (-1 = idle)
    double blink_value_ = 0;    // lid amount this frame (0 = open)
    uint32_t blink_rng_ = 0x9E3779B9u; // xorshift32 state
    void blink_advance(double frames);
    double blink_rand01() {
        blink_rng_ ^= blink_rng_ << 13;
        blink_rng_ ^= blink_rng_ >> 17;
        blink_rng_ ^= blink_rng_ << 5;
        return double(blink_rng_ & 0xFFFFFFu) / double(0x1000000u);
    }
    double blink_interval_min_ = 30, blink_interval_max_ = 180;

    double view_scale_ = 0.6; // game standard "no"; setScale overrides
    double view_x_ = 0, view_y_ = 0;
    double scale_origin_x_ = 0, scale_origin_y_ = 0;

    std::vector<uint8_t> rgba_;
};

} // namespace oa::emote
