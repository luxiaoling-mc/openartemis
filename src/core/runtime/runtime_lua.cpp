// Lua 5.1 host bridge — ONE translation unit.
//
// "Small interface, big file": the public contract is the single thin header
// core/runtime/runtime_lua.h (LuaHost + LuaBridge); everything else the Lua
// host surface needs lives here. The Lua domain lives in the runtime family
// (beside runtime_iet/runtime_media/runtime_save) and the former core/lua
// directory is gone. This file merges the eight former TUs of that directory
//
//   lua_bridge.cpp bridge object + shared helpers
//   lua_tag.cpp    e:tag / e:enqueueTag
//   lua_sys.cpp    variables/clock/random/include/file/status/handlers/debug
//   lua_input.cpp  input/touch/mouse/overrideKey
//   lua_surface.cpp surface + font cache
//   lua_emote.cpp  E-mote layer objects
//   lua_filter.cpp tag/event/log filter setters + filter dispatch
//   lua_pluto.cpp  global `pluto` persist/unpersist codec
//
// and the former internal contract header lua_api.h, whose shared
// helpers (bridge_from_state / merge_bridge_methods / value_to_string /
// read_command_table / l_traceback) and per-domain openers (register_*) are
// now TU-internal anonymous-namespace entities. Lua-side shape is
// unchanged: ONE guarded engine-handle userdata whose metatable carries the
// whole e:* method surface, plus the `pluto` global.
#include "core/runtime/runtime_lua.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "core/runtime/runtime_iet.h"
#include "core/util/binary_stream.h"

namespace oa::runtime {

// ---------------------------------------------------------------------------
// Shared internal helpers.
// ---------------------------------------------------------------------------
// Module openers (defined in the tag/sys/input/surface/emote/filter/pluto
// sections below): install_engine_api in the bridge-object section calls them
// in order, the Lua linit.c/luaL_openlibs shape. Internal
// linkage — this TU is their only caller (they were the last names
// declared by the deleted lua_api.h contract header).
static void register_pluto(lua_State* L);
static void register_tag(lua_State* L, LuaBridge* bridge);
static void register_sys(lua_State* L, LuaBridge* bridge);
static void register_input(lua_State* L, LuaBridge* bridge);
static void register_surface(lua_State* L, LuaBridge* bridge);
static void register_emote(lua_State* L, LuaBridge* bridge);
static void register_filter(lua_State* L, LuaBridge* bridge);

namespace {

LuaBridge* bridge_from_state(lua_State* L) {
    // Every engine method is a C closure whose upvalue 1 is a lightuserdata
    // holding the owning LuaBridge (see merge_bridge_methods below). The
    // bridge is recovered from the running closure — never from a registry
    // or global name — so the Lua environment carries no engine entry for
    // host bookkeeping.
    return static_cast<LuaBridge*>(lua_touserdata(L, lua_upvalueindex(1)));
}

void merge_bridge_methods(lua_State* L, LuaBridge* bridge, const luaL_Reg* methods) {
    // Native-library merge into the table on top of the stack (the analog of
    // luaL_register with a NULL module name), except each entry is pushed as
    // a C closure with the owning bridge as upvalue 1 so host functions can
    // recover it from the call itself (bridge_from_state).
    for (const luaL_Reg* m = methods; m && m->name; ++m) {
        lua_pushlightuserdata(L, bridge);
        lua_pushcclosure(L, m->func, 1);
        lua_setfield(L, -2, m->name);
    }
}

std::string value_to_string(lua_State* L, int idx) {
    const int t = lua_type(L, idx);
    switch (t) {
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            return std::string(s, len);
        }
        case LUA_TBOOLEAN:
            return lua_toboolean(L, idx) ? "true" : "false";
        case LUA_TNUMBER: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.14g", lua_tonumber(L, idx));
            return std::string(buf);
        }
        case LUA_TNIL:
            return "";
        default:
            return std::string("<") + lua_typename(L, t) + ">";
    }
}

std::string read_command_table(lua_State* L, int idx,
                               std::map<std::string, std::string>* params) {
    std::string tag;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        const int kt = lua_type(L, -2);
        if (kt == LUA_TNUMBER) {
            const double n = lua_tonumber(L, -2);
            const long long i = (long long)n;
            if (n == (double)i && i == 1) {
                tag = value_to_string(L, -1);
            } else {
                (*params)[std::to_string(i)] = value_to_string(L, -1);
            }
        } else if (kt == LUA_TSTRING) {
            (*params)[value_to_string(L, -2)] = value_to_string(L, -1);
        }
        lua_pop(L, 1);
    }
    return tag;
}

/// Resolve `dotted` to a Lua function on top of the stack: the fast path
/// through the global of that exact name, then a field walk from _G. On
/// success the function is the only pushed value and true is returned; on
/// failure nothing is left behind. call_function / call_plain shared two
/// byte-identical copies of this prologue.
bool push_dotted_function(lua_State* L, const std::string& dotted) {
    lua_getglobal(L, dotted.c_str());
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_getglobal(L, "_G");
        size_t start = 0;
        for (;;) {
            const size_t dot = dotted.find('.', start);
            const std::string part = dot == std::string::npos
                                         ? dotted.substr(start)
                                         : dotted.substr(start, dot - start);
            lua_getfield(L, -1, part.c_str());
            lua_remove(L, -2);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                return false;
            }
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
    }
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

int l_traceback(lua_State* L) {
    // pcall message handler: decorate string errors with debug.traceback and
    // return the combined string (pcall surfaces it as the error object, so
    // LuaError messages carry the call trace with no registry side-channel).
    // Non-string error objects pass through untouched.
    if (!lua_isstring(L, 1)) return 1;
    lua_getglobal(L, "debug");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    lua_getfield(L, -1, "traceback");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return 1;
    }
    lua_pushvalue(L, 1);
    if (lua_pcall(L, 1, 1, 0) != 0 || !lua_isstring(L, -1)) {
        lua_pop(L, 2); // traceback failure + debug table: keep original message
        return 1;
    }
    lua_replace(L, 1); // decorated message replaces the original
    lua_pop(L, 1);     // debug table
    return 1;
}

} // namespace

// ---------------------------------------------------------------------------
// Standard-library block list (os / io / loaders).
// ---------------------------------------------------------------------------
// Frameworks in the target ecosystem genuinely use os.date, io.open and
// io.close (save-row date stamps, ordinary file reads/writes), so the stock
// libraries stay in place. What must not stay reachable is the process-level
// surface a desktop scripting library of that era exposes:
//
//   os.execute / os.system      run an arbitrary host command
//   os.exit                     terminate the engine process
//   os.remove / os.rename       rewrite the host filesystem
//   os.setlocale                process-wide locale change
//   os.tmpname                  host temp-file creation
//   io.popen                    spawn a shell
//   io.tmpfile / io.input / io.output   ambient host streams
//   dofile / loadfile / loadstring / load, package.loadlib   load native or
//                               arbitrary code
//
// (*os.system exists only in the Windows Lua build; blocking it by name keeps
// the policy identical on every platform.)
//
// Entries are removed with the plain Lua C API (pushnil + setfield), so what
// survives keeps upstream 5.1 behaviour untouched. OA_LUA_STDLIB=stock skips
// the block list (A/B arm).
namespace {

bool stdlib_block_disabled() {
    const char* v = std::getenv("OA_LUA_STDLIB");
    return v && std::strcmp(v, "stock") == 0;
}

void drop_table_fields(lua_State* L, const char* tname, const char* const* names, size_t n) {
    lua_getglobal(L, tname);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        lua_pushnil(L);
        lua_setfield(L, -2, names[i]);
    }
    lua_pop(L, 1);
}

void apply_stdlib_block_list(lua_State* L) {
    if (stdlib_block_disabled()) return;

    static const char* const kOsBanned[] = {
        "execute", "exit", "remove", "rename", "setlocale", "tmpname", "system",
    };
    drop_table_fields(L, LUA_OSLIBNAME, kOsBanned, sizeof(kOsBanned) / sizeof(kOsBanned[0]));

    // io keeps open/close plus the handle-oriented helpers (read/write/lines/
    // seek/flush/setvbuf); only stream redirection and process spawn go.
    static const char* const kIoBanned[] = {"popen", "tmpfile", "input", "output"};
    drop_table_fields(L, LUA_IOLIBNAME, kIoBanned, sizeof(kIoBanned) / sizeof(kIoBanned[0]));

    static const char* const kGlobalBanned[] = {"dofile", "loadfile", "loadstring", "load"};
    for (const char* n : kGlobalBanned) {
        lua_pushnil(L);
        lua_setglobal(L, n);
    }

    // package keeps its table (frameworks read package.path) minus the native
    // loader and the writer tables.
    static const char* const kPkgBanned[] = {"loadlib", "loaders", "preload", "seeall"};
    drop_table_fields(L, "package", kPkgBanned, sizeof(kPkgBanned) / sizeof(kPkgBanned[0]));
}

}  // namespace

// ---------------------------------------------------------------------------
// Bridge object — VM boot, host->Lua calls, filter storage.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Bridge object.
// ---------------------------------------------------------------------------

LuaBridge::LuaBridge(LuaHost host) : host_(std::move(host)) {
    engine_ref_ = LUA_NOREF;
    tag_filter_ref_ = LUA_NOREF;
    event_filter_ref_ = LUA_NOREF;
    log_filter_value_ = 0;

    L_ = luaL_newstate();
    if (!L_) throw LuaError("luaL_newstate failed");
    luaL_openlibs(L_);
    apply_stdlib_block_list(L_);

    install_engine_api();
}

LuaBridge::~LuaBridge() {
    // Release luaL_ref references before closing the state.
    if (L_) {
        if (engine_ref_ != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, engine_ref_);
        if (tag_filter_ref_ != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, tag_filter_ref_);
        if (event_filter_ref_ != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, event_filter_ref_);
    }
    if (L_) lua_close(L_);
}

void LuaBridge::install_engine_api() {
    // Lua native-library boot (the analog of luaL_openlibs / linit.c): the
    // slices of the Lua host surface are the tag/sys/input/surface/emote/
    // filter/pluto sections of THIS file, each exporting one register_*
    // opener declared above. Slice sections
    // merge their static luaL_Reg tables into the engine handle's method
    // table on top of the stack; pluto owns its own global table.
    //
    // The Lua side meets the bridge as ONE engine handle (full userdata):
    //   * its payload stores the owning LuaBridge* (identity; the working
    //     recovery path is the method-closure upvalue, see
    //     bridge_from_state / merge_bridge_methods),
    //   * its metatable IS the method table — merged by the openers above —
    //     with __index pointing back at itself (method lookup) and a
    //     __metatable guard: scripts can call e:* methods but cannot
    //     enumerate, overwrite or re-metatable the API surface,
    //   * it is anchored by ONE anonymous luaL_ref (engine_ref_). The handle
    //     only ever reaches Lua as the first argument `e` of a host->Lua call
    //     (calllua rows / FPM (e, p) event handlers / tag + event filter
    //     callbacks) — every such call goes through push_engine, so scripts
    //     that only ever get the engine as a parameter (the original engine's
    //     contract) see a fully populated surface without any global name.

    LuaBridge** slot = static_cast<LuaBridge**>(lua_newuserdata(L_, sizeof(LuaBridge*)));
    *slot = this;
    lua_createtable(L_, 0, 64); // method table (= the metatable)
    register_tag(L_, this);
    register_sys(L_, this);
    register_input(L_, this);
    register_surface(L_, this);
    register_emote(L_, this);
    register_filter(L_, this);
    // metatable setup: duplicate the method table as the __index value
    lua_pushvalue(L_, -1);            // [handle, mt, mt]
    lua_pushboolean(L_, 0);
    lua_setfield(L_, -2, "__metatable"); // guard: no getmetatable/setmetatable
    lua_setfield(L_, -2, "__index");     // mt.__index = mt (classic OO)
    lua_setmetatable(L_, -2);            // handle gets its method table
    engine_ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);

    register_pluto(L_);
}

void LuaBridge::push_engine(lua_State* L) const {
    lua_rawgeti(L, LUA_REGISTRYINDEX, engine_ref_);
}

int64_t LuaBridge::next_random() {
    if (!random_seeded_) {
        random_state_ = (uint64_t)(host_.now_ms ? host_.now_ms() : 0);
        random_state_ ^= (uint64_t)std::time(nullptr) << 20;
        if (random_state_ == 0) random_state_ = 0x9E3779B97F4A7C15ull;
        random_seeded_ = true;
    }
    random_state_ = random_state_ * 6364136223846793005ull + 1442695040888963407ull;
    return (int64_t)((random_state_ >> 33) & 0x7fffffff);
}

void LuaBridge::run_code(const std::string& code, const std::string& chunk_name) {
    if (std::getenv("OA_LUA_DEBUG")) std::fprintf(stderr, "[lua] run_code: %s\n", chunk_name.c_str());
    if (luaL_loadbuffer(L_, code.data(), code.size(), chunk_name.c_str()) != 0) {
        const std::string msg = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "load error";
        lua_pop(L_, 1);
        throw LuaError(msg);
    }
    // Message handler below the chunk: pcall returns the error already
    // decorated with debug.traceback.
    lua_pushcfunction(L_, l_traceback);
    lua_insert(L_, -2); // [handler, chunk]
    const int status = lua_pcall(L_, 0, 0, -2);
    if (status != 0) {
        std::string msg = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "lua error";
        lua_pop(L_, 2);
        throw LuaError(msg);
    }
    lua_pop(L_, 1); // handler
}

bool LuaBridge::call_function(const std::string& dotted,
                              const std::map<std::string, std::string>& params) {
    if (std::getenv("OA_LUA_DEBUG"))
        std::fprintf(stderr, "[lua] call_function: %s\n", dotted.c_str());
    // OA_SELTRACE: state dump at every select_exit
    // chain entry (engine dispatch, see Interpreter::call_lua_function in
    // runtime_iet.cpp) plus direct select_click calls. Prints the Lua-side
    // story position and the scr.select state at the exact moment
    // clicknext/exittrans runs, so a user crash log (select.lua:482/484
    // family) identifies the chain that reached clicknext without a live
    // scr.select. Env-gated: zero output unless OA_SELTRACE is set.
    if (std::getenv("OA_SELTRACE") && dotted == "estag_call") {
        static uint64_t est_seq = 0;
        lua_getglobal(L_, "scr");
        std::string est;
        if (lua_istable(L_, -1)) {
            lua_getfield(L_, -1, "est");
            if (lua_istable(L_, -1)) {
                char buf[400];
                std::snprintf(buf, sizeof(buf), "est#%d", (int)lua_objlen(L_, -1));
                est = buf;
                for (int i = 1; i <= (int)lua_objlen(L_, -1); ++i) {
                    lua_rawgeti(L_, -1, i);
                    if (lua_istable(L_, -1)) {
                        lua_rawgeti(L_, -1, 1);
                        const char* tg = lua_tostring(L_, -1);
                        if (tg && est.size() < 360) {
                            est += " [";
                            est += tg;
                            est += "]";
                        }
                        lua_pop(L_, 1);
                    }
                    lua_pop(L_, 1);
                }
            }
            lua_pop(L_, 1);
        }
        lua_pop(L_, 1);
        std::fprintf(stderr, "[seltrace] s%llu estag_call %s ms=%llu\n",
                     (unsigned long long)++est_seq, est.c_str(),
                     (unsigned long long)(host_.now_ms ? host_.now_ms() : 0));
    }
    if (std::getenv("OA_SELTRACE") &&
        (dotted == "select_clicknext" || dotted == "select_exittrans" ||
         dotted == "select_click")) {
        static uint64_t sel_trace_seq = 0;
        auto field_str = [&](int tbl, const char* key) -> std::string {
            // reads tbl[key]; leaves the stack balanced (Lua 5.1 has no
            // lua_absindex: normalize negative indexes manually)
            if (lua_type(L_, tbl) != LUA_TTABLE) return "?";
            const int abs = tbl > 0 ? tbl : lua_gettop(L_) + tbl + 1;
            lua_getfield(L_, abs, key);
            const char* v = lua_tostring(L_, -1);
            std::string out = v ? v : "";
            if (out.empty() && !lua_isnil(L_, -1)) {
                if (lua_isnumber(L_, -1) || lua_isboolean(L_, -1)) {
                    const char* n = lua_tostring(L_, -1);
                    out = n ? n : "";
                }
            }
            lua_pop(L_, 1);
            return out.empty() ? "-" : out;
        };
        std::string ip_file, ip_block, ip_count, ip_sel;
        bool ip_ok = false;
        lua_getglobal(L_, "scr");
        const bool scr_tbl = lua_istable(L_, -1) != 0;
        if (scr_tbl) {
            lua_getfield(L_, -1, "ip");
            if (lua_istable(L_, -1)) {
                ip_ok = true;
                ip_file = field_str(-1, "file");
                ip_block = field_str(-1, "block");
                ip_count = field_str(-1, "count");
            }
            lua_pop(L_, 1); // ip
        }
        // select state: presence / # / id / autoselect / name / hide / wait
        std::string sel_state = "nil";
        if (scr_tbl) {
            lua_getfield(L_, -1, "select");
            if (lua_istable(L_, -1)) {
                char buf[320];
                std::string idv = field_str(-1, "id");
                std::string asv = field_str(-1, "autoselect");
                std::string nmv = field_str(-1, "name");
                std::string hdv = field_str(-1, "hide");
                std::string wtv = field_str(-1, "wait");
                std::snprintf(buf, sizeof(buf),
                              "tbl(#%d,id=%s,auto=%s,name=%s,hide=%s,wait=%s)",
                              (int)lua_objlen(L_, -1), idv.c_str(), asv.c_str(),
                              nmv.c_str(), hdv.c_str(), wtv.c_str());
                sel_state = buf;
            }
            lua_pop(L_, 1); // select
        }
        lua_pop(L_, 1); // scr
        std::string caller;
        lua_Debug ar;
        for (int lvl = 1; lvl <= 4; ++lvl) {
            if (!lua_getstack(L_, lvl, &ar)) break;
            if (!lua_getinfo(L_, "nSl", &ar)) continue;
            char one[512];
            // lua_Debug::short_src is a char array (never null).
            std::snprintf(one, sizeof(one), "%s @%s", ar.name ? ar.name : "?",
                          ar.short_src);
            caller += one;
            caller += "; ";
            if (caller.size() > 380) break;
        }
        std::fprintf(stderr,
                     "[seltrace] s%llu lua %s ms=%llu ip{file=%s block=%s "
                     "count=%s} select=%s lua_callers[%s]\n",
                     (unsigned long long)++sel_trace_seq, dotted.c_str(),
                     (unsigned long long)(host_.now_ms ? host_.now_ms() : 0),
                     ip_file.c_str(), ip_block.c_str(), ip_count.c_str(),
                     sel_state.c_str(), caller.c_str());
        (void)ip_ok;
    }
    // OA_NM_SELDBG: attribute select_* Lua calls — the engine
    // layer/push dispatch invokes the handler directly (no Lua caller) while
    // the keyconfig chain calls it from setonpush_calllua (Lua caller).
    if (std::getenv("OA_NM_SELDBG") && dotted.rfind("select_", 0) == 0) {
        static uint64_t sel_seq = 0;
        std::string caller;
        lua_Debug ar;
        for (int lvl = 1; lvl <= 4; ++lvl) {
            if (!lua_getstack(L_, lvl, &ar)) break;
            if (!lua_getinfo(L_, "n", &ar)) continue;
            if (ar.name) {
                caller = ar.name;
                break;
            }
        }
        std::fprintf(stderr, "[seldbg] s%llu lua %s caller=%s\n",
                     (unsigned long long)++sel_seq, dotted.c_str(),
                     caller.empty() ? "(engine)" : caller.c_str());
        if (dotted == "select_clicknext") {
            // state probe: scr.select rows + id at entry (kept off-stack)
            char idbuf[32] = "(nil)";
            lua_getglobal(L_, "scr");
            const bool scr_tbl = lua_istable(L_, -1) != 0;
            if (scr_tbl) {
                lua_getfield(L_, -1, "select");
                const bool sel_tbl = lua_istable(L_, -1) != 0;
                if (sel_tbl) {
                    lua_pushliteral(L_, "id");
                    lua_gettable(L_, -2);
                    if (lua_isnumber(L_, -1))
                        std::snprintf(idbuf, sizeof(idbuf), "%lld",
                                      (long long)lua_tonumber(L_, -1));
                    else if (lua_isstring(L_, -1))
                        std::snprintf(idbuf, sizeof(idbuf), "%s",
                                      lua_tostring(L_, -1));
                    lua_pop(L_, 1);
                    std::fprintf(stderr,
                                 "[seldbg]   clicknext probe: #select=%d id=%s\n",
                                 (int)lua_objlen(L_, -1), idbuf);
                } else {
                    std::fprintf(stderr, "[seldbg]   clicknext probe: scr.select=nil\n");
                }
                lua_pop(L_, 1); // select
            }
            lua_pop(L_, 1); // scr
        }
    }
    // Fast path then dotted walk from _G.
    if (!push_dotted_function(L_, dotted)) return false;
    // First argument: the engine handle (FPM (e, p) convention), re-pushed
    // from the anonymous registry anchor; there is no engine global to read.
    push_engine(L_);
    lua_createtable(L_, 0, (int)params.size());
    for (const auto& [k, v] : params) {
        lua_pushlstring(L_, v.data(), v.size());
        lua_setfield(L_, -2, k.c_str());
    }
    // lua_pcall expects [function, arg1, arg2] with the function first; the
    // traceback handler goes below it so errors come back pre-decorated.
    if (std::getenv("OA_LUA_DEBUG")) {
        std::fprintf(stderr, "[lua] pre-call stack: %d items (fn at -3)\n", lua_gettop(L_));
    }
    lua_pushcfunction(L_, l_traceback);
    lua_insert(L_, -4); // [handler, fn, engine, params]
    if (lua_pcall(L_, 2, 0, -4) != 0) {
        std::string msg = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "lua error";
        lua_pop(L_, 2); // error + handler
        throw LuaError(msg);
    }
    lua_pop(L_, 1); // handler
    return true;
}

bool LuaBridge::call_plain(const std::string& dotted) {
    if (!push_dotted_function(L_, dotted)) return false;
    lua_pushcfunction(L_, l_traceback);
    lua_insert(L_, -2); // [handler, fn]
    if (lua_pcall(L_, 0, 0, -2) != 0) {
        std::string msg = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "lua error";
        lua_pop(L_, 2); // error + handler
        throw LuaError(msg);
    }
    lua_pop(L_, 1); // handler
    return true;
}

void LuaBridge::report_dispatch_error(const char* what, const std::string& name,
                                      const std::string& msg) const {
    // Fail-fast escape hatch (see the header contract): the pre-130 behavior.
    if (std::getenv("OA_LUA_STRICT")) throw LuaError(msg);
    // Rate limit per (what,name): a broken per-frame handler (vsync) would
    // otherwise log at frame rate — the reference runtime guards it exactly
    // this way.
    static std::map<std::string, uint64_t> seen;
    const std::string key = std::string(what) + " " + name;
    const uint64_t n = ++seen[key];
    if (n > 3 && (n % 600) != 0) return;
    std::fprintf(stderr, "[lua] %s error: %s%s%s (#%llu)\n", what,
                 name.empty() ? "" : name.c_str(), name.empty() ? "" : ": ",
                 msg.c_str(), (unsigned long long)n);
    if (n == 1) {
        std::fprintf(stderr,
                     "[lua]   (game-Lua error tolerated — original-runtime policy, "
                     "docs/research/130; OA_LUA_STRICT=1 restores fail-fast)\n");
    }
}

void LuaBridge::include_file(const std::string& path) {
    if (!host_.read_file) {
        std::fprintf(stderr, "[lua] e:include: no file reader; cannot include %s\n", path.c_str());
        return;
    }
    const auto data = host_.read_file(path);
    if (!data) {
        std::fprintf(stderr, "[lua] e:include: file not found: %s\n", path.c_str());
        return;
    }
    run_code(std::string(reinterpret_cast<const char*>(data->data()), data->size()), path);
}

std::string LuaBridge::convert_encoding(const std::string& from, const std::string& to,
                                        const std::string& source) {
    auto norm = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        }
        if (s == "sjis") s = "shift_jis";
        if (s == "utf8") s = "utf-8";
        return s;
    };
    const std::string f = norm(from);
    const std::string t = norm(to);
    // UTF-8 variants are identity; Shift_JIS decode is pending (table work).
    if (f == t) return source;
    if (t.find("utf-8") != std::string::npos) return source;
    if (t.find("shift_jis") != std::string::npos) {
        // ASCII subset stays identical; multibyte would need the table
        return source;
    }
    return source;
}
// ---------------------------------------------------------------------------
// §2 tag — e:tag / e:enqueueTag.
// ---------------------------------------------------------------------------


namespace {

int l_tag(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    std::map<std::string, std::string> params;
    std::string tag;
    if (lua_istable(L, 2)) {
        tag = read_command_table(L, 2, &params);
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        tag = value_to_string(L, 2);
        if (lua_istable(L, 3)) (void)read_command_table(L, 3, &params);
    }
    if (tag.empty()) {
        lua_pushliteral(L, "e:tag requires a tag name");
        return lua_error(L);
    }
    if (tag == "var") {
        if (!bridge->host().apply_var_tag) {
            lua_pushliteral(L, "var application not wired");
            return lua_error(L);
        }
        if (!bridge->host().apply_var_tag(params)) {
            lua_pushliteral(L, "var application failed");
            return lua_error(L);
        }
        return 0;
    }
    // e:tag{"calllua",...} from Lua runs synchronously (the
    // .asb [calllua] tag runs inline; the deferred queue drain let the
    // enclosing Lua flow rebuild state — button groups, dialogs — before the
    // callback executed, so per-button out handlers read a stale group and
    // game code indexed nil (thyt exui.lua:100 crash family). Errors inside
    // the nested call are converted to a regular Lua error at this pcall
    // boundary (the outer engine call unwinds cleanly). A missing/empty
    // "function" keeps the historical queue path so its drain-time error
    // surfaces unchanged.
    if (tag == "calllua" && bridge->host().run_calllua_sync) {
        auto fnit = params.find("function");
        const std::string fn = fnit == params.end() ? std::string() : fnit->second;
        if (!fn.empty()) {
            try {
                if (bridge->host().run_calllua_sync(fn, params)) return 0;
                // dispatch hook not wired / unresolved -> queue (drain-time
                // behavior for degenerate calls).
            } catch (const std::exception& ex) {
                const std::string msg = ex.what();
                lua_pushlstring(L, msg.data(), msg.size());
                return lua_error(L);
            }
        }
    }
    if (!bridge->host().enqueue_tag) {
        lua_pushliteral(L, "tag queue not wired");
        return lua_error(L);
    }
    bridge->host().enqueue_tag(std::move(tag), std::move(params));
    return 0;
}

int l_enqueue_tag(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    std::map<std::string, std::string> params;
    std::string tag;
    if (lua_istable(L, 2)) {
        tag = read_command_table(L, 2, &params);
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        tag = value_to_string(L, 2);
        if (lua_istable(L, 3)) (void)read_command_table(L, 3, &params);
    }
    if (tag.empty()) {
        lua_pushliteral(L, "e:enqueueTag requires a tag name");
        return lua_error(L);
    }
    if (!bridge->host().enqueue_tag) {
        lua_pushliteral(L, "tag queue not wired");
        return lua_error(L);
    }
    bridge->host().enqueue_tag(std::move(tag), std::move(params));
    return 0;
}

const luaL_Reg kTagMethods[] = {
    {"tag", l_tag},
    {"enqueueTag", l_enqueue_tag},
    {nullptr, nullptr},
};

} // namespace

// Opener: the engine handle's method table is on top of the stack; merge the
// methods in as bridge-carrying closures.
static void register_tag(lua_State* L, LuaBridge* bridge) {
    merge_bridge_methods(L, bridge, kTagMethods);
}

// ---------------------------------------------------------------------------
// §3 sys — variables/clock/random/include/file/status/handlers/debug.
// ---------------------------------------------------------------------------


namespace {

int l_var(lua_State* L) {
    const LuaBridge* bridge = bridge_from_state(L);
    const std::string name = value_to_string(L, 2);
    const oa::runtime::Value* v = bridge->host().get_var ? bridge->host().get_var(name) : nullptr;
    if (!v || v->kind == oa::runtime::ValueKind::Null) {
        lua_pushliteral(L, "0"); // never nil; missing -> "0"
        return 1;
    }
    // e:var 语义 = 变量的文本形态(与 Artemis 一致):数值/布尔一律以字符串
    // 呈现("0"/"1"/"500"),缺失 -> "0"。mkmh(2023+) 框架对 e:var 结果做
    // 字符串操作(== "0"、:gsub、字符串连接),数字返回会直接炸
    // (attempt to index a number value);需要算术的地方都显式 tonumber()。
    // 老游戏(fpm/NekoMiko/thyt)的 Lua 对数值变量一律字符串比较(== "0"),
    // 数值返回反而是错的;脚本侧 $ 表达式不受影响(内部仍是 Value)。
    switch (v->kind) {
        case oa::runtime::ValueKind::Int: {
            // lua_pushfstring 不支持 %lld(C 格式);经 snprintf 中转。
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", (long long)v->int_val);
            lua_pushstring(L, buf);
            return 1;
        }
        case oa::runtime::ValueKind::Float: {
            char buf[40];
            const double d = v->float_val;
            if (d == (long long)d && d < 1e15 && d > -1e15) {
                std::snprintf(buf, sizeof(buf), "%lld", (long long)d);
            } else {
                std::snprintf(buf, sizeof(buf), "%.6g", d);
            }
            lua_pushstring(L, buf);
            return 1;
        }
        case oa::runtime::ValueKind::Bool:
            if (v->bool_val) {
                lua_pushliteral(L, "1");
            } else {
                lua_pushliteral(L, "0");
            }
            return 1;
        case oa::runtime::ValueKind::String:
            lua_pushlstring(L, v->str_val.data(), v->str_val.size());
            return 1;
        default:
            lua_pushliteral(L, "0");
            return 1;
    }
}

int l_now(lua_State* L) {
    const LuaBridge* bridge = bridge_from_state(L);
    lua_pushnumber(L, (lua_Number)(bridge->host().now_ms ? bridge->host().now_ms() : 0));
    return 1;
}

int l_random(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    lua_pushnumber(L, (lua_Number)bridge->next_random());
    return 1;
}

int l_include(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    const std::string path = value_to_string(L, 2);
    if (path.empty()) {
        lua_pushliteral(L, "e:include: empty path");
        return lua_error(L);
    }
    // A broken include abandons ONLY the include (research/130 policy): the
    // caller keeps running, exactly like the original runtime's include
    // ("include load/run error" -> log, continue). Without the guard the
    // LuaError would unwind out of this C function through the Lua VM.
    try {
        bridge->include_file(path);
    } catch (const LuaError& e) {
        bridge->report_dispatch_error("include", path, e.what());
    }
    return 0;
}

int l_is_file_exists(lua_State* L) {
    const LuaBridge* bridge = bridge_from_state(L);
    const std::string path = value_to_string(L, 2);
    bool ok = false;
    if (bridge->host().is_file_exists) {
        ok = bridge->host().is_file_exists(path);
    } else if (bridge->host().read_file) {
        ok = bridge->host().read_file(path).has_value();
    }
    lua_pushboolean(L, ok);
    return 1;
}

int l_file(lua_State* L) {
    const LuaBridge* bridge = bridge_from_state(L);
    if (lua_type(L, 2) == LUA_TSTRING) {
        const std::string path = value_to_string(L, 2);
        if (bridge->host().read_file) {
            const auto data = bridge->host().read_file(path);
            if (data) {
                lua_pushlstring(L, reinterpret_cast<const char*>(data->data()), data->size());
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }
    lua_pushnil(L); // {command=...} operations land in later milestones
    return 1;
}

int l_get_frame_number(lua_State* L) {
    const LuaBridge* bridge = bridge_from_state(L);
    lua_pushnumber(L, (lua_Number)(bridge->host().frame_number
                                       ? bridge->host().frame_number()
                                       : 0));
    return 1;
}

int l_get_script_status(lua_State* L) {
    const LuaBridge* bridge = bridge_from_state(L);
    lua_pushnumber(L, bridge->host().get_script_status ? bridge->host().get_script_status() : 0);
    return 1;
}

int l_set_script_status(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    const int status = (int)lua_tonumber(L, 2);
    if (bridge->host().set_script_status) bridge->host().set_script_status(status);
    return 0;
}

int l_debug_skip(lua_State* L) {
    // FPM e:debugSkip{index=99999} (次の選択肢に進む / 高速スキップ): starts
    // the engine exskip fast-forward state (runtime side, see
    // GameRuntime::start_debug_skip). index carries no registered meaning
    // (behavioral inference; logged by the runtime under OA_EXSKIPDBG).
    LuaBridge* bridge = bridge_from_state(L);
    int64_t index = 0;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "index");
        if (lua_isnumber(L, -1)) index = (int64_t)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    if (bridge->host().debug_skip_start) bridge->host().debug_skip_start(index);
    return 0;
}

int l_set_event_handler(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    if (!lua_istable(L, 2)) return 0;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        const std::string ev = value_to_string(L, -2);
        const std::string fn = lua_isnil(L, -1) ? "" : value_to_string(L, -1);
        if (bridge->host().set_event_handler) bridge->host().set_event_handler(ev, fn);
        lua_pop(L, 1);
    }
    return 0;
}

int l_debug(lua_State* L) {
    int level = 0;
    std::string data;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "level");
        if (lua_isnumber(L, -1)) level = (int)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "data");
        data = value_to_string(L, -1);
        lua_pop(L, 1);
    } else {
        data = value_to_string(L, 2);
    }
    if (std::getenv("OA_LUA_DEBUG")) {
        std::fprintf(stderr, "[lua-debug lv=%d] %s\n", level, data.c_str());
    }
    return 0;
}

int l_set_magic_path(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    std::string name;
    std::string path;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "name");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_rawgeti(L, 2, 1);
        }
        name = value_to_string(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "path");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_rawgeti(L, 2, 2);
        }
        path = value_to_string(L, -1);
        lua_pop(L, 1);
    } else if (lua_type(L, 2) == LUA_TSTRING && lua_type(L, 3) == LUA_TSTRING) {
        name = value_to_string(L, 2);
        path = value_to_string(L, 3);
    }
    if (bridge->host().set_magic_path && !name.empty()) bridge->host().set_magic_path(name, path);
    return 0;
}

int l_exit(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    if (bridge->host().enqueue_tag) bridge->host().enqueue_tag("exit", {});
    return 0;
}

int l_get_wait_reason(lua_State* L) {
    const LuaBridge* bridge = bridge_from_state(L);
    lua_createtable(L, 0, 4);
    if (bridge->host().get_wait_reason) {
        for (const auto& [k, v] : bridge->host().get_wait_reason()) {
            lua_pushstring(L, v.c_str());
            lua_setfield(L, -2, k.c_str());
        }
    }
    return 1;
}

int l_convert_encoding(lua_State* L) {
    std::string from, to, source;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "from");
        from = value_to_string(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "to");
        to = value_to_string(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "source");
        source = value_to_string(L, -1);
        lua_pop(L, 1);
    } else {
        from = value_to_string(L, 2);
        to = value_to_string(L, 3);
        source = value_to_string(L, 4);
    }
    const std::string out = LuaBridge::convert_encoding(from, to, source);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// Unimplemented engine surface: registered as inert no-ops.
int l_noop(lua_State* L) {
    (void)L;
    return 0;
}

// getScriptBlock/getScriptStack default stubs (host fills them later).
int l_get_script_block(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

int l_get_script_stack(lua_State* L) {
    lua_createtable(L, 0, 0);
    return 1;
}

const luaL_Reg kSysMethods[] = {
    {"debug", l_debug},
    {"var", l_var},
    {"now", l_now},
    {"random", l_random},
    {"include", l_include},
    {"file", l_file},
    {"isFileExists", l_is_file_exists},
    {"getScriptStatus", l_get_script_status},
    {"getFrameNumber", l_get_frame_number},
    {"setScriptStatus", l_set_script_status},
    {"debugSkip", l_debug_skip},
    {"setEventHandler", l_set_event_handler},
    {"getScriptWaitReason", l_get_wait_reason},
    {"setMagicPath", l_set_magic_path},
    {"exit", l_exit},
    {"convertEncoding", l_convert_encoding},
    {"getScriptBlock", l_get_script_block},
    {"getScriptStack", l_get_script_stack},
    {"callShellExecute", l_noop},
    {"writeClipboard", l_noop},
    {"setScriptStack", l_noop},
    {"setMasterVolume", l_noop},
    {"setBgmVolume", l_noop},
    {"setSeVolume", l_noop},
    {"setVoiceVolume", l_noop},
    {nullptr, nullptr},
};

} // namespace

// Opener: the engine handle's method table is on top of the stack.
static void register_sys(lua_State* L, LuaBridge* bridge) {
    merge_bridge_methods(L, bridge, kSysMethods);
}

// ---------------------------------------------------------------------------
// §4 input — input/touch/mouse/overrideKey.
// ---------------------------------------------------------------------------
// (the former lua_input.cpp copy of l_noop was dropped — the identical sys
// copy serves both method tables.)


namespace {

int l_input_query(lua_State* L, const char* which) {
    const LuaBridge* bridge = bridge_from_state(L);
    const int key = (int)lua_tonumber(L, 2);
    bool result = false;
    if (bridge->host().is_down && std::strcmp(which, "down") == 0)
        result = bridge->host().is_down(key);
    if (bridge->host().is_down_edge && std::strcmp(which, "downedge") == 0)
        result = bridge->host().is_down_edge(key);
    if (bridge->host().is_up_edge && std::strcmp(which, "upedge") == 0)
        result = bridge->host().is_up_edge(key);
    if (bridge->host().is_push && std::strcmp(which, "push") == 0)
        result = bridge->host().is_push(key);
    if (bridge->host().is_decide && std::strcmp(which, "decide") == 0)
        result = bridge->host().is_decide(key);
    lua_pushboolean(L, result);
    return 1;
}

int l_is_down(lua_State* L) { return l_input_query(L, "down"); }
int l_is_down_edge(lua_State* L) { return l_input_query(L, "downedge"); }
int l_is_up_edge(lua_State* L) { return l_input_query(L, "upedge"); }
int l_is_push(lua_State* L) { return l_input_query(L, "push"); }
int l_is_decide(lua_State* L) { return l_input_query(L, "decide"); }

int l_touch_query(lua_State* L, const char* which) {
    if (std::strcmp(which, "count") == 0) {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int l_get_touch_count(lua_State* L) { return l_touch_query(L, "count"); }
int l_get_touch_point(lua_State* L) { return l_touch_query(L, "point"); }

int l_get_mouse_point(lua_State* L) {
    const LuaBridge* bridge = bridge_from_state(L);
    std::pair<int, int> p{0, 0};
    if (bridge->host().get_mouse_point) p = bridge->host().get_mouse_point();
    lua_createtable(L, 0, 2);
    lua_pushnumber(L, p.first);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, p.second);
    lua_setfield(L, -2, "y");
    return 1;
}

int l_override_key(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    int key = -1;
    int status = 0;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "key");
        if (lua_isnumber(L, -1)) key = (int)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "status");
        if (lua_isnumber(L, -1)) status = (int)lua_tonumber(L, -1);
        lua_pop(L, 1);
    } else if (lua_type(L, 2) == LUA_TNUMBER) {
        key = (int)lua_tonumber(L, 2);
        if (lua_type(L, 3) == LUA_TNUMBER) status = (int)lua_tonumber(L, 3);
    }
    if (bridge->host().override_key && key >= 0) bridge->host().override_key(key, status);
    return 0;
}


const luaL_Reg kInputMethods[] = {
    {"isDown", l_is_down},
    {"isDownEdge", l_is_down_edge},
    {"isUpEdge", l_is_up_edge},
    {"isPush", l_is_push},
    {"isDecide", l_is_decide},
    {"getTouchCount", l_get_touch_count},
    {"getTouchPoint", l_get_touch_point},
    {"getMousePoint", l_get_mouse_point},
    {"overrideKey", l_override_key},
    {"setUseMultiTouch", l_noop},
    {"setUseTouchHold", l_noop},
    {"setFlickSensitivity", l_noop},
    {nullptr, nullptr},
};

} // namespace

// Opener: the engine handle's method table is on top of the stack.
static void register_input(lua_State* L, LuaBridge* bridge) {
    merge_bridge_methods(L, bridge, kInputMethods);
}

// ---------------------------------------------------------------------------
// §5 surface — surface/font-cache operations.
// ---------------------------------------------------------------------------


namespace {

int l_surface_query(lua_State* L, const char* which) {
    LuaBridge* bridge = bridge_from_state(L);
    const std::string path = lua_type(L, 2) == LUA_TSTRING ? value_to_string(L, 2) : "";
    if (std::strcmp(which, "bind_async") == 0) {
        bool ok = true;
        if (bridge->host().bind_surface_async) ok = bridge->host().bind_surface_async(path);
        lua_pushboolean(L, ok);
        return 1;
    }
    if (std::strcmp(which, "unbind") == 0) {
        if (bridge->host().unbind_surface) bridge->host().unbind_surface(path);
        return 0;
    }
    if (std::strcmp(which, "clear_queue") == 0) {
        if (bridge->host().clear_surface_load_queue) bridge->host().clear_surface_load_queue();
        return 0;
    }
    if (std::strcmp(which, "is_loading") == 0) {
        bool loading = false;
        if (bridge->host().is_loading_surface) loading = bridge->host().is_loading_surface();
        lua_pushboolean(L, loading);
        return 1;
    }
    if (std::strcmp(which, "restore_font") == 0) {
        if (bridge->host().restore_font_cache) bridge->host().restore_font_cache();
        return 0;
    }
    if (std::strcmp(which, "load_png_comments") == 0) {
        if (bridge->host().load_png_comments) {
            const auto comments = bridge->host().load_png_comments(path);
            if (comments) {
                lua_createtable(L, 0, (int)comments->size());
                for (const auto& [k, v] : *comments) {
                    lua_pushlstring(L, v.data(), v.size());
                    lua_setfield(L, -2, k.c_str());
                }
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int l_bind_surface_async(lua_State* L) { return l_surface_query(L, "bind_async"); }
int l_bind_surface(lua_State* L) { return l_surface_query(L, "bind_async"); }
int l_unbind_surface(lua_State* L) { return l_surface_query(L, "unbind"); }
int l_clear_surface_load_queue(lua_State* L) { return l_surface_query(L, "clear_queue"); }
int l_is_loading_surface(lua_State* L) { return l_surface_query(L, "is_loading"); }
int l_restore_font_cache(lua_State* L) { return l_surface_query(L, "restore_font"); }
int l_load_png_comments(lua_State* L) { return l_surface_query(L, "load_png_comments"); }

const luaL_Reg kSurfaceMethods[] = {
    {"bindSurfaceAsync", l_bind_surface_async},
    {"bindSurface", l_bind_surface},
    {"unbindSurface", l_unbind_surface},
    {"clearSurfaceLoadQueue", l_clear_surface_load_queue},
    {"isLoadingSurface", l_is_loading_surface},
    {"restoreFontCache", l_restore_font_cache},
    {"loadPngComments", l_load_png_comments},
    {nullptr, nullptr},
};

} // namespace

// Opener: the engine handle's method table is on top of the stack.
static void register_surface(lua_State* L, LuaBridge* bridge) {
    merge_bridge_methods(L, bridge, kSurfaceMethods);
}

// ---------------------------------------------------------------------------
// §6 emote — E-mote layer objects.
// ---------------------------------------------------------------------------


namespace {

const char* const kEmoteLayerMethods[] = {
    "setScale", "setCoord", "playTimeline", "fadeInTimeline", "stopTimeline",
    "fadeOutTimeline", "pass", "step", "skip", "setVariable", "getVariable",
    "stop", "getTimelinePlaying", "isTimelinePlaying", "progress",
    // KukkoroDays system/adv/emote.lua (research/127) — the older Windows
    // framework variant saves/restores the whole variable domain around a pose
    // change, so the layer object needs the domain enumeration and the 差分
    // (difference) writer:
    //   playTimeline(): for i=0, layer:countVariables() do
    //                       local l = layer:getVariableLabelAt(i) ... end
    //                   layer:setVariableDiff("cheek", "face_cheek", diff,0,0)
    //   vsync():        layer:getVariable("face_cheek") + setVariableDiff
    "countVariables", "getVariableLabelAt", "setVariableDiff",
};

int l_emote_layer_method(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    const char* which =
        static_cast<const char*>(lua_touserdata(L, lua_upvalueindex(2)));
    std::string id;
    if (lua_type(L, lua_upvalueindex(3)) == LUA_TSTRING)
        id = lua_tostring(L, lua_upvalueindex(3));

    // parse arguments by method family. Lua method syntax puts the self
    // table at index 1: string-first methods take their string at 2 and
    // numbers at 3..5 (setVariableDiff is the two-string shape:
    // srcLabel at 2, dstLabel at 3, value at 4); numeric-first methods
    // (setScale/setCoord/setZoom, progress(ct) — 甜蜜女友3
    // ex.progress/emote.vsync — and getVariableLabelAt(index))
    // take their numbers at 2..4. For playTimeline this is exactly the
    // official SDK PlayTimeline(label, flags) shape: the
    // label lands in s and the flags bitfield in n1 (arg 3), which
    // runtime_media forwards to EmotePlayer::play_timeline(label, flags) —
    // TIMELINE_PLAY_PARALLEL=1 appends to the foreground list.
    const bool two_string = std::strcmp(which, "setVariableDiff") == 0;
    std::string s;
    std::string s2;
    double n1 = 0, n2 = 0, n3 = 0;
    bool want_string = std::strcmp(which, "playTimeline") == 0 ||
                       std::strcmp(which, "fadeInTimeline") == 0 ||
                       std::strcmp(which, "stopTimeline") == 0 ||
                       std::strcmp(which, "fadeOutTimeline") == 0 ||
                       std::strcmp(which, "setVariable") == 0 ||
                       std::strcmp(which, "getVariable") == 0 ||
                       std::strcmp(which, "isTimelinePlaying") == 0 ||
                       two_string;
    bool numeric_first = std::strcmp(which, "setScale") == 0 ||
                         std::strcmp(which, "setCoord") == 0 ||
                         std::strcmp(which, "setZoom") == 0 ||
                         std::strcmp(which, "progress") == 0 ||
                         std::strcmp(which, "getVariableLabelAt") == 0;
    if (want_string && lua_type(L, 2) == LUA_TSTRING) s = lua_tostring(L, 2);
    if (two_string && lua_type(L, 3) == LUA_TSTRING) s2 = lua_tostring(L, 3);
    const int nb = numeric_first ? 2 : (two_string ? 4 : 3); // first numeric arg
    if (lua_isnumber(L, nb)) n1 = lua_tonumber(L, nb);
    if (lua_isnumber(L, nb + 1)) n2 = lua_tonumber(L, nb + 1);
    if (lua_isnumber(L, nb + 2)) n3 = lua_tonumber(L, nb + 2);

    double out = 0;
    std::string sout;
    bool routed = false;
    if (bridge && bridge->host().emote_method) {
        try {
            routed = bridge->host().emote_method(id, which, s, s2, n1, n2, n3,
                                                 &out, &sout);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[emote] %s(%s) error: %s\n", which, id.c_str(),
                         ex.what());
            routed = false;
        }
    }
    if (!routed) {
        // runtime-less hosts (pure lua tests): keep the placeholder shape
        if (std::strcmp(which, "getVariable") == 0) out = 0.0;
        else out = 0.0;
    }
    // String-returning getter (the domain enumeration label).
    if (std::strcmp(which, "getVariableLabelAt") == 0) {
        lua_pushlstring(L, sout.data(), sout.size());
        return 1;
    }
    if (std::strcmp(which, "getVariable") == 0 ||
        std::strcmp(which, "getTimelinePlaying") == 0 ||
        std::strcmp(which, "isTimelinePlaying") == 0 ||
        std::strcmp(which, "countVariables") == 0) {
        lua_pushnumber(L, out);
        return 1;
    }
    return 0;
}

void lua_push_emote_layer_object(lua_State* L, LuaBridge* bridge, const std::string& id) {
    lua_createtable(L, 0, int(sizeof(kEmoteLayerMethods) / sizeof(kEmoteLayerMethods[0])));
    for (const char* const m : kEmoteLayerMethods) {
        lua_pushlightuserdata(L, bridge);               // upvalue 1 (engine convention)
        lua_pushlightuserdata(L, const_cast<char*>(m)); // static literals, no free
        lua_pushlstring(L, id.data(), id.size());       // per-object layer id
        lua_pushcclosure(L, l_emote_layer_method, 3);
        lua_setfield(L, -2, m);
    }
}

int l_create_emote_layer(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    std::string id;
    int width = 0, height = 0;
    std::string files;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "id");
        id = value_to_string(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "width");
        if (lua_isnumber(L, -1)) width = int(lua_tointeger(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 2, "height");
        if (lua_isnumber(L, -1)) height = int(lua_tointeger(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 2, "files");
        if (lua_istable(L, -1)) {
            // join with \x1f; psb paths may contain backslashes on win
            // archives (game files are 'image\fhd\...' style)
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                if (lua_type(L, -1) == LUA_TSTRING) {
                    if (!files.empty()) files += '\x1f';
                    files += value_to_string(L, -1);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        id = value_to_string(L, 2);
    }
    if (id.empty() || files.empty()) {
        // Missing/unsupported payload: keep the object shape (the Lua chain
        // stores chars[ch].layer and calls methods on it), no texture.
        lua_push_emote_layer_object(L, bridge, id);
        return 1;
    }
    // Real static layer. Materialize the carrier chain
    // through the normal tag queue (an empty lyc2 at the full dotted id
    // creates every missing ancestor too), then ask the runtime to decode
    // the first psb file and render the static pose onto that layer.
    if (bridge->host().enqueue_tag) {
        bridge->host().enqueue_tag("lyc2", {{"id", id}});
        std::map<std::string, std::string> p;
        p["id"] = id;
        p["files"] = files;
        if (width > 0) p["width"] = std::to_string(width);
        if (height > 0) p["height"] = std::to_string(height);
        bridge->host().enqueue_tag("emotestatic", std::move(p));
    }
    lua_push_emote_layer_object(L, bridge, id);
    return 1;
}

int l_get_emote_layer(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    std::string id;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "id");
        id = value_to_string(L, -1);
        lua_pop(L, 1);
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        id = value_to_string(L, 2);
    }
    if (id.empty()) {
        lua_pushnil(L);
        return 1;
    }
    // `next` (save/trans re-acquire) carries no extra state; the runtime
    // layer (player) is looked up per method call by id.
    lua_push_emote_layer_object(L, bridge, id);
    return 1;
}

const luaL_Reg kEmoteMethods[] = {
    {"createEmoteLayer", l_create_emote_layer},
    {"getEmoteLayer", l_get_emote_layer},
    {nullptr, nullptr},
};

} // namespace

// Opener: the engine handle's method table is on top of the stack.
static void register_emote(lua_State* L, LuaBridge* bridge) {
    merge_bridge_methods(L, bridge, kEmoteMethods);
}

// ---------------------------------------------------------------------------
// §7 filter — tag/event/log filter setters + dispatch.
// ---------------------------------------------------------------------------


namespace {

int l_set_tag_filter(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    bridge->store_tag_filter(L, 2);
    return 0;
}

int l_set_event_filter(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    bridge->store_event_filter(L, 2);
    return 0;
}

int l_set_log_filter(lua_State* L) {
    LuaBridge* bridge = bridge_from_state(L);
    bridge->store_log_filter(L, 2);
    return 0;
}

const luaL_Reg kFilterMethods[] = {
    {"setTagFilter", l_set_tag_filter},
    {"setEventFilter", l_set_event_filter},
    {"setLogFilter", l_set_log_filter},
    {nullptr, nullptr},
};

} // namespace

// Opener: the engine handle's method table is on top of the stack.
static void register_filter(lua_State* L, LuaBridge* bridge) {
    merge_bridge_methods(L, bridge, kFilterMethods);
}

void LuaBridge::store_tag_filter(lua_State* L, int idx) {
    if (tag_filter_ref_ != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, tag_filter_ref_);
        tag_filter_ref_ = LUA_NOREF;
    }
    if (!lua_isnil(L, idx) && lua_istable(L, idx)) {
        lua_pushvalue(L, idx);
        tag_filter_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
    }
}

void LuaBridge::store_event_filter(lua_State* L, int idx) {
    if (event_filter_ref_ != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, event_filter_ref_);
        event_filter_ref_ = LUA_NOREF;
    }
    if (!lua_isnil(L, idx) && lua_isfunction(L, idx)) {
        lua_pushvalue(L, idx);
        event_filter_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
    }
}

void LuaBridge::store_log_filter(lua_State* L, int idx) {
    // 0 = raw output (default), 1 = suppressed; non-number clears.
    log_filter_value_ = lua_isnumber(L, idx) ? (int)lua_tonumber(L, idx) : 0;
}

FilterDecision LuaBridge::run_tag_filter(const std::string& tag,
                                         const std::map<std::string, std::string>& params) {
    if (tag_filter_ref_ == LUA_NOREF) return FilterDecision::Missing;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, tag_filter_ref_);
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        return FilterDecision::Missing;
    }
    size_t start = 0;
    for (;;) {
        const size_t dot = tag.find('.', start);
        const std::string part =
            dot == std::string::npos ? tag.substr(start) : tag.substr(start, dot - start);
        lua_getfield(L_, -1, part.c_str());
        lua_remove(L_, -2);
        if (lua_isnil(L_, -1)) {
            lua_pop(L_, 1);
            return FilterDecision::Missing;
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        return FilterDecision::Missing;
    }
    // First argument: the engine handle (FPM (e, p) convention), re-pushed
    // from the anonymous registry anchor.
    push_engine(L_);
    lua_createtable(L_, 0, (int)params.size());
    for (const auto& [k, v] : params) {
        lua_pushlstring(L_, v.data(), v.size());
        lua_setfield(L_, -2, k.c_str());
    }
    lua_pushcfunction(L_, l_traceback);
    lua_insert(L_, -4); // [handler, fn, engine, params]
    if (lua_pcall(L_, 2, 1, -4) != 0) {
        const std::string msg = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "lua error";
        lua_pop(L_, 2); // error + handler
        throw LuaError("tag filter " + tag + ": " + msg);
    }
    bool consume = false;
    switch (lua_type(L_, -1)) {
        case LUA_TNIL:
            consume = false;
            break;
        case LUA_TBOOLEAN:
            consume = lua_toboolean(L_, -1) != 0;
            break;
        case LUA_TNUMBER:
            consume = lua_tonumber(L_, -1) != 0.0;
            break;
        case LUA_TSTRING: {
            char* end = nullptr;
            const double d = std::strtod(lua_tostring(L_, -1), &end);
            consume = end != lua_tostring(L_, -1) && d != 0.0;
            break;
        }
        default:
            consume = true;
            break;
    }
    lua_pop(L_, 2); // verdict + handler
    return consume ? FilterDecision::Consume : FilterDecision::PassThrough;
}

std::optional<int> LuaBridge::run_event_filter(
    const std::string& name, const std::map<std::string, std::string>& params) {
    // No filter installed: zero-overhead path.
    if (event_filter_ref_ == LUA_NOREF) return std::nullopt;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, event_filter_ref_);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        return std::nullopt;
    }
    // eventFilter(e, name, param): e is the engine handle (the opaque
    // userdata host->Lua calls pass as the first argument, re-pushed from
    // the registry anchor); name and the string-valued params follow.
    push_engine(L_);
    lua_pushlstring(L_, name.data(), name.size());
    lua_createtable(L_, 0, (int)params.size());
    for (const auto& [k, v] : params) {
        lua_pushlstring(L_, v.data(), v.size());
        lua_setfield(L_, -2, k.c_str());
    }
    // stack: [handler, fn, e, name, params]
    lua_pushcfunction(L_, l_traceback);
    lua_insert(L_, -5);
    if (lua_pcall(L_, 3, 1, -5) != 0) {
        const std::string msg = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "lua error";
        lua_pop(L_, 2); // error + handler
        std::fprintf(stderr, "[lua] eventFilter %s error: %s\n", name.c_str(), msg.c_str());
        return std::nullopt;
    }
    std::optional<int> verdict;
    switch (lua_type(L_, -1)) {
        case LUA_TNUMBER: {
            const double d = lua_tonumber(L_, -1);
            if (d == 1.0 || d == 2.0) verdict = (int)d;
            break;
        }
        default:
            break; // nil / boolean / string: not 1 or 2 -> engine dispatch
    }
    lua_pop(L_, 2); // verdict + handler
    return verdict;
}

// ---------------------------------------------------------------------------
// §8 pluto — global `pluto` persist/unpersist codec.
// ---------------------------------------------------------------------------


namespace {

struct PersistError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr int kPlutoMaxDepth = 200;

void write_pluto_number(oa::util::Writer& w, double v) {
    // Integral doubles inside the exact-int64 range ride the minimal-width
    // int ladder (save tables are number-heavy); everything else bit-exact f64.
    double ipart = 0;
    if (std::isfinite(v) && std::modf(v, &ipart) == 0.0 &&
        std::fabs(v) < 9007199254740992.0) {
        w.i64((int64_t)v);
    } else {
        w.f64(v);
    }
}

void write_pluto_value(lua_State* L, int idx, oa::util::Writer& w, int depth) {
    if (depth > kPlutoMaxDepth) throw PersistError("pluto persist: nesting too deep");
    // snapshot absolute index: later pushes invalidate relative indices
    const int abs = idx > 0 ? idx : lua_gettop(L) + idx + 1;
    switch (lua_type(L, abs)) {
        case LUA_TNIL:
            w.nil();
            return;
        case LUA_TBOOLEAN:
            w.boolean(lua_toboolean(L, abs) != 0);
            return;
        case LUA_TNUMBER:
            write_pluto_number(w, lua_tonumber(L, abs));
            return;
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, abs, &len);
            w.str(std::string(s, len));
            return;
        }
        case LUA_TTABLE: {
            // Two lua_next passes: count for map_begin, then encode. Key
            // slots are read only with type-checked accessors (tonumber /
            // toboolean; tolstring after a LUA_TSTRING check) so traversal
            // keys are never coerced in place.
            size_t count = 0;
            lua_pushnil(L);
            while (lua_next(L, abs) != 0) {
                ++count;
                lua_pop(L, 1);
            }
            w.map_begin(count);
            lua_pushnil(L);
            while (lua_next(L, abs) != 0) {
                switch (lua_type(L, -2)) {
                    case LUA_TSTRING: {
                        size_t len = 0;
                        const char* s = lua_tolstring(L, -2, &len);
                        w.str(std::string(s, len));
                        break;
                    }
                    case LUA_TNUMBER:
                        write_pluto_number(w, lua_tonumber(L, -2));
                        break;
                    case LUA_TBOOLEAN:
                        w.boolean(lua_toboolean(L, -2) != 0);
                        break;
                    default:
                        lua_pop(L, 1);
                        throw PersistError("pluto persist: unsupported key type");
                }
                write_pluto_value(L, -1, w, depth + 1);
                lua_pop(L, 1);
            }
            return;
        }
        default:
            throw PersistError("pluto persist: unsupported value type");
    }
}

void read_pluto_value(lua_State* L, oa::util::Reader& r, int depth) {
    if (depth > kPlutoMaxDepth)
        throw oa::util::FormatError("pluto unpersist: nesting too deep");
    const uint8_t t = r.peek_tag();
    if (oa::util::Reader::is_int_tag(t)) {
        lua_pushnumber(L, (lua_Number)r.i64());
        return;
    }
    if (t == oa::util::kTagFloat32 || t == oa::util::kTagFloat64) {
        lua_pushnumber(L, (lua_Number)r.f64());
        return;
    }
    if ((t >= 0xa0 && t <= 0xbf) || t == oa::util::kTagStr8 ||
        t == oa::util::kTagStr16 || t == oa::util::kTagStr32) {
        const std::string s = r.str();
        lua_pushlstring(L, s.data(), s.size());
        return;
    }
    if ((t >= 0x80 && t <= 0x8f) || t == oa::util::kTagMap16 ||
        t == oa::util::kTagMap32) {
        const size_t n = r.map_entries();
        lua_createtable(L, 0, (int)n);
        for (size_t i = 0; i < n; ++i) {
            read_pluto_value(L, r, depth + 1); // key
            // Only a *real* NaN number key is unusable (rawset would make it
            // unreachable) and nil keys are illegal. The numeric-string
            // coercion path must NOT apply here: lua_isnumber/lua_tonumber
            // accept the plain string "nan"/"inf" (strtod), so a legitimate
            // string key named "nan" (Artemis config rows, e.g. snll
            // conf.nan) used to be rejected as a NaN number and the whole
            // pluto document collapsed to {} — 读档 conf_reload then hit
            // conf.mspeed == nil (getMSpeed crash).
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                throw oa::util::FormatError("pluto unpersist: invalid map key");
            }
            if (lua_type(L, -1) == LUA_TNUMBER &&
                std::isnan(lua_tonumber(L, -1))) {
                lua_pop(L, 1);
                throw oa::util::FormatError("pluto unpersist: invalid map key");
            }
            read_pluto_value(L, r, depth + 1); // value
            lua_rawset(L, -3);
        }
        return;
    }
    if ((t >= 0x90 && t <= 0x9f) || t == oa::util::kTagArray16 ||
        t == oa::util::kTagArray32) {
        const size_t n = r.array_items();
        lua_createtable(L, (int)n, 0);
        for (size_t i = 0; i < n; ++i) {
            read_pluto_value(L, r, depth + 1);
            lua_rawseti(L, -2, (int)i + 1);
        }
        return;
    }
    switch (t) {
        case oa::util::kTagNil:
            r.skip_value();
            lua_pushnil(L);
            return;
        case oa::util::kTagTrue:
        case oa::util::kTagFalse:
            lua_pushboolean(L, r.boolean() ? 1 : 0);
            return;
        default:
            throw oa::util::FormatError("pluto unpersist: unexpected tag");
    }
}

int l_pluto_persist(lua_State* L) {
    const int base = lua_gettop(L);
    try {
        oa::util::Writer w;
        write_pluto_value(L, 2, w, 0);
        const std::string& s = w.data();
        lua_pushlstring(L, s.data(), s.size());
        return 1;
    } catch (const std::exception&) {
        lua_settop(L, base);
        lua_pushliteral(L, "");
        return 1;
    }
}

int l_pluto_unpersist(lua_State* L) {
    if (lua_type(L, 2) != LUA_TSTRING) {
        lua_newtable(L);
        return 1;
    }
    size_t len = 0;
    const char* s = lua_tolstring(L, 2, &len);
    const int base = lua_gettop(L);
    try {
        oa::util::Reader r(reinterpret_cast<const uint8_t*>(s), len);
        read_pluto_value(L, r, 0);
        r.expect_eof("pluto unpersist");
    } catch (const std::exception&) {
        lua_settop(L, base);
    }
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_settop(L, base);
        lua_newtable(L);
    }
    return 1;
}

const luaL_Reg kPlutoMethods[] = {
    {"persist", l_pluto_persist},
    {"unpersist", l_pluto_unpersist},
    {nullptr, nullptr},
};

} // namespace

// Opener: pluto owns a global table — merged into an existing one when
// present, mirroring the old `pluto = pluto or {}` polyfill preamble.
static void register_pluto(lua_State* L) {
    lua_getglobal(L, "pluto");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_createtable(L, 0, 2);
    }
    luaL_register(L, nullptr, kPlutoMethods);
    lua_setglobal(L, "pluto");
}

} // namespace oa::runtime

std::string oa::runtime::LuaBridge::stack_trace(int max_depth) const {
    std::string out;
    if (!L_) return out;
    lua_Debug ar;
    for (int depth = 0; depth < max_depth; ++depth) {
        if (!lua_getstack(L_, depth, &ar)) break;
        lua_getinfo(L_, "nSl", &ar);
        char buf[384];
        // short_src is a char array (never null); an unset source is "".
        std::snprintf(buf, sizeof(buf), "  #%d %s:%d in %s\n", depth,
                      ar.short_src,
                      ar.currentline,
                      (ar.name && ar.name[0]) ? ar.name : (ar.what ? ar.what : "?"));
        out += buf;
    }
    return out;
}
