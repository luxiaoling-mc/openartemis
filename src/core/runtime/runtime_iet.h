#pragma once
// openartemis script runtime (oa::runtime) — the single public header of the
// .iet/.ast/.asb scenario module.
//
// Design: "small interface, big file". Everything this module
// does — value/variable conversions, expression evaluation, the .iet/.ast text
// parser, the .asb binary decoder, and the interpreter that executes the rows
// (control tags, wait model, Lua hooks, script store + call stack + the
// save/restore of the execution position) — is implemented in ONE translation
// unit, runtime/runtime_iet.cpp. Every parser, tag dispatcher, depth scan and
// formatting helper there is internal linkage (anonymous namespace) and never
// appears below; this header carries only the types callers actually name.
//
//   Value / VariableStore  — variable model (+ runtime_save serialization)
//   expressions            — ExpressionEvaluator ($expr resolution)
//   instructions           — Instruction rows + parse_params
//   Script                 — one parsed scenario (labels + rows)
//   ASB decoder            — decode_asb: bytes -> Artemis text
//   Interpreter            — execution state, events/waits, host hooks
//
// Organization mirrors krkrsdl3 cpp/core/script/tjsNativeKAGParser.cpp (the
// KAG scenario unit this format descends from: scenario cache/loading,
// parsing, execution state and its save/restore in one file). Only the
// organization is borrowed.
//
// Provenance: merged from src/core/script/{value,expression,instruction,
// script,asb,interpreter}.{h,cpp} (six headers + six TUs) with no behavior
// change; the per-section banners below name the block's subject.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
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

// ---------------------------------------------------------------------------
// expressions — ExpressionEvaluator
// ---------------------------------------------------------------------------
// Artemis expression evaluator.
//   - single-quoted strings (no escapes), numbers (0x/dec/float),
//   - identifiers may contain '.' and '_' (so g.score is one token),
//   - operators: || && == != < <= > >= + - * / %  ,
//   - no function calls, no bool literals, no unary '!',
//   - precedence low->high: || && ==!= rel +- (string concat if either side
//     is String) * / % unary-minus primary,
//   - comparisons convert both sides to f64 (string fallback 0.0),
//   - missing variables evaluate to Int(0),
//   - dynamic names: foo.(expr) segments appended via to_string.
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
    };

    Kind kind = Kind::Tag;
    std::string tag; // valid when kind == Kind::Tag; empty otherwise
    std::map<std::string, std::string> params; // insertion-independent (last wins)
    size_t line = 0;

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

// ---------------------------------------------------------------------------
// script text — Script
// ---------------------------------------------------------------------------
// Parsed script (labels + instruction list). Textual .iet/.ast content
// parsing follows the preprocessed-format rules:
//   - empty lines, "//" and ";" comments skipped
//   - "/* ... */" block comments skipped (content opaque; may span lines
//     when opened at the start of a line; unterminated blocks error)
//   - "*label" defines a label at the current instruction index
//   - lines with '[' are split into tags and inline story text segments
//   - "[lua] ... [/lua]" becomes one LuaBlock instruction (kind, not a
//     reserved tag name; code in params["code"])
//   - un-bracketed story text becomes Text instructions (kind, not a
//     reserved tag name; text in params["text"])
// Script-sourced rows always have kind Tag, so a script tag that happens
// to be named like a magic marker can never collide with synthesized
// text/lua rows (dispatch keys on Instruction::Kind, never on names).
// Preprocessor directives ("[&...]") are not expanded yet (fast path rule:
// only run when content contains "[&").
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

// ---------------------------------------------------------------------------
// ASB decoder — decode_asb
// ---------------------------------------------------------------------------
// ASB binary script decoder (format from asb-decrypt crate, verified against
// fpm system/{ui,save,script}.asb):
//   header: "ASB\0" + u8 flag + u32le entry_count
//   entry : u32le type (1=label, 0=instruction), u32le name_len, name, NUL
//   label  -> "*name"
//   instr  -> u32le serial (ignored), u32le param_count,
//             param_count x (u32le key_len, key, NUL, u32le val_len, val, NUL)
// Output is Artemis text syntax so the regular Script parser can reuse it.
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
enum class ExecutionResult { Completed, Wait };

struct CallFrame {
    std::string script;
    size_t return_line = 0;
};

// Host-side capability hooks (filled by the engine runtime and its Lua layer).
struct InterpreterHooks {
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
    /// Rewrite ONLY the call stack, leaving the current position and the
    /// queue-wait/arrival flags untouched (inline-event settle uses this
    /// to drop the synthetic marker frame — restore_position would clear
    /// last_wait_from_queue_, making a later next_line skip the parked
    /// wait's [return] row).
    void set_call_stack(std::vector<CallFrame> stack) {
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
    std::optional<std::string> current_script_name_;
    size_t current_line_ = 0;
    std::vector<CallFrame> call_stack_;
    bool arrived_by_jump_ = false;
    bool last_wait_from_queue_ = false;
    bool queued_saw_call_ = false;
    bool queued_saw_jump_ = false;
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
