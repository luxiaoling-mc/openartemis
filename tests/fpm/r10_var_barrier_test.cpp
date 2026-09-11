// R10 regression: uihelp centering var must measure the help slot under
// rapid bt->bt hover churn (R10 follow-up to research/33 M10c).
//
// User real-device report: in the save/load/config screens, rapidly moving
// the pointer between buttons probabilistically pushed the bottom dock tip
// (uihelp) far up. Engine attribution: FPM's uihelp_over chain measures the
// just-printed help with a synchronous [var system=get_message_layer_height]
// (msg/ui.lua centering) — the var evaluates at its position in the ordered
// tag stream of the current Lua event. When the pointer hops straight from
// one button to another, the SAME event first runs the previous button's
// btn_out chain, which queues a [flip] between its chgmsg/rp/print segment
// and the new button's print. The M10c barrier pre-executes only the
// message-text segment and stopped at that control tag, so the metric read
// whatever layer was ambient (the story body behind the UI, h=31) instead of
// the help layer (h=14) — the y formula then placed the tip too high.
// (Windowed OA_AUTODRIVE=r10 measured the story body on 199/200 centering
// reads during dwell-1 alternation; fixed run reads the help slot every
// time.)
//
// This test reproduces the queue shape headless and asserts the metric
// hook sees the help layer as active on every measurement while the pointer
// alternates between two dock buttons at one frame per hover.
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
bool is_clickable_wait(const oa::runtime::WaitReason* w) {
    using K = oa::runtime::WaitReason::Kind;
    if (!w) return false;
    return w->kind == K::Generic || w->kind == K::Generic0 ||
           w->kind == K::Timed || w->kind == K::Se || w->kind == K::KeyWait;
}
struct Target {
    std::string key;
    std::string layer;
    int cx = 0, cy = 0;
};
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
        for (size_t f = 0; f < 20000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (!title) continue;
            const auto* w = rt.current_wait();
            if (w && w->kind == oa::runtime::WaitReason::Kind::Stop &&
                w->id.empty())
                break;
        }
        {
            oa::runtime::FrameInput in;
            const int cands[][2] = {{140, 136}, {80, 124}, {200, 150}};
            for (int ci = 0; ci < 3 && rt.scene().find("500"); ++ci) {
                in.left_click_edge = true;
                in.left_down = true;
                in.mouse_x = cands[ci][0];
                in.mouse_y = cands[ci][1];
                for (size_t f = 0; f < 600 && !rt.exit_requested(); ++f) {
                    rt.tick(16, in);
                    in.left_click_edge = false;
                    in.left_down = false;
                    in.mouse_x = -1;
                    in.mouse_y = -1;
                    if (rt.scene().find("500") == nullptr) break;
                }
            }
        }
        bool body = false;
        for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (rt.text().page_has_visible_text("1.80.mw.adv_adv")) {
                body = true;
                break;
            }
        }
        check(body, "story body parked");
        auto park = [&](size_t budget) {
            for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                if (rt.transition().is_in_progress(rt.now_ms())) continue;
                const auto* w = rt.current_wait();
                if (is_clickable_wait(w)) return true;
            }
            return false;
        };
        for (int p = 0; p < 30 && !rt.exit_requested(); ++p) {
            if (!park(2000)) break;
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = 640;
            cl.mouse_y = 600;
            rt.tick(16, cl);
            for (size_t f = 0; f < 2000 && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                const auto* w = rt.current_wait();
                if (w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                          w->kind == oa::runtime::WaitReason::Kind::Generic0))
                    break;
            }
        }
        // canned metric hook: exactly the renderer-hook shape, but the
        // "measurement" is a per-active-layer constant so the test asserts
        // WHICH layer the centering var measured (the actual defect).
        int help_reads = 0, wrong_reads = 0;
        std::string last_active;
        rt.interpreter().hooks().message_layer_metrics =
            [&]() -> std::tuple<double, double, double> {
            const std::string aid = rt.text().active_layer_id();
            last_active = aid;
            if (aid == "500.z.help")
                ++help_reads;
            else
                ++wrong_reads;
            return {0.0, aid == "500.z.help" ? 14.0 : 31.0, 0.0};
        };
        // F10 config screen
        {
            oa::runtime::FrameInput k;
            k.key_down_edges = {121};
            k.keys_down.insert(121);
            for (size_t f = 0; f < 60; ++f) rt.tick(16, f == 0 ? k : idle);
        }
        auto has_key = [&](const std::string& key) {
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                const auto* h = rt.scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto k = h->params.find("key");
                if (k != h->params.end() && k->second == key) return true;
            }
            return false;
        };
        bool open = false;
        for (size_t f = 0; f < 9000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (rt.scene().size() > 150 && has_key("bt_exit")) {
                open = true;
                break;
            }
        }
        check(open, "config screen open");
        if (!open) return failures ? 1 : 0;
        for (size_t f = 0; f < 400; ++f) rt.tick(16, idle);
        // enumerate click targets
        std::vector<Target> targets;
        std::vector<std::string> seen;
        for (const oa::render::Layer* l : rt.scene().draw_order()) {
            const auto* h = rt.scene().find_event_handler(l->id, "click");
            if (!h) continue;
            const auto k = h->params.find("key");
            const std::string key = k == h->params.end() ? std::string()
                                                         : k->second;
            if (key.empty()) continue;
            bool dup = false;
            for (const std::string& s2 : seen)
                if (s2 == key) dup = true;
            if (dup) continue;
            seen.push_back(key);
            double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 0);
            double hh = l->has_clip ? l->clip_h
                                    : (l->height > 0 ? l->height : 0);
            double x0 = 0, y0 = 0, rw = 0, rh = 0;
            if (rt.scene().world_rect(l->id, w > 0 ? w : 80, hh > 0 ? hh : 40,
                                      &x0, &y0, &rw, &rh)) {
                Target t;
                t.key = key;
                t.layer = l->id;
                t.cx = int(x0 + rw / 2);
                t.cy = int(y0 + rh / 2);
                targets.push_back(t);
            }
        }
        std::vector<int> helpful;
        for (size_t i = 0; i < targets.size(); ++i) {
            for (int f = 0; f < 12; ++f) {
                oa::runtime::FrameInput mv;
                mv.mouse_x = targets[i].cx;
                mv.mouse_y = targets[i].cy;
                rt.tick(16, mv);
            }
            const oa::render::MessageLayer* ml = rt.text().layer("500.z.help");
            if (ml && !ml->page.empty()) helpful.push_back((int)i);
        }
        check(helpful.size() >= 2, ">=2 dock/page targets show uihelp");
        if (helpful.size() < 2) return failures ? 1 : 0;
        // rapid alternation between two helpful buttons at 1 frame per hover
        // (user repro: 快速 bt→bt). Before the R10 barrier fix the previous
        // button's btn_out flip stopped the flush and the metric read the
        // ambient story layer on every change.
        wrong_reads = 0;
        help_reads = 0;
        for (int it = 0; it < 400 && !rt.exit_requested(); ++it) {
            const Target& t = targets[helpful[it % helpful.size()]];
            oa::runtime::FrameInput mv;
            mv.mouse_x = t.cx;
            mv.mouse_y = t.cy;
            rt.tick(16, mv);
        }
        // settle: one more hover, then read the resting node top
        std::printf("[r10v] metrics help=%d wrong=%d last_active='%s'\n",
                    help_reads, wrong_reads, last_active.c_str());
        check(wrong_reads == 0, "every centering metric measured the help "
                                "layer (no ambient/story read)");
        check(help_reads > 0, "centering metrics actually ran");
        // resting placement: with the canned help height (14) the y formula
        // (uihelp anchor 34-ish, half-gap) parks the slot node at top=10.
        const oa::render::Layer* bn =
            rt.scene().find(rt.scene().bound_scene_id("500.z.help"));
        std::printf("[r10v] help node top=%.1f\n", bn ? bn->top : -1e9);
        if (bn)
            check(bn->top > 8.0 && bn->top < 12.0,
                  "help node rests at the canonical top (~10)");
        else
            check(false, "help node bound in scene");
        std::printf(failures ? "r10_var_barrier FAIL (%d)\n" : "r10_var_barrier OK\n",
                    failures);
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
}
