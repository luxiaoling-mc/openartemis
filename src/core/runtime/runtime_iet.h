#pragma once
// openartemis script runtime (oa::runtime) — the single public header of the .iet/.ast/.asb scenario module.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "core/runtime/runtime_lua.h"  // Lua host contract

namespace oa::runtime {

// ---------------------------------------------------------------------------
// values — Value + VariableStore
// ---------------------------------------------------------------------------
// Artemis variable Value + VariableStore (value conversions, four domains,
// prefix routing).
enum class ValueKind { Int, Float, String, Bool, Null };

struct Value {
    ValueKind kind = ValueKind::Null;
    int64_t int_val = 0;
    double float_val = 0.0;
    std::string str_val;
    bool bool_val = false;

    static Value make_int(int64_t v) {
        Value vv;
        vv.kind = ValueKind::Int;
        vv.int_val = v;
        return vv;
    }
    static Value make_float(double v) {
        Value vv;
        vv.kind = ValueKind::Float;
        vv.float_val = v;
        return vv;
    }
    static Value make_string(std::string v) {
        Value vv;
        vv.kind = ValueKind::String;
        vv.str_val = std::move(v);
        return vv;
    }
    static Value make_bool(bool v) {
        Value vv;
        vv.kind = ValueKind::Bool;
        vv.bool_val = v;
        return vv;
    }
    static Value make_null() { return Value{}; }

    std::optional<int64_t> as_int() const;
    std::optional<double> as_float() const;
    bool to_bool() const;
    std::string to_string() const;
    /// Display-style formatting (concat uses Rust fmt::Display semantics).
    std::string to_display() const;
    bool operator==(const Value& o) const = default;
};

/// VariableStore: four hash domains with prefix routing:
///   "g." -> global, "t." -> temp (never persisted), "s." -> system,
///   no prefix -> local. Missing variables are absent (evaluator turns
///   absence into Int(0) itself).
class VariableStore {
public:
    using Map = std::map<std::string, Value>;

    // Routing: returns the owning map for a full variable name.
    Map* domain_of(const std::string& name);
    const Map* domain_of(const std::string& name) const;

    std::optional<Value> get(const std::string& name) const;
    void set(const std::string& name, Value value);
    void remove(const std::string& name);

    /// [reset]: clear local + temp (g./s. survive).
    void reset_local_temp();
    /// Clear every domain (hosts/tests only; the [var system=delete] tag
    /// never triggers this — a nameless delete is a no-op).
    void clear_all();

    Map local;
    Map global;
    Map temp;
    Map system;
    std::string platform; // "windows" / "android" / ... (not persisted)
};

// --------------------------------------------------------------------------- expressions — ExpressionEvaluator...
struct ExpressionError : std::runtime_error {
    explicit ExpressionError(const std::string& msg) : std::runtime_error(msg) {}
};

/// Remove '$' prefixes in front of identifier-ish chars; '$' inside string
/// literals is kept. Used on whole expressions entered from tags.
std::string normalize_expr(std::string_view expr);

class ExpressionEvaluator {
public:
    explicit ExpressionEvaluator(const VariableStore& variables) : vars_(&variables) {}

    /// Evaluate a full expression (after normalize_expr by callers).
    Value evaluate(std::string_view expr) const;

    /// resolve_param: '$'-prefixed -> evaluate; '...' literals -> String;
    /// numeric text -> Int/Float; otherwise String.
    Value resolve_param(const std::string& value) const;
    /// resolve_param_str: same but never coerces numeric-looking strings to
    /// numbers (keeps "1.80" as "1.80").
    std::string resolve_param_str(const std::string& value) const;
    /// `$$<expr>` indirect reference: evaluate <expr> to a variable NAME, then
    /// read that variable. The framework builds names dynamically
    /// ($$'g.movie_read_' + file, $$('pos_' + pos)) and also embeds the form
    /// inside larger expressions ([if estimate="$$flag != 0"]).
    Value resolve_indirect(std::string_view name_expr) const;
    /// Rewrite every embedded `$$<operand>` in an expression into the value it
    /// references (string values are quoted so they stay one token).
    std::string expand_indirect(std::string_view expr) const;

private:
    const VariableStore* vars_;
};

// ---------------------------------------------------------------------------
// instructions — Instruction + parse_params
// ---------------------------------------------------------------------------
struct ParseError : std::runtime_error {
    size_t line = 0;
    ParseError(size_t line_no, const std::string& msg)
        : std::runtime_error("script line " + std::to_string(line_no) + ": " + msg),
          line(line_no) {}
};

struct Instruction {
    /// Row kind. Story text and [lua] blocks are NOT tag rows: they carry
    /// this discriminator instead of a reserved tag name, so a script that
    /// literally contains "[__text ...]" / "[__lua_block ...]" parses them
    /// as ordinary (custom) tags and can never be confused with engine-
    /// synthesized rows.
    enum class Kind {
        Tag,      // a "[tag ...]" row; `tag` holds its script tag name
        Text,     // parser-synthesized story text (params["text"])
        LuaBlock, // parser-synthesized "[lua] ... [/lua]" block (params["code"])
        /// A [&linetag] row: `params["text"]` holds "tag p0,p1,...". The
        /// parameter COLUMN order lives in the project's tag.ini, which the
        /// parser cannot see, so the row is expanded at execution time
        /// (Interpreter::line_tag_params).
        LineTag,
    };

    Kind kind = Kind::Tag;
    std::string tag; // valid when kind == Kind::Tag; empty otherwise
    std::map<std::string, std::string> params; // insertion-independent (last wins)
    size_t line = 0;
    /// Script that was executing when this row was ENQUEUED from Lua
    std::string origin_script;

    const std::string* get(std::string_view key) const {
        const auto it = params.find(std::string(key));
        return it == params.end() ? nullptr : &it->second;
    }
    bool has(std::string_view key) const { return get(key) != nullptr; }
    std::string get_or(std::string_view key, const std::string& fallback) const {
        const std::string* v = get(key);
        return v ? *v : fallback;
    }
    /// Positional-parameter access: key "0" wins, else first param by key order.
    const std::string* get_default() const {
        if (const std::string* v = get("0")) return v;
        if (!params.empty()) return &params.begin()->second;
        return nullptr;
    }
};

/// Parse the parameter string of one tag ("k=v ..." / positional words).
/// Quoted values may contain spaces; quotes do not support escapes and an
/// unterminated quote is a ParseError. Numeric keys are stored verbatim.
std::map<std::string, std::string> parse_params(std::string_view params_str, size_t line);

// --------------------------------------------------------------------------- script text — Script...
struct Script {
    std::string name;
    std::map<std::string, size_t> labels; // label -> instruction index
    std::vector<Instruction> instructions;

    static Script parse(const std::string& name, std::string_view content);

    /// Empty label means "file start" (index 0); missing label -> nullopt.
    std::optional<size_t> get_label_line(std::string_view label) const {
        if (label.empty()) return 0;
        const auto it = labels.find(std::string(label));
        if (it == labels.end()) return std::nullopt;
        return it->second;
    }
};

// --------------------------------------------------------------------------- ASB decoder — decode_asb...
struct AsbError : std::runtime_error {
    explicit AsbError(const std::string& msg) : std::runtime_error(msg) {}
};

/// Decode bytes into text lines joined by "\n" (labels as *name, tags as
/// [name k="v" ...]). Throws AsbError on malformed data.
std::string decode_asb(const std::vector<uint8_t>& data);

// ---------------------------------------------------------------------------
// interpreter — events, waits, hooks, Interpreter
// ---------------------------------------------------------------------------
// Artemis script interpreter core: script store, call stack,
// control tags, wait model. Lua hooks are injected through InterpreterHooks.

struct ScriptError : std::runtime_error {
    size_t line = 0;
    std::string kind; // "label" | "runtime" | "script-not-found" | "lua" | ...
    ScriptError(std::string kind_, const std::string& msg, size_t line_ = 0)
        : std::runtime_error(msg), line(line_), kind(std::move(kind_)) {}
};

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

struct WaitReason {
    enum class Kind {
        Generic,        // [@]
        Generic0,       // [wt0]
        Timed,          // [wt]/[wait]: ms + input policy
        Stop,           // [stop]: positional "0" reason text
        Se,             // [wait se=]: id + optional ms from play start
        VideoLayer,     // [wait video=]: layer id
        ScenarioTween,  // [wait scenario=1|2]
        KeyWait,        // [exkey]: button list
    };
    Kind kind = Kind::Generic;
    std::string id; // Stop reason / Se id / VideoLayer id
    std::vector<std::string> buttons;
    uint64_t milliseconds = 0;
    int64_t input = 0; // Timed: 0 timer-only, 1 clickable, 2 skip-pass
    int64_t mode = 0;  // ScenarioTween mode
    /// [wait se=ID time=N] with an explicit time param: N counts from the
    /// SE's play start; without
    /// the param the wait holds until the sound actually ends.
    bool time_given = false;
};

struct Event {
    enum class Kind {
        ScenarioText,
        Wait_,
        Custom,
        Reset,
        GoTitle,
        Exit,
        // ---- native tag events ----
        LayerCreate,     // lyc / lyc2
        LayerDelete,     // lydel
        LayerSetProps,   // lyprop
        LayerEventCmd,   // lyevent/lytween/tweenset/anime/video...
        Trans,           // [trans]: scene transition (type 0/1/2)
        Flip,            // [flip]: commit layer changes immediately
        MessageLayerSwitch, // chgmsg
        MessageLayerPop,    // /chgmsg
        ScenarioLine,    // print
        LineBreak,       // rt
        PageBreak,       // rp
        TextConfig,      // font/fontinit/glyph/scetween/indent/...
        ConfigEvent,     // everything else (config/audio/etc, kept verbatim)
    };
    Kind kind = Kind::Wait_;
    std::string content; // ScenarioText
    bool inline_text = false;
    WaitReason reason; // Wait_
    std::string tag;   // Custom / TextConfig / ConfigEvent / LayerEventCmd
    std::map<std::string, std::string> params;
    std::string id;    // layer / message-layer events

    static Event wait(WaitReason r) {
        Event e;
        e.kind = Kind::Wait_;
        e.reason = std::move(r);
        return e;
    }
    static Event custom(std::string t, std::map<std::string, std::string> p) {
        Event e;
        e.kind = Kind::Custom;
        e.tag = std::move(t);
        e.params = std::move(p);
        return e;
    }
    static Event scenario_text(std::string content) {
        Event e;
        e.kind = Kind::ScenarioText;
        e.content = std::move(content);
        return e;
    }
};

enum class CallbackResult { Continue, Pause, Abort };
enum class ExecutionResult {
    Completed,
    Wait,
    /// 单次 run() 用完了每帧 tag 预算（轮询型主循环）：既没停也没结束，
    /// 下一帧从当前位置继续。宿主**不要**因此清场或算作完成。
    BudgetExhausted,
};

struct CallFrame {
    std::string script;
    size_t return_line = 0;
};

// Host-side capability hooks (filled by the engine runtime and its Lua layer).
struct InterpreterHooks {
    /// Framework flavour switch for e:tag ordering: when true (default) the
    bool immediate_tags = true;
    /// Queued-[call] hold (see Interpreter::queue_hold_depth_). When true, a
    bool queue_call_hold = true;
    /// Execute a [lua] block (called at script load; fallback when stepped over).
    std::function<void(const std::string& code, const std::string& script_name, size_t line)>
        run_lua_block;
    /// Invoke a Lua function by dotted name with resolved params. Returns
    /// false when the function does not exist (optional-callback convention).
    std::function<bool(const std::string& function,
                       const std::map<std::string, std::string>& params,
                       const std::string& script_name, size_t line)>
        call_lua_function;
    /// File loader used by load_external_script / [macroadd] / Lua file reads.
    std::function<std::optional<std::vector<uint8_t>>(const std::string&)> file_loader;
    /// Read the effective props of one scene layer by id (e:var system=
    /// "get_layer_info"; slider readback). Empty map when absent.
    std::function<std::map<std::string, std::string>(const std::string&)> layer_info;
    /// e:var system=get_backlog_size: stored backlog page count (misc-A).
    /// Absent hook
    /// behaves as 0 ( conservative fallback).
    std::function<size_t()> backlog_size;
    /// e:var system=get_backlog_tags: reproduction tag sequence of backlog
    /// page `page` (0-based, allfont prefix rule); nullopt when out of range
    /// or hook absent → caller falls back to size=0 only.
    std::function<std::optional<std::vector<std::string>>(size_t page, bool allfont)>
        backlog_tags;
    /// e:var system=get_message_tags: executed text tags of the current page
    /// on message layer `id`; nullopt when the layer does not exist / hook
    /// absent → caller falls back to size=0 only.
    std::function<std::optional<std::vector<std::string>>(const std::string& id,
                                                          bool allfont)>
        message_tags;
    /// Sound playback snapshot for e:var system=get_sound_info (misc-B):
    /// BGM
    /// slot + SE list sorted by id; voices riding the SE bus appear there).
    struct SoundChannelSnap {
        std::string id;
        bool playing = false;
        int64_t gain = 1000; // raw Artemis scale
        int64_t pan = 0;
    };
    struct SoundInfoSnap {
        std::optional<SoundChannelSnap> bgm;
        std::vector<SoundChannelSnap> se;
    };
    std::function<std::optional<SoundInfoSnap>()> sound_info;
    /// Resource (asset) existence/read after magic-path resolution. The
    /// runtime wires these save-area aware (names under the savepath resolve
    /// through the save store first). Absent => file_loader fallback.
    std::function<bool(const std::string& resolved)> resource_exists;
    std::function<std::optional<std::vector<uint8_t>>(const std::string& resolved)>
        resource_read;
    /// Save-area existence (e:var file_exists save=1). Absent => false.
    std::function<bool(const std::string& file)> save_file_exists;
    /// Absolute save-area root. Backs the Lua `io.open` redirect: the iMel
    std::function<std::string()> save_root;
    /// Save-area modification time (e:var file_update_time).
    /// Absent => noexist default.
    std::function<std::optional<std::array<int64_t, 6>>(const std::string& file)>
        file_mtime;
    /// Monotonic per-tick frame number (e:getFrameNumber). Absent => the
    /// lua host falls back to an fps projection of the interpreter clock.
    std::function<uint64_t()> frame_number;
    /// 当前消息层文本度量 (整体宽度, 总高度, 最后一行宽度)。
    /// 供 var system=get_message_layer_width/height/line_width（Lua get_fontsize
    /// 字体测量流程）。
    /// 无钩子时查询落 0。
    std::function<std::tuple<double, double, double>()> message_layer_metrics;
};

class Interpreter {
public:
    struct Config {
        std::string charset = "Shift_JIS"; // script file decoding
        std::string platform;
        int stage_width = 1280;
        int stage_height = 720;
        int fps = 60;
        std::map<std::string, std::string> env;
    };

    Interpreter();
    explicit Interpreter(const Config& config);

    // -- loading -------------------------------------------------------------
    void load_script(const std::string& name, std::string_view text);
    const Script* get_script(const std::string& name) const;

    // -- execution -----------------------------------------------------------
    void start(const std::string& script, std::string_view label);
    /// Boot: *main -> *start -> *_start -> file head; tries macro.iet silently.
    void boot(const std::string& script);
    /// Iterate until Completed or a Wait event (callback returned Pause).
    ExecutionResult run();
    /// Host call after a Wait resolved: advance past the wait instruction.
    void next_line();

    /// Restore the execution position from a save: script (loaded on
    /// demand), line and call stack. `script` empty keeps the interpreter
    /// unpositioned.
    void restore_position(const std::string& script, size_t line,
                          const std::vector<CallFrame> stack);
    /// 读档预检用的**非抛**版本：脚本已在缓存 → true；否则按
    /// restore_position 的同一条路径载入并解析（load_external_script），失败
    /// 只把原因写进 `error`（不抛）。语义上等价于 restore_position 的第一步，
    /// 使"读档失败"能在任何清场之前判定（原子失败）。
    bool try_load_script(const std::string& file, std::string* error);
    /// tag.ini column names for one [&linetag] command (loaded lazily from
    /// the project's tag.ini). Empty when the file or the section is absent,
    /// in which case the row's columns stay positional ("1", "2", ...).
    const std::vector<std::string>& line_tag_params(const std::string& tag);
    /// Rewrite ONLY the call stack, leaving the current position and the
    /// queue-wait/arrival flags untouched (inline-event settle uses this
    /// to drop the synthetic marker frame — restore_position would clear
    /// last_wait_from_queue_, making a later next_line skip the parked
    /// wait's [return] row).
    void set_call_stack(std::vector<CallFrame> stack) {
        if (queue_hold_depth_ != 0 && stack.size() < queue_hold_depth_) {
            queue_hold_depth_ = 0; // held callee frame removed outside the run loop
        }
        call_stack_ = std::move(stack);
    }
    /// Temporarily detach / re-queue pending engine tags (used by the save
    /// domain to flush only the tags an onSave/onLoad handler generated).
    /// restore_tag_queue re-queues the taken items BEHIND whatever the flushed
    /// handler left queued (generated-leftovers-first order).
    std::vector<Instruction> take_tag_queue();
    void restore_tag_queue(std::vector<Instruction> items);
    size_t queued_tag_count() const { return tag_queue_.size(); }

    /// Queue an engine tag the interpreter executes from its tag queue (the
    /// drain that Lua e:enqueueTag uses; the runtime enqueues lytween
    /// completion handlers through it).
    void enqueue_tag(std::string tag, std::map<std::string, std::string> params);
    /// Whether the tag queue holds pending tags.
    bool has_queued_tags() const { return !tag_queue_.empty(); }
    /// Macro registry: [macroadd file=F] registers every label of F as a
    /// callable tag (`[btn ...]` -> `[call file=F label=btn]`), the iMel
    /// framework's whole UI layer (button.iet/msg.iet/chara_*.iet).
    const std::map<std::string, std::string>& macros() const { return macros_; }
    void add_macro(const std::string& label, const std::string& file) {
        macros_[label] = file;
    }
    void remove_macros_of_file(const std::string& file) {
        for (auto it = macros_.begin(); it != macros_.end();) {
            if (it->second == file) it = macros_.erase(it);
            else ++it;
        }
    }
    /// Macro parameter scope snapshot (see macro_param_saves_).
    struct MacroParamSave {
        size_t depth = 0;
        std::map<std::string, std::optional<Value>> saved;
        /// Parameter names THIS call supplied. Artemis gives every macro call
        /// its own local frame: an outer call's parameter must not look like a
        /// local variable inside a nested macro that did not pass it
        /// ([set_functionkeys] with no mode used to see an enclosing
        /// selectbtn's mode=1 and jump to registration_functionkeys_1).
        std::set<std::string> provided;
        /// Names written with writelocal=1 while this frame was active
        /// (frame-local temporaries the script itself declares).
        std::set<std::string> written;
    };
    void push_macro_param_save(MacroParamSave save) {
        macro_param_saves_.push_back(std::move(save));
    }
    /// Undo the parameter variables of the macro frame that just returned
    /// (depth = call stack depth BEFORE the pop).
    void restore_macro_params(size_t depth);
    /// `[var system=var_exist local=1]`: is `name` local to the CURRENT macro
    /// frame (parameter or writelocal declaration)? Outer frames' parameters
    /// and stale leftovers of returned frames do not count.
    bool local_var_declared(const std::string& name) const;
    /// Remember a writelocal=1 write in the innermost macro frame.
    void note_local_write(const std::string& name);
    /// First `n` queued tags in FIFO order (diagnostics only).
    std::vector<Instruction> peek_tag_queue(size_t n) const {
        std::vector<Instruction> out;
        for (size_t i = 0; i < n && i < tag_queue_.size(); ++i)
            out.push_back(tag_queue_[i]);
        return out;
    }
    /// Drain ONLY the queued tags (no inline script stepping). Returns Wait
    /// when a queued tag paused the interpreter (parked-wait drains
    /// run queued tags only, no inline stepping); Completed when the queue
    /// emptied without a
    /// pause. Queued jump/call/return tags change the interpreter position.
    ExecutionResult run_queued();
    /// Queued-drain shape flags:
    /// saw_call/saw_jump set while run_queued executes one queued tag of
    /// that kind; consumed by the runtime's inline-event-frame settle after
    /// each drain round.
    bool queued_saw_call() const { return queued_saw_call_; }
    /// Click-handler return floor: while the queued tags of one pointer click
    void set_return_floor(size_t depth) { return_floor_ = depth; }
    size_t return_floor() const { return return_floor_; }
    void clear_return_floor() { return_floor_ = 0; }
    bool queued_saw_jump() const { return queued_saw_jump_; }

    // -- state ---------------------------------------------------------------
    const std::string* current_script() const;
    size_t current_line() const { return current_line_; }
    const std::vector<CallFrame>& call_stack() const { return call_stack_; }
    VariableStore& variables() { return variables_; }
    const VariableStore& variables() const { return variables_; }
    void set_variable(const std::string& name, Value v) { variables_.set(name, std::move(v)); }
    const Config& config() const { return config_; }

    // -- Lua (created lazily; one shared VM) ----------------------------------
    oa::runtime::LuaBridge& lua_bridge();
    /// Apply a [var ...] tag (simple + system= subset); used by the var tag
    /// and by Lua e:tag{"var",...}. Returns false for unknown system= keys.
    bool apply_var(const std::map<std::string, std::string>& params);
    /// Fire a registered event handler (e.g. onEnterFrame -> Lua vsync).
    bool fire_event(const std::string& event_name);
    /// `e:tag{...}` from Lua executes the tag NOW (Artemis semantics), unlike
    /// `e:enqueueTag` which defers. Only presentation tags are accepted here;
    /// control-flow outcomes return false so the caller can queue the tag and
    /// let the main loop own the redirect. Re-entrant (an [lyc] filter
    /// re-issuing [lyc]) up to a bounded depth, then falls back to the queue.
    bool run_tag_now(const std::string& tag, std::map<std::string, std::string> params);

    /// A scenario text row is `[print data="<row>"]` to the framework side:
    /// hand it to the registered tagFilters.print (the iMel multi-language
    /// layer lives there — it picks the text of the active [lang xx] block and
    /// re-issues e:tag{"print", data=…}). Returns true when the filter took
    /// ownership (the row must not be emitted as an engine text event).
    bool dispatch_text_row_to_print_filter(const std::string& text);

    /// e:getScriptBlock backing: tag row at script : instruction index
    /// (command = tag name, parameter = raw params). nullopt when the script
    /// is unknown or the index is out of range.
    std::optional<std::pair<std::string, std::map<std::string, std::string>>>
    script_block(const std::string& file, size_t index) const;
    /// Resolve a logical file name through the magic-path table
    /// (e:setMagicPath, ":bg/" prefixes etc).
    std::string resolve_magic_path(const std::string& name) const;

    // -- wiring --------------------------------------------------------------
    const InterpreterHooks& hooks() const { return hooks_; }
    InterpreterHooks& hooks() { return hooks_; }
    void set_callback(std::function<CallbackResult(const Event&)> cb) {
        callback_ = std::move(cb);
    }
    /// The Wait event that made run return Wait (cleared on next_line).
    const Event* last_wait_event() const { return last_wait_event_.has_value() ? &*last_wait_event_ : nullptr; }
    /// Whether the current wait came from the tag queue (next_line no-op).
    bool last_wait_from_queue() const { return last_wait_from_queue_; }
    /// Diagnostics: called for every executed instruction (inline + queued).
    std::function<void(const std::string& script, size_t line, const Instruction& ins)>
        on_step;
    /// Engine-registered tags (override builtin semantics, e.g. [reset]).
    /// Returned event is emitted like a tag Emit; nullopt means Continue.
    using EngineTagFn =
        std::function<std::optional<Event>(Interpreter&, const Instruction&)>;
    void register_engine_tag(const std::string& name, EngineTagFn fn) {
        engine_tags_[name] = std::move(fn);
    }
    const std::map<std::string, EngineTagFn>& engine_tags() const { return engine_tags_; }

private:
    Config config_;
    std::map<std::string, Script> scripts_;
    /// tag.ini: command -> ordered parameter names ([&linetag] columns).
    std::map<std::string, std::vector<std::string>> line_tags_;
    bool line_tags_loaded_ = false;
    std::optional<std::string> current_script_name_;
    size_t current_line_ = 0;
    std::vector<CallFrame> call_stack_;
    bool arrived_by_jump_ = false;
    bool last_wait_from_queue_ = false;
    bool queued_saw_call_ = false;
    bool queued_saw_jump_ = false;
    /// While non-zero, the queued-tag drain is held: a queued [call] handed
    size_t queue_hold_depth_ = 0;
    size_t return_floor_ = 0;
    int immediate_tag_depth_ = 0;
    /// True when the last run() ended by yielding a wait/stop to the host.
    /// queue_hold_depth_ is released on that edge (see flush_tag_queue).
    bool parked_ = false;
    // Macro tag -> defining file ([macroadd]); see macros().
    std::map<std::string, std::string> macros_;
    // Macro parameter scope: tag params become plain variables for the
    // duration of the macro call; previous values are restored on [return].
    std::vector<MacroParamSave> macro_param_saves_;
    VariableStore variables_;
    InterpreterHooks hooks_;
    std::function<CallbackResult(const Event&)> callback_ =
        [](const Event&) { return CallbackResult::Continue; };
    std::map<std::string, EngineTagFn> engine_tags_;
    std::map<std::pair<std::string, size_t>, bool> executed_lua_blocks_;
    // queue of engine tags enqueued from Lua (drained before inline steps)
    std::vector<Instruction> tag_queue_;
    // Lua-side event handlers: event name -> global function name
    std::map<std::string, std::string> event_handlers_;
    // e:getScriptWaitReason backing store
    std::map<std::string, std::string> wait_reason_info_;
    std::optional<Event> last_wait_event_;
    std::unique_ptr<oa::runtime::LuaBridge> lua_bridge_;
    std::map<std::string, std::string> magic_paths_;

    friend struct LuaBootstrap;
    // internals ------------------------------------------------------------
    /// Load one scenario from bytes: text goes straight to Script::parse,
    /// ".asb" bytes through decode_asb. Internal (used only by
    /// load_external_script below).
    void load_file(const std::string& name, const std::vector<uint8_t>& bytes);
    /// Load a scenario file through InterpreterHooks::file_loader (decoding
    /// .asb bytes through load_file/decode_asb). Internal: hosts reach
    /// scenarios via boot()/start()/the file_loader hook only ("small
    /// interface").
    void load_external_script(const std::string& file);
    std::optional<ExecutionResult> flush_tag_queue();
    /// Sync metric vars read the active layer, but FPM writes them
    /// right after queueing a chgmsg/rp/print segment (uihelp centering,
    /// get_fontsize, line/backlog measuring). Run the queued message-text
    /// tags ahead of the query so it measures the intended layer; stops at
    /// the first non-text tag (waits/control never pre-execute).
    void flush_pending_text_tags();
    void note_wait_reason(const Event& event);
    const oa::runtime::Value* lookup_var_ptr(const std::string& name) const;
    oa::runtime::LuaHost build_lua_host();
    uint64_t now_ms() const;
    void seed_engine_variables_impl();
};

} // namespace oa::runtime
