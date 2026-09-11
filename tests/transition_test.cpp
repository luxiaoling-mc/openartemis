// P2a transition/tween tests (see
// docs/research/09-transition-notes.md §9, T1-T6).
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/fs/fs.h"
#include "core/runtime/runtime.h"
#include "core/render/layer.h"
// research/111: anim（Easing/Tween/TweenDone）与 transition（Transition）收为
// render 内部面；本测试是这两个语义机的直接使用者，故 include 内部头
// （公开头 renderer.h/layer.h/runtime.h 不再暴露它们）。
#include "core/render/render_internal.h"
#include "core/runtime/runtime_iet.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool approx(double a, double b, double eps = 1e-6) {
    const double d = a - b;
    return d > -eps && d < eps;
}

using oa::render::Easing;

void test_easing() { // T3 ( easing accuracy/alias tests)
    // Linear identity
    check(approx(oa::render::ease_value(Easing::Linear, 0.0), 0.0) &&
              approx(oa::render::ease_value(Easing::Linear, 0.5), 0.5) &&
              approx(oa::render::ease_value(Easing::Linear, 1.0), 1.0),
          "linear identity");
    // endpoints of every easing are 0 at t=0 and 1 at t=1 (elastic tolerant)
    const Easing all[] = {
        Easing::EaseInQuad, Easing::EaseOutQuad, Easing::EaseInOutQuad,
        Easing::EaseInCubic, Easing::EaseOutCubic, Easing::EaseInOutCubic,
        Easing::EaseInQuart, Easing::EaseOutQuart, Easing::EaseInOutQuart,
        Easing::EaseInQuint, Easing::EaseOutQuint, Easing::EaseInOutQuint,
        Easing::EaseInExpo, Easing::EaseOutExpo, Easing::EaseInOutExpo,
        Easing::EaseInCirc, Easing::EaseOutCirc, Easing::EaseInOutCirc,
        Easing::EaseInSine, Easing::EaseOutSine, Easing::EaseInOutSine,
        Easing::EaseInBack, Easing::EaseOutBack, Easing::EaseInOutBack,
        Easing::EaseInElastic, Easing::EaseOutElastic, Easing::EaseInOutElastic,
        Easing::EaseInBounce, Easing::EaseOutBounce, Easing::EaseInOutBounce,
    };
    for (Easing e : all) {
        const double eps =
            (e == Easing::EaseInElastic || e == Easing::EaseOutElastic ||
             e == Easing::EaseInOutElastic)
                ? 0.01
                : 1e-4;
        check(std::fabs(oa::render::ease_value(e, 0.0) - 0.0) < eps, "easing t=0 -> 0");
        check(std::fabs(oa::render::ease_value(e, 1.0) - 1.0) < eps, "easing t=1 -> 1");
    }
    // in/out symmetry: in(t) + out(1-t) == 1
    {
        const Easing pairs[][2] = {{Easing::EaseInQuad, Easing::EaseOutQuad},
                                   {Easing::EaseInCubic, Easing::EaseOutCubic},
                                   {Easing::EaseInBack, Easing::EaseOutBack},
                                   {Easing::EaseInBounce, Easing::EaseOutBounce}};
        for (const auto& p : pairs) {
            for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                const double eps =
                    (p[0] == Easing::EaseInBack || p[0] == Easing::EaseInBounce) ? 0.01
                                                                                 : 1e-4;
                const double sum = oa::render::ease_value(p[0], t) +
                                   oa::render::ease_value(p[1], 1.0 - t);
                check(std::fabs(sum - 1.0) < eps, "ease in/out symmetric");
            }
        }
    }
    // parse aliases 
    check(oa::render::parse_easing("easein") == Easing::EaseInQuad, "parse easein");
    check(oa::render::parse_easing("easeout") == Easing::EaseOutQuad, "parse easeout");
    check(oa::render::parse_easing("easeinout") == Easing::EaseInOutQuad, "parse easeinout");
    check(oa::render::parse_easing("easein_cubic") == Easing::EaseInCubic, "parse cubic");
    check(oa::render::parse_easing("easeout_elastic") == Easing::EaseOutElastic,
          "parse elastic");
    check(oa::render::parse_easing("easeinout_bounce") == Easing::EaseInOutBounce,
          "parse bounce");
    check(oa::render::parse_easing("easeoutbounce") == Easing::EaseOutBounce,
          "parse no-underscore bounce");
    check(oa::render::parse_easing("garbage") == Easing::Linear, "parse garbage -> linear");
    // in slower at start
    check(oa::render::ease_value(Easing::EaseInQuad, 0.5) < 0.5, "easein quad < .5");
    check(oa::render::ease_value(Easing::EaseOutQuad, 0.5) > 0.5, "easeout quad > .5");
}

oa::render::Tween make_tween(double from, double to, uint64_t dur) {
    oa::render::Tween t;
    t.param = "alpha";
    t.from = from;
    t.to = to;
    t.easing = Easing::Linear;
    t.start_ms = 1000;
    t.duration_ms = dur;
    return t;
}

void test_tween_get_value() { // T1/T2 ( get_value/finish/loop tests)
    { // linear endpoints and midpoint
        oa::render::Tween t = make_tween(0.0, 100.0, 1000);
        check(approx(t.get_value(1000), 0.0), "value at start == from");
        check(approx(t.get_value(1500), 50.0), "value at midpoint");
        check(approx(t.get_value(2000), 100.0), "value at end == to");
    }
    { // clamps outside range
        oa::render::Tween t = make_tween(0.0, 100.0, 1000);
        check(approx(t.get_value(500), 0.0), "before start == from");
        check(approx(t.get_value(9999), 100.0), "after end == to");
    }
    { // zero duration snaps
        oa::render::Tween t = make_tween(0.0, 100.0, 0);
        check(approx(t.get_value(1000), 100.0), "zero duration snaps to target");
    }
    { // finished detection
        oa::render::Tween t = make_tween(0.0, 1.0, 1000);
        check(!t.is_finished(1500), "not finished mid-tween");
        check(t.is_finished(2000), "finished at end");
    }
    { // delay defers start (start_ms includes the delay)
        oa::render::Tween t = make_tween(0.0, 100.0, 1000);
        t.start_ms = 1500; // 500 ms delay from clock = 1000
        check(approx(t.get_value(1200), 0.0), "delay keeps from");
        check(approx(t.get_value(2000), 50.0), "midpoint after delay");
        check(approx(t.get_value(2500), 100.0), "end after delay");
    }
    { // infinite loop never finishes
        oa::render::Tween t = make_tween(0.0, 100.0, 1000);
        t.infinite_loop = true;
        check(!t.is_finished(99999), "infinite loop never finished");
        const double v = t.get_value(2500);
        check(v >= 0.0 && v <= 100.0, "infinite loop value in range");
    }
    { // finite loop completes after N cycles
        oa::render::Tween t = make_tween(0.0, 100.0, 1000);
        t.loop_count = 2;
        check(!t.is_finished(2500), "finite loop mid-cycle-2 running");
        check(t.is_finished(3000), "finite loop done after 2 cycles");
    }
    { // yoyo alternates direction
        oa::render::Tween t = make_tween(0.0, 100.0, 1000);
        t.loop_count = 2;
        t.yoyo = true;
        check(approx(t.get_value(1500), 50.0), "yoyo cycle 1 forward");
        check(t.is_yoyo_reverse(2500), "yoyo cycle 2 is reverse");
    }
    { // loop delay adds gap between cycles
        oa::render::Tween t = make_tween(0.0, 100.0, 1000);
        t.loop_count = 2;
        t.loop_delay_ms = 500;
        check(!t.is_finished(3400), "loop delay cycle 2 not finished");
        check(t.is_finished(3500), "loop delay done incl delay");
    }
}

void test_transition_machine() { // T4 subset ( state machine)
    using oa::render::Transition;
    const uint64_t t0 = 10000;
    { // type 0 clears and never leaves a state behind
        Transition tr;
        tr.start(0, 500, "", std::nullopt, 1, t0);
        check(!tr.active(), "type 0 leaves no active transition");
    }
    { // capture flow: begin -> needs capture -> in progress -> mark captured
        Transition tr;
        tr.start(1, 1000, "", std::nullopt, 1, t0);
        check(tr.active() && tr.needs_capture(), "needs capture right after start");
        check(tr.is_in_progress(t0), "in progress while needs_capture");
        // no capture yet: clear_finished must not clear
        tr.clear_finished(t0 + 5000);
        check(tr.active(), "not cleared before capture");
        tr.mark_captured(t0 + 20);
        check(!tr.needs_capture() && tr.is_captured(), "capture completed");
        check(tr.is_in_progress(t0 + 20), "in progress after capture");
        check(tr.is_in_progress(t0 + 1000), "in progress before duration elapses");
        check(!tr.is_in_progress(t0 + 1020), "finished after duration");
        // progress clamped 0..1
        check(approx(tr.progress(t0 + 20), 0.0), "progress 0 at capture");
        check(approx(tr.progress(t0 + 520), 0.5, 1e-4), "progress 0.5 at half");
        check(approx(tr.progress(t0 + 3000), 1.0), "progress clamps at 1");
        tr.clear_finished(t0 + 2000);
        check(!tr.active(), "clear_finished drops finished transition");
    }
    { // time default 1000ms
        Transition tr;
        tr.start(1, std::nullopt, "", std::nullopt, 1, 0);
        tr.mark_captured(10);
        check(!tr.is_in_progress(10 + 1000), "default duration is 1000ms");
    }
    { // skip_by_input respects the input policy 
        // input 1 (default/allow): skipping clears
        Transition a;
        a.start(2, 1000, "rule.png", 32, 1, 0);
        check(a.skip_by_input(false), "input=1 allows skip");
        check(!a.active(), "input=1 skip cleared");
        // input 0: never skips
        Transition b;
        b.start(2, 1000, "rule.png", 32, 0, 0);
        check(!b.skip_by_input(false), "input=0 denies skip");
        check(b.active(), "input=0 transition stays");
        check(!b.skip_by_input(true), "input=0 denies skip even in skip mode");
        // input 2: only in skip mode
        Transition c;
        c.start(2, 1000, "rule.png", 32, 2, 0);
        check(!c.skip_by_input(false), "input=2 denies outside skip mode");
        check(c.active(), "input=2 transition stays");
        check(c.skip_by_input(true), "input=2 allows in skip mode");
        check(!c.active(), "input=2 skip cleared");
        // no active transition: no-op
        Transition d;
        check(!d.skip_by_input(true), "no transition skip is a no-op");
    }
}

void test_compositor_tweens() { // T1/T5 through the Compositor (anim reduce)
    using oa::render::Compositor;
    { // apply + per-frame advance + settle (alpha in 0-255 units)
        Compositor c;
        c.create("1", {});
        std::map<std::string, std::string> p = {{"id", "1"},
                                                {"param", "alpha"},
                                                {"from", "0"},
                                                {"to", "255"},
                                                {"time", "100"}};
        c.apply_lytween(p, 1000);
        check(c.has_tweens(), "tween registered");
        auto done = c.advance_tweens(1000);
        check(done.empty(), "no handler at start");
        check(approx(c.find("1")->alpha * 255.0, 0.0), "alpha at from right away");
        done = c.advance_tweens(1050);
        check(done.empty(), "no handler mid tween");
        // linear midpoint 127.5 rounds to 128 (format_value int rule)
        check(approx(c.find("1")->alpha * 255.0, 128.0, 1.0), "alpha at midpoint");
        done = c.advance_tweens(1100);
        check(done.empty(), "finish without handler emits nothing");
        check(approx(c.find("1")->alpha, 1.0), "alpha settled to 255");
        check(!c.has_tweens(), "finished tween removed");
    }
    { // delay keeps from until start
        Compositor c;
        c.create("1", {});
        std::map<std::string, std::string> p = {{"id", "1"}, {"param", "alpha"},
                                                {"from", "0"}, {"to", "255"},
                                                {"time", "100"}, {"delay", "200"}};
        c.apply_lytween(p, 1000);
        c.advance_tweens(1150);
        check(approx(c.find("1")->alpha * 255.0, 0.0), "delay keeps from");
        c.advance_tweens(1250);
        check(approx(c.find("1")->alpha * 255.0, 128.0, 1.0), "after delay midpoint");
        c.advance_tweens(1300);
        check(approx(c.find("1")->alpha, 1.0), "after delay settled");
    }
    { // delete_on_finish removes the layer
        Compositor c;
        c.create("1", {});
        std::map<std::string, std::string> p = {{"id", "1"}, {"param", "alpha"},
                                                {"from", "0"}, {"to", "0"},
                                                {"time", "100"}, {"delete", "1"}};
        c.apply_lytween(p, 1000);
        c.advance_tweens(1099);
        check(c.find("1") != nullptr, "delete layer still alive before finish");
        auto done = c.advance_tweens(1100);
        check(c.find("1") == nullptr, "delete_on_finish removed layer");
        check(done.empty(), "pure delete has no callback");
    }
    { // handler completion surfaces via TweenDone
        Compositor c;
        c.create("n", {});
        std::map<std::string, std::string> p = {{"id", "n"}, {"param", "alpha"},
                                                {"from", "0"}, {"to", "255"},
                                                {"time", "100"},
                                                {"handler", "calllua"},
                                                {"function", "notify_wait"}};
        c.apply_lytween(p, 1000);
        auto done = c.advance_tweens(1000 + 100);
        check(done.size() == 1, "handler emitted at finish");
        if (!done.empty()) {
            check(done[0].handler == "calllua", "handler name kept");
            const auto f = done[0].extra.find("function");
            check(f != done[0].extra.end() && f->second == "notify_wait",
                  "extra function param preserved");
        }
    }
    { // lytweendel settles to final (finish_tweens)
        Compositor c;
        c.create("1", {});
        std::map<std::string, std::string> p = {{"id", "1"}, {"param", "alpha"},
                                                {"from", "0"}, {"to", "255"},
                                                {"time", "1000"}};
        c.apply_lytween(p, 1000);
        c.apply_lytweendel("1");
        check(approx(c.find("1")->alpha, 1.0), "lytweendel settled to to");
        check(!c.has_tweens(), "lytweendel cleared tweens");
    }
    { // tweenset: sequential starts + group cascade removal (
      // T5)
        Compositor c;
        c.create("1", {});
        c.create("2", {});
        c.tweenset_start();
        std::map<std::string, std::string> a = {{"id", "1"}, {"param", "alpha"},
                                                {"from", "0"}, {"to", "255"},
                                                {"time", "100"}};
        std::map<std::string, std::string> b = {{"id", "2"}, {"param", "alpha"},
                                                {"from", "0"}, {"to", "255"},
                                                {"time", "100"}};
        c.apply_lytween(a, 1000);
        c.apply_lytween(b, 1000);
        check(!c.has_tweens(), "set members not started before /tweenset");
        c.tweenset_end(1000);
        check(c.has_tweens(), "set members scheduled at /tweenset");
        // first member runs 1000..1100, second starts at 1100
        c.advance_tweens(1050);
        check(approx(c.find("1")->alpha * 255.0, 128.0, 1.0), "set member 1 midpoint");
        check(approx(c.find("2")->alpha * 255.0, 255.0, 1.0),
              "set member 2 shows its base value before its start");
        c.advance_tweens(1150);
        check(approx(c.find("1")->alpha, 1.0), "set member 1 settled");
        check(approx(c.find("2")->alpha * 255.0, 128.0, 1.0), "set member 2 midpoint");
        c.advance_tweens(1200);
        check(approx(c.find("2")->alpha, 1.0), "set member 2 settled");
        check(!c.has_tweens(), "all set members settled");
        // cascade: lytweendel on member 1 removes member 2 across the layer
        c.tweenset_start();
        c.apply_lytween(a, 2000);
        c.apply_lytween(b, 2000);
        c.tweenset_end(2000);
        c.advance_tweens(2050);
        c.apply_lytweendel("1"); // settles 1, cascade-drops group incl 2
        check(approx(c.find("1")->alpha, 1.0), "cascade lytweendel settled 1");
        check(c.find("2") != nullptr, "cascade keeps the layer");
        check(!c.has_tweens(), "cascade removed every group member");
        // layer deletion cascades the same way 
        c.tweenset_start();
        c.apply_lytween(a, 3000);
        c.apply_lytween(b, 3000);
        c.tweenset_end(3000);
        c.remove("1");
        check(c.find("2") != nullptr, "remove cascade keeps sibling layer");
        check(!c.has_tweens(), "remove cascade dropped group members");
    }
    { // layer deletion alone drops its own tweens
        Compositor c;
        c.create("1", {});
        std::map<std::string, std::string> p = {{"id", "1"}, {"param", "alpha"},
                                                {"from", "0"}, {"to", "255"},
                                                {"time", "500"}};
        c.apply_lytween(p, 1000);
        c.remove("1");
        check(!c.has_tweens(), "lydel dropped the layer's tweens");
    }
}

// ---------------------------------------------------------------- T6: runtime
struct MemFs : oa::fs::IFileSystem {
    std::map<std::string, std::string> files;
    std::optional<std::vector<uint8_t>> read(std::string_view path) const override {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return std::nullopt;
        return std::vector<uint8_t>(it->second.begin(), it->second.end());
    }
    bool exists(std::string_view path) const override {
        return files.count(std::string(path)) > 0;
    }
    const char* kind() const override { return "mem"; }
};

const oa::runtime::Event* find_event_kind(const std::vector<oa::runtime::Event>& evs,
                                         oa::runtime::Event::Kind k) {
    for (const auto& e : evs)
        if (e.kind == k) return &e;
    return nullptr;
}

void test_runtime_trans_wait() {
    // synthetic project: [trans type=1] parks the interpreter in a
    // Stop{reason:"trans"} wait that releases once the transition's duration
    // elapses ( runtime/).
    MemFs fs;
    fs.files["system.ini"] =
        "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
        "BOOT = system/first.iet\nSAVEPATH = save\n";
    fs.files["system/first.iet"] =
        "*main\n[trans type=\"1\" time=\"120\" input=\"1\"]\n[stop]\n";
    oa::runtime::GameRuntime rt(std::make_shared<MemFs>(fs));
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    // First tick: the interpreter runs up to [trans] and parks.
    rt.tick(16, idle);
    {
        const oa::runtime::WaitReason* w = rt.current_wait();
        check(w != nullptr, "trans parks the interpreter");
        check(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id == "trans",
              "trans wait is Stop{reason:trans}");
    }
    // Host applies the drained Trans event (dispatch order), starting the
    // transition at the current clock.
    bool applied = false;
    for (const auto& e : rt.drain_events()) {
        if (e.kind == oa::runtime::Event::Kind::Trans) {
            rt.transition_begin(e.params);
            applied = true;
        }
    }
    check(applied, "a Trans event was emitted");
    check(rt.transition().is_in_progress(rt.now_ms()), "transition in progress");

    // The wait must survive several ticks while the transition runs.
    int held = 0;
    int released_at = -1;
    for (int i = 0; i < 40; ++i) {
        const bool was_trans_wait = rt.current_wait() &&
                                    rt.current_wait()->id == "trans";
        rt.tick(16, idle);
        (void)rt.drain_events();
        if (was_trans_wait && rt.waiting_stop() &&
            rt.current_wait() && rt.current_wait()->id == "trans")
            ++held;
        if (was_trans_wait && !rt.current_wait()) {
            released_at = i;
            break;
        }
    }
    check(held >= 3, "trans wait held for several ticks");
    check(released_at >= 0, "trans wait released by elapsed duration");
    // next tick executes [stop] -> plain stop park (no reason)
    rt.tick(16, idle);
    (void)rt.drain_events();
    const oa::runtime::WaitReason* w = rt.current_wait();
    check(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty(),
          "plain [stop] parks after the trans wait");
}

void test_runtime_trans_input_skip() {
    // input=1: a physical click during the transition skips it immediately
    // ( skip_by_input +  advance_wait_state).
    MemFs fs;
    fs.files["system.ini"] =
        "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
        "BOOT = system/first.iet\nSAVEPATH = save\n";
    fs.files["system/first.iet"] =
        "*main\n[trans type=\"1\" time=\"100000\" input=\"1\"]\n[stop]\n";
    oa::runtime::GameRuntime rt(std::make_shared<MemFs>(fs));
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    rt.tick(16, idle);
    for (const auto& e : rt.drain_events())
        if (e.kind == oa::runtime::Event::Kind::Trans) rt.transition_begin(e.params);
    check(rt.transition().is_in_progress(rt.now_ms()), "long transition running");

    oa::runtime::FrameInput click;
    click.left_click_edge = true;
    rt.tick(16, click);
    const oa::runtime::WaitReason* w = rt.current_wait();
    check(w == nullptr || !(w->kind == oa::runtime::WaitReason::Kind::Stop &&
                            w->id == "trans"),
          "click skipped the trans wait (input=1)");
    check(!rt.transition().active(), "click cleared the transition state");
}

void test_interpreter_event_kinds() {
    // [trans]/[flip] are typed events with verbatim params (P2a).
    oa::runtime::Interpreter it;
    std::vector<oa::runtime::Event> events;
    it.set_callback([&](const oa::runtime::Event& e) {
        events.push_back(e);
        return e.kind == oa::runtime::Event::Kind::Wait_ ? oa::runtime::CallbackResult::Pause
                                                        : oa::runtime::CallbackResult::Continue;
    });
    it.load_script("test", R"(
*main
[trans type="2" time="800" rule="rule000" vague="16" input="0"]
[trans type="0"]
[flip]
[lyc id="1" file="a"]
[lytween id="1" param="alpha" from="0" to="255" time="100" ease="easeinout_quad" handler="calllua" function="f"]
[lytweendel id="1"]
[tweenset]
[lytween id="1" param="left" from="0" to="10" time="50"]
[/tweenset]
[stop]
)");
    it.start("test", "main");
    for (;;) {
        const oa::runtime::ExecutionResult r = it.run();
        if (r == oa::runtime::ExecutionResult::Completed) break;
        it.next_line();
    }
    size_t trans = 0, flip = 0, tween = 0, del = 0, set_end = 0;
    const oa::runtime::Event* trans2 = nullptr;
    for (const auto& e : events) {
        if (e.kind == oa::runtime::Event::Kind::Trans) {
            ++trans;
            if (e.params.at("type") == "2") trans2 = &e;
        } else if (e.kind == oa::runtime::Event::Kind::Flip) {
            ++flip;
        } else if (e.kind == oa::runtime::Event::Kind::LayerEventCmd && e.tag == "lytween") {
            ++tween;
        } else if (e.kind == oa::runtime::Event::Kind::LayerEventCmd &&
                   e.tag == "lytweendel") {
            ++del;
        } else if (e.kind == oa::runtime::Event::Kind::LayerEventCmd &&
                   e.tag == "/tweenset") {
            ++set_end;
        }
    }
    check(trans == 2, "two [trans] events");
    check(flip == 1, "[flip] typed event");
    check(tween == 2, "two [lytween] LayerEventCmd events");
    check(del == 1, "[lytweendel] event");
    check(set_end == 1, "[/tweenset] event");
    check(trans2 != nullptr, "type=2 trans event present");
    if (trans2) {
        check(trans2->params.at("time") == "800" &&
                  trans2->params.at("rule") == "rule000" &&
                  trans2->params.at("vague") == "16" && trans2->params.at("input") == "0",
              "trans params preserved verbatim");
    }
}

} // namespace

int main() {
    test_easing();
    test_tween_get_value();
    test_transition_machine();
    test_compositor_tweens();
    test_interpreter_event_kinds();
    test_runtime_trans_wait();
    test_runtime_trans_input_skip();
    if (failures) {
        std::fprintf(stderr, "transition_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("transition_test: all ok\n");
    return 0;
}
