// P1c input dispatch tests (docs/research/11-input-dispatch.md §1-§8,
// I1-I19). Two levels:
//   core:    clickablethreshold alpha sampling , lyevent
//            mode table + FPM glue (tags/),
//            draggable/dragarea clamp .
//   runtime: hover rollover/rollout set diff, click consumption order with
//            penetration, drag chain, global push, e:setEventFilter verdicts,
//            click swallowing of input waits (runtime/
//            962-1080).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
}

#include "core/fs/fs.h"
#include "core/runtime/runtime_lua.h"
#include "core/runtime/runtime.h"
#include "core/render/layer.h"

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
using oa::render::Compositor;

// ---------------------------------------------------------------------------
// core: clickablethreshold (I1-I4)
// ---------------------------------------------------------------------------

/// Sampler fixture: transparent strip on tex x in [start, start+strip_w).
/// Caller checks how the clip offset moves the strip.
struct StripSampler {
    int strip_w = 10; // strip width in tex pixels
    int start = 0;    // strip start in tex x
    uint8_t alpha0 = 0;
    uint8_t operator()(void* ptr, const oa::render::Layer&, int tx, int) const {
        return tx >= start && tx < start + strip_w ? alpha0 : 255;
    }
};

void test_i1_unset_or_bad_threshold_always_clickable() {
    Compositor c;
    c.create("b", {});
    c.set_props("b", {{"left", "0"}, {"top", "0"}, {"width", "100"}, {"height", "100"}});
    // no threshold at all: every pixel (sampler irrelevant) hits
    StripSampler sampler;
    auto hits = c.hit_test_all(50, 50, nullptr, nullptr, sampler);
    check(hits.size() == 1 && hits[0] == "b", "I1 no threshold -> hit regardless of alpha");
    // unparsable threshold behaves as unset (custom parse, )
    c.set_props("b", {{"clickablethreshold", "abc"}});
    check(c.hit_test(50, 50, nullptr, nullptr, sampler) == "b",
          "I1 unparsable clickablethreshold -> clickable");
    // threshold 0: alpha < 0 is impossible -> never transparent 
    c.set_props("b", {{"clickablethreshold", "0"}});
    check(c.hit_test(1, 1, nullptr, nullptr, sampler) == "b",
          "I1 threshold 0 never transparent (0 < 0 false)");
}

void test_i2_alpha_below_threshold_transparent() {
    Compositor c;
    c.create("dock", {});
    c.set_props("dock", {{"left", "0"}, {"top", "0"}, {"width", "100"},
                         {"height", "100"}, {"alpha", "0"},
                         {"clickablethreshold", "128"}});
    StripSampler sampler;
    check(c.hit_test(90, 50, nullptr, nullptr, sampler) == "dock",
          "I2 pixel alpha 255 >= 128 hits (layer alpha 0 irrelevant)");
    check(c.hit_test(2, 50, nullptr, nullptr, sampler) == std::string(),
          "I2 pixel alpha 0 < 128 transparent (layer alpha must not be used)");
}

void test_i3_sample_coords_include_clip_offset() {
    // clip = texture source rect [x,y,w,h]; quad size = w x h; texture sample
    // coords = local pixel + clip offset . Strip sampler
    // is transparent on tex x in [10,20).
    Compositor c;
    c.create("sp", {});
    c.set_props("sp", {{"left", "0"}, {"top", "0"}, {"clip", "10,0,100,100"},
                       {"clickablethreshold", "128"}});
    StripSampler sampler;
    sampler.start = 10;
    // local x in [0,10) -> tex x in [10,20): transparent => miss. If the clip
    // offset were ignored (tex=local), local 5 would sample the opaque strip
    // and hit — the miss proves the +10 shift (I3).
    check(c.hit_test(5, 50, nullptr, nullptr, sampler) == std::string(),
          "I3 local 5 -> tex 15 (transparent strip): miss");
    check(c.hit_test(15, 50, nullptr, nullptr, sampler) == "sp",
          "I3 local 15 -> tex 25 (opaque): hit with the clip offset");
    // without any clip the same sampler is transparent on local [0,10): local
    // 5 now samples tex 5 (opaque) and hits — behaviour difference documents
    // that clip shifts sampling.
    Compositor plain;
    plain.create("sp2", {});
    plain.set_props("sp2", {{"left", "0"}, {"top", "0"}, {"clip", "0,0,100,100"},
                            {"clickablethreshold", "128"}});
    check(plain.hit_test(5, 50, nullptr, nullptr, sampler) == "sp2",
          "I3 no clip shift: local 5 samples tex 5 (opaque)");
}

void test_i4_fallback_to_layer_alpha_when_sampler_missing() {
    Compositor c;
    c.create("s", {});
    c.set_props("s", {{"left", "0"}, {"top", "0"}, {"width", "50"},
                      {"height", "50"}, {"alpha", "0"}, {"clickablethreshold", "128"}});
    // sampler unavailable (fallback -> layer alpha)
    check(c.hit_test(25, 25, nullptr, nullptr) == std::string(),
          "I4 no sampler + layer alpha 0 < 128 -> transparent");
    c.set_props("s", {{"alpha", "255"}});
    check(c.hit_test(25, 25, nullptr, nullptr) == "s",
          "I4 no sampler + layer alpha 255 -> clickable");
}

// ---------------------------------------------------------------------------
// core: lyevent registration mode table + FPM glue (I13/I16)
// ---------------------------------------------------------------------------

void test_i13_modes_and_enable_without_row() {
    Compositor c;
    c.set_props("slot", {{"width", "100"}, {"height", "100"}}); // autovivify
    c.apply_lyevent("slot", {{"type", "rollover"}, {"mode", "init"},
                             {"handler", "calllua"}, {"function", "btn_over"},
                             {"key", "bt_save10"}});
    const auto* row = c.find_event_handler("slot", "rollover");
    check(row && row->enabled && row->handler == "calllua", "I13 init registers a row");
    check(row && row->params.at("function") == "btn_over" &&
              row->params.at("key") == "bt_save10",
          "I13 registration extras preserved");
    // disable keeps params, disables
    c.apply_lyevent("slot", {{"type", "rollover"}, {"mode", "disable"}});
    row = c.find_event_handler("slot", "rollover");
    check(row && !row->enabled, "I13 disable suspends the row");
    check(row && row->params.at("function") == "btn_over" &&
              row->params.at("key") == "bt_save10",
          "I13 disable keeps key/function params");
    check(!c.has_any_enabled_handler("slot"), "I14 disabled row leaves no enabled handler");
    // enable restores (and enable without an existing row registers)
    c.apply_lyevent("slot", {{"type", "rollover"}, {"mode", "enable"}});
    row = c.find_event_handler("slot", "rollover");
    check(row && row->enabled && row->params.at("function") == "btn_over",
          "I13 enable restores the original row");
    c.apply_lyevent("other", {{"type", "click"}, {"mode", "enable"},
                              {"handler", "calllua"}, {"function", "late"}});
    row = c.find_event_handler("other", "click");
    check(row && row->enabled && row->params.at("function") == "late",
          "I13 enable without an existing row registers");
    // reset removes
    c.apply_lyevent("slot", {{"type", "rollover"}, {"mode", "reset"}});
    check(c.find_event_handler("slot", "rollover") == nullptr, "I13 reset removes the row");
}

void test_i16_fpm_typless_row_expansion() {
    Compositor c;
    c.set_props("500.b.2.0", {{"left", "0"}, {"top", "0"}, {"width", "80"},
                              {"height", "48"}});
    // FPM button.lua:125-128 shape: no type/handler, function keys in params.
    c.apply_lyevent("500.b.2.0", {{"click", "btn_click"}, {"over", "btn_over"},
                                  {"out", "btn_out"}, {"name", "ttl1"},
                                  {"key", "bt_start"}});
    const auto* click = c.find_event_handler("500.b.2.0", "click");
    const auto* over = c.find_event_handler("500.b.2.0", "rollover");
    const auto* out = c.find_event_handler("500.b.2.0", "rollout");
    check(click && over && out, "I16 one typless row expands to click/rollover/rollout");
    check(click && click->enabled && click->handler == "calllua" &&
              click->params.at("function") == "btn_click",
          "I16 click row carries function=btn_click + calllua handler");
    check(click && click->params.at("key") == "bt_start" &&
              click->params.at("name") == "ttl1",
          "I16 extras (name/key) forwarded");
    check(over && over->params.at("function") == "btn_over",
          "I16 rollover row function = over value");
    // a typed disable row (setBtnStat shape) only touches its own type
    c.apply_lyevent("500.b.2.0", {{"type", "click"}, {"mode", "disable"}});
    check(!c.find_event_handler("500.b.2.0", "click")->enabled,
          "I16 disable click leaves the click row suspended");
    check(c.find_event_handler("500.b.2.0", "rollover")->enabled,
          "I16 disable click does not touch rollover");
    // filter params must include registration id/type/function for the Lua
    // event filter (complete_event_filter_params).
    check(click && click->filter_params.at("id") == "500.b.2.0" &&
              click->filter_params.at("type") == "click" &&
              click->filter_params.at("function") == "btn_click",
          "I16 filter_params carry id/type/function");
}

// ---------------------------------------------------------------------------
// core: draggable / dragarea clamp (I10)
// ---------------------------------------------------------------------------

void test_i10_drag_layer_clamps_into_dragarea() {
    Compositor c;
    c.set_props("knob", {{"left", "80"}, {"top", "0"}, {"draggable", "1"},
                         {"dragarea", "0,0,160,0"}});
    check(c.is_layer_draggable("knob"), "I10 draggable=1 is draggable");
    double l = 0, t = 0;
    check(c.drag_layer_to("knob", 80, 0, 200, 20, &l, &t), "I10 drag_layer_to ok");
    check(approx(l, 160.0) && approx(t, 0.0), "I10 +200 clamped to dragarea max 160");
    check(approx(c.find("knob")->left, 160.0), "I10 layer left written back");
    c.drag_layer_to("knob", 80, 0, -200, -20, &l, &t);
    check(approx(l, 0.0) && approx(t, 0.0), "I10 -200 clamped to dragarea min 0");
    check(c.is_layer_draggable("nope") == false, "I10 missing layer not draggable");
    Compositor off;
    off.set_props("k2", {{"draggable", "0"}});
    check(!off.is_layer_draggable("k2"), "I10 draggable=0 is not draggable");
}

// ---------------------------------------------------------------------------
// runtime: mini project harness
// ---------------------------------------------------------------------------

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
    std::vector<oa::runtime::GameRuntime::PointerDispatch> notes;
    Harness(const std::string& script) {
        fs->files["system.ini"] =
            "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
            "BOOT = system/first.iet\nSAVEPATH = save\n";
        fs->files["system/first.iet"] = script;
        rt = std::make_unique<oa::runtime::GameRuntime>(fs);
        rt->open_project("windows");
        rt->boot_project();
        rt->set_pointer_observer([this](const oa::runtime::GameRuntime::PointerDispatch& d) {
            notes.push_back(d);
        });
        rt->set_hit_providers(oa::render::Compositor::QuadSizeFn(), oa::render::Compositor::AlphaSamplerFn(), nullptr);
    }
    void run_lua(const char* code) {
        rt->interpreter().lua_bridge().run_code(code, "input_dispatch_test");
    }
    /// Invoke a Lua function with the engine handle as its first argument.
    bool call_lua(const char* fn) {
        return rt->interpreter().lua_bridge().call_function(fn, {});
    }
    /// Execute the boot script lines (they run on the first tick, until the
    /// script parks on its stop/wait) with the pointer far away so nothing can
    /// hover or dispatch.
    void warm(int n = 3) {
        for (int i = 0; i < n; ++i) tick(-5, -5);
    }
    /// Read a Lua string global ("" when nil / not a string).
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
    size_t notes_of(oa::runtime::GameRuntime::PointerDispatch::Kind k) const {
        size_t n = 0;
        for (const auto& d : notes)
            if (d.kind == k) ++n;
        return n;
    }
    const oa::render::Layer* layer(const std::string& id) const { return rt->scene().find(id); }
};

const char* kHandlers =
    "function over_h(e,p) oa_log=(oa_log or '')..'over:'..(p.key or '?')..';' end\n"
    "function out_h(e,p)  oa_log=(oa_log or '')..'out:'..(p.key or '?')..';' end\n"
    "function click_h(e,p) oa_log=(oa_log or '')..'click:'..(p.key or '?')..':'.."
    "(p.click=='1' and '1' or '0')..';' end\n"
    "function drag_h(e,p) oa_log=(oa_log or '')..'drag:'..(p.key or '?')..':'.."
    "(p.drag or '?')..';' end\n";

// runtime helper layers: 60,60 40x40 (center 80,80); 60,120 40x40; 60,180 40x40.
const char* kBaseScript =
    "*main\n"
    "[lyc2 id=\"top\" width=\"40\" height=\"40\" x=\"60\" y=\"60\"]\n"
    "[lyc2 id=\"mid\" width=\"40\" height=\"40\" x=\"60\" y=\"120\"]\n"
    "[lyc2 id=\"low\" width=\"40\" height=\"40\" x=\"60\" y=\"180\"]\n";

void test_rt_hover_over_out_and_click() {
    Harness h(std::string(kBaseScript) +
              "[lyevent id=\"top\" click=\"click_h\" over=\"over_h\" out=\"out_h\" name=\"g\" key=\"k1\"]\n"
              "[stop]\n");
    h.warm();
    h.run_lua(kHandlers);
    check(h.rt->scene().find_event_handler("top", "click") != nullptr,
          "RT1 registrations landed in the scene");
    check(h.rt->scene().find_event_handler("top", "click")->params.at("function") == "click_h",
          "RT1 glue function forwarding");
    // enter the button: rollover (function h, key k1)
    h.tick(80, 80);
    check(h.rt->is_hovered("top"), "RT1 hover set contains top");
    check(h.notes_of(oa::runtime::GameRuntime::PointerDispatch::Kind::HoverIn) == 1,
          "RT1 one rollover dispatch on entry");
    check(h.lua_string("oa_log") == "over:k1;", "RT1 rollover Lua called with over:key");
    // pointer still: no extra events
    h.tick(80, 80);
    check(h.notes_of(oa::runtime::GameRuntime::PointerDispatch::Kind::HoverIn) == 1,
          "RT1 stationary pointer does not re-fire rollover");
    // click inside: click handler with runtime param click=1
    h.tick(80, 80, true, true);
    check(h.notes_of(oa::runtime::GameRuntime::PointerDispatch::Kind::Click) == 1,
          "RT1 click dispatch happened");
    check(h.lua_string("oa_log").find("click:k1:1;") != std::string::npos,
          "RT1 click Lua got runtime click=1 + key=k1");
    // leave: rollout
    h.tick(300, 300);
    check(!h.rt->is_hovered("top"), "RT1 hover cleared after leaving");
    check(h.notes_of(oa::runtime::GameRuntime::PointerDispatch::Kind::HoverOut) == 1,
          "RT1 rollout dispatch on leave");
    check(h.lua_string("oa_log").find("out:k1;") != std::string::npos,
          "RT1 rollout Lua called with out");
}

void test_rt_disabled_click_not_dispatched() {
    Harness h(std::string(kBaseScript) +
              "[lyevent id=\"top\" click=\"click_h\" over=\"over_h\" out=\"out_h\" name=\"g\" key=\"k1\"]\n"
              "[lyevent id=\"top\" type=\"click\" mode=\"disable\"]\n"
              "[stop]\n");
    h.warm();
    h.run_lua(kHandlers);
    check(!h.rt->scene().find_event_handler("top", "click")->enabled,
          "RT2 disable mode applied");
    check(h.rt->scene().find_event_handler("top", "rollover")->enabled,
          "RT2 rollover unaffected by the click disable");
    h.tick(80, 80);
    check(h.rt->is_hovered("top"), "RT2 rollover still works while click is disabled");
    h.tick(80, 80, true, true);
    check(h.notes_of(oa::runtime::GameRuntime::PointerDispatch::Kind::Click) == 0,
          "RT2 disabled click row never dispatches");
}

void test_rt_penetration_click_order() {
    // overlap all three at 80,80: top (z first) covers mid and low.
    Harness h(std::string(kBaseScript) +
              "[lyc2 id=\"top\" width=\"200\" height=\"200\" x=\"40\" y=\"40\"]\n"
              "[lyc2 id=\"mid\" width=\"200\" height=\"200\" x=\"40\" y=\"40\"]\n"
              "[lyc2 id=\"low\" width=\"200\" height=\"200\" x=\"40\" y=\"40\"]\n"
              "[lyevent id=\"top\" click=\"click_h\" name=\"g\" key=\"k_top\"]\n"
              "[lyevent id=\"mid\" click=\"click_h\" name=\"g\" key=\"k_mid\"]\n"
              "[lyevent id=\"low\" click=\"click_h\" penetration=\"1\" name=\"g\" key=\"k_low\"]\n"
              "[stop]\n");
    h.warm();
    h.run_lua(kHandlers);
    h.tick(140, 140, true, true); // inside all three
    const std::string log = h.lua_string("oa_log");
    // top only + penetration lower, reversed -> lower then top; mid (no
    // penetration) skipped.
    check(log.find("click:k_low:1;click:k_top:1;") != std::string::npos,
          "RT3 click order bottom->top among {low(pen), top}; mid skipped");
    check(log.find("k_mid") == std::string::npos, "RT3 non-penetrating middle skipped");
}

void test_rt_drag_chain_and_move() {
    Harness h(std::string(kBaseScript) +
              "[lyc2 id=\"knob\" width=\"20\" height=\"20\" x=\"60\" y=\"60\" "
              "draggable=\"1\" dragarea=\"0,0,120,0\"]\n"
              "[lyevent id=\"knob\" dragin=\"drag_h\" drag=\"drag_h\" dragout=\"drag_h\" "
              "name=\"sld\" key=\"pin\"]\n"
              "[stop]\n");
    h.warm();
    h.run_lua(kHandlers);
    // down on the knob: dragin
    h.tick(70, 70, true, true);
    check(h.notes_of(oa::runtime::GameRuntime::PointerDispatch::Kind::DragIn) == 1,
          "RT4 dragin on left-down over the draggable knob");
    check(h.lua_string("oa_log").find("drag:pin:1;") != std::string::npos,
          "RT4 dragin Lua (drag=1, id) called");
    // drag to 110,70 while held: knob follows the pointer delta (60 + 40)
    h.tick(110, 70, true, false);
    check(h.notes_of(oa::runtime::GameRuntime::PointerDispatch::Kind::DragMove) >= 1,
          "RT4 drag dispatch while moving");
    check(h.layer("knob") && approx(h.layer("knob")->left, 100.0),
          "RT4 knob moved with the pointer (60+40 within dragarea)");
    // drag past the dragarea right bound (move to 300): clamped to 120
    h.tick(300, 70, true, false);
    check(h.layer("knob") && approx(h.layer("knob")->left, 120.0),
          "RT4 drag clamped into dragarea");
    // release: dragout
    h.tick(300, 70, false, false);
    check(h.notes_of(oa::runtime::GameRuntime::PointerDispatch::Kind::DragOut) == 1,
          "RT4 dragout on release");
    check(h.lua_string("oa_log").find("drag:pin:0;") != std::string::npos,
          "RT4 dragout Lua (drag=0)");
}

void test_rt_global_push_and_filter_verdicts() {
    // global push registry + event-filter 1/2 semantics (I17-I19).
    Harness h(std::string(kBaseScript) +
              "[setonpush key=\"13\" handler=\"calllua\" function=\"push_h\" adv=\"X\"]\n"
              "[stop]\n");
    h.warm();
    h.run_lua("function push_h(e,p) "
              "oa_log = (oa_log or '') .. 'push:' .. (p.key or '?') .. ':' .. "
              "(p.type or '?') .. ';' end");
    h.tick(80, 80, false, false, {13});
    check(h.lua_string("oa_log").find("push:13:key;") != std::string::npos,
          "RT5 keydown 13 dispatches the setonpush row (key+type params)");
    // filter verdict 2 (pretend failure) suppresses the handler entirely
    Harness h2(std::string(kBaseScript) +
               "[lyevent id=\"top\" click=\"click_h\" name=\"g\" key=\"k1\"]\n"
               "[setonpush key=\"13\" handler=\"calllua\" function=\"push_h\"]\n"
               "[stop]\n");
    h2.warm();
    h2.run_lua("function push_h(e,p) oa_log = (oa_log or '') .. 'push13;' end");
    h2.run_lua("function click_h(e,p) oa_log = (oa_log or '') .. 'click13;' end");
    h2.run_lua("function ef_block_push(e) e:setEventFilter(function(e,nm,p) "
               "if nm == 'setonpush' then return 2 end return 0 end) end");
    h2.call_lua("ef_block_push");
    h2.tick(80, 80, false, false, {13});
    check(h2.lua_string("oa_log").find("push13") == std::string::npos,
          "RT6 filter verdict 2 blocks the push handler");
    // verdict 1 (pretend success): engine does not run the handler either
    h2.run_lua("function ef_claim_click(e) e:setEventFilter(function(e,nm,p) "
               "if nm == 'lyevent' then return 1 end return 0 end) end");
    h2.call_lua("ef_claim_click");
    h2.tick(80, 80, true, true);
    check(h2.lua_string("oa_log").find("click13") == std::string::npos,
          "RT6 filter verdict 1 claims the layer click (no Lua dispatch)");
    check(h2.notes_of(oa::runtime::GameRuntime::PointerDispatch::Kind::Click) == 0,
          "RT6 verdict 1 never dispatches");
}

void test_rt_layer_click_swallows_wait_click() {
    // : clicked = left edge && !handled_by_layer ... .
    // A layer click row that handles the edge must NOT release an input wait.
    Harness h(std::string(kBaseScript) +
              "[lyevent id=\"top\" click=\"click_h\" name=\"g\" key=\"k1\"]\n"
              "[wt time=\"600000\" input=\"1\"]\n"
              "[print data=\"after-wait\"]\n");
    h.warm();
    h.run_lua(kHandlers);
    // warm ticks consumed registration and parked on the [wt]
    for (int i = 0; i < 3; ++i) h.tick(80, 80);
    check(h.rt->current_wait() != nullptr &&
              h.rt->current_wait()->kind == oa::runtime::WaitReason::Kind::Timed,
          "RT7 parked on the clickable wait");
    h.tick(80, 80, true, true); // click handled by the top layer
    check(h.rt->current_wait() != nullptr, "RT7 handled layer click swallows the wait click");
    // a second click row-free setup must still release through the plain edge
    Harness h2(std::string(kBaseScript) + "[wt time=\"600000\" input=\"1\"]\n");
    h2.warm();
    h2.tick(80, 80, true, true);
    check(h2.rt->current_wait() == nullptr,
          "RT7 unhandled left edge releases the clickable wait");
}

} // namespace

int main() {
    test_i1_unset_or_bad_threshold_always_clickable();
    test_i2_alpha_below_threshold_transparent();
    test_i3_sample_coords_include_clip_offset();
    test_i4_fallback_to_layer_alpha_when_sampler_missing();
    test_i13_modes_and_enable_without_row();
    test_i16_fpm_typless_row_expansion();
    test_i10_drag_layer_clamps_into_dragarea();
    test_rt_hover_over_out_and_click();
    test_rt_disabled_click_not_dispatched();
    test_rt_penetration_click_order();
    test_rt_drag_chain_and_move();
    test_rt_global_push_and_filter_verdicts();
    test_rt_layer_click_swallows_wait_click();
    if (failures) {
        std::fprintf(stderr, "input_dispatch_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("input_dispatch_test: all ok\n");
    return 0;
}
