// P1c2 control domain + row-navigation tests (ALIGNMENT queue item 7 /
// research/11 §9 + research/18). MemFs runtime harness (mirrors
// input_dispatch_test): script lines drive a real GameRuntime; assertions
// pin the engine surfaces added by P1c2 part 1:
//   C0 layer-row file/label navigation: label 'jump' is permanent, 'call'
//      returns to the origin (filter inline round trip);
//   C1 keyconfig role-0 advance releases an input wait (no mouse);
//   C2 [exec skip] toggles command skip and fires the registered
//      commandskipin mode handler;
//   C3 control-skip (role-14 key held) engages/disengages skip and fires
//      controlskipin/out;
//   C4 rclick chain: key 2 runs the configured rclick script as a call;
//   C5 hide mode: window layers hide, a left click recovers and swallows
//      the click;
//   C6 decide edge (e:overrideKey status=32) advances an input wait;
//   C7 e:var system=get_layer_info reads typed layer props (slider face).
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
}

#include "core/fs/fs.h"
#include "core/runtime/runtime.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

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

struct Harness {
    std::shared_ptr<MemFs> fs = std::make_shared<MemFs>();
    std::unique_ptr<oa::runtime::GameRuntime> rt;
    Harness(const std::string& script) {
        fs->files["system.ini"] =
            "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
            "BOOT = system/first.iet\nSAVEPATH = save\n";
        fs->files["system/first.iet"] = script;
        rt = std::make_unique<oa::runtime::GameRuntime>(fs);
        rt->open_project("windows");
        rt->boot_project();
        rt->set_hit_providers(oa::render::Compositor::QuadSizeFn(),
                              oa::render::Compositor::AlphaSamplerFn(), nullptr);
    }
    void run_lua(const char* code) { rt->interpreter().lua_bridge().run_code(code, "p1c2_control_test"); }
    /// Invoke a Lua function with the engine handle as its first argument.
    bool call_lua(const char* fn) { return rt->interpreter().lua_bridge().call_function(fn, {}); }
    void warm(int n = 4) {
        for (int i = 0; i < n; ++i) tick(-5, -5);
    }
    std::string lua_string(const char* name) {
        lua_State* L = rt->interpreter().lua_bridge().state();
        lua_getglobal(L, name);
        const char* s = lua_tostring(L, -1);
        std::string out = s ? s : "";
        lua_pop(L, 1);
        return out;
    }
    void tick(int mx, int my, bool down = false, bool click_edge = false,
              const std::vector<int>& keys = {}) {
        oa::runtime::FrameInput in;
        in.mouse_x = mx;
        in.mouse_y = my;
        in.left_down = down;
        in.left_click_edge = click_edge;
        in.key_down_edges = keys;
        in.keys_down.insert(keys.begin(), keys.end());
        rt->tick(16, in);
    }
    /// Tick until the runtime parks on the given wait kind (bounded).
    bool park_on(oa::runtime::WaitReason::Kind kind, int mx, int my) {
        for (int i = 0; i < 60; ++i) {
            tick(mx, my);
            const oa::runtime::WaitReason* w = rt->current_wait();
            if (w && w->kind == kind) return true;
        }
        return false;
    }
    std::string wait_str() const {
        const oa::runtime::WaitReason* w = rt->current_wait();
        if (!w) return "none";
        return std::string(1, char('0' + (int)w->kind));
    }
};

const char* kLoggers =
    "function landed() oa_log=(oa_log or '')..'landed;' end\n"
    "function jumped() oa_log=(oa_log or '')..'jumped;' end\n"
    "function rclk() oa_log=(oa_log or '')..'rclk;' end\n"
    "function after() oa_log=(oa_log or '')..'after;' end\n"
    "function ev_skipin() oa_log=(oa_log or '')..'skipin;' end\n"
    "function ev_skipout() oa_log=(oa_log or '')..'skipout;' end\n"
    "function ctl_in() oa_log=(oa_log or '')..'ctlin;' end\n"
    "function ctl_out() oa_log=(oa_log or '')..'ctlout;' end\n";

// C0: a click row whose registration carries file/label navigates on click:
// plain label = permanent jump into the label; call = returns to origin.
void test_c0_row_file_label_navigation() {
    {
        Harness h("*main\n"
                  "[lyc2 id=\"top\" width=\"40\" height=\"40\" x=\"60\" y=\"60\"]\n"
                  "[lyevent id=\"top\" type=\"click\" label=\"sub\"]\n"
                  "[stop]\n"
                  "*sub\n"
                  "[calllua function=\"jumped\"]\n"
                  "[stop]\n");
        h.warm();
        h.run_lua(kLoggers);
        check(h.rt->scene().find_event_handler("top", "click") != nullptr, "C0a row registered");
        const oa::render::LayerEventHandler* row =
            h.rt->scene().find_event_handler("top", "click");
        check(row && row->label == "sub", "C0a row carries the label");
        h.tick(80, 80, true, true); // click the button while parked at [stop]
        check(h.lua_string("oa_log").find("jumped;") != std::string::npos,
              "C0a label-jump row navigated into *sub (queued jump drained)");
    }
    {
        // call=true: the same click returns to the origin after the label's
        // [return] — the "inline" round trip the deferred stub documented.
        Harness h("*main\n"
                  "[lyc2 id=\"top\" width=\"40\" height=\"40\" x=\"60\" y=\"60\"]\n"
                  "[lyevent id=\"top\" type=\"click\" call=\"1\" label=\"sub\"]\n"
                  "[stop]\n"
                  "*sub\n"
                  "[calllua function=\"jumped\"]\n"
                  "[return]\n");
        h.warm();
        h.run_lua(kLoggers);
        check(h.wait_str() == "3", "C0b parked at the origin [stop]");
        h.tick(80, 80, true, true); // click: enqueue call to *sub
        check(h.lua_string("oa_log").find("jumped;") != std::string::npos,
              "C0b call-row executed the target label");
        h.tick(-5, -5); // [return] pops the queued-call frame
        // queued call keeps its return_line at the parked origin instruction:
        // the story resumes waiting at the same [stop] (menu-return semantics),
        // never advancing past it on its own.
        check(h.wait_str() == "3",
              "C0b [return] landed back on the origin [stop] park (inline round trip)");
        check(h.lua_string("oa_log").find("jumped;") != std::string::npos,
              "C0b round trip executed exactly once (no repeat loop)");
    }
}

// C1: keyconfig role-0 keys advance an input wait (click substitute).
void test_c1_role0_advance() {
    Harness h("*main\n"
              "[keyconfig role=\"0\" keys=\"124\"]\n"
              "[wt0]\n"
              "[calllua function=\"landed\"]\n"
              "[stop]\n");
    h.warm();
    h.run_lua(kLoggers);
    check(h.park_on(oa::runtime::WaitReason::Kind::Generic0, -5, -5), "C1 parked at the Generic0 wait");
    h.tick(-5, -5, false, false, {124}); // role-0 key edge, no mouse
    h.tick(-5, -5); // released wait runs the following calllua on this tick
    check(h.lua_string("oa_log").find("landed;") != std::string::npos,
          "C1 role-0 key edge advanced the wait (clicked substitute)");
}

// C2: [exec command=skip mode=1] engages command skip and fires
// commandskipin; the skip releases a following generic wait.
void test_c2_exec_skip_mode_event() {
    Harness h("*main\n"
              "[setoncommandskipin handler=\"calllua\" function=\"ev_skipin\"]\n"
              "[exec command=\"skip\" mode=\"1\"]\n"
              "[wt0]\n"
              "[calllua function=\"landed\"]\n"
              "[stop]\n");
    h.run_lua(kLoggers);
    h.warm();
    h.tick(-5, -5);
    h.tick(-5, -5); // released wt0 runs the following calllua
    check(h.rt->skip_active(), "C2 skip active after exec skip");
    check(h.park_on(oa::runtime::WaitReason::Kind::Stop, -5, -5), "C2 script reached the [stop] (skip released wt0)");
    check(h.lua_string("oa_log").find("landed;") != std::string::npos, "C2 landed ran");
}

// C3: control-skip — role-14 key held across ticks engages effective skip and
// fires controlskipin/out on the transitions.
void test_c3_control_skip_hold() {
    Harness h("*main\n"
              "[keyconfig role=\"14\" keys=\"17\"]\n"
              "[setoncontrolskipin handler=\"calllua\" function=\"ctl_in\"]\n"
              "[setoncontrolskipout handler=\"calllua\" function=\"ctl_out\"]\n"
              "[stop]\n");
    h.warm();
    h.run_lua(kLoggers);
    check(!h.rt->skip_active(), "C3 skip inactive before the hold");
    h.tick(-5, -5, false, false, {17});
    check(h.rt->control_skip_effective(), "C3 control-skip effective while 17 held");
    check(h.rt->skip_active(), "C3 effective skip while 17 held");
    check(h.lua_string("oa_log").find("ctlin;") != std::string::npos,
          "C3 controlskipin fired on press");
    h.tick(-5, -5, false, false, {}); // release
    check(!h.rt->skip_active(), "C3 skip clears on release");
    check(h.lua_string("oa_log").find("ctlout;") != std::string::npos,
          "C3 controlskipout fired on release");
}

// C4: rclick chain — with [rclick allow=1 file] the right key runs the
// configured script as a call (returning to the origin).
void test_c4_rclick_chain() {
    Harness h("*main\n"
              "[rclick allow=\"1\" file=\"r.iet\"]\n"
              "[stop]\n");
    h.fs->files["r.iet"] =
        "*main\n[calllua function=\"rclk\"]\n[return]\n";
    h.warm();
    h.run_lua(kLoggers);
    check(h.rt->rclick_allowed(), "C4 rclick enabled");
    h.tick(-5, -5, false, false, {2}); // mouse right
    check(h.lua_string("oa_log").find("rclk;") != std::string::npos,
          "C4 right key ran the rclick script (call, returned)");
}

// C5: hide mode — window layers hide, left click recovers and swallows.
void test_c5_hide_mode() {
    Harness h("*main\n"
              "[lyc2 id=\"win\" width=\"40\" height=\"40\" x=\"0\" y=\"0\"]\n"
              "[hide allow=\"1\" window=\"win\"]\n"
              "[exec command=\"hide\"]\n"
              "[wt0]\n"
              "[calllua function=\"landed\"]\n"
              "[stop]\n");
    h.warm();
    h.run_lua(kLoggers);
    check(h.rt->hide_active(), "C5 hide mode active after exec hide");
    const oa::render::Layer* win = h.rt->scene().find("win");
    check(win && win->visible == 0.0, "C5 window layer hidden");
    check(h.park_on(oa::runtime::WaitReason::Kind::Generic0, -5, -5), "C5 parked at the generic wait");
    h.tick(20, 20, true, true); // click inside the (hidden) window area
    check(!h.rt->hide_active(), "C5 click recovered the message window");
    check(win && win->visible == 1.0, "C5 window layer restored");
    check(h.lua_string("oa_log").find("landed;") == std::string::npos,
          "C5 recovery click was swallowed (no advance)");
    h.tick(20, 20, true, true); // now a normal click releases the wait
    h.tick(20, 20); // released wait runs the following calllua
}

// C6: decide edge (overrideKey status=32, the FPM dummy click) advances an
// input wait.
void test_c6_decide_advances_input_wait() {
    Harness h("*main\n"
              "[wt0]\n"
              "[calllua function=\"landed\"]\n"
              "[stop]\n");
    h.run_lua(kLoggers);
    // onEnterFrame hook arms a one-shot override-decide injection (mirrors
    // vsync.lua: flg.exclick -> e:overrideKey{key=124, status=32} inside the
    // frame handler; the engine handle arrives as the handler's `e`).
    h.run_lua("oa_armed=false\n"
              "function oa_decide(e) if oa_armed then oa_armed=false "
              "e:overrideKey{key=124, status=32} end end\n"
              "function oa_install(e) e:setEventHandler{['onEnterFrame']='oa_decide'} end\n");
    h.call_lua("oa_install");
    h.warm();
    check(h.park_on(oa::runtime::WaitReason::Kind::Generic0, -5, -5), "C6 parked at the Generic0 wait");
    h.run_lua("oa_armed=true");
    h.tick(-5, -5); // vsync fires: decide edge lands in this same advance
    h.tick(-5, -5); // released wait runs the following calllua
    check(h.lua_string("oa_log").find("landed;") != std::string::npos,
          "C6 decide edge advanced the input wait (dummy click)");
}

// C7: e:var system=get_layer_info reads the typed layer props after
// [lyprop] (slider drag readback: name.left/top).

void test_r5_decide_after_reveal_gate_releases() {
    // R5(项3) 回归：reveal 门吞掉一次 decide 后，后续 decide 必须仍然能推进
    // （每帧清空按键覆盖；滞留位会把之后的边沿全部吞掉 → “剧情点击不推进”）。
    // decide 经 onEnterFrame 注入（同 FPM vsync exclick 路径）。
    Harness h("*main\n"
              "[font face=\"f\" size=\"40\" width=\"300\"]\n"
              "[scetween mode=\"init\" type=\"show\"]\n"
              "[scetween mode=\"add\" type=\"show\" param=\"alpha\" ease=\"none\" "
              "diff=\"0\" time=\"1000\" delay=\"200\"]\n"
              "[print data=\"あいうえお\"]\n"
              "[@]\n[calllua function=\"landed\"]\n[stop]\n");
    h.run_lua(kLoggers);
    h.run_lua("oa_armed=false\n"
              "function oa_decide(e) if oa_armed then oa_armed=false "
              "e:overrideKey{key=124, status=32} end end\n"
              "function oa_install(e) e:setEventHandler{['onEnterFrame']='oa_decide'} end\n");
    h.call_lua("oa_install");
    h.warm(2);
    check(h.rt->current_wait() &&
              (h.rt->current_wait()->kind == oa::runtime::WaitReason::Kind::Generic ||
               h.rt->current_wait()->kind == oa::runtime::WaitReason::Kind::Generic0),
          "R5-1 parked on the click wait with reveal pending");
    // 第一次 decide：揭示门只整页揭示，不推进
    h.run_lua("oa_armed=true");
    h.tick(-5, -5);
    const oa::render::MessageLayer* ml = h.rt->text().layer("adv01");
    check(ml && ml->reveal_index >= ml->char_count && !ml->reveal_pending,
          "R5-2 first decide revealed the full page");
    check(h.rt->current_wait() &&
              (h.rt->current_wait()->kind == oa::runtime::WaitReason::Kind::Generic ||
               h.rt->current_wait()->kind == oa::runtime::WaitReason::Kind::Generic0),
          "R5-3 still parked after the reveal-gated decide");
    check(h.lua_string("oa_log").find("landed;") == std::string::npos,
          "R5-3b story did not advance on the reveal-gated decide");
    // 第二次 decide：必须推进
    h.run_lua("oa_armed=true");
    h.tick(-5, -5);
    h.tick(-5, -5);
    check(h.lua_string("oa_log").find("landed;") != std::string::npos,
          "R5-4 second decide edge advances the story (no override mask stall)");
}

void test_c7_layer_info_readback() {
    Harness h("*main\n"
              "[lyc2 id=\"win\" width=\"40\" height=\"40\" x=\"60\" y=\"60\"]\n"
              "[lyprop id=\"win\" left=\"80\" top=\"70\"]\n"
              "[var name=\"t.ly\" system=\"get_layer_info\" id=\"win\"]\n"
              "[stop]\n");
    h.warm();
    const auto left = h.rt->interpreter().variables().get("t.ly.left");
    const auto top = h.rt->interpreter().variables().get("t.ly.top");
    check(left.has_value() && left->to_string() == "80",
          "C7 get_layer_info reads typed left (after lyprop)");
    check(top.has_value() && top->to_string() == "70",
          "C7 get_layer_info reads typed top (after lyprop)");
}
// C8: [mouse left/top] (config tag) requests an OS pointer warp. The tag is
// consumed at dispatch (never forwarded to the host stream), the host
// callback receives stage coords (clamped to the stage). On hosts that can
// warp the OS cursor the arrival comes back as real SDL motion events; on
// hosts without feedback (hidden window / headless) the runtime emulates the
// move itself while no fresh raw input arrives — the pointer advances to the
// requested target through the normal pointer chain and the first raw input
// change hands control back.
void test_c8_mouse_warp_request() {
    {
        Harness h("*main\n"
                  "[stop]\n");
        h.warm();
        int lx = -1, ly = -1, calls = 0;
        h.rt->set_pointer_warp_callback([&](int x, int y) { lx = x; ly = y; ++calls; });
        h.run_lua("function mouse_warp_a(e) e:tag{\"mouse\", left=\"321\", top=\"123\"} end");
        h.call_lua("mouse_warp_a");
        h.tick(-5, -5); // parked drain dispatches the queued [mouse] tag
        check(calls == 1 && lx == 321 && ly == 123,
              "C8a [mouse] fired the host warp callback with stage coords");
        check(h.rt->pointer_warp_requests() == 1, "C8a warp request counter");
        h.run_lua("function mouse_warp_b(e) e:tag{\"mouse\", left=\"99999\", top=\"-40\"} end");
        h.call_lua("mouse_warp_b");
        h.tick(-5, -5);
        check(calls == 2 && lx == 1279 && ly == 0,
              "C8b warp coords clamped to the stage (1280x720 ini)");
        h.rt->drain_events(); // flush any boot-time leftovers first
        h.run_lua("function mouse_warp_c(e) e:tag{\"mouse\", left=\"7\", top=\"8\"} end");
        h.call_lua("mouse_warp_c");
        h.tick(-5, -5);
        check(h.rt->drain_events().empty(),
              "C8c [mouse] consumed at dispatch (not forwarded to the host)");
    }
    {
        // No-feedback host: with the raw pointer static, the runtime emulates
        // the warp — pointer lands on the target and the layer under it rolls
        // over; the first raw input change clears the emulation (rollout).
        Harness h("*main\n"
                  "[lyc2 id=\"top\" width=\"40\" height=\"40\" x=\"60\" y=\"60\"]\n"
                  "[lyevent id=\"top\" over=\"btn_over\" out=\"btn_out\" "
                  "key=\"bt_yes\"]\n"
                  "[stop]\n");
        h.warm(); // raw pointer parked at (-5,-5), no motion events
        h.run_lua("function mouse_warp_d(e) e:tag{\"mouse\", left=\"80\", top=\"80\"} end");
        h.call_lua("mouse_warp_d");
        h.tick(-5, -5); // tick 1: drain dispatches the [mouse] tag (pending)
        h.tick(-5, -5); // tick 2: static raw input -> emulation takes effect
        const auto mp1 = h.rt->mouse_point();
        check(mp1.first == 80 && mp1.second == 80,
              "C8d emulated warp moves the engine pointer onto the target");
        check(h.rt->is_hovered("top"),
              "C8d emulated warp rolls the layer under the target over");
        h.tick(300, 300); // fresh raw input: host takes the pointer back
        const auto mp2 = h.rt->mouse_point();
        check(mp2.first == 300 && mp2.second == 300 && !h.rt->is_hovered("top"),
              "C8e raw input change clears the warp emulation (rollout fires)");
    }
}

} // namespace

int main() {
    test_c0_row_file_label_navigation();
    test_c1_role0_advance();
    test_c2_exec_skip_mode_event();
    test_c3_control_skip_hold();
    test_c4_rclick_chain();
    test_c5_hide_mode();
    test_c6_decide_advances_input_wait();
    test_c7_layer_info_readback();
    test_r5_decide_after_reveal_gate_releases();
    test_c8_mouse_warp_request();
    if (failures) {
        std::fprintf(stderr, "p1c2_control_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("p1c2_control_test: all ok\n");
    return 0;
}
