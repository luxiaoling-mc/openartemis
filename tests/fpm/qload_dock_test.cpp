// Phenomenon C regression (research/37): quickload (F5 adv QLOAD) then the
// story-dock save/load/config buttons died — clicks and the F6 SAVE key both
// unresponsive after quickload while a normal F7 slot load stayed healthy.
//
// Root cause (engine-side): the engine [load] restore wiped the global
// setonpush input registry (Compositor::clear_scene) and only replayed
// per-layer handler rows from the save record. FPM's Lua load tail re-runs
// the key rows only when a UI page was open at [load] time (the normal load
// page close swaps the whole key set); the quickload path (dialog only, no
// page) never re-registers — so every push row (dock clicks ride the
// left-push fallback and adv keys like F6) stayed gone.
//
// Fix: the global input registry now travels with the scene snapshot (same
// rule as layer handler rows) and is restored on [load] — the engine hands
// back the rows recorded at the save moment.
//
// Test (real fpm, OA_TEST_FPM_PFS gate): story -> quick save (F4) -> advance
// -> quick load (F5, dialog confirm) -> assert the push rows are back and
// the dock save/load/config buttons open their screens; then a fresh
// runtime does the normal F6-save/F7-load round trip and asserts the same
// dock behaviour (control arm).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_save.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
const char* ws(const oa::runtime::WaitReason* w) {
    using K = oa::runtime::WaitReason::Kind;
    if (!w) return "none";
    switch (w->kind) {
        case K::Generic: return "generic";
        case K::Generic0: return "wt0";
        case K::Timed: return "timed";
        case K::Stop: return "stop";
        case K::Se: return "se";
        case K::VideoLayer: return "video";
        case K::ScenarioTween: return "stween";
        case K::KeyWait: return "key";
    }
    return "?";
}
bool is_story_wait(const oa::runtime::WaitReason* w) {
    return w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                 w->kind == oa::runtime::WaitReason::Kind::Generic0);
}
std::string sample(oa::runtime::GameRuntime& rt) {
    const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
    if (!ml) return "-";
    for (const auto& u : ml->page)
        if (!u.data.empty()) return u.data.substr(0, 16);
    return "-";
}
bool has_push_row(oa::runtime::GameRuntime& rt, int key) {
    return rt.scene().get_input_handler("push", std::to_string(key)) != nullptr;
}
bool reach_title(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        if (!(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty())) continue;
        for (const oa::render::Layer* l : rt.scene().draw_order()) {
            const auto* h = rt.scene().find_event_handler(l->id, "click");
            if (!h) continue;
            const auto k = h->params.find("key");
            if (k != h->params.end() && k->second == "bt_start") return true;
        }
    }
    return false;
}
void start_game(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    oa::runtime::FrameInput in;
    const int cands[][2] = {{140, 136}, {80, 124}, {200, 150}};
    for (int ci = 0; ci < 3 && rt.scene().find("500"); ++ci) {
        in.left_click_edge = true;
        in.left_down = true;
        in.mouse_x = cands[ci][0];
        in.mouse_y = cands[ci][1];
        for (size_t f = 0; f < 500 && !rt.exit_requested(); ++f) {
            rt.tick(16, in);
            in.left_click_edge = false;
            in.left_down = false;
            in.mouse_x = -1;
            in.mouse_y = -1;
            if (rt.scene().find("500") == nullptr) break;
        }
    }
}
bool park_story(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                size_t budget = 3000) {
    for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
        if (is_story_wait(w) && ml && ml->reveal_index >= ml->char_count && !ml->reveal_pending)
            return true;
    }
    return false;
}
bool reach_story_page(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.text().page_has_visible_text("1.80.mw.adv_adv")) break;
    }
    for (int t = 0; t < 6 && !rt.exit_requested(); ++t) {
        if (!park_story(rt, idle, 2500)) break;
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = 640;
        cl.mouse_y = 600;
        rt.tick(16, cl);
        (void)park_story(rt, idle, 2500);
    }
    return park_story(rt, idle, 2500);
}
void press_tick(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle, int key) {
    oa::runtime::FrameInput k;
    k.key_down_edges = {key};
    k.keys_down.insert(key);
    rt.tick(16, k);
    for (size_t f = 1; f < 40 && !rt.exit_requested(); ++f) rt.tick(16, idle);
}
/// Yes/no dialog (node 600.1.bt.1.0 = YES) -> Enter. Returns true when the
/// dialog appeared.
bool confirm_dialog(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    size_t held = 0;
    bool seen = false;
    for (size_t f = 0; f < 6000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.scene().find("600.1.bt.1.0") != nullptr) {
            seen = true;
            if (++held > 120) break;
        } else {
            held = 0;
        }
    }
    if (!seen) return false;
    oa::runtime::FrameInput en;
    en.key_down_edges = {13};
    en.keys_down.insert(13);
    for (size_t f = 0; f < 60 && !rt.exit_requested(); ++f) rt.tick(16, f == 0 ? en : idle);
    return true;
}
/// Idle until the story re-parks; returns the current sample.
std::string wait_story(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                       size_t budget) {
    for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
        if (is_story_wait(w) && ml && ml->reveal_index >= ml->char_count && !ml->reveal_pending)
            return sample(rt);
    }
    return sample(rt);
}
/// Hover then click at (x,y); the pointer stays parked `after` frames.
void hover_click(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle, int x, int y,
                 size_t after = 400) {
    oa::runtime::FrameInput mv;
    mv.mouse_x = x;
    mv.mouse_y = y;
    for (size_t f = 0; f < 30; ++f) rt.tick(16, mv);
    oa::runtime::FrameInput cl;
    cl.mouse_x = x;
    cl.mouse_y = y;
    cl.left_click_edge = true;
    cl.left_down = true;
    rt.tick(16, cl);
    oa::runtime::FrameInput hold;
    hold.mouse_x = x;
    hold.mouse_y = y;
    for (size_t f = 0; f < after && !rt.exit_requested(); ++f) rt.tick(16, hold);
    oa::runtime::FrameInput out;
    out.mouse_x = -1;
    out.mouse_y = -1;
    for (size_t f = 0; f < 20 && !rt.exit_requested(); ++f) rt.tick(16, out);
}
/// World center of the layer whose click row carries `key`.
bool row_center(oa::runtime::GameRuntime& rt, const std::string& key, int* x, int* y) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end() || k->second != key) continue;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 60.0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 60.0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (!rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) return false;
        *x = int(x0 + rw / 2);
        *y = int(y0 + rh / 2);
        return true;
    }
    return false;
}
/// Hover+click one dock button and report whether a UI screen opened.
bool dock_opens(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                const char* key) {
    int x = 0, y = 0;
    if (!row_center(rt, key, &x, &y)) return false;
    const size_t base = rt.scene().size();
    hover_click(rt, idle, x, y, 200);
    for (size_t f = 0; f < 1200 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        if (rt.scene().size() > base + 60 ||
            (w && w->kind == oa::runtime::WaitReason::Kind::Stop) ||
            rt.scene().find("500.bt.1.bg.0") != nullptr) {
            return true;
        }
    }
    return false;
}
/// Close any UI screen with Esc and settle back at the story.
bool close_ui_and_park(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    if (rt.scene().find("500.bt.1.bg.0") != nullptr || rt.scene().size() > 200) {
        press_tick(rt, idle, 27);
        (void)wait_story(rt, idle, 12000);
    }
    return park_story(rt, idle, 2500);
}
bool ui_or_page_open(oa::runtime::GameRuntime& rt) {
    return rt.scene().find("500.bt.1.bg.0") != nullptr || rt.scene().size() > 200;
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_FPM_PFS unset; skipping\n");
        return 77;
    }
    namespace fsf = std::filesystem;
    std::error_code ec;
    const fsf::path store_dir =
        fsf::temp_directory_path(ec) / ("oa_qload_" + std::to_string(::getpid()));
    fsf::remove_all(store_dir, ec);
    auto store = std::make_shared<oa::runtime::DirSaveStore>(store_dir.string());
    const auto count_saves = [&store]() {
        int n = 0;
        std::error_code ec2;
        for (auto it = std::filesystem::recursive_directory_iterator(store->root(), ec2);
             !ec2 && it != std::filesystem::recursive_directory_iterator(); it.increment(ec2)) {
            if (it->is_regular_file(ec2) &&
                it->path().extension().generic_string() == ".dat" &&
                it->path().generic_string().find("save") != std::string::npos)
                ++n;
        }
        return n;
    };

    // ---- Runtime A: quick-save + quickload round trip ---------------------
    {
                auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rt(fs);
        rt.set_save_store(store);
        rt.open_project("windows");
        rt.boot_project();
        rt.set_transition_capture_callback([&]() { rt.mark_transition_captured(); });
        oa::runtime::FrameInput idle;
        check(reach_title(rt, idle), "C-1 title parked");
        start_game(rt, idle);
        check(reach_story_page(rt, idle), "C-2 story body parked");
        const std::string s0 = sample(rt);
        check(s0 != "-" && !s0.empty(), "C-3 saved-page sample captured");
        check(has_push_row(rt, 117) && has_push_row(rt, 116),
              "C-4 story push rows present before any load (117/116)");
        check(dock_opens(rt, idle, "bt_save"), "C-5 dock save works before any load");
        check(close_ui_and_park(rt, idle), "C-6 story re-parked after closing");
        std::printf("[qd] story0 sample='%s'\n", s0.c_str());

        // quick save (F4 = adv QSAVE): dialog (def 0) -> Enter
        const int saves_before = count_saves();
        press_tick(rt, idle, 115);
        (void)confirm_dialog(rt, idle);
        bool qsaved = false;
        for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (count_saves() > saves_before && rt.scene().size() < 200) {
                qsaved = true;
                break;
            }
        }
        check(qsaved, "C-7 quick save wrote a numbered save (quick slot)");
        (void)wait_story(rt, idle, 6000);
        check(park_story(rt, idle, 3000), "C-8 story re-parked after quick save");

        // advance past the saved page
        bool advanced = false;
        for (int t = 0; t < 4 && !rt.exit_requested(); ++t) {
            if (!park_story(rt, idle, 2500)) break;
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = 640;
            cl.mouse_y = 600;
            rt.tick(16, cl);
            const std::string nv = wait_story(rt, idle, 3000);
            if (nv != s0) {
                advanced = true;
                break;
            }
        }
        check(advanced, "C-9 story advanced past the saved page");

        // quick load (F5 = adv QLOAD): dialog -> Enter -> engine [load]
        press_tick(rt, idle, 116);
        check(confirm_dialog(rt, idle), "C-10 quickload confirmation dialog appeared");
        bool restored = false;
        for (size_t f = 0; f < 20000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const auto* w = rt.current_wait();
            const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
            if (sample(rt) == s0 && !ui_or_page_open(rt) && is_story_wait(w) && ml &&
                ml->reveal_index >= ml->char_count && !ml->reveal_pending) {
                restored = true;
                break;
            }
        }
        check(restored, "C-11 quickload restored the story to the saved page");
        std::printf("[qd] post-qload sample='%s' layers=%zu wait=%s\n", sample(rt).c_str(),
                    rt.scene().size(), ws(rt.current_wait()));
        check(has_push_row(rt, 117) && has_push_row(rt, 116) && has_push_row(rt, 115) &&
                  has_push_row(rt, 1),
              "C-12 push rows restored after quickload (engine snapshot)");
        // the reported bug: dock save/load/config + F6 all dead after qload
        check(dock_opens(rt, idle, "bt_save"), "C-13 dock SAVE opens after quickload");
        check(close_ui_and_park(rt, idle), "C-14 story re-parked after dock save");
        check(dock_opens(rt, idle, "bt_load"), "C-15 dock LOAD opens after quickload");
        check(close_ui_and_park(rt, idle), "C-16 story re-parked after dock load");
        check(dock_opens(rt, idle, "bt_conf"), "C-17 dock CONFIG opens after quickload");
        check(close_ui_and_park(rt, idle), "C-18 story re-parked after dock config");
        {
            const size_t pd0 = rt.pointer_dispatch_count();
            press_tick(rt, idle, 117); // F6 = SAVE key path
            bool opened = false;
            for (size_t f = 0; f < 3000 && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                if (ui_or_page_open(rt)) {
                    opened = true;
                    break;
                }
            }
            check(opened && rt.pointer_dispatch_count() > pd0,
                  "C-19 F6 SAVE key opens the save screen after quickload");
            check(close_ui_and_park(rt, idle), "C-20 story re-parked after F6");
        }
    }

    // ---- Runtime B (fresh): normal F6-save / F7-load dock control ----------
    {
                auto fs2 = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rc(fs2);
        rc.set_save_store(store);
        rc.open_project("windows");
        rc.boot_project();
        rc.set_transition_capture_callback([&]() { rc.mark_transition_captured(); });
        oa::runtime::FrameInput idleC;
        check(reach_title(rc, idleC), "C-21 (fresh) title parked");
        start_game(rc, idleC);
        check(reach_story_page(rc, idleC), "C-22 (fresh) story body parked");
        const std::string sC = sample(rc);
        // normal slot-1 save via the F6 page
        press_tick(rc, idleC, 117);
        bool page = false;
        for (size_t f = 0; f < 8000 && !rc.exit_requested(); ++f) {
            rc.tick(16, idleC);
            if (rc.scene().find("500.bt.1.bg.0") != nullptr && rc.scene().size() > 300) {
                page = true;
                break;
            }
        }
        check(page, "C-23 F6 opened the save screen (fresh)");
        bool slot1 = false;
        if (page) {
            int x = 0, y = 0;
            if (row_center(rc, "bt_save01", &x, &y)) {
                hover_click(rc, idleC, x, y, 200);
                // The save page stays open after a slot save (saveload_reload
                // refreshes it); the file landing is the completion marker
                // (U3 flow closes the page afterwards).
                for (size_t f = 0; f < 12000 && !rc.exit_requested(); ++f) {
                    rc.tick(16, idleC);
                    if (store->exists("savedata_cn/save0001.dat")) {
                        slot1 = true;
                        break;
                    }
                }
            }
            press_tick(rc, idleC, 27);
            (void)wait_story(rc, idleC, 12000);
        }
        check(slot1, "C-24 slot-1 save written (fresh)");
        // advance a page, then normal load via F7
        for (int t = 0; t < 4 && !rc.exit_requested(); ++t) {
            if (!park_story(rc, idleC, 2500)) break;
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = 640;
            cl.mouse_y = 600;
            rc.tick(16, cl);
            if (wait_story(rc, idleC, 3000) != sC) break;
        }
        press_tick(rc, idleC, 118); // F7 = LOAD
        bool loadpage = false;
        for (size_t f = 0; f < 8000 && !rc.exit_requested(); ++f) {
            rc.tick(16, idleC);
            if (rc.scene().find("500.bt.1.bg.0") != nullptr && rc.scene().size() > 300) {
                loadpage = true;
                break;
            }
        }
        check(loadpage, "C-25 F7 opened the load screen (fresh)");
        bool loaded = false;
        if (loadpage) {
            int x = 0, y = 0;
            if (row_center(rc, "bt_save01", &x, &y)) {
                hover_click(rc, idleC, x, y, 300);
                check(confirm_dialog(rc, idleC), "C-26 load confirm dialog (fresh)");
                for (size_t f = 0; f < 20000 && !rc.exit_requested(); ++f) {
                    rc.tick(16, idleC);
                    const auto* w = rc.current_wait();
                    const oa::render::MessageLayer* ml =
                        rc.text().layer("1.80.mw.adv_adv");
                    if (sample(rc) == sC && !ui_or_page_open(rc) && is_story_wait(w) && ml &&
                        ml->reveal_index >= ml->char_count && !ml->reveal_pending) {
                        loaded = true;
                        break;
                    }
                }
            }
        }
        check(loaded, "C-27 normal load restored the story (fresh)");
        check(has_push_row(rc, 117), "C-28 push rows present after normal load");
        check(dock_opens(rc, idleC, "bt_save"), "C-29 dock SAVE opens after normal load");
        check(close_ui_and_park(rc, idleC), "C-30 story re-parked (fresh control)");
        check(dock_opens(rc, idleC, "bt_load"), "C-31 dock LOAD opens after normal load");
        check(close_ui_and_park(rc, idleC), "C-32 story re-parked (fresh control)");
        check(dock_opens(rc, idleC, "bt_conf"), "C-33 dock CONFIG opens after normal load");
        check(close_ui_and_park(rc, idleC), "C-34 story re-parked (fresh control)");
    }

    // ---- Runtime C (fresh process): F5 quickload of the PREVIOUS session's
    // quick record ------------------------------------------------
    {
        std::printf("[qd] ==== ARM C: FRESH process F5 quickload ====\n");
                auto fs3 = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rc(fs3);
        rc.set_save_store(store);
        rc.open_project("windows");
        rc.boot_project();
        rc.set_transition_capture_callback([&]() { rc.mark_transition_captured(); });
        oa::runtime::FrameInput idleC;
        const bool tit = reach_title(rc, idleC);
        std::printf("[qd] (C) title parked=%d\n", (int)tit);
        start_game(rc, idleC);
        const bool st = reach_story_page(rc, idleC);
        std::printf("[qd] (C) story parked=%d sample='%s' rows117=%d\n", (int)st,
                    sample(rc).c_str(), (int)has_push_row(rc, 117));
        press_tick(rc, idleC, 116); // F5 quickload
        const bool dlg = confirm_dialog(rc, idleC);
        std::printf("[qd] (C) qload dialog=%d\n", (int)dlg);
        bool restored = false;
        for (size_t f = 0; f < 20000 && !rc.exit_requested(); ++f) {
            rc.tick(16, idleC);
            const auto* w = rc.current_wait();
            const oa::render::MessageLayer* ml = rc.text().layer("1.80.mw.adv_adv");
            if (is_story_wait(w) && !ui_or_page_open(rc) && ml &&
                ml->reveal_index >= ml->char_count && !ml->reveal_pending) {
                restored = true;
                break;
            }
        }
        check(restored, "C-35 fresh-process quickload restored the story page");
        std::printf("[qd] (C) restored sample='%s' layers=%zu wait=%s rows117=%d\n",
                    sample(rc).c_str(), rc.scene().size(), ws(rc.current_wait()),
                    (int)has_push_row(rc, 117));
        check(has_push_row(rc, 117) && has_push_row(rc, 116) && has_push_row(rc, 1),
              "C-36 fresh-process quickload keeps the push rows (cross-process)");
        check(dock_opens(rc, idleC, "bt_save"),
              "C-37 dock SAVE opens after fresh-process quickload");
        if (ui_or_page_open(rc)) {
            press_tick(rc, idleC, 27);
            (void)wait_story(rc, idleC, 12000);
        }
        check(dock_opens(rc, idleC, "bt_conf"),
              "C-38 dock CONFIG opens after fresh-process quickload");
        if (ui_or_page_open(rc)) press_tick(rc, idleC, 27);
        (void)wait_story(rc, idleC, 12000);
        check(has_push_row(rc, 117),
              "C-39 push rows still present at the end of the fresh-process arm");
    }

    // ---- Runtime D (fresh process): quickload THROUGH the F7 load screen --
    {
        std::printf("[qd] ==== ARM D: FRESH process load-screen quickload ====\n");
                auto fs4 = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rd(fs4);
        rd.set_save_store(store);
        rd.open_project("windows");
        rd.boot_project();
        rd.set_transition_capture_callback([&]() { rd.mark_transition_captured(); });
        oa::runtime::FrameInput idleD;
        const bool tit = reach_title(rd, idleD);
        std::printf("[qd] (D) title parked=%d\n", (int)tit);
        start_game(rd, idleD);
        const bool st = reach_story_page(rd, idleD);
        std::printf("[qd] (D) story parked=%d sample='%s' rows117=%d\n", (int)st,
                    sample(rd).c_str(), (int)has_push_row(rd, 117));
        // F7 -> load page; then switch the page to the quickload view
        press_tick(rd, idleD, 118);
        bool page = false;
        for (size_t f = 0; f < 8000 && !rd.exit_requested(); ++f) {
            rd.tick(16, idleD);
            if (rd.scene().find("500.bt.1.bg.0") != nullptr && rd.scene().size() > 300) {
                page = true;
                break;
            }
        }
        std::printf("[qd] (D) load page open=%d\n", (int)page);
        if (page) {
            // find any click row mentioning qload on the page
            std::string qkey;
            for (const oa::render::Layer* l : rd.scene().draw_order()) {
                const auto* h = rd.scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto k = h->params.find("key");
                if (k == h->params.end()) continue;
                if (k->second.find("qload") != std::string::npos ||
                    k->second.find("ql") == 0) {
                    qkey = k->second;
                    break;
                }
            }
            std::printf("[qd] (D) qload switch key='%s'\n", qkey.c_str());
            if (!qkey.empty()) {
                int x = 0, y = 0;
                if (row_center(rd, qkey, &x, &y)) hover_click(rd, idleD, x, y, 300);
                std::printf("[qd] (D) clicked %s; rows now:\n", qkey.c_str());
                std::string slots;
                for (const oa::render::Layer* l : rd.scene().draw_order()) {
                    const auto* h = rd.scene().find_event_handler(l->id, "click");
                    if (!h) continue;
                    const auto k = h->params.find("key");
                    if (k == h->params.end()) continue;
                    if (k->second.rfind("bt_save", 0) == 0)
                        slots += k->second + " ";
                }
                std::printf("[qd] (D)   slot keys: %s\n", slots.c_str());
                // click the first quick slot if present
                for (const char* sk : {"bt_save01", "bt_save02"}) {
                    if (slots.find(sk) == std::string::npos) continue;
                    int x2 = 0, y2 = 0;
                    if (row_center(rd, sk, &x2, &y2)) hover_click(rd, idleD, x2, y2, 300);
                    std::printf("[qd] (D) clicked slot %s\n", sk);
                    break;
                }
                const bool dlg = confirm_dialog(rd, idleD);
                std::printf("[qd] (D) load confirm dialog=%d\n", (int)dlg);
            }
        }
        bool restored = false;
        for (size_t f = 0; f < 20000 && !rd.exit_requested(); ++f) {
            rd.tick(16, idleD);
            const auto* w = rd.current_wait();
            const oa::render::MessageLayer* ml = rd.text().layer("1.80.mw.adv_adv");
            if (is_story_wait(w) && !ui_or_page_open(rd) && ml &&
                ml->reveal_index >= ml->char_count && !ml->reveal_pending) {
                restored = true;
                break;
            }
        }
        check(restored,
              "C-40 fresh-process load-page quickload restored the story page");
        std::printf("[qd] (D) restored sample='%s' layers=%zu wait=%s rows117=%d\n",
                    sample(rd).c_str(), rd.scene().size(), ws(rd.current_wait()),
                    (int)has_push_row(rd, 117));
        check(has_push_row(rd, 117) && has_push_row(rd, 1),
              "C-41 load-page quickload keeps the push rows (cross-process)");
        check(dock_opens(rd, idleD, "bt_save"),
              "C-42 dock SAVE opens after load-page quickload (fresh process)");
        if (ui_or_page_open(rd)) {
            press_tick(rd, idleD, 27);
            (void)wait_story(rd, idleD, 12000);
        }
        check(dock_opens(rd, idleD, "bt_conf"),
              "C-43 dock CONFIG opens after load-page quickload (fresh process)");
        if (ui_or_page_open(rd)) press_tick(rd, idleD, 27);
        (void)wait_story(rd, idleD, 12000);
        check(has_push_row(rd, 117),
              "C-44 push rows still present at the end of the load-page arm");
    }

    std::error_code ec2;
    fsf::remove_all(store_dir, ec2);
    if (failures) {
        std::fprintf(stderr, "qload_dock_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("qload_dock_test: all ok (quickload keeps dock + keys alive)\n");
    return 0;
}
