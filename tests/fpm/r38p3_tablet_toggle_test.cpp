// R38 P3 regression: the top touchbar containment toggles (tblt/tbrt) must
// park the bar at the proper edges (msg/tablet.lua tab_left/tab_right).
//
// FPM facts (Windows build, config_tabletui=1): the in-game touchbar
// container is 1.80.mw.tb with the full-width drag overlay tb_mask
// (1.80.mw.zmask) registered over=tab_over/drag=tab_drag; its containment
// buttons are tblt (1.80.mw.tb.17, exec=tab_left, at the bar's LOCAL left
// end) and tbrt (1.80.mw.tb.18, exec=tab_right, local right end).
// tab_reset parks the bar right with left = w - b (b = tblt.p3 = 40) so
// x=1240; tab_left: x>0 -> 0 else -bx ; tab_right: x<0 -> 0 else bx,
// bx = p.w - b where p.w is the MASK width the engine reports through
// get_layer_info. x: 1240 <-> 0 (right-park <-> visible) and 0 <-> -1240
// (visible <-> left-park).
//
// Engine defect (R38 P3): csvbtn3 "obj" layers (the zmask overlay) are
// created as clipped images WITHOUT explicit width/height, so the engine
// reported width=0 in get_layer_info -> Lua p.w=0 -> bx=-40 -> the second
// containment click parked the bar at x=+40 (tab_left x2 = -bx) instead of
// -1240, and every later toggle was stuck (user real-device: containment
// toggle state machine wrong). Fix: get_layer_info width/height report the
// layer's effective size (explicit > clip), matching the reference engine
// and the engine's own hit-test precedence.
//
// This test drives the real chain headless (boot -> title -> story) and
// walks the toggle machine: tblt (1240->0), tblt (0->-1240), tbrt
// (-1240->0), tbrt (0->1240), asserting the bar container (1.80.mw.tb.0
// world left) lands on each expected park.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool is_clickable_wait(const oa::runtime::WaitReason* w) {
    using K = oa::runtime::WaitReason::Kind;
    if (!w) return false;
    return w->kind == K::Generic || w->kind == K::Generic0 ||
           w->kind == K::Timed || w->kind == K::Se || w->kind == K::KeyWait;
}
struct Key {
    std::string key;
    int cx = 0, cy = 0;
};
bool find_key(oa::runtime::GameRuntime& rt, const std::string& key, Key* out) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end() || k->second != key) continue;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (rt.scene().world_rect(l->id, w > 0 ? w : 80, hh > 0 ? hh : 40,
                                  &x0, &y0, &rw, &rh)) {
            out->key = key;
            out->cx = int(x0 + rw / 2);
            out->cy = int(y0 + rh / 2);
            return true;
        }
    }
    return false;
}
/// Bar container world-left (1.80.mw.tb.0 is a clipped 1280x120 child of
/// the tb container at local 0,0). Returns -1e9 when not measurable.
double bar_left(oa::runtime::GameRuntime& rt) {
    const oa::render::Layer* l = rt.scene().find("1.80.mw.tb.0");
    if (!l) return -1e9;
    double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 0);
    double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 0);
    double x0 = 0, y0 = 0, rw = 0, rh = 0;
    if (!rt.scene().world_rect(l->id, w > 0 ? w : 80, hh > 0 ? hh : 40,
                               &x0, &y0, &rw, &rh))
        return -1e9;
    return x0;
}
void click_at(oa::runtime::GameRuntime& rt, int x, int y) {
    oa::runtime::FrameInput cl;
    cl.left_click_edge = true;
    cl.left_down = true;
    cl.mouse_x = x;
    cl.mouse_y = y;
    rt.tick(16, cl);
    oa::runtime::FrameInput idle;
    for (size_t f = 0; f < 6; ++f) rt.tick(16, idle);
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::fprintf(stderr, "OA_TEST_FPM_PFS not set\n");
        return 77;
    }
    try {
                auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        rt.boot_project();
        oa::runtime::FrameInput idle;
        bool title = false;
        rt.interpreter().on_step = [&](const std::string&, size_t,
                                       const oa::runtime::Instruction& i) {
            const std::string* f = i.get("function");
            if (f && *f == "title_init") title = true;
        };
        bool at_title = false;
        for (size_t f = 0; f < 20000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (!title) continue;
            const auto* w = rt.current_wait();
            if (w && w->kind == oa::runtime::WaitReason::Kind::Stop &&
                w->id.empty()) {
                at_title = true;
                break;
            }
        }
        check(at_title, "boot parked at the title");
        for (size_t f = 0; f < 300; ++f) rt.tick(16, idle);
        {
            Key start;
            if (find_key(rt, "bt_start", &start)) {
                oa::runtime::FrameInput cl;
                cl.left_click_edge = true;
                cl.left_down = true;
                cl.mouse_x = start.cx;
                cl.mouse_y = start.cy;
                rt.tick(16, cl);
            } else {
                check(false, "bt_start found");
            }
        }
        bool body = false;
        for (size_t f = 0; f < 20000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (rt.text().page_has_visible_text("1.80.mw.adv_adv") &&
                is_clickable_wait(rt.current_wait())) {
                body = true;
                break;
            }
        }
        check(body, "story body parked");
        for (size_t f = 0; f < 300; ++f) rt.tick(16, idle);
        // The touchbar must exist (config_tabletui=1) with both containments.
        check(rt.scene().find("1.80.mw.tb.0") != nullptr, "touchbar bar exists");
        check(rt.scene().find("1.80.mw.zmask") != nullptr, "tb_mask exists");
        const double x0 = bar_left(rt);
        std::printf("[r38p3] initial bar_left=%.0f (expect ~1240)\n", x0);
        check(x0 > 1180 && x0 < 1300, "bar initially parked right (x~1240)");

        // Walk the toggle machine. Each click: mouse to the containment
        // center, click, then settle past the 300ms tablet_movetime.
        auto press = [&](const char* key, double expect, const char* what) {
            Key k;
            if (!find_key(rt, key, &k)) {
                check(false, (std::string(key) + " containment present").c_str());
                return;
            }
            std::printf("[r38p3] %s: click %s at (%d,%d)\n", what, key, k.cx,
                        k.cy);
            click_at(rt, k.cx, k.cy);
            for (size_t f = 0; f < 80; ++f) rt.tick(16, idle);
            const double x1 = bar_left(rt);
            std::printf("[r38p3]   -> bar_left=%.0f (expect %.0f)\n", x1,
                        expect);
            check(x1 > expect - 6 && x1 < expect + 6, what);
        };
        press("tblt", 0.0, "tblt parks the bar visible (x=0)");
        press("tblt", -1240.0, "tblt again parks the bar LEFT (x=-1240)");
        press("tbrt", 0.0, "tbrt brings the bar back visible (x=0)");
        press("tbrt", 1240.0, "tbrt parks the bar RIGHT (x=1240)");
        std::printf(failures ? "r38p3_tablet_toggle FAIL (%d)\n" : "r38p3_tablet_toggle OK\n",
                    failures);
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
}
