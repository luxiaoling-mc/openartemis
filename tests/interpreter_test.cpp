// M3c interpreter tests: control tags, waits, if/loop chains, call/jump/return.
// Behavior pins (research/02 §7).
#include <cstdio>
#include <string>
#include <vector>

#include "core/runtime/runtime_iet.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* what) {
    if (!(a == b)) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using oa::runtime::CallbackResult;
using oa::runtime::Event;
using oa::runtime::ExecutionResult;
using oa::runtime::Interpreter;
using oa::runtime::ScriptError;
using oa::runtime::WaitReason;
using oa::runtime::Value;
using oa::runtime::ValueKind;

struct Harness {
    std::vector<Event> events;
    int waits = 0;
    CallbackResult cb(const Event& e) {
        events.push_back(e);
        if (e.kind == Event::Kind::Wait_) {
            ++waits;
            return CallbackResult::Pause;
        }
        return CallbackResult::Continue;
    }
};

/// Run script from label "main" until Completed (resolving waits by
/// next_line like the host would).
void run_script(Interpreter& it, Harness& h, const std::string& label = "main") {
    it.set_callback([&h](const Event& e) { return h.cb(e); });
    it.start("test", label);
    for (;;) {
        const ExecutionResult r = it.run();
        if (r == ExecutionResult::Completed) break;
        it.next_line();
    }
}

std::optional<Value> var_after(const std::string& script, const std::string& name) {
    Interpreter it;
    Harness h;
    it.load_script("test", script);
    it.set_callback([&h](const Event& e) { return h.cb(e); });
    it.start("test", "main");
    for (;;) {
        const ExecutionResult r = it.run();
        if (r == ExecutionResult::Completed) break;
        it.next_line();
    }
    return it.variables().get(name);
}

void test_if_else() {
    // if false must run else body
    const auto r = var_after(R"(
*main
[var name="r" data="'none'"]
[if estimate="0"]
[var name="r" data="'then'"]
[else]
[var name="r" data="'else'"]
[/if]
[stop]
)",
                             "r");
    check(r && r->kind == ValueKind::String && r->str_val == "else", "if-false runs else");

    // if true: elseif/else branches must not re-run
    const auto n = var_after(R"(
*main
[var name="i" data="3"]
[var name="n" data="0"]
[if estimate="$i == 3"]
[var name="n" data="$n + 1"]
[elseif estimate="$i == 3"]
[var name="n" data="$n + 10"]
[else]
[var name="n" data="$n + 100"]
[/if]
[stop]
)",
                             "n");
    check(n && n->as_int() == 1, "if-true skips elseif/else");

    // elseif chain first-match; all-false falls to else
    const auto m = var_after(R"(
*main
[var name="i" data="2"]
[var name="r" data="'none'"]
[if estimate="$i == 1"]
[var name="r" data="'one'"]
[elseif estimate="$i == 2"]
[var name="r" data="'two'"]
[elseif estimate="$i == 2"]
[var name="r" data="'two-again'"]
[else]
[var name="r" data="'other'"]
[/if]
[stop]
)",
                             "r");
    check(m && m->str_val == "two", "elseif first-match wins");

    const auto p = var_after(R"(
*main
[var name="i" data="9"]
[var name="r" data="'none'"]
[if estimate="$i == 1"]
[var name="r" data="'one'"]
[elseif estimate="$i == 2"]
[var name="r" data="'two'"]
[else]
[var name="r" data="'other'"]
[/if]
[stop]
)",
                             "r");
    check(p && p->str_val == "other", "all-false falls to else");

    // nested if false must not re-activate outer elseif
    const auto q = var_after(R"(
*main
[var name="r" data="'none'"]
[if estimate="1"]
[var name="r" data="'outer'"]
[if estimate="0"]
[var name="r" data="'inner'"]
[/if]
[elseif estimate="1"]
[var name="r" data="'elseif'"]
[/if]
[stop]
)",
                             "r");
    check(q && q->str_val == "outer", "nested-if false keeps outer flow");

    // bare if false without else jumps to /if
    const auto s = var_after(R"(
*main
[var name="r" data="'a'"]
[if estimate="0"]
[var name="r" data="'b'"]
[/if]
[var name="r" data="'c'"]
[stop]
)",
                             "r");
    check(s && s->str_val == "c", "bare if false continues after /if");
}

void test_jump_call_return() {
    Interpreter it;
    Harness h;
    it.load_script("test", R"(
*main
[var name="r" data="'start'"]
[jump label="skip"]
[var name="r" data="'bad'"]
*skip
[call label="sub"]
[var name="r" data="$r + '|' + $t.v"]
[return]
*sub
[var name="t.v" data="'subval'"]
[return]
)");
    run_script(it, h);
    const auto r = it.variables().get("r");
    check(r && r->str_val == "start|subval", "jump/call/return flow");

    // cond-false jump continues in order
    Interpreter it2;
    Harness h2;
    it2.load_script("test", R"(
*main
[var name="r" data="'a'"]
[jump cond="0" label="elsewhere"]
[var name="r" data="'b'"]
[return]
*elsewhere
[var name="r" data="'c'"]
[return]
)");
    run_script(it2, h2);
    check(it2.variables().get("r")->str_val == "b", "jump cond false continues");

    // external call loads the target script through the file loader hook
    Interpreter it3;
    Harness h3;
    it3.load_script("test", R"(
*main
[call file="sub"]
[stop]
)");
    it3.hooks().file_loader = [](const std::string& f) -> std::optional<std::vector<uint8_t>> {
        if (f == "sub") {
            const std::string s = "*top\n[var name=\"r\" data=\"'called'\"]\n[return]\n";
            return std::vector<uint8_t>(s.begin(), s.end());
        }
        return std::nullopt;
    };
    run_script(it3, h3);
    check(it3.variables().get("r")->str_val == "called", "external call loads script");
}

void test_wait_kinds() {
    // [wt] -> Timed input=1 time=0
    {
        Interpreter it;
        Harness h;
        it.load_script("test", "*main\n[wt]\n[var name=\"r\" data=\"1\"]\n");
        run_script(it, h);
        check(h.waits == 1, "wt waits once");
        check(h.events.size() == 1 && h.events[0].kind == Event::Kind::Wait_, "wt event");
        check(h.events[0].reason.kind == WaitReason::Kind::Timed && h.events[0].reason.input == 1 &&
                  h.events[0].reason.milliseconds == 0,
              "wt defaults time=0 input=1");
        check(it.variables().get("r")->as_int() == 1, "advance past wt runs next");
    }
    // [wt0] Generic0
    {
        Interpreter it;
        Harness h;
        it.load_script("test", "*main\n[wt0]\n");
        run_script(it, h);
        check(h.events[0].reason.kind == WaitReason::Kind::Generic0, "wt0 Generic0");
    }
    // [@] Generic
    {
        Interpreter it;
        Harness h;
        it.load_script("test", "*main\n[@]\n");
        run_script(it, h);
        check(h.events[0].reason.kind == WaitReason::Kind::Generic, "@ Generic");
    }
    // [wait time=500] input defaults 0
    {
        Interpreter it;
        Harness h;
        it.load_script("test", "*main\n[wait time=500]\n");
        run_script(it, h);
        check(h.events[0].reason.kind == WaitReason::Kind::Timed &&
                  h.events[0].reason.input == 0 && h.events[0].reason.milliseconds == 500,
              "wait timed");
    }
    // [wait video=1.0] VideoLayer
    {
        Interpreter it;
        Harness h;
        it.load_script("test", "*main\n[wait video=\"1.0\"]\n");
        run_script(it, h);
        check(h.events[0].reason.kind == WaitReason::Kind::VideoLayer &&
                  h.events[0].reason.id == "1.0",
              "wait video layer");
    }
    // [wait scenario=2] ScenarioTween; scenario=0 falls back to Timed
    {
        Interpreter it;
        Harness h;
        it.load_script("test", "*main\n[wait scenario=2]\n");
        run_script(it, h);
        check(h.events[0].reason.kind == WaitReason::Kind::ScenarioTween &&
                  h.events[0].reason.mode == 2,
              "wait scenario=2");
        Interpreter it2;
        Harness h2;
        it2.load_script("test", "*main\n[wait scenario=0]\n");
        run_script(it2, h2);
        check(h2.events[0].reason.kind == WaitReason::Kind::Timed, "scenario=0 -> Timed");
    }
    // [stop reason] Stop
    {
        Interpreter it;
        Harness h;
        it.load_script("test", "*main\n[stop exskip]\n");
        run_script(it, h);
        check(h.events[0].reason.kind == WaitReason::Kind::Stop && h.events[0].reason.id == "exskip",
              "stop carries positional reason");
    }
    // stop keeps position: var after stop still runs after advance
    {
        Interpreter it;
        Harness h;
        it.load_script("test", "*main\n[stop]\n[var name=\"r\" data=\"'after'\"]\n");
        run_script(it, h);
        check(it.variables().get("r")->str_val == "after", "advance over stop continues");
    }
}

void test_loop() {
    const auto n = var_after(R"(
*main
[var name="i" data="0"]
[loop estimate="$i < 3"]
[var name="i" data="$i + 1"]
[/loop]
[stop]
)",
                             "i");
    check(n && n->as_int() == 3, "loop counts to 3");

    const auto m = var_after(R"(
*main
[var name="r" data="'a'"]
[loop estimate="0"]
[var name="r" data="'body'"]
[/loop]
[var name="r" data="'end'"]
[stop]
)",
                             "r");
    check(m && m->str_val == "end", "loop false skips body");
}

void test_unknown_tag_custom_event() {
    Interpreter it;
    Harness h;
    it.load_script("test", R"(
*main
[chara_a value="ok"]
[var name="r" data="'x'"]
)");
    run_script(it, h);
    check(h.events.size() == 1 && h.events[0].kind == Event::Kind::Custom &&
              h.events[0].tag == "chara_a",
          "unknown tag becomes Custom event");
    check(h.events[0].params.at("value") == "ok", "custom event keeps params");
}

void test_var_expression_data() {
    const auto v = var_after(R"(
*main
[var name="t.a" data="2"]
[var name="t.b" data="$t.a * 3 + 1"]
[var name="t.c" data="'text'"]
[var name="t.d" data="$t.a == 2"]
[stop]
)",
                             "t.d");
    check(v && v->kind == ValueKind::Bool && v->bool_val, "var data expression");
    Interpreter it;
    Harness h;
    it.load_script("test",
                   "*main\n[var name=\"t.a\" data=\"2\"]\n[var name=\"t.b\" data=\"$t.a*3+1\"]\n");
    run_script(it, h);
    check(it.variables().get("t.b")->as_int() == 7, "var arithmetic data");
}

void test_boot_label_priority() {
    Interpreter it;
    Harness h;
    it.load_script("test", "*start\n[var name=\"r\" data=\"'start'\"]\n[stop]\n");
    it.set_callback([&h](const Event& e) { return h.cb(e); });
    it.boot("test");
    for (;;) {
        const ExecutionResult r = it.run();
        if (r == ExecutionResult::Completed) break;
        it.next_line();
    }
    check(it.variables().get("r")->str_val == "start", "boot prefers *start");
}

void test_exit_and_reset_events() {
    Interpreter it;
    Harness h;
    it.load_script("test", "*main\n[exit]\n[var name=\"r\" data=\"1\"]\n");
    run_script(it, h);
    check(h.events.size() == 1 && h.events[0].kind == Event::Kind::Exit, "exit event");
    Interpreter it2;
    Harness h2;
    it2.set_variable("t.x", Value::make_int(5));
    it2.set_variable("g.y", Value::make_int(6));
    it2.load_script("test", "*main\n[reset]\n");
    run_script(it2, h2);
    check(h2.events.size() == 1 && h2.events[0].kind == Event::Kind::Reset, "reset event");
    check(!it2.variables().get("t.x").has_value(), "reset clears temp");
    check(it2.variables().get("g.y").has_value(), "reset keeps global");
}

} // namespace

int main() {
    test_if_else();
    test_jump_call_return();
    test_wait_kinds();
    test_loop();
    test_unknown_tag_custom_event();
    test_var_expression_data();
    test_boot_label_priority();
    test_exit_and_reset_events();
    if (failures) {
        std::fprintf(stderr, "interpreter_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("interpreter_test: all ok\n");
    return 0;
}
