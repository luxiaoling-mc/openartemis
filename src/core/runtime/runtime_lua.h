#pragma once
// Lua 5.1 host bridge for Artemis — public contract.
//
// The runtime's Lua domain: one thin contract header + ONE implementation TU
// (core/runtime/runtime_lua.cpp) beside the other runtime files
// (runtime_iet.cpp / runtime_media.cpp / runtime_save.cpp); the former lua/
// directory under core is gone.
//
// LuaBridge is exactly what its name says: a
// *bridge* between the Artemis script interpreter (oa::runtime::Interpreter)
// and one shared Lua 5.1 VM per interpreter — it owns the lua_State, boots
// the Lua-side host surface and forwards every engine request back into the
// interpreter/runtime through LuaHost closures (variables, tag queue,
// input/media state) without circular includes.
//
// Lua-side injections (install_engine_api, VM boot):
//   * the engine handle — ONE opaque full userdata per VM whose guarded
//     metatable carries the whole e:* method surface (merged in by the host
//     surface sections; see the register_* openers in
//     core/runtime/runtime_lua.cpp). The
//     handle only travels as the first argument `e` of every host->Lua call (calllua rows, FPM (e, p)
//     event handlers, tag/event filter callbacks) — the Lua-side contract
//     of the original engine. Host code recovers the owning bridge from
//     the method-closure upvalue (bridge_from_state) and re-pushes the
//     handle through an anonymous luaL_ref anchor (push_engine) whenever
//     it calls back into Lua.
//   * `pluto` — persist/unpersist (binary stream based) global table.
// Every engine pcall installs a debug.traceback message handler, so LuaError
// messages carry the Lua call trace directly (no global/registry
// side-channel). Host services are closures so the interpreter/runtime can
// wire variables, tag queue and input/media state without circular includes.
// The Lua host surface is ONE translation unit (core/runtime/runtime_lua.cpp,
// "small interface, big file"): this header is the whole public contract, and
// the register_* openers / shared helpers that used to live in the deleted
// lua_api.h are now TU-internal sections of that file (shared helpers +
// openers, bridge object, tag/sys/input/surface/emote/filter/pluto).
// install_engine_api calls the openers in order, the same shape as Lua's own
// linit.c / luaL_openlibs.
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

struct lua_State;

namespace oa::runtime {
struct Value;
} // namespace oa::runtime

namespace oa::runtime {

struct LuaError : std::runtime_error {
    explicit LuaError(const std::string& msg) : std::runtime_error(msg) {}
};

/// Tag-filter dispatch verdict ( LuaTagFilterDecision).
enum class FilterDecision { Missing, PassThrough, Consume };

struct LuaHost {
    // -- files ----------------------------------------------------------------
    std::function<std::optional<std::vector<uint8_t>>(const std::string&)> read_file;
    std::function<bool(const std::string&)> is_file_exists;

    // -- variables ------------------------------------------------------------
    std::function<const oa::runtime::Value*(const std::string&)> get_var;
    std::function<void(const std::string&, oa::runtime::Value)> set_var;

    // -- interpreter wiring ---------------------------------------------------
    /// Queue one engine tag (bypasses the tag filter). Filled by Interpreter.
    std::function<void(std::string tag, std::map<std::string, std::string> params)>
        enqueue_tag;
    /// Sync e:tag{"var",...} application (never queued).
    std::function<bool(std::map<std::string, std::string> params)> apply_var_tag;
    /// Sync Lua-originated e:tag{"calllua",...} dispatch.
    /// Mirrors the inline [calllua] tag semantics of .asb streams: the Lua
    /// callback runs at enqueue time — before the enclosing Lua flow can
    /// rebuild the state the callback reads (e.g. button groups) — instead
    /// of at the deferred queue drain. Params arrive as Lua literals; the
    /// interpreter resolves variable references exactly like the asb path.
    /// Returns false when "function" is missing/empty (caller keeps the
    /// historical deferred path so a broken call still surfaces the same
    /// drain-time error); true when dispatched (missing VM function = silent
    /// no-op, the optional-callback convention). Lua errors from the
    /// callback are thrown (caller converts them to lua_error at the pcall
    /// boundary).
    std::function<bool(const std::string& function,
                       const std::map<std::string, std::string>& params)>
        run_calllua_sync;
    /// Register/clear an event-handler name (onEnterFrame=..., onSave=..., ...).
    std::function<void(const std::string& event_name, const std::string& fn_name)>
        set_event_handler;
    /// Call a Lua global function by dotted name from the engine side (no
    /// args); used for event handlers. Returns false when missing.
    std::function<bool(const std::string& fn_name)> call_handler;

    // -- E-mote layer methods -------------------------------------------------
    /// Route one EmoteLayer Lua method call to the runtime emote player for
    /// the layer id. `s` carries the first string argument and `s2` the second
    /// one (timeline/variable name; setVariableDiff's src+dst labels are the
    /// only two-string shape the frameworks use), n1..n3 the numeric ones;
    /// `out` receives the numeric result for getters and `sout` the string
    /// result of getVariableLabelAt. False when unwired (bridge falls back to
    /// no-ops).
    std::function<bool(const std::string& id, const std::string& method,
                       const std::string& s, const std::string& s2, double n1,
                       double n2, double n3, double* out, std::string* sout)>
        emote_method;

    // -- status/waits ---------------------------------------------------------
    std::function<int()> get_script_status;
    std::function<void(int)> set_script_status;
    /// Monotonic per-tick frame number (e:getFrameNumber); absent host ->
    /// interpreter falls back to an fps projection of its clock.
    std::function<uint64_t()> frame_number;
    /// FPM e:debugSkip (次の選択肢に進む / 高速スキップ; select.lua
    /// goNextSelectLoop). index = 99999 in FPM calls. Wired by GameRuntime to
    /// start the engine exskip fast-forward state.
    std::function<void(int64_t index)> debug_skip_start = [](int64_t) {};
    /// Fields for e:getScriptWaitReason (time/textTween/textClearTween/sound/video).
    std::function<std::map<std::string, std::string>()> get_wait_reason;

    // -- clock / platform -----------------------------------------------------
    std::function<uint64_t()> now_ms;
    std::function<std::string()> platform;

    // -- input (defaults: empty; runtime wires real state in M5) --------------
    std::function<bool(int)> is_down = [](int) { return false; };
    std::function<bool(int)> is_down_edge = [](int) { return false; };
    std::function<bool(int)> is_up_edge = [](int) { return false; };
    std::function<bool(int)> is_push = [](int) { return false; };
    std::function<bool(int)> is_decide = [](int) { return false; };
    std::function<std::pair<int, int>()> get_mouse_point = [] { return std::pair(0, 0); };
    std::function<void(int, int)> override_key = [](int, int) {};
    std::function<void(const std::string&, const std::string&)> set_magic_path =
        [](const std::string&, const std::string&) {};

    // -- surfaces/fonts (defaults inert; runtime provides in M5) ---------------
    std::function<bool(const std::string&)> bind_surface_async = [](const std::string&) {
        return true;
    };
    std::function<void(const std::string&)> unbind_surface = [](const std::string&) {};
    std::function<void()> clear_surface_load_queue = [] {};
    std::function<bool()> is_loading_surface = [] { return false; };
    std::function<void()> restore_font_cache = [] {};
    std::function<std::optional<std::map<std::string, std::string>>(const std::string&)>
        load_png_comments = [](const std::string&)
        -> std::optional<std::map<std::string, std::string>> { return std::nullopt; };
};

class LuaBridge {
public:
    explicit LuaBridge(LuaHost host);
    ~LuaBridge();
    LuaBridge(const LuaBridge&) = delete;
    LuaBridge& operator=(const LuaBridge&) = delete;

    lua_State* state() const { return L_; }
    LuaHost& host() { return host_; }
    const LuaHost& host() const { return host_; }

    /// Run Lua source in the shared VM.
    void run_code(const std::string& code, const std::string& chunk_name);
    /// Call a global dotted function with string-valued params (the engine
    /// handle userdata is passed as the first argument, FPM's (e, p)
    /// convention). False when the function is missing.
    bool call_function(const std::string& dotted,
                       const std::map<std::string, std::string>& params);
    /// Call a plain global function by (dotted) name with no arguments.
    bool call_plain(const std::string& dotted);
    /// e:include semantics: execute a project file in the same VM.
    void include_file(const std::string& path);

    /// Debug: current Lua call stack (short_src:line name) — valid while a
    /// Lua function is on the C stack (e.g. inside an e:* call).
    std::string stack_trace(int max_depth = 12) const;

    /// Engine->game-Lua dispatch error policy (docs/research/130).
    ///
    /// A Lua error raised while the ENGINE was calling into game Lua aborts
    /// only THAT call: it is reported here (the message already carries the
    /// debug.traceback decoration) and the dispatch site returns its
    /// "not dispatched" verdict. The original runtime never lets a game-Lua
    /// error kill the process. This engine already fired that policy at two
    /// boundaries (lyevent handler, event filter); the remaining ones were
    /// fatal, which turned a data-side script error into an app exit 
    /// (research/130: blanked system/extend/auth.lua).
    ///
    /// Repeats of the same (what,name) pair are rate-limited — the reference
    /// guards a broken per-frame handler the same way. `OA_LUA_STRICT=1`
    /// restores the pre-130 fail-fast (rethrows, i.e. the tick aborts) for
    /// defect hunting / CI.
    void report_dispatch_error(const char* what, const std::string& name,
                               const std::string& msg) const;

    /// Inline tag filter dispatch (registry table + dotted lookup + call).
    FilterDecision run_tag_filter(const std::string& tag,
                                  const std::map<std::string, std::string>& params);

    /// e:setEventFilter dispatch: calls the stored eventFilter(e, name,
    /// params). Returns
    /// 1 = script claims the event (engine must NOT dispatch), 2 = pretend
    /// failure; nullopt when no filter is installed or it returned 0/errored.
    std::optional<int> run_event_filter(const std::string& name,
                                        const std::map<std::string, std::string>& params);

    /// e:setTagFilter / e:setEventFilter / e:setLogFilter storage.
    /// Tag and event filters use luaL_ref to hold Lua objects without
    /// polluting the registry with named string keys.
    void store_tag_filter(lua_State* L, int idx);
    void store_event_filter(lua_State* L, int idx);
    void store_log_filter(lua_State* L, int idx);

    /// LCG (31-bit non-negative integers).
    int64_t next_random();

    /// Encoding conversion (utf-8 identity today; sjis pairs later).
    static std::string convert_encoding(const std::string& from, const std::string& to,
                                        const std::string& source);

private:
    void install_engine_api();
    /// Push the engine handle userdata onto the stack (the `e` value of
    /// host->Lua calls); engine_ref_ is its anonymous registry anchor.
    void push_engine(lua_State* L) const;

    lua_State* L_ = nullptr;
    LuaHost host_;
    uint64_t random_state_ = 0x9E3779B97F4A7C15ull;
    bool random_seeded_ = false;

    /// Anonymous registry anchor of the engine handle userdata (luaL_ref,
    /// set in the constructor; see the bridge-object section of
    /// runtime_lua.cpp). No named registry entry exists.
    int engine_ref_;

    /// luaL_ref handles for filter callbacks (LUA_NOREF = not installed).
    int tag_filter_ref_;
    int event_filter_ref_;
    /// Log filter is a plain integer (0 = raw, 1 = suppressed); no Lua ref.
    int log_filter_value_;
};

} // namespace oa::runtime
