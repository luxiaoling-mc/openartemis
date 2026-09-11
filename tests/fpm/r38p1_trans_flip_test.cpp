// R38 P1 regression: an in-flight scene transition must survive [flip]
// ("repaint" request) — the flip must NOT clear/cancel it.
//
// FPM facts: UI entrances (uiopenanime -> uitrans -> [trans type=1
// time=init.ui_fade], extend/user.lua) run a short crossfade and park the
// script (Wait(Trans) -> Stop{reason:"trans"}). FPM repaints constantly —
// every ui_message / estag step queues [flip] tags, and parked queues keep
// draining while a transition runs (drain_parked_queue). The host renderer
// used to treat [flip] as "cancel the transition" (transition_clear +
// destroy the capture), so the first flip queued right behind an entrance
// [trans] truncated the UI crossfade after ~1 frame: the dock save/load/
// config screens appeared without their entrance transition (user real-
// device report, R38 P1).
//
// Engine interpretation (R38 P1): [flip] is a repaint request, not a
// transition-cancel. The transition overlay now lives until it completes on
// its own clock (or a type-0 [trans] / a new [trans] replaces it).
//
// This test drives the real chain headless with the same host event loop the
// app uses (tick -> drain_events -> RenderEngine::process_event): boot ->
// title -> story -> F6 (save UI). When the first [trans] after F6 begins,
// the test injects [flip] events mid-transition (the exact pre-fix killer)
// and asserts the transition stays in progress for >= 10 ticks (~160ms of
// the 250ms entrance), i.e. flips no longer truncate it, and the save UI
// then opens.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "core/fs/physfs_fs.h"
#include "core/render/renderer.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_iet.h"

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
bool has_key(oa::runtime::GameRuntime& rt, const std::string& key) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end() || k->second != key) continue;
        return true;
    }
    return false;
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
        // The same host object the app uses for [trans]/[flip] (process_event
        // on drained events). Constructed after open_project (the ctor hooks
        // the interpreter's message-layer metrics). Never create_renderer():
        // headless hosts have no SDL surface; process_event needs none
        // (capture-less transitions).
        oa::render::RenderEngine host(fs.get(), &rt);
        rt.boot_project();
        oa::runtime::FrameInput idle;
        bool title = false;
        rt.interpreter().on_step = [&](const std::string&, size_t,
                                       const oa::runtime::Instruction& i) {
            const std::string* f = i.get("function");
            if (f && *f == "title_init") title = true;
        };
        // Host event loop mirroring src/app/main.cpp: tick, then drain the
        // script events and apply them to the renderer.
        size_t drained_flips = 0;
        auto pump = [&](oa::runtime::FrameInput in, size_t ticks) {
            for (size_t n = 0; n < ticks; ++n) {
                rt.tick(16, in);
                for (auto& e : rt.drain_events()) {
                    if (e.kind == oa::runtime::Event::Kind::Flip) ++drained_flips;
                    host.process_event(e);
                }
                in = oa::runtime::FrameInput{}; // edges are one-shot
            }
        };
        bool at_title = false;
        for (size_t f = 0; f < 20000 && !rt.exit_requested(); ++f) {
            pump(idle, 1);
            if (!title) continue;
            const auto* w = rt.current_wait();
            if (w && w->kind == oa::runtime::WaitReason::Kind::Stop &&
                w->id.empty()) {
                at_title = true;
                break;
            }
        }
        check(at_title, "boot parked at the title");
        {
            Key start;
            if (find_key(rt, "bt_start", &start)) {
                oa::runtime::FrameInput cl;
                cl.left_click_edge = true;
                cl.left_down = true;
                cl.mouse_x = start.cx;
                cl.mouse_y = start.cy;
                pump(cl, 1);
            } else {
                check(false, "bt_start found");
            }
        }
        bool body = false;
        for (size_t f = 0; f < 20000 && !rt.exit_requested(); ++f) {
            pump(idle, 1);
            if (rt.text().page_has_visible_text("1.80.mw.adv_adv") &&
                is_clickable_wait(rt.current_wait())) {
                body = true;
                break;
            }
        }
        check(body, "story body parked");
        for (size_t f = 0; f < 200; ++f) pump(idle, 1);

        // F6 (key 117) opens the save screen (adv/save flow), which runs the
        // [trans] entrance crossfade (type=1, time=init.ui_fade=250).
        oa::runtime::FrameInput f6;
        f6.key_down_edges.push_back(117);
        f6.keys_down.insert(117);
        pump(f6, 1);
        pump(idle, 2);

        // Latch the first type!=0 [trans] that begins after F6 and watch it.
        int latched_tick = -1; // tick index of the latch (in 16ms ticks)
        int in_progress_ticks = 0;
        bool saw_flip_mid_trans = false;
        const size_t flips_before = drained_flips;
        oa::runtime::Event injected_flip;
        injected_flip.kind = oa::runtime::Event::Kind::Flip;
        for (size_t t = 0; t < 600 && !rt.exit_requested(); ++t) {
            rt.tick(16, idle);
            for (auto& e : rt.drain_events()) {
                if (e.kind == oa::runtime::Event::Kind::Flip) ++drained_flips;
                host.process_event(e);
                if (e.kind == oa::runtime::Event::Kind::Trans && latched_tick < 0) {
                    int type = 1;
                    if (const auto it = e.params.find("type"); it != e.params.end()) {
                        try {
                            type = std::stoi(it->second);
                        } catch (...) {
                            type = 1;
                        }
                    }
                    if (type != 0) {
                        latched_tick = int(t);
                        std::printf("[r38p1] entrance trans latched at tick %zu "
                                    "(time=%s)\n",
                                    t, e.params.count("time") ? e.params.at("time").c_str()
                                                              : "?");
                    }
                }
            }
            if (latched_tick >= 0 && int(t) - latched_tick <= 8) {
                // Deterministic pre-fix killer: [flip] events arriving while
                // the transition runs (a parked queue drains flips every
                // frame in real flows; simulate the same interleaving).
                host.process_event(injected_flip);
                if (int(t) - latched_tick > 0) saw_flip_mid_trans = true;
            }
            if (latched_tick >= 0) {
                if (rt.transition().is_in_progress(rt.now_ms()))
                    ++in_progress_ticks;
                else if (in_progress_ticks > 0 && int(t) - latched_tick > 2)
                    break; // transition finished: stop sampling
            }
            if (latched_tick >= 0 && int(t) - latched_tick > 120) break;
            if (latched_tick < 0 && t > 200) break;
        }
        std::printf("[r38p1] entrance trans in_progress_ticks=%d "
                    "(~%dms of 250ms), injected flips mid-trans=%s\n",
                    in_progress_ticks, in_progress_ticks * 16,
                    saw_flip_mid_trans ? "yes" : "no");
        check(latched_tick >= 0, "save-entrance [trans] observed after F6");
        check(saw_flip_mid_trans, "injected [flip] events ran mid-transition");
        check(in_progress_ticks >= 10,
              "entrance transition survived interleaved [flip]s for >= 10 "
              "ticks (~160ms) — flips must not truncate it");
        check(in_progress_ticks <= 60,
              "transition did not overrun (released near its 250ms clock)");
        // Integration: the save UI actually opened afterwards.
        bool save_open = false;
        for (size_t f = 0; f < 3000 && !rt.exit_requested(); ++f) {
            pump(idle, 1);
            if (has_key(rt, "bt_save01") || has_key(rt, "bt_close")) {
                save_open = true;
                break;
            }
        }
        check(save_open, "save UI opened after the entrance transition");
        (void)flips_before;
        std::printf(failures ? "r38p1_trans_flip FAIL (%d)\n" : "r38p1_trans_flip OK\n",
                    failures);
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
}
