// R10c/R10d regression: [reset] = clear domains + VM KEPT + re-run the boot
// script head (user ruling, 方案一; handle_engine_reset shape). The engine
// hardcodes NO label — the FPM boot chain (first.iet *top -> init.lua
// system_starting decision) consumes the surviving `systemreset` flag and
// routes to first.iet *title. Multi-round stability (no reset storm), and
// the render/font metric hook survives so post-title-return uihelp centering
// measures the help layer (tip at top=10).
//
// R1 boot -> title
// R2 start (はじめから) -> story (round 1)
// R3 config (F10) -> bt_title -> confirm -> [reset] -> title (round 1 return)
// R3c canned metric hook (installed pre-boot) still set after [reset]
// R4 start -> story (round 2)          [the historical loop point]
// R5 return to title (round 2 return)
// R6 config opened FROM the title -> hover dock -> centering var measured
//    the help slot (hook alive) and the tip node sits at top ~10
// R7 start -> story (round 3)
// exactly two engine resets, no storm
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <tuple>

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
bool at_title(oa::runtime::GameRuntime& rt) {
    const auto* w = rt.current_wait();
    if (!(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty()))
        return false;
    Key k;
    return find_key(rt, "bt_start", &k);
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
        // Canned per-layer metric hook (help slots h=14, else h=31).
        int help_reads = 0;
        rt.interpreter().hooks().message_layer_metrics =
            [&]() -> std::tuple<double, double, double> {
            const std::string aid = rt.text().active_layer_id();
            if (aid == "500.z.help" || aid == "500.help") ++help_reads;
            const double h = (aid == "500.z.help" || aid == "500.help") ? 14.0
                                                                        : 31.0;
            return {0.0, h, 0.0};
        };
        rt.boot_project();
        oa::runtime::FrameInput idle;
        auto tick = [&](oa::runtime::FrameInput& in) { rt.tick(16, in); };
        bool title_seen = false;
        rt.interpreter().on_step = [&](const std::string&, size_t,
                                       const oa::runtime::Instruction& i) {
            const std::string* f = i.get("function");
            if (f && *f == "title_init") title_seen = true;
        };
        bool title = false;
        for (size_t f = 0; f < 20000 && !rt.exit_requested(); ++f) {
            tick(idle);
            if (title_seen && at_title(rt)) {
                title = true;
                break;
            }
        }
        check(title, "R1 boot parked at the title");
        int resets = 0;
        auto click_key = [&](const std::string& key) {
            Key k;
            if (!find_key(rt, key, &k)) return false;
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = k.cx;
            cl.mouse_y = k.cy;
            tick(cl);
            return true;
        };
        auto start_and_walk = [&](const char* tag) {
            for (size_t f = 0; f < 300; ++f) tick(idle);
            if (!click_key("bt_start")) return false;
            bool body = false;
            for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
                tick(idle);
                if (rt.text().page_has_visible_text("1.80.mw.adv_adv") &&
                    is_clickable_wait(rt.current_wait())) {
                    body = true;
                    break;
                }
            }
            std::printf("[r10d] %s story body=%d\n", tag, body ? 1 : 0);
            if (!body) return false;
            int turns = 0;
            for (size_t f = 0; f < 20000 && turns < 2 && !rt.exit_requested();
                 ++f) {
                if (!is_clickable_wait(rt.current_wait())) {
                    tick(idle);
                    continue;
                }
                oa::runtime::FrameInput c2;
                c2.left_click_edge = true;
                c2.left_down = true;
                c2.mouse_x = 640;
                c2.mouse_y = 600;
                tick(c2);
                ++turns;
                for (size_t g = 0; g < 400; ++g) tick(idle);
            }
            return turns == 2;
        };
        auto return_to_title = [&](const char* tag) {
            for (size_t f = 0; f < 300; ++f) tick(idle);
            oa::runtime::FrameInput k;
            k.key_down_edges = {121}; // F10 config
            k.keys_down.insert(121);
            tick(k);
            Key ttl;
            bool cfg = false;
            for (size_t f = 0; f < 9000 && !rt.exit_requested(); ++f) {
                tick(idle);
                // in-game config: scene grows well past the story/dock set
                if (rt.scene().size() > 160 && find_key(rt, "bt_title", &ttl)) {
                    cfg = true;
                    break;
                }
            }
            if (!cfg) {
                std::printf("[r10d] %s config never opened (layers=%zu)\n",
                            tag, rt.scene().size());
                return false;
            }
            for (size_t f = 0; f < 240; ++f) tick(idle);
            if (!click_key("bt_title")) return false;
            bool back = false;
            long dialog_seen_at = -1;
            for (size_t f = 0; f < 20000 && !rt.exit_requested(); ++f) {
                if (rt.scene().find("600.1.bt.1.0") != nullptr) {
                    if (dialog_seen_at < 0) {
                        dialog_seen_at = (long)f;
                    } else if (f - (size_t)dialog_seen_at > 240) {
                        oa::runtime::FrameInput en;
                        en.key_down_edges = {13};
                        en.keys_down.insert(13);
                        tick(en);
                        dialog_seen_at = -2;
                    }
                }
                if (dialog_seen_at != -2) tick(idle);
                if (at_title(rt)) {
                    back = true;
                    break;
                }
            }
            std::printf("[r10d] %s back at title=%d\n", tag, back ? 1 : 0);
            return back;
        };
        if (!title) return failures ? 1 : 0;
        check(start_and_walk("R2"), "R2 start->story round 1");
        check(return_to_title("R3"), "R3 return to title #1");
        ++resets;
        check(rt.interpreter().hooks().message_layer_metrics != nullptr,
              "R3c metric hook survives the [reset] (VM kept)");
        check(start_and_walk("R4"), "R4 start->story round 2");
        check(return_to_title("R5"), "R5 return to title #2");
        ++resets;
        // R6: config FROM the title -> hover dock -> tip centering
        for (size_t f = 0; f < 900; ++f) tick(idle); // full title settle
        Key cfg;
        check(find_key(rt, "bt_config", &cfg), "R6 title has bt_config");
        if (find_key(rt, "bt_config", &cfg)) {
            // hover first (title_click acts on btn.cursor), then click
            for (size_t f = 0; f < 30; ++f) {
                oa::runtime::FrameInput mv;
                mv.mouse_x = cfg.cx;
                mv.mouse_y = cfg.cy;
                tick(mv);
            }
            std::printf("[r10d] R6 bt_config @(%d,%d) layers=%zu\n", cfg.cx,
                        cfg.cy, rt.scene().size());
            {
                const auto& order = rt.scene().draw_order();
                std::string ids;
                for (size_t i = 0; i < order.size() && i < 10; ++i) {
                    if (!ids.empty()) ids += ",";
                    ids += order[i]->id;
                }
                std::printf("[r10d] R6 layers head: %s\n", ids.c_str());
            }
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = cfg.cx;
            cl.mouse_y = cfg.cy;
            tick(cl);
            bool open = false;
            for (size_t f = 0; f < 9000 && !rt.exit_requested(); ++f) {
                tick(idle);
                Key v;
                if (rt.scene().size() > 60 && find_key(rt, "bt1011", &v)) {
                    open = true;
                    break;
                }
            }
            std::printf("[r10d] R6 config open=%d layers=%zu\n", open ? 1 : 0,
                        rt.scene().size());
            {
                const auto& order = rt.scene().draw_order();
                std::string ids;
                for (size_t i = 0; i < order.size() && i < 12; ++i) {
                    if (!ids.empty()) ids += ",";
                    ids += order[i]->id;
                }
                std::string tailids;
                for (size_t i = order.size() > 6 ? order.size() - 6 : 0;
                     i < order.size(); ++i) {
                    if (!tailids.empty()) tailids += ",";
                    tailids += order[i]->id;
                }
                std::printf("[r10d] R6 post head: %s | tail: %s\n",
                            ids.c_str(), tailids.c_str());
                std::string keys;
                int kn = 0;
                std::vector<std::string> kseen;
                for (const oa::render::Layer* l : order) {
                    const auto* h = rt.scene().find_event_handler(l->id, "click");
                    if (!h) continue;
                    const auto kk = h->params.find("key");
                    if (kk == h->params.end()) continue;
                    bool dup = false;
                    for (const auto& s2 : kseen)
                        if (s2 == kk->second) dup = true;
                    if (dup) continue;
                    kseen.push_back(kk->second);
                    if (++kn <= 60) {
                        if (!keys.empty()) keys += ",";
                        keys += kk->second;
                    }
                }
                std::printf("[r10d] R6 post keys(%d): %s\n", kn,
                            keys.c_str());
            }
            check(open, "R6a config opened from the title");
            if (open) {
                for (size_t f = 0; f < 300; ++f) tick(idle);
                Key voice;
                const int before = help_reads;
                if (find_key(rt, "bt1011", &voice)) {
                    for (size_t f = 0; f < 90; ++f) {
                        oa::runtime::FrameInput mv;
                        mv.mouse_x = voice.cx;
                        mv.mouse_y = voice.cy;
                        tick(mv);
                    }
                    const oa::render::Layer* bn = rt.scene().find(
                        rt.scene().bound_scene_id("500.z.help"));
                    std::printf("[r10d] R6 hover help_reads_delta=%d node "
                                "top=%.1f\n",
                                help_reads - before, bn ? bn->top : -1e9);
                    check(help_reads - before > 0,
                          "R6b centering var measured the help slot");
                    if (bn)
                        check(bn->top > 8.0 && bn->top < 12.0,
                              "R6c tip node at the canonical top (~10)");
                    else
                        check(false, "R6c help node bound in scene");
                } else {
                    check(false, "R6d config dock has bt_voice");
                }
            }
        }
        // close the config back to the title (FPM UI screens exit on the
        // right button / rclick chain), then round 3
        for (size_t f = 0; f < 300; ++f) tick(idle);
        if (!at_title(rt)) {
            oa::runtime::FrameInput rc;
            rc.key_down_edges = {2}; // vk 2 = right button
            rc.keys_down.insert(2);
            tick(rc);
            for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
                tick(idle);
                if (at_title(rt)) break;
            }
            std::printf("[r10d] R6 close back at title=%d\n",
                        at_title(rt) ? 1 : 0);
        }
        check(start_and_walk("R7"), "R7 start->story round 3");
        std::printf("[r10d] engine resets=%d (expect 2)\n", resets);
        check(resets == 2, "exactly two engine resets (no storm)");
        std::printf(failures ? "r10d_reset_title FAIL (%d)\n"
                             : "r10d_reset_title OK\n",
                    failures);
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
}
