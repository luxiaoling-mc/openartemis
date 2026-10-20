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

void test_line_tag_columns() {
    // [&linetag] rows are COMMANDS, not prose: tag.ini names the CSV columns
    // and the row runs as the normal tag. iMel scenarios open with
    //   [&linetag allow="1" prefix="#"]
    // and then write  #立ち絵表示 中_星_制服,c,210  (st,pos,time from tag.ini).
    // Without this they render as story text and the sprites never load.
    Interpreter it;
    Harness h;
    it.hooks().file_loader = [](const std::string& f)
        -> std::optional<std::vector<uint8_t>> {
        if (f == "tag.ini") {
            const std::string s =
                "; line type command\n[var]\n0=name\n1=data\n";
            return std::vector<uint8_t>(s.begin(), s.end());
        }
        return std::nullopt;
    };
    it.load_script("test", R"(
*main
[&linetag allow="1" prefix="#"]
#var r,7
[var name="after" data="1"]
)");
    run_script(it, h);
    check(it.variables().get("r")->as_int() == 7,
          "line tag columns come from tag.ini (r=7)");
    check(it.variables().get("after")->as_int() == 1, "story continues past it");
}

void test_wait_kinds() {
    // [wt] bare -> Generic click wait (id "wt"): holds until a PHYSICAL click,
    // and a scripted decide edge (the boot-time setexclick dummy click) does
    // not pass it — runtime.cpp resolve_wait Generic excludes id "wt". The
    // pre-change form was Timed(0), i.e. "resolved on the next tick", which
    // made every splash ([calllua brand_logo][wt]) skip itself.
    {
        Interpreter it;
        Harness h;
        it.load_script("test", "*main\n[wt]\n[var name=\"r\" data=\"1\"]\n");
        run_script(it, h);
        check(h.waits == 1, "wt waits once");
        check(h.events.size() == 1 && h.events[0].kind == Event::Kind::Wait_, "wt event");
        check(h.events[0].reason.kind == WaitReason::Kind::Generic &&
                  h.events[0].reason.id == "wt",
              "bare wt is a click wait (Generic, id=wt)");
        check(it.variables().get("r")->as_int() == 1, "advance past wt runs next");
    }
    // [wt] with an explicit parameter keeps the TIMED form and keeps input=
    // verbatim (0 = never clickable, 2 = skip-mode only). The bare-wt click
    // wait above must not swallow authored parameters — this is the counter-
    // guard for that change.
    {
        Interpreter it;
        Harness h;
        it.load_script("test",
                       "*main\n[wt time=500 input=0]\n[var name=\"r\" data=\"1\"]\n");
        run_script(it, h);
        check(h.waits == 1, "wt with args waits once");
        check(h.events[0].reason.kind == WaitReason::Kind::Timed &&
                  h.events[0].reason.milliseconds == 500 &&
                  h.events[0].reason.input == 0,
              "wt time=500 input=0 stays Timed(500) input=0");
        check(it.variables().get("r")->as_int() == 1, "advance past wt(time) runs next");
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

void test_text_row_reaches_print_filter() {
    // A scenario text row is `[print data="<row>"]` as far as the framework is
    // concerned: Artemis hands it to tagFilters.print, which is where the iMel
    // multi-language layer decides which [lang xx] block to draw. Emitting an
    // engine-private ScenarioText event instead (the pre-fix path) meant the
    // framework never saw the row — Watamari's story came up on a blank
    // message window because the language filter owns every body line.
    {
        Interpreter it;
        Harness h;
        it.load_script("test", "*main\n"
                               "[lua]\n"
                               "function install_filter(e)\n"
                               "    tagFilters = {}\n"
                               "    tagFilters[\"print\"] = function(e2, param)\n"
                               "        e2:tag{\"var\", name = \"t.printed\", data = param.data}\n"
                               "        return 1\n"
                               "    end\n"
                               "    e:setTagFilter(tagFilters)\n"
                               "end\n"
                               "[/lua]\n"
                               "[calllua function=\"install_filter\"]\n"
                               "ただの文\n"
                               "[var name=\"after\" data=\"1\"]\n");
        run_script(it, h);
        const auto printed = it.variables().get("t.printed");
        check(printed && printed->to_string() == "ただの文",
              "text row reaches tagFilters.print with data=<row>");
        check(it.variables().get("after") && it.variables().get("after")->as_int() == 1,
              "story continues past a filtered text row");
        bool leaked = false;
        for (const auto& e : h.events)
            if (e.kind == Event::Kind::ScenarioText) leaked = true;
        check(!leaked, "the row the filter consumed is not also emitted as engine text");
    }
    // Counter-guard: without a print filter the row must still reach the
    // engine as a text event (every non-iMel title's normal path).
    {
        Interpreter it;
        Harness h;
        it.load_script("test", "*main\nただの文\n");
        run_script(it, h);
        bool saw_text = false;
        for (const auto& e : h.events)
            if (e.kind == Event::Kind::ScenarioText && e.content == "ただの文")
                saw_text = true;
        check(saw_text, "no print filter -> the row still emits ScenarioText");
    }
}

void test_macro_runs_when_filter_declines() {
    // A framework filter returning 0 means "not handled, run it yourself".
    // For a [macroadd]-registered tag "yourself" is the macro, so a declining
    // filter must not swallow it: iMel's 背景/イベントCG filters return 0 for
    // every st=/add= row, and consuming those left the story page with no
    // background and no event CG.
    Interpreter it;
    Harness h;
    it.load_script("macros", "*bg\n[var name=\"t.bg\" data=\"1\"]\n[return]\n");
    it.load_script("test", "*main\n"
                           "[macroadd file=\"macros\"]\n"
                           "[lua]\n"
                           "function install_filter(e)\n"
                           "    local f = {}\n"
                           "    f[\"bg\"] = function(e2, p) return 0 end\n"
                           "    e:setTagFilter(f)\n"
                           "end\n"
                           "[/lua]\n"
                           "[calllua function=\"install_filter\"]\n"
                           "[bg st=\"x\" hide=\"1\"]\n");
    run_script(it, h);
    const auto ran = it.variables().get("t.bg");
    check(ran && ran->as_int() == 1,
          "a tag whose filter returns 0 still runs its macro");
}

void test_if_without_estimate_is_false() {
    // A misspelled parameter (`[if estiamte = "$t.sceneskip"]`, seven rows in
    // Watamari's sysbtn.iet) leaves `estimate` unset. The original evaluates
    // nil, i.e. false — treating the empty condition as true ran the guarded
    // [return] and every sysbtn_* macro bailed before creating the bottom-right
    // skip/menu buttons.
    Interpreter it;
    Harness h;
    it.load_script("test", "*main\n"
                           "[var name=\"r\" data=\"0\"]\n"
                           "[if estiamte = \"1\"]\n"
                           "[var name=\"r\" data=\"1\"]\n"
                           "[/if]\n");
    run_script(it, h);
    const auto r = it.variables().get("r");
    check(r && r->as_int() == 0, "an [if] without estimate takes the false branch");
    // Counter-guard: a well-formed condition still gates both branches.
    Interpreter it2;
    Harness h2;
    it2.load_script("test", "*main\n"
                            "[var name=\"a\" data=\"0\"]\n"
                            "[var name=\"x\" data=\"1\"]\n"
                            "[if estimate=\"$x == 1\"]\n"
                            "[var name=\"a\" data=\"1\"]\n"
                            "[/if]\n"
                            "[if estimate=\"$x == 2\"]\n"
                            "[var name=\"a\" data=\"2\"]\n"
                            "[/if]\n");
    run_script(it2, h2);
    const auto a = it2.variables().get("a");
    check(a && a->as_int() == 1, "well-formed if/endif still gates both branches");
}

void test_forcedcalllua_runs_like_calllua() {
    // The framework's "run it even mid-transition" flavour of calllua. iMel's
    // lycaution notice ends with a tween handler of this name that pops the
    // message layer back to the story window; an unknown handler name was
    // dropped, the notice layer stayed active, and every later story line
    // printed into the bottom notice strip instead of the window.
    Interpreter it;
    Harness h;
    it.load_script("test", "*main\n"
                           "[lua]\n"
                           "function mark(e)\n"
                           "    e:tag{\"var\", name = \"t.m\", data = \"1\"}\n"
                           "end\n"
                           "[/lua]\n"
                           "[forcedcalllua function=\"mark\"]\n");
    run_script(it, h);
    const auto m = it.variables().get("t.m");
    check(m && m->as_int() == 1, "forcedcalllua calls the Lua function like calllua");
}

int main() {
    test_if_else();
    test_jump_call_return();
    test_line_tag_columns();
    test_text_row_reaches_print_filter();
    test_macro_runs_when_filter_declines();
    test_if_without_estimate_is_false();
    test_forcedcalllua_runs_like_calllua();
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
