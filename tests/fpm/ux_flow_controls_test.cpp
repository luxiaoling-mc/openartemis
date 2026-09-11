// QA-queue U5/U7/U8 (headless, deterministic): wheel keys, title return,
// game exit. Real fpm/root.pfs (env-gated like ux_journey_test).
//
// U5 wheel: Artemis maps the mouse wheel onto vk 136 (up, HUP) / 137 (down,
// HDW) — the same keys FPM's keyconfig table routes (≡ PageUp/PageDown; adv
// BACKLOG/CLICK). At a parked story page a wheel-up edge must open the
// backlog (the FPM BACKLOG action, same chain as F8) and a wheel-down edge
// must script-advance the page (adv_click -> decide edge).
//
// U7 title return: the menu/config "返回标题" button runs adv_title ->
// yes/no dialog -> sv.go_title -> jump system/ui.asb *go_title ->
// [stop 0=exskip][syssave][reset]; the [reset] control event must restart
// the project (fresh interpreter + boot) back to the parked title with the
// bt_start button re-registered and the story scene gone.
//
// U8 game exit: the config "结束游戏" button runs adv_exit -> dialog ->
// sv.go_exit -> jump system/ui.asb *go_exit -> [syssave][exit]; the [exit]
// control event must raise GameRuntime::exit_requested (the SDL host
// polls it and quits with exit 0).
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <memory>
#include <string>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"

namespace {
int failures = 0;
char g_last_fail[512] = {0};
void check(bool cond, const char* fmt, ...) {
    if (!cond) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(g_last_fail, sizeof(g_last_fail), fmt, ap);
        va_end(ap);
        std::fprintf(stderr, "FAIL: %s\n", g_last_fail);
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
/// Boot to the parked title (Stop wait + bt_start click row registered).
bool reach_title(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                 size_t budget = 12000) {
    for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
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
/// Click bt_start candidates to leave the title into the game start flow.
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
/// Advance clicks until a fully parked story page (Generic wait + reveal
/// complete). Returns the parked page sample.
bool reach_story_page(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                      std::string* page_out) {
    for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.text().page_has_visible_text("1.80.mw.adv_adv")) break;
    }
    auto fully = [&](size_t budget) {
        for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const auto* w = rt.current_wait();
            const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
            if (is_story_wait(w) && ml && ml->reveal_index >= ml->char_count &&
                !ml->reveal_pending)
                return true;
        }
        return false;
    };
    for (int t = 0; t < 6 && !rt.exit_requested(); ++t) {
        if (!fully(2500)) break;
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = 640;
        cl.mouse_y = 600;
        rt.tick(16, cl);
        (void)fully(2500);
    }
    const std::string page = sample(rt);
    if (page_out) *page_out = page;
    return page != "-" && !page.empty();
}
/// Wait until a fully-parked story page (idempotent settle helper).
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
/// Find a scene layer whose click handler row carries `key` and click its
/// world center once. Returns true when the row existed.
bool click_key_layer(oa::runtime::GameRuntime& rt, const std::string& key) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end() || k->second != key) continue;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? double(l->width) : 160.0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? double(l->height) : 48.0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (!rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) {
            x0 = l->left;
            y0 = l->top;
            rw = w;
            rh = hh;
        }
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = int(x0 + rw / 2);
        cl.mouse_y = int(y0 + rh / 2);
        rt.tick(16, cl);
        std::printf("[uf] clicked key=%s layer=%s @(%d,%d) wait=%s layers=%zu\n", key.c_str(),
                    l->id.c_str(), cl.mouse_x, cl.mouse_y, ws(rt.current_wait()),
                    rt.scene().size());
        return true;
    }
    return false;
}
/// Press one key edge for one tick, then keep ticking up to `budget` frames.
void press_key(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle, int key,
               size_t budget) {
    oa::runtime::FrameInput k;
    k.key_down_edges = {key};
    k.keys_down.insert(key);
    for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) rt.tick(16, f == 0 ? k : idle);
}
/// Wait for the FPM yes/no dialog to finish opening (its button node present
/// while the UI flow parks on a Stop wait), then settle a few frames so the
/// auto-cursor (YES) is live, and confirm with Enter (CLICK key).
bool confirm_dialog(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                    size_t settle = 120) {
    const auto* w = rt.current_wait();
    bool parked = w && w->kind == oa::runtime::WaitReason::Kind::Stop;
    for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
        if (parked && rt.scene().find("600.1.bt.1.0") != nullptr) break;
        rt.tick(16, idle);
        const auto* w2 = rt.current_wait();
        parked = w2 && w2->kind == oa::runtime::WaitReason::Kind::Stop;
    }
    for (size_t f = 0; f < settle && !rt.exit_requested(); ++f) rt.tick(16, idle);
    oa::runtime::FrameInput en;
    en.key_down_edges = {13};
    en.keys_down.insert(13);
    for (size_t f = 0; f < 200 && !rt.exit_requested(); ++f) rt.tick(16, f == 0 ? en : idle);
    return rt.scene().find("600.1.bt.1.0") != nullptr; // still on screen pre-confirm
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_FPM_PFS unset; skipping\n");
        return 77;
    }
    // ---- U5: wheel keys 136/137 -------------------------------
    {
                auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        rt.boot_project();
        oa::runtime::FrameInput idle;
        check(reach_title(rt, idle), "U5-0 title parked before the wheel run");
        start_game(rt, idle);
        std::string page0;
        check(reach_story_page(rt, idle, &page0), "U5-1 story body page parked");
        check(park_story(rt, idle), "U5-2 story fully parked before wheel-up");
        // wheel-up (136): adv BACKLOG -> open_ui('blog') like F8
        bool blog = false;
        {
            oa::runtime::FrameInput wu;
            wu.key_down_edges = {136};
            wu.keys_down.insert(136);
            for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
                rt.tick(16, f == 0 ? wu : idle);
                if (rt.scene().find("500.z.bt.tx.1.1") != nullptr && rt.scene().size() > 180) {
                    blog = true;
                    break;
                }
            }
        }
        check(blog, "U5-3 wheel-up (136) opened the backlog (layers+content)");
        check(!rt.scene().is_message_layer_visible("1.80.mw.adv_adv"),
              "U5-4 story text hidden while the wheel-opened backlog is up");
        // close the backlog (Esc = UI EXIT inside the engine)
        press_key(rt, idle, 27, 2000);
        check(park_story(rt, idle), "U5-5 story parked again after closing the backlog");
        // wheel-down (137): adv CLICK -> adv_click decide -> page advance
        bool moved = false;
        {
            const std::string s1 = sample(rt);
            oa::runtime::FrameInput wd;
            wd.key_down_edges = {137};
            wd.keys_down.insert(137);
            for (size_t f = 0; f < 3000 && !rt.exit_requested(); ++f) {
                rt.tick(16, f == 0 ? wd : idle);
                const std::string nv = sample(rt);
                if (nv != s1) {
                    moved = true;
                    break;
                }
            }
            std::printf("[uf] wheel-down page '%s' -> '%s'\n", s1.c_str(), sample(rt).c_str());
        }
        check(moved, "U5-6 wheel-down (137) advanced the story page");
    }
    // ---- U7: 返回标题 (menu flow -> [reset] -> engine restart) ----
    {
                auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        rt.boot_project();
        oa::runtime::FrameInput idle;
        check(reach_title(rt, idle), "U7-0 title parked before the return run");
        start_game(rt, idle);
        std::string page0;
        check(reach_story_page(rt, idle, &page0), "U7-1 story body page parked");
        // F12 (123) = the TITLE adv action (same chain as the config
        // "返回标题" button: csv.advkey.tbl[TITLE] = adv_title)
        bool dlg = false;
        {
            oa::runtime::FrameInput f12;
            f12.key_down_edges = {123};
            f12.keys_down.insert(123);
            for (size_t f = 0; f < 6000 && !rt.exit_requested(); ++f) {
                rt.tick(16, f == 0 ? f12 : idle);
                // the yes/no dialog draws its own button group with bt_yes
                if (rt.scene().find("600.1.bt.1.0") != nullptr) {
                    dlg = true;
                    break;
                }
            }
        }
        check(dlg, "U7-2 title-return confirmation dialog opened");
        // confirm the default (Enter = CLICK on the auto-cursor YES) after
        // the dialog finished opening (its UI flow parks on a Stop wait)
        bool reset_seen = false;
        {
            (void)confirm_dialog(rt, idle);
            for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                // after [reset] the engine re-boots: the story layer and the
                // parked title both reappear only on the fresh boot; detect
                // the reset via the parked title + bt_start + story gone.
                const auto* w = rt.current_wait();
                if (!(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty()))
                    continue;
                bool bt = false;
                for (const oa::render::Layer* l : rt.scene().draw_order()) {
                    const auto* h = rt.scene().find_event_handler(l->id, "click");
                    if (!h) continue;
                    const auto k = h->params.find("key");
                    if (k != h->params.end() && k->second == "bt_start") {
                        bt = true;
                        break;
                    }
                }
                if (bt) {
                    reset_seen = true;
                    std::printf("[uf] title restored at f=%zu layers=%zu sample='%s'\n", f,
                                rt.scene().size(), sample(rt).c_str());
                    break;
                }
            }
        }
        check(reset_seen, "U7-3 [reset] restarted the engine to the parked title");
        check(sample(rt) == "-" &&
                  !rt.scene().is_message_layer_visible("1.80.mw.adv_adv"),
              "U7-4 story scene cleared by the title return");
        // the fresh title must still be live: はじめから -> story again
        start_game(rt, idle);
        std::string page1;
        check(reach_story_page(rt, idle, &page1), "U7-5 story reachable again after the reset");
        std::printf("[uf] post-reset story page='%s'\n", page1.c_str());
    }
    // ---- U8: 结束游戏 (config button -> [exit] -> host quit request) ----
    {
                auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        rt.boot_project();
        oa::runtime::FrameInput idle;
        check(reach_title(rt, idle), "U8-0 title parked before the exit run");
        start_game(rt, idle);
        check(reach_story_page(rt, idle, nullptr), "U8-1 story body page parked");
        // open the config screen (F10)
        bool cfg = false;
        {
            oa::runtime::FrameInput f10;
            f10.key_down_edges = {121};
            f10.keys_down.insert(121);
            for (size_t f = 0; f < 6000 && !rt.exit_requested(); ++f) {
                rt.tick(16, f == 0 ? f10 : idle);
                if (rt.scene().size() > 150) {
                    cfg = true;
                    break;
                }
            }
        }
        check(cfg, "U8-2 config screen opened");
        check(click_key_layer(rt, "bt_end"), "U8-3 config 结束游戏 button row present");
        // the exit dialog: Enter confirms the default (as in U7)
        bool dlg = false;
        for (size_t f = 0; f < 6000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (rt.scene().find("600.1.bt.1.0") != nullptr) {
                dlg = true;
                break;
            }
        }
        check(dlg, "U8-4 exit confirmation dialog opened");
        bool exited = false;
        {
            (void)confirm_dialog(rt, idle);
            for (size_t f = 0; f < 6000 && !exited; ++f) {
                if (rt.exit_requested()) {
                    exited = true;
                    break;
                }
                rt.tick(16, idle);
            }
        }
        check(exited, "U8-5 [exit] raised exit_requested() (host quits)");
    }
    std::printf("ux_flow_controls_test: all ok (failures=%d last_fail='%s')\n", failures,
                g_last_fail);
    std::fflush(stdout);
    return failures ? 1 : 0;
}
