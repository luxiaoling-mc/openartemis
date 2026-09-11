// R38 P2 regression: the unpinned story dock must not retract when the
// pointer moves onto the dock buttons (user real-device: unpinned dock
// "retracts instead of popping out" on hover).
//
// FPM facts (ui/config mwdock in extend/adv_mw.lua): with conf.dock==0 the
// invisible dockarea strip (1.80.mw.-1, y670..720) carries over=mwarea_over
// / out=mwarea_out. Hovering the strip opens the floating dock
// (mwarea_open: dock buttons slide up into the strip zone, scr.mwlock=true);
// mwarea_out closes it again. The dock buttons occupy the same y band, so
// with occlusion-based hover (topmost only) the moment the buttons slide
// under the pointer the area loses hover -> rollout -> mwarea_out -> close
// -> buttons vanish -> area topmost again -> over -> open ... oscillating
// every frame while the pointer sits still (the retract defect).
//
// Engine interpretation (R38): hover membership is RECT-BASED — a hovered
// layer keeps its over state while the pointer stays inside its rect and
// the layer remains visible; rollout fires only on rect leave or hide. New
// overs still dispatch topmost-first. The dock then opens once and stays
// open while the pointer is in the strip zone.
//
// This test drives the real chain headless: boot -> story -> click bt_lock
// (unpin) -> hover the strip at x=200 (open the dock) -> move to x=1050
// (over the bt_load button zone) and hold still — asserts the dockarea is
// NOT rolled out by the button occluding it (no open/close oscillation) and
// the dock stays open.
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
            }
        }
        bool body = false;
        for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (rt.text().page_has_visible_text("1.80.mw.adv_adv") &&
                is_clickable_wait(rt.current_wait())) {
                body = true;
                break;
            }
        }
        check(body, "story body parked");
        for (size_t f = 0; f < 300; ++f) rt.tick(16, idle);
        // pointer observer: count dockarea over/out + button overs
        int area_over = 0, area_out = 0, btn_over_cnt = 0, btn_out_cnt = 0;
        using PD = oa::runtime::GameRuntime::PointerDispatch;
        rt.set_pointer_observer([&](const PD& d) {
            const bool is_area = d.layer == "1.80.mw.-1";
            const bool is_btn =
                d.layer.rfind("1.80.mw.bt.dc.", 0) == 0 &&
                d.layer.find("help") == std::string::npos;
            switch (d.kind) {
                case PD::Kind::HoverIn:
                    if (is_area) ++area_over;
                    if (is_btn) ++btn_over_cnt;
                    break;
                case PD::Kind::HoverOut:
                    if (is_area) ++area_out;
                    if (is_btn) ++btn_out_cnt;
                    break;
                default:
                    break;
            }
        });
        // unpin: click bt_lock
        Key lk;
        check(find_key(rt, "bt_lock", &lk), "dock bt_lock present");
        if (find_key(rt, "bt_lock", &lk)) {
            for (size_t f = 0; f < 10; ++f) {
                oa::runtime::FrameInput mv;
                mv.mouse_x = lk.cx;
                mv.mouse_y = lk.cy;
                rt.tick(16, mv);
            }
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = lk.cx;
            cl.mouse_y = lk.cy;
            rt.tick(16, cl);
        }
        for (size_t f = 0; f < 60; ++f) rt.tick(16, idle);
        // hover the strip at x=200 (empty zone) -> dock opens
        for (size_t f = 0; f < 30; ++f) {
            oa::runtime::FrameInput mv;
            mv.mouse_x = 200;
            mv.mouse_y = 695;
            rt.tick(16, mv);
        }
        std::printf("[r38p2] after strip: area_over=%d area_out=%d\n",
                    area_over, area_out);
        check(area_over >= 1, "dockarea over fired on the strip (dock opened)");
        // hold still over the bt_load button zone (x=1050); pre-fix this
        // oscillates open/close (area_out climbs, dock visibility flaps)
        const int area_out_before = area_out;
        for (size_t f = 0; f < 120; ++f) {
            oa::runtime::FrameInput mv;
            mv.mouse_x = 1050;
            mv.mouse_y = 695;
            rt.tick(16, mv);
        }
        std::printf("[r38p2] at button zone: area_out_delta=%d btn_over=%d "
                    "btn_out=%d\n",
                    area_out - area_out_before, btn_over_cnt, btn_out_cnt);
        check(area_out - area_out_before == 0,
              "dockarea NOT rolled out while occluded by a dock button "
              "(no open/close oscillation)");
        check(btn_over_cnt >= 1, "the occluding dock button received hover");
        check(btn_out_cnt == 0 || btn_out_cnt < 2,
              "no repeated button out/in flap on a still pointer");
        // dock must still be open: the dc container visible
        const oa::render::Layer* dc = rt.scene().find("1.80.mw.bt.dc");
        if (dc)
            check(dc->visible != 0.0, "dock stays open (dc visible) at the end");
        else
            check(false, "dc container layer exists");
        std::printf(failures ? "r38p2_dock_hover FAIL (%d)\n" : "r38p2_dock_hover OK\n",
                    failures);
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
}
