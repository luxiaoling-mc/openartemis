#pragma once
// render_internal.h —— render 的**内部实现面**。
//
// 分层（每层只 include 上一层）：
//   renderer.h / layer.h    对外契约：宿主渲染接口 + 场景层树。调用方只 include 它们。
//   render_internal.h       实现面：anim（缓动 + [lytween] 时间线 + [anime] 帧
//                           动画）与 transition（[trans] 语义机）的全部类型。
//                           只有 render/runtime 的 .cpp 与专门测这两个语义机的
//                           测试（tests/transition_test.cpp）允许 include。
//   layer.cpp               §8 = anim/tween/[anime]/[trans] 的语义核实现
//                           （与其状态所有者 Compositor 同 TU；见 §8 banner）。
//
// 这两个模块的类型只有实现面需要**定义**，外部调用方从不依赖：
//   - [trans] 的观察面收为 oa::runtime::TransitionStatus 只读投影（runtime.h），
//     写入口（start/clear/mark_captured/skip_by_input）留在 runtime 实现面；
//   - Compositor 的 tween/anime 方法只有 std::vector<TweenDone> 返回值需要这个
//     名字，layer.h 前向声明即可（定义只在 render/runtime 的 .cpp 里用）。
// 因此本头接管两份声明面，公开头（renderer.h / layer.h / runtime.h）不再
// include anim/transition。
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::render {


/// 30 standard Artemis easings (10 families x in/out/inout) + Linear.
/// Formulas are the canonical Robert Penner equations (public domain),
/// identical to the framework's easing table.
enum class Easing {
    Linear,
    EaseInQuad, EaseOutQuad, EaseInOutQuad,
    EaseInCubic, EaseOutCubic, EaseInOutCubic,
    EaseInQuart, EaseOutQuart, EaseInOutQuart,
    EaseInQuint, EaseOutQuint, EaseInOutQuint,
    EaseInExpo, EaseOutExpo, EaseInOutExpo,
    EaseInCirc, EaseOutCirc, EaseInOutCirc,
    EaseInSine, EaseOutSine, EaseInOutSine,
    EaseInBack, EaseOutBack, EaseInOutBack,
    EaseInElastic, EaseOutElastic, EaseInOutElastic,
    EaseInBounce, EaseOutBounce, EaseInOutBounce,
};

/// Parse an Artemis ease name (all 30 + no-underscore spellings + the legacy
/// aliases in/out/inout -> Quad). Unknown names map to Linear.
Easing parse_easing(std::string_view name);

/// Apply the easing to a linear progress t (clamped to 0..1 first,
/// then eased).
double ease_value(Easing e, double t);

/// One numeric property tween. Times are milliseconds; `start_ms` already
/// includes the delay offset.
struct Tween {
    std::string param;          // "alpha" | "left" | "top" | "xscale" | ...
    double from = 0.0;
    double to = 0.0;
    Easing easing = Easing::Linear;
    uint64_t start_ms = 0;      // includes delay
    uint64_t duration_ms = 0;   // 0 = snap to `to`
    bool infinite_loop = false; // loop="-1"
    std::optional<uint32_t> loop_count; // loop=N: N cycles then settle
    bool yoyo = false;          // yoyo != 0
    bool yoyo_reverse = false;  // per-cycle back-swing flag
    uint64_t loop_delay_ms = 0; // delay between cycles
    bool delete_on_finish = false; // delete="1": remove the layer when done
    std::string handler;        // handler= (e.g. "calllua"); empty = none
    std::string handler_file;   // file= / label= dispatch targets
    std::string handler_label;
    std::map<std::string, std::string> extra; // unknown [lytween] params
    std::optional<uint64_t> set_id; // [tweenset] group number

    /// Value at `now`. Before start -> from; after (all loops) -> to;
    /// duration==0 -> to.
    double get_value(uint64_t now) const;
    /// True once the tween (incl. all loops/delays) is completely done.
    /// Infinite loops never finish.
    bool is_finished(uint64_t now) const;
    bool is_looping() const;
    /// True when a looping yoyo tween is currently in its back-swing cycle.
    /// NOTE: nothing writes this back into get_value
    /// during frame builds (no caller toggles yoyo_reverse), so observable
    /// yoyo == plain looping; we mirror that faithfully.
    bool is_yoyo_reverse(uint64_t now) const;

private:
    uint64_t effective_time(uint64_t now) const;
};

/// A tween that finished naturally this tick, with a completion handler.
/// `delete_layer` is applied by the compositor before this is returned; the
/// host uses handler/extra to run [calllua]-style callbacks through the
/// runtime's tween-handler dispatch.
struct TweenDone {
    std::string id;
    std::string handler;
    std::string handler_file;
    std::string handler_label;
    std::map<std::string, std::string> extra;
};

/// Format a tween value back to an Artemis property string (the same
/// LayerProps::format_value whitelist): integer properties
/// (alpha/visible/...) round to an integer string so the u8 parser path is
/// used; everything else is the shortest numeric representation.
std::string format_tween_param(std::string_view param, double value);

// ---------------------------------------------------------------------------
// [anime] frame animation (compositor-side; frame swap/merge semantics)
// ---------------------------------------------------------------------------

/// One [anime] frame: when the playback time reaches `time_ms` the layer's
/// file/mask are swapped and `props` merged.
struct AnimeFrame {
    uint64_t time_ms = 0;
    std::string file; // layer.file while this frame is current
    std::string mask; // layer.mask ("" = cleared while this frame current)
    std::map<std::string, std::string> props;
};

/// Playback state of one [anime] layer.
/// `total_duration_ms == 0` means end never ran — playback stays dormant.
struct AnimeState {
    std::vector<AnimeFrame> frames; // time-ms sorted at mode=end
    int32_t loop_count = -1;        // -1 infinite, 0 play once, N play N rounds
    uint64_t start_ms = 0;          // set at mode=end (current clock)
    uint64_t total_duration_ms = 0; // set at mode=end (time param)
};



class Transition {
public:
    struct State {
        int type = 1;
        uint64_t start_ms = 0;
        uint64_t duration_ms = 1000;
        bool captured = false;     // capture texture uploaded
        bool needs_capture = true; // awaiting the host snapshot
        int input = 1;             // 0 deny / 1 allow / 2 skip-mode-only
        std::string rule;          // type=2 rule image path (host resolves it)
        int vague = 32;            // 0-255 rule-dissolve edge softness
    };

    /// [trans] type 0 clears any active transition; type != 0 begins one.
    void start(int trans_type, std::optional<uint64_t> time, std::string rule,
               std::optional<int> vague, int input, uint64_t clock_ms);
    /// [flip] / new type-0 request: drop the active transition.
    void clear();

    bool active() const { return state_.has_value(); }
    bool needs_capture() const {
        return state_ && state_->needs_capture;
    }
    bool is_captured() const { return state_ && state_->captured; }

    /// The host captured the previous frame into a texture: the transition
    /// clock now starts (start_ms = the capture clock).
    void mark_captured(uint64_t clock_ms);

    /// Auto-clear when captured and elapsed >= duration.
    void clear_finished(uint64_t clock_ms);

    /// Whether the transition visually occupies the frame right now.
    bool is_in_progress(uint64_t clock_ms) const {
        if (!state_) return false;
        if (state_->needs_capture) return true;
        return clock_ms - state_->start_ms < state_->duration_ms;
    }

    /// User input asked to skip (input policy 0/1/2). Returns true when the
    /// transition was actually cleared.
    bool skip_by_input(bool in_skip_mode);

    /// 0..1 overlay progress for the host (elapsed/duration clamped).
    double progress(uint64_t clock_ms) const;

    int type() const { return state_ ? state_->type : 0; }
    int input() const { return state_ ? state_->input : 1; }
    const std::string& rule() const { return state_ ? state_->rule : kEmpty; }
    int vague() const { return state_ ? state_->vague : 32; }

private:
    static inline const std::string kEmpty;
    std::optional<State> state_;
};

} // namespace oa::render
