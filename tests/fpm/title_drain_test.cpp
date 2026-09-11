// ALIGNMENT queue item 6 acceptance: real fpm/root.pfs title interactions
// advance while the title [stop] park holds.  drains the interpreter
// tag queue in every wait state every frame ; openartemis
// now does the same under Stop parks (research/14 §6 fix). This test drives
// the full GameRuntime headless:
//   (a) boot to the parked title (bt_start handler rows registered);
//   (b) park the virtual pointer on the bt_start world center -> rollover
//       dispatches btn_over and its queued [lyprop clip=clip_a] must LAND on
//       the scene layer (hover highlight becomes visible — previously the
//       queue sat parked and nothing changed);
//   (c) a left-click on bt_start runs the click chain (btn_clickex -> push
//       key1 -> keyconfig -> title_click -> title_start) whose queued estag/
//       jump tags drain under the [stop] park: the title group ("500") must
//       be deleted and the runtime must leave the title stop into the game
//       start flow.
// Later progress past the start screen depends on the M8 save/conf/menu
// domains (recorded dependency, not an item-6 assertion).
// Env-gated like the other real-fpm tests (OA_TEST_FPM_PFS, skip code 77).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

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
const char* kind_str(oa::runtime::WaitReason::Kind k) {
    using K = oa::runtime::WaitReason::Kind;
    switch (k) {
        case K::Generic: return "generic";
        case K::Generic0: return "wt0";
        case K::Timed: return "timed";
        case K::Stop: return "stop";
        case K::Se: return "se";
        case K::VideoLayer: return "video";
        case K::ScenarioTween: return "scenario-tween";
        case K::KeyWait: return "key";
    }
    return "?";
}
std::string wait_str(const oa::runtime::WaitReason* w) {
    if (!w) return "none";
    std::string s = kind_str(w->kind);
    if (!w->id.empty()) s += "('" + w->id + "')";
    return s;
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_FPM_PFS unset; skipping\n");
        return 77;
    }
    bool saw_title_init = false;
    std::string target; // bt_start layer id
    std::string last_wait_log;
    bool title_gone = false;
    std::string exit_note;
    try {
                auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        rt.boot_project();
        rt.interpreter().on_step = [&](const std::string&, size_t,
                                       const oa::runtime::Instruction& i) {
            const std::string* f = i.get("function");
            if (f && *f == "title_init") saw_title_init = true;
        };

        oa::runtime::FrameInput idle;
        // ---- (a) boot to the parked title ---------------------------------
        size_t boot = 0;
        for (; boot < 2400 && !rt.exit_requested(); ++boot) {
            rt.tick(16, idle);
            if (!saw_title_init) continue;
            const auto* w = rt.current_wait();
            if (!(w && w->kind == oa::runtime::WaitReason::Kind::Stop &&
                  w->id.empty()))
                continue;
            // parked at the title [stop]: locate the bt_start click row
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                const auto* h = rt.scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto k = h->params.find("key");
                if (k == h->params.end() || k->second != "bt_start") continue;
                const auto n = h->params.find("name");
                if (n != h->params.end() && n->second == "ttl1") {
                    target = l->id;
                    break;
                }
            }
            if (target.empty()) {
                for (const oa::render::Layer* l : rt.scene().draw_order()) {
                    const auto* h = rt.scene().find_event_handler(l->id, "click");
                    if (!h) continue;
                    const auto k = h->params.find("key");
                    if (k == h->params.end() || k->second != "bt_start") continue;
                    target = l->id;
                    break;
                }
            }
            if (!target.empty()) break;
        }
        std::printf("[td] boot frames=%zu title_init=%s target=%s wait=%s\n", boot,
                    saw_title_init ? "yes" : "no", target.c_str(),
                    wait_str(rt.current_wait()).c_str());
        check(saw_title_init, "boot reached title_init");
        check(!target.empty(), "bt_start click row registered at title");
        check(boot < 2400 && !rt.exit_requested(), "boot bounded, no exit");

        if (saw_title_init && !target.empty()) {
            const oa::render::Layer* pl = rt.scene().find(target);
            double w = pl && pl->has_clip ? pl->clip_w : 0;
            double h = pl && pl->has_clip ? pl->clip_h : 0;
            if ((w <= 0 || h <= 0) && pl) { // width/height sized solids
                w = pl->width > 0 ? pl->width : w;
                h = pl->height > 0 ? pl->height : h;
            }
            if (w <= 0 || h <= 0) { // fallback: known title button quad
                w = 160;
                h = 48;
            }
            auto rect_center = [&](int* cx, int* cy) -> bool {
                double x0 = 0, y0 = 0, rw = 0, rh = 0;
                if (!rt.scene().world_rect(target, w, h, &x0, &y0, &rw, &rh)) return false;
                *cx = int(x0 + rw / 2);
                *cy = int(y0 + rh / 2);
                return true;
            };
            // Debug dispatch observer (mirrors the app smoke wording).
            using PD = oa::runtime::GameRuntime::PointerDispatch;
            rt.set_pointer_observer([&](const PD& d) {
                if (d.kind == PD::Kind::HoverIn)
                    std::printf("[td] hover-in %s over=%s key=%s\n", d.layer.c_str(),
                                d.function.c_str(), d.key.c_str());
                else if (d.kind == PD::Kind::HoverOut)
                    std::printf("[td] hover-out %s out=%s\n", d.layer.c_str(),
                                d.function.c_str());
                else if (d.kind == PD::Kind::Click)
                    std::printf("[td] click layer %s key=%s\n", d.layer.c_str(),
                                d.key.c_str());
            });

            // ---- (a2) settle: wait for the title entrance to park -----------
            // The title buttons exist right after title_init but the 500.b
            // group slides in over seconds (button world x < 0 off-screen, and
            // FPM's get_gamemode ignores out-of-screen button events). Wait
            // for the rect to be on-screen and stationary, with no transition
            // in flight, while the [stop] park holds.
            double last_x = -1e9;
            size_t settle_quiet = 0;
            size_t stl = 0;
            for (; stl < 4000 && !rt.exit_requested(); ++stl) {
                rt.tick(16, idle);
                const auto* wq = rt.current_wait();
                if (!(wq && wq->kind == oa::runtime::WaitReason::Kind::Stop &&
                      wq->id.empty()))
                    continue;
                if (rt.transition().is_in_progress(rt.now_ms())) continue;
                double x0 = 0, y0 = 0, rw = 0, rh = 0;
                if (!rt.scene().world_rect(target, w, h, &x0, &y0, &rw, &rh)) continue;
                if (x0 < 0) {
                    last_x = x0;
                    settle_quiet = 0;
                    continue;
                }
                if (std::fabs(x0 - last_x) < 0.5)
                    ++settle_quiet;
                else
                    settle_quiet = 0;
                last_x = x0;
                if (settle_quiet >= 150) break; // stationary on-screen
            }
            int cx = 0, cy = 0;
            const bool have_rect = rect_center(&cx, &cy);
            check(have_rect, "world rect resolvable for bt_start");
            std::printf("[td] settle frames=%zu target=%s center=(%d,%d) wait=%s\n",
                        stl, target.c_str(), cx, cy, wait_str(rt.current_wait()).c_str());
            check(stl < 4000 && settle_quiet >= 150,
                  "title parked on-screen and stationary (entrance finished)");

            // ---- (b) hover: rollover + queued lyprop must land -------------
            const double clip_before =
                pl && pl->has_clip ? pl->clip_x : -1.0;
            bool hovered = false;
            double clip_after = clip_before;
            size_t hov = 0;
            size_t hover_hold = 0;
            for (; hov < 400 && !rt.exit_requested(); ++hov) {
                oa::runtime::FrameInput hover;
                if (rect_center(&hover.mouse_x, &hover.mouse_y)) { /* track */ }
                rt.tick(16, hover);
                if (rt.is_hovered(target)) hovered = true;
                if (const oa::render::Layer* l2 = rt.scene().find(target)) {
                    if (l2->has_clip) clip_after = l2->clip_x;
                }
                if (hovered && std::fabs(clip_after - 160.0) < 0.5) {
                    if (++hover_hold >= 20) break;
                } else {
                    hover_hold = 0;
                }
            }
            std::printf("[td] hover frames=%zu hovered=%s clip %.0f -> %.0f "
                        "(clip_a.x=160) wait=%s\n",
                        hov, hovered ? "yes" : "no", clip_before, clip_after,
                        wait_str(rt.current_wait()).c_str());
            check(hovered, "bt_start hovered by the runtime (rollover hit)");
            check(std::fabs(clip_after - 160.0) < 0.5,
                  "btn_over [lyprop clip=clip_a] landed under the title [stop] "
                  "(queued-tag drain)");

            // ---- (c) click: the title must leave the [stop] into start -----
            bool edge_pending = true;
            size_t clk = 0;
            for (; clk < 3000 && !rt.exit_requested(); ++clk) {
                oa::runtime::FrameInput in;
                if (rect_center(&in.mouse_x, &in.mouse_y)) { /* track */ }
                if (edge_pending) {
                    in.left_click_edge = true;
                    in.left_down = true;
                    edge_pending = false;
                }
                try {
                    rt.tick(16, in);
                } catch (const std::exception& e) {
                    exit_note = std::string("tick error: ") + e.what();
                    break;
                }
                if (rt.scene().find("500") == nullptr) title_gone = true;
                const auto* w = rt.current_wait();
                const std::string ws = wait_str(w);
                if (last_wait_log != ws) {
                    last_wait_log = ws;
                    std::printf("[td] click f=%zu wait=%s pos=%s:%zu\n", clk, ws.c_str(),
                                rt.interpreter().current_script()
                                    ? rt.interpreter().current_script()->c_str()
                                    : "(none)",
                                rt.interpreter().current_line());
                }
                if (title_gone) break;
            }
            std::printf("[td] click frames=%zu title_gone=%s wait=%s layers=%zu%s%s\n",
                        clk, title_gone ? "yes" : "no", wait_str(rt.current_wait()).c_str(),
                        rt.scene().size(),
                        exit_note.empty() ? "" : " note=", exit_note.c_str());
            check(title_gone,
                  "click on bt_start deleted the title group (queued estag/"
                  "jump chain drained under the [stop] park)");
            check(!rt.exit_requested(), "no engine exit requested");
            if (!exit_note.empty()) {
                // Post-title progress ends at the documented M8 save/conf/menu
                // domain — recorded dependency, not an item-6 failure.
                std::printf("[td] post-title stop: %s (remaining dependency: M8 "
                            "save/conf/menu domains)\n",
                            exit_note.c_str());
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "title_drain_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("title_drain_test: all ok\n");
    return 0;
}
