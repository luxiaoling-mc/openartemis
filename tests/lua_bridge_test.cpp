// M4a tests: Lua bridge polyfills + e: API + interpreter<->Lua wiring
// (blocks/calllua/include/tag filter/queue drain). The e:* surface is one
// opaque engine-handle userdata: no engine global exists — every host->Lua
// call (calllua / (e, p) handlers / filters) passes the handle as the
// first argument `e`.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
#include "core/runtime/runtime_lua.h"
#include "core/runtime/runtime_iet.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using oa::runtime::CallbackResult;
using oa::runtime::Event;
using oa::runtime::ExecutionResult;
using oa::runtime::Interpreter;
using oa::runtime::Value;

// ---- helpers ---------------------------------------------------------------

Value read_var(Interpreter& it, const std::string& name) {
    return it.variables().get(name).value_or(Value::make_null());
}

std::string run_interpreter(Interpreter& it, const std::string& content,
                            std::vector<Event>* events = nullptr) {
    it.load_script("test", content);
    it.set_callback([events](const Event& e) {
        if (events) events->push_back(e);
        return e.kind == Event::Kind::Wait_ ? CallbackResult::Pause
                                            : CallbackResult::Continue;
    });
    it.start("test", "top");
    std::string err;
    try {
        for (;;) {
            const ExecutionResult r = it.run();
            if (r == ExecutionResult::Completed) break;
            it.next_line();
        }
    } catch (const std::exception& e) {
        err = e.what();
    }
    return err;
}

void test_pluto_roundtrip() {
    oa::runtime::LuaBridge eng(oa::runtime::LuaHost{});
    eng.run_code(R"(
local t = { saveslot = {[1]={a=1}, [4]={b='x'}, last='ok'}, check=true, n=2.5 }
local s = pluto.persist({}, t)
assert(type(s) == 'string' and #s > 0)
assert(s:sub(1,4) == 'OASB', 'binary stream header')
local back = pluto.unpersist({}, s)
assert(back.saveslot[1].a == 1, 'slot1')
assert(back.saveslot[4].b == 'x', 'slot4')
assert(back.saveslot.last == 'ok', 'last')
assert(back.check == true, 'check')
assert(back.n == 2.5, 'float')
bad = pluto.unpersist({}, 'not binary')
assert(type(bad) == 'table' and next(bad) == nil, 'bad payload -> {}')
bad2 = pluto.unpersist({}, 42)
assert(type(bad2) == 'table', 'non-string -> {}')
)", "pluto");
}

void test_random_31bit() {
    oa::runtime::LuaBridge eng(oa::runtime::LuaHost{});
    eng.run_code(
        "function check_random(e)\n"
        "  for i = 1, 100 do\n"
        "    local v = e:random()\n"
        "    assert(v >= 0 and v <= 0x7fffffff and v == math.floor(v), '31-bit int')\n"
        "  end\n"
        "end\n",
        "rand");
    check(eng.call_function("check_random", {}), "e:random ran");
}

void test_var_semantics() {
    oa::runtime::VariableStore store;
    store.set("t.b", Value::make_bool(true));
    store.set("t.i", Value::make_int(7));
    store.set("t.f", Value::make_float(1.5));
    store.set("t.s", Value::make_string("hi"));
    oa::runtime::LuaHost host;
    host.get_var = [&store](const std::string& n) -> const oa::runtime::Value* {
        const auto* map = store.domain_of(n);
        const auto it = map->find(n);
        return it == map->end() ? nullptr : &it->second;
    };
    oa::runtime::LuaBridge eng(host);
    // e:var 语义 = 变量的文本形态(与 Artemis 一致):数值/布尔以字符串
    // 呈现,缺失 -> "0"。mkmh(2023+) 框架对 e:var 结果做字符串操作
    // (== "0"、:gsub、连接),数字返回会炸;老游戏一律字符串比较/tonumber,
    // 脚本侧 $ 表达式不受影响(内部仍是 Value)。
    eng.run_code(R"(
function check_vars(e)
assert(e:var('t.missing') == '0', 'missing -> "0"')
assert(e:var('t.b') == '1', 'bool -> "1"')
assert(e:var('t.i') == '7', 'int -> "7"')
assert(e:var('t.f') == '1.5', 'float -> "1.5"')
assert(e:var('t.s') == 'hi', 'string')
end
)", "vars");
    check(eng.call_function("check_vars", {}), "e:var ran");
}

void test_engine_handle_shape() {
    // The e:* API surface is ONE opaque engine-handle userdata delivered
    // ONLY as the first argument `e` of host->Lua calls.
    oa::runtime::LuaBridge eng(oa::runtime::LuaHost{});
    eng.run_code(R"(
function handle_probe(e)
  assert(type(e) == 'userdata', 'engine handle is a userdata')
  assert(getmetatable(e) == false, 'engine metatable is guarded')
  assert(not pcall(function() for _ in pairs(e) do end end),
         'engine handle is not a table (not enumerable)')
  assert(not pcall(function() e.extra = 1 end),
         'engine handle is not writable')
  assert(type(e.var) == 'function' and type(e.tag) == 'function',
         'methods reachable through the handle')
  got_e = e
end
)", "handle");
    check(eng.call_function("handle_probe", {}), "call_function passed the engine handle as e");
    // The very same handle rides as the first argument of every host->Lua
    // call (FPM (e, p) handlers), not a per-call copy.
    eng.run_code("function handle_same(e) assert(e == got_e, 'handle identity is stable') end\n",
                 "handle-same");
    check(eng.call_function("handle_same", {}), "second host->Lua call still passes the handle");
}

void test_interpreter_lua_flow() {
    Interpreter it;
    it.hooks().file_loader = [](const std::string& f) -> std::optional<std::vector<uint8_t>> {
        if (f == "lib.lua") {
            const std::string c = "function lib_greet() return 'hi' end\n";
            return std::vector<uint8_t>(c.begin(), c.end());
        }
        if (f == "script2") {
            const std::string c = "*sub\n[var name=\"t.ext\" data=\"'called'\"]\n[return]\n";
            return std::vector<uint8_t>(c.begin(), c.end());
        }
        return std::nullopt;
    };
    const std::string script = R"(
[lua]
function boot_ready(engine)
  engine:include("lib.lua")
  engine:tag{"var", name="t.a", data="41"}
  engine:tag{"var", name="t.b", data="$t.a + 1"}
  engine:tag{"var", name="t.bool", data="1", system="os"}
  tags = {}
  function tags.skipped(engine, p) return 1 end
  function tags.nilhandler(engine, p) return nil end
  engine:setTagFilter(tags)
end
function greet_call(engine)
  engine:tag{"var", name="t.g", data=lib_greet()}
  engine:tag{"call", file="script2"}
end
[/lua]
*top
[calllua function="boot_ready"]
[calllua function="greet_call"]
[var name="t.c" data="$t.b * 2"]
[skipped x="1"]
[nilhandler]
[unknownone y="2"]
[stop]
)";
    std::vector<Event> events;
    const std::string err = run_interpreter(it, script, &events);
    check(err.empty(), ("interpreter+lua ran clean: " + err).c_str());
    check(read_var(it, "t.a").as_int() == 41, "e:tag var sync a");
    check(read_var(it, "t.b").as_int() == 42, "e:tag var sync b expr");
    check(read_var(it, "t.c").as_int() == 84, "inline var after lua calls");
    check(read_var(it, "t.g").to_string() == "hi", "calllua used included lib fn");
    check(read_var(it, "t.ext").to_string() == "called", "queued call cross-script");
    check(read_var(it, "t.bool").to_string() == "unknown", "var system=os");
    bool custom_seen = false;
    for (const auto& e : events) {
        if (e.kind == Event::Kind::Custom) custom_seen = true;
    }
    check(custom_seen, "unknown tag reached Custom event");
    // skipped/nilhandler consumed by filter: exactly one custom
    int customs = 0;
    for (const auto& e : events)
        if (e.kind == Event::Kind::Custom) ++customs;
    check(customs == 1, "filter consumed skipped/nilhandler");
}

void test_queue_wait_advance_noop() {
    Interpreter it;
    const std::string script = R"(
[lua]
function qwait(engine)
  engine:tag{"wait", time="0", input="1"}
end
[/lua]
*top
[calllua function="qwait"]
[var name="t.r" data="1"]
)";
    it.load_script("test", script);
    int pauses = 0;
    it.set_callback([&pauses](const Event& e) {
        if (e.kind == Event::Kind::Wait_) {
            ++pauses;
            return CallbackResult::Pause;
        }
        return CallbackResult::Continue;
    });
    it.start("test", "top");
    ExecutionResult r = it.run();
    check(r == ExecutionResult::Wait, "queued wait pauses");
    check(read_var(it, "t.r").kind == oa::runtime::ValueKind::Null,
          "var after wait not yet executed");
    it.next_line(); // must be a no-op for queue-sourced waits
    r = it.run();
    check(r == ExecutionResult::Completed, "completes after queued wait");
    check(read_var(it, "t.r").as_int() == 1, "inline var executed exactly once");
}

void test_inline_filter_engine_flow() {
    // Filter that swallows builtin [stop] only when returning 1 (test 15 pin).
    Interpreter it;
    const std::string script = R"(
[lua]
tags = {}
function tags.stop(engine, p) return 1 end
function install_filter(e) e:setTagFilter(tags) end
[/lua]
*top
[calllua function="install_filter"]
[stop]
[var name="t.r" data="1"]
)";
    it.load_script("test", script);
    it.set_callback([](const Event& e) {
        if (e.kind == Event::Kind::Wait_) return CallbackResult::Pause;
        return CallbackResult::Continue;
    });
    it.start("test", "top");
    // [stop] consumed by filter -> no wait at all
    const ExecutionResult r = it.run();
    check(r == ExecutionResult::Completed, "filter swallowed builtin stop");
    check(read_var(it, "t.r").as_int() == 1, "execution continued past stop");
}

} // namespace

int main() {
    try {
    test_pluto_roundtrip();
    test_random_31bit();
    test_var_semantics();
    test_engine_handle_shape();
    test_interpreter_lua_flow();
    test_queue_wait_advance_noop();
    test_inline_filter_engine_flow();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "UNCAUGHT in tests: %s\n", e.what());
        return 2;
    }
    if (failures) {
        std::fprintf(stderr, "lua_bridge_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("lua_bridge_test: all ok\n");
    return 0;
}
