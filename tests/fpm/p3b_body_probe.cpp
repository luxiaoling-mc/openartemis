// P3b acceptance: drive the real fpm/root.pfs headless past the title start
// click (queue-item-6 boot/settle/click harness) and assert that the story
// message layers accumulate NON-BLANK body text ( glyph pipeline;
// research/15). Env: OA_TEST_FPM_PFS (skip code 77 when unset).
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
    std::string target;
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
        size_t boot = 0;
        for (; boot < 2400 && !rt.exit_requested(); ++boot) {
            rt.tick(16, idle);
            if (!saw_title_init) continue;
            const auto* w = rt.current_wait();
            if (!(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty()))
                continue;
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
        std::printf("[p3b] boot=%zu target=%s wait=%s\n", boot, target.c_str(),
                    wait_str(rt.current_wait()).c_str());

        if (saw_title_init && !target.empty()) {
            const oa::render::Layer* pl = rt.scene().find(target);
            double w = pl && pl->has_clip ? pl->clip_w : 0;
            double h = pl && pl->has_clip ? pl->clip_h : 0;
            if ((w <= 0 || h <= 0) && pl) {
                w = pl->width > 0 ? pl->width : w;
                h = pl->height > 0 ? pl->height : h;
            }
            if (w <= 0 || h <= 0) {
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
            // settle: parked [stop], rect on-screen stationary
            double last_x = -1e9;
            size_t quiet = 0;
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
                    quiet = 0;
                    continue;
                }
                quiet = std::fabs(x0 - last_x) < 0.5 ? quiet + 1 : 0;
                last_x = x0;
                if (quiet >= 150) break;
            }
            int cx = 0, cy = 0;
            (void)rect_center(&cx, &cy);
            std::printf("[p3b] settle=%zu center=(%d,%d) wait=%s\n", stl, cx, cy,
                        wait_str(rt.current_wait()).c_str());
            // click once (edge+down a few frames)
            size_t clk = 0;
            bool first = true;
            bool title_gone = false;
            for (; clk < 800 && !rt.exit_requested(); ++clk) {
                oa::runtime::FrameInput in;
                if (rect_center(&in.mouse_x, &in.mouse_y)) { /*track*/ }
                if (first) {
                    in.left_click_edge = true;
                    in.left_down = true;
                    first = false;
                }
                rt.tick(16, in);
                if (rt.scene().find("500") == nullptr) title_gone = true;
                if (title_gone) break;
            }
            std::printf("[p3b] click=%zu title_gone=%s wait=%s\n", clk,
                        title_gone ? "yes" : "no", wait_str(rt.current_wait()).c_str());
            // watch for body text (bounded: 4000 ticks ≈ well past the first
            // story page)
            bool reached = false;
            std::string reached_layer;
            std::string sample;
            for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                if (reached) continue;
                for (const std::string& id : rt.text().visible_content_layers()) {
                    if (!rt.text().page_has_visible_text(id)) continue;
                    reached = true;
                    reached_layer = id;
                    const oa::render::MessageLayer* ml = rt.text().layer(id);
                    for (const auto& u : ml->page) {
                        if (!u.data.empty()) {
                            sample = u.data.substr(0, 40);
                            break;
                        }
                    }
                    std::printf("[p3b] BODY-REACHED f=%zu layer=%s sample='%s' "
                                "wait=%s layers=%zu\n",
                                f, id.c_str(), sample.c_str(),
                                wait_str(rt.current_wait()).c_str(),
                                rt.scene().size());
                    break;
                }
            }
            std::printf("[p3b] verdict body_reached=%s\n", reached ? "yes" : "no");
            check(reached, "story body text (non-blank) reached after title start");
            if (reached) {
                check(!sample.empty(), "body layer carries visible characters");
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "p3b_body_probe: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("p3b_body_probe: all ok\n");
    return 0;
}
