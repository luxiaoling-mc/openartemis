// Final acceptance journey (headless, deterministic): boot -> title -> start ->
// story body -> >=3 page turns (click advance) -> backlog open/close -> save
// open (slot text nodes materialized at slot frames) /close -> load-less config
// open/close -> story text visibility returns and the page sample is unchanged
// (no residue), zero exit. Real fpm/root.pfs (env-gated like title_drain/p3b).
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

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
const char* wsid(const oa::runtime::WaitReason* w) {
    return (w && !w->id.empty()) ? w->id.c_str() : "-";
}
std::string stkstr(const oa::runtime::GameRuntime& rt) {
    std::string o;
    const auto& cs = rt.interpreter().call_stack();
    for (const auto& f : cs) {
        o += "[" + f.script + ":" + std::to_string(f.return_line) + "]";
    }
    return o;
}
std::string sample(oa::runtime::GameRuntime& rt, const std::string& id) {
    const oa::render::MessageLayer* ml = rt.text().layer(id);
    if (!ml) return "-";
    for (const auto& u : ml->page)
        if (!u.data.empty()) return u.data.substr(0, 16);
    return "-";
}
bool is_story_wait(const oa::runtime::WaitReason* w) {
    if (!w) return false;
    return w->kind == oa::runtime::WaitReason::Kind::Generic ||
           w->kind == oa::runtime::WaitReason::Kind::Generic0 ||
           w->kind == oa::runtime::WaitReason::Kind::Timed;
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_FPM_PFS unset; skipping\n");
        return 77;
    }
    try {
                auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        rt.boot_project();
        oa::runtime::FrameInput idle;
        if (std::getenv("OA_UX_TRACE")) {
            rt.interpreter().on_step = [&rt](const std::string& scr, size_t ln,
                                          const oa::runtime::Instruction& i) {
                std::string ps;
                for (const auto& [kk, vv] : i.params) ps += " " + kk + "=" + vv;
                if (ps.size() > 100) ps.resize(100);
                const std::string tg = i.tag;
                if (tg != "call" && tg != "return" && tg != "jump" && tg != "stop" &&
                    tg != "wait" && tg != "wt" && tg != "wt0")
                    return;
                std::printf("[ux][step] d=%zu %s:%zu %s%s\n",
                            rt.interpreter().call_stack().size(), scr.c_str(), ln,
                            tg.c_str(), ps.c_str());
            };
        }
        bool title = false;
        rt.interpreter().on_step = [&](const std::string&, size_t,
                                       const oa::runtime::Instruction& i) {
            const std::string* f = i.get("function");
            if (f && *f == "title_init") title = true;
        };
        std::string target;
        for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (!title) continue;
            const auto* w = rt.current_wait();
            if (!(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty())) continue;
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                const auto* h = rt.scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto k = h->params.find("key");
                if (k != h->params.end() && k->second == "bt_start") {
                    target = l->id;
                    break;
                }
            }
            if (!target.empty()) break;
        }
        check(title && !target.empty(), "UJ-1 boot parked at the title with bt_start");
        // start: click the parked button candidates
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
        bool body = false;
        for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (rt.text().page_has_visible_text("1.80.mw.adv_adv")) {
                body = true;
                break;
            }
        }
        check(body, "UJ-2 story body text reached after the start click");
        if (std::getenv("OA_UX_TRACE")) {
            rt.interpreter().on_step = [&rt](const std::string& scr, size_t ln,
                                          const oa::runtime::Instruction& i) {
                std::string ps;
                for (const auto& [kk, vv] : i.params) ps += " " + kk + "=" + vv;
                if (ps.size() > 100) ps.resize(100);
                const std::string tg = i.tag;
                if (tg != "call" && tg != "return" && tg != "jump" && tg != "stop" &&
                    tg != "wait" && tg != "wt" && tg != "wt0")
                    return;
                std::printf("[ux][step] d=%zu %s:%zu %s%s\n",
                            rt.interpreter().call_stack().size(), scr.c_str(), ln,
                            tg.c_str(), ps.c_str());
            };
        }
        const std::string page0 = sample(rt, "1.80.mw.adv_adv");
        check(!page0.empty() && page0 != "-", "UJ-3 body page carries content");
        // ---- page turns: click when fully parked (Generic + reveal complete),
        //      exactly the real-user cadence; each click must advance the
        //      wait machine (page sample changes within a bounded walk) ----
        auto fully_parked = [&](size_t budget) {
            for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                const auto* w = rt.current_wait();
                const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
                if (w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                          w->kind == oa::runtime::WaitReason::Kind::Generic0) &&
                    ml && ml->reveal_index >= ml->char_count && !ml->reveal_pending)
                    return true;
            }
            return false;
        };
        std::string last = page0;
        int advanced = 0;
        int clicks = 0;
        for (int t = 0; t < 8 && !rt.exit_requested(); ++t) {
            if (!fully_parked(2500)) break; // 久等不到完整停驻：停止翻页
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = 640;
            cl.mouse_y = 600;
            rt.tick(16, cl);
            ++clicks;
            // 点击后推进可能跨若干帧/块；等下一个完整停驻并观察页内容变化
            bool moved = false;
            for (size_t f = 0; f < 2500 && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                const std::string now = sample(rt, "1.80.mw.adv_adv");
                if (now != last) {
                    last = now;
                    moved = true;
                    break;
                }
                const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
                const auto* w = rt.current_wait();
                if (w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                          w->kind == oa::runtime::WaitReason::Kind::Generic0) &&
                    ml && ml->reveal_index >= ml->char_count && !ml->reveal_pending &&
                    f > 4)
                    break;
            }
            if (moved) ++advanced;
            if (advanced >= 3) break;
        }
        check(clicks >= 3 && advanced >= 2,
              "UJ-4/5 multiple clicks advanced the story pages (advanced=%d/%d)",
              advanced, clicks);
        std::printf("[ux] clicks=%d advanced=%d final='%s'\n", clicks, advanced,
                    last.c_str());
        if (std::getenv("OA_UX_TRACE")) {
            rt.set_pointer_observer([](const oa::runtime::GameRuntime::PointerDispatch& dd) {
                std::printf("[ux][disp] kind=%d layer='%s' fn='%s' key='%s' ran=%d\n",
                            (int)dd.kind, dd.layer.c_str(), dd.function.c_str(),
                            dd.key.c_str(), dd.ran_lua ? 1 : 0);
            });
        }
        // ---- backlog (F8=119) ----
        std::string before_ui;
        {
            const bool fp = fully_parked(3000);
            std::printf("[ux] before F8 fully_parked=%d wait=%s layers=%zu sample='%s'\n",
                        fp ? 1 : 0, ws(rt.current_wait()), rt.scene().size(),
                        sample(rt, "1.80.mw.adv_adv").c_str());
            if (std::getenv("OA_UX_TRACE")) {
                std::string pv = sample(rt, "1.80.mw.adv_adv");
                for (size_t f = 0; f < 1500; ++f) {
                    rt.tick(16, idle);
                    const std::string nv = sample(rt, "1.80.mw.adv_adv");
                    const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
                    const auto* w = rt.current_wait();
                    if (nv != pv) {
                        std::printf("[ux][tr] f=%zu sample '%s' -> '%s' wait=%s ri=%zu cc=%zu rp=%d\n",
                                    f, pv.c_str(), nv.c_str(), ws(w),
                                    ml ? ml->reveal_index : 0, ml ? ml->char_count : 0,
                                    (ml && ml->reveal_pending) ? 1 : 0);
                        pv = nv;
                    }
                }
            }
            if (!fp) {
                // 退一步：再点击一次到完整停驻再开 UI
                oa::runtime::FrameInput cl;
                cl.left_click_edge = true;
                cl.left_down = true;
                cl.mouse_x = 640;
                cl.mouse_y = 600;
                rt.tick(16, cl);
                (void)fully_parked(2500);
            }
            std::printf("[ux] pre-UI park wait=%s/%s script=%s:%zu layers=%zu stack=%s\n",
                        ws(rt.current_wait()), wsid(rt.current_wait()),
                        rt.interpreter().current_script()
                            ? rt.interpreter().current_script()->c_str()
                            : "-",
                        rt.interpreter().current_line(), rt.scene().size(),
                        stkstr(rt).c_str());
            // UJ-18 baseline: the *settled* page (after the click cascade
            // parked) — comparing a mid-cascade snapshot is meaningless.
            before_ui = sample(rt, "1.80.mw.adv_adv");
            oa::runtime::FrameInput k;
            k.key_down_edges = {119};
            k.keys_down.insert(119);
            bool jumped = false;
            for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
                rt.tick(16, f == 0 ? k : idle);
                if (rt.scene().size() >= 185) {
                    jumped = true;
                    break;
                }
            }
            check(jumped, "UJ-6 backlog opened (layer count jumped)");
            if (jumped) {
                for (size_t f = 0; f < 3000 && !rt.exit_requested(); ++f) {
                    rt.tick(16, idle);
                    if (rt.scene().find("sl.1") != nullptr ||
                        rt.scene().find("500.z.bt.tx.1.1") != nullptr)
                        break;
                }
            }
            check(rt.scene().find("sl.1") != nullptr ||
                      rt.scene().find("500.z.bt.tx.1.1") != nullptr,
                  "UJ-6b backlog content nodes present");
            // msg_hide: story text invisible while the UI is open
            check(!rt.scene().is_message_layer_visible("1.80.mw.adv_adv"),
                  "UJ-7 story text hidden while the backlog is open");
            // backlog text node materialized under the tree with a translation
            const oa::render::Layer* bn = rt.scene().find("500.z.bt.tx.1.1");
            check(bn != nullptr, "UJ-8 backlog text node materialized");
            oa::runtime::FrameInput esc;
            esc.key_down_edges = {27};
            esc.keys_down.insert(27);
            std::string wprev2;
            for (size_t f = 0; f < 1500 && !rt.exit_requested(); ++f) {
                rt.tick(16, f == 0 ? esc : idle);
                if (std::getenv("OA_UX_TRACE")) {
                    const auto* w = rt.current_wait();
                    char b[256];
                    std::snprintf(b, sizeof(b), "%s/%s@%s:%zu L%zu",
                                  ws(w), wsid(w),
                                  rt.interpreter().current_script()
                                      ? rt.interpreter().current_script()->c_str()
                                      : "-",
                                  rt.interpreter().current_line(), rt.scene().size());
                    if (wprev2 != b) {
                        std::printf("[ux][trC] f=%zu %s sample='%s'\n", f, b,
                                    sample(rt, "1.80.mw.adv_adv").c_str());
                        wprev2 = b;
                    }
                }
                if (rt.scene().find("sl.1") == nullptr) break;
            }
            check(rt.scene().find("sl.1") == nullptr, "UJ-9 backlog closed (Esc)");
            std::printf("[ux] after blog close wait=%s sample='%s'\n", ws(rt.current_wait()),
                        sample(rt, "1.80.mw.adv_adv").c_str());
        }
        // ---- save (F6=117): open, check slot text nodes at slot frames, close ----
        {
            oa::runtime::FrameInput k;
            k.key_down_edges = {117};
            k.keys_down.insert(117);
            bool save_up = false;
            for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
                rt.tick(16, f == 0 ? k : idle);
                if (rt.scene().find("500.bt.1") != nullptr && rt.scene().size() > 200) {
                    save_up = true;
                    break;
                }
            }
            check(save_up, "UJ-10 save screen opened (slot group present)");
            const oa::render::Layer* txt = rt.scene().find("500.bt.1.bg.20");
            oa::render::Affine2 w;
            check(txt != nullptr &&
                      rt.scene().world_transform("500.bt.1.bg.20", &w) &&
                      w.is_plain_translation() && w.e > 30.0 && w.f > 60.0,
                  "UJ-11 slot-1 text node sits at its slot frame (world ~32,73)");
            check(rt.text().page_has_visible_text("500.bt.1.bg.20"),
                  "UJ-12 slot-1 label text present");
            check(!rt.scene().is_message_layer_visible("1.80.mw.adv_adv"),
                  "UJ-13 story text hidden on the save screen");
            oa::runtime::FrameInput esc;
            esc.key_down_edges = {27};
            esc.keys_down.insert(27);
            for (size_t f = 0; f < 1500 && !rt.exit_requested(); ++f) {
                rt.tick(16, f == 0 ? esc : idle);
                if (rt.scene().find("500.bt.1") == nullptr) break;
            }
            check(rt.scene().find("500.bt.1") == nullptr, "UJ-14 save screen closed (Esc)");
            std::printf("[ux] after save close wait=%s sample='%s'\n", ws(rt.current_wait()),
                        sample(rt, "1.80.mw.adv_adv").c_str());
        }
        // ---- config (F10=121): open/close; story text hidden while open ----
        {
            oa::runtime::FrameInput k;
            k.key_down_edges = {121};
            k.keys_down.insert(121);
            bool cfg = false;
            for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
                rt.tick(16, f == 0 ? k : idle);
                if (rt.scene().size() > 150) {
                    cfg = true;
                    break;
                }
            }
            // (config may need a generic park: if it didn't open, click once to
            //  reach a fully revealed park first and retry once)
            if (!cfg) {
                oa::runtime::FrameInput cl;
                cl.left_click_edge = true;
                cl.left_down = true;
                cl.mouse_x = 640;
                cl.mouse_y = 600;
                for (size_t f = 0; f < 900 && !rt.exit_requested(); ++f) {
                    rt.tick(16, f == 0 ? cl : idle);
                    if (is_story_wait(rt.current_wait()) && f > 2) break;
                }
                oa::runtime::FrameInput k2;
                k2.key_down_edges = {121};
                k2.keys_down.insert(121);
                for (size_t f = 0; f < 3000 && !rt.exit_requested(); ++f) {
                    rt.tick(16, f == 0 ? k2 : idle);
                    if (rt.scene().size() > 150) {
                        cfg = true;
                        break;
                    }
                }
            }
            check(cfg, "UJ-15 config screen opened");
            if (cfg) {
                check(!rt.scene().is_message_layer_visible("1.80.mw.adv_adv"),
                      "UJ-16 story text hidden on the config screen");
                oa::runtime::FrameInput esc;
                esc.key_down_edges = {27};
                esc.keys_down.insert(27);
                for (size_t f = 0; f < 2000 && !rt.exit_requested(); ++f) {
                    rt.tick(16, f == 0 ? esc : idle);
                    const auto* w = rt.current_wait();
                    if (w && is_story_wait(w) && rt.scene().size() < 150) break;
                }
                check(rt.scene().is_message_layer_visible("1.80.mw.adv_adv"),
                      "UJ-17 story text visible again after closing the config");
                std::printf("[ux] after config close wait=%s sample='%s'\n",
                            ws(rt.current_wait()),
                            sample(rt, "1.80.mw.adv_adv").c_str());
            }
        }
        // ---- back at the story: same page (no residue / no drift) ----
        const std::string settled = sample(rt, "1.80.mw.adv_adv");
        check(settled == before_ui,
              "UJ-18 story page unchanged across the UI detour (no residue/state drift)");
        std::printf("[ux] final settled='%s' wait=%s/%s layers=%zu script=%s:%zu stack=%s\n",
                    settled.c_str(), ws(rt.current_wait()), wsid(rt.current_wait()),
                    rt.scene().size(),
                    rt.interpreter().current_script()
                        ? rt.interpreter().current_script()->c_str()
                        : "-",
                    rt.interpreter().current_line(), stkstr(rt).c_str());
        // UJ-19: the story must still advance on a click after the whole UI
        // detour (the real-user "2 more clicks" step; the window detour must
        // not leave the wait machine dead — the R6 fix).
        {
            const std::string s0 = settled;
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = 640;
            cl.mouse_y = 600;
            rt.tick(16, cl);
            std::string cur = s0;
            bool moved = false;
            for (size_t f = 0; f < 2500 && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                const std::string nv = sample(rt, "1.80.mw.adv_adv");
                const auto* w = rt.current_wait();
                const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
                if (nv != cur) {
                    cur = nv;
                    moved = true;
                    break;
                }
                if (w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                          w->kind == oa::runtime::WaitReason::Kind::Generic0) &&
                    ml && ml->reveal_index >= ml->char_count && !ml->reveal_pending &&
                    f > 2)
                    break;
            }
            // UJ-19: the story must advance on a click after the whole UI
            // detour. Fixed by the R6 inline-event marker lifecycle rework
            // (QA-queue U10 → ✅, research/23): the boot-era marker is now
            // detached when the first queued [call] runs and markers are
            // armed strictly per helper flow, so every UI detour returns to
            // the parked story line instead of unwinding into system/ui.asb
            // [stop]. Hard check — no KNOWN-FAIL branch remains.
            check(moved, "UJ-19 story advances again after the UI detour");
            if (!moved) {
                std::printf(
                    "[ux] UJ-19 FAIL: UI detour kills story advance — click moved=0 "
                    "wait=%s script=%s:%zu\n",
                    ws(rt.current_wait()),
                    rt.interpreter().current_script()
                        ? rt.interpreter().current_script()->c_str()
                        : "-",
                    rt.interpreter().current_line());
            }
            std::printf("[ux] post-detour click moved=%d wait=%s sample='%s' layers=%zu\n",
                        moved ? 1 : 0, ws(rt.current_wait()), cur.c_str(),
                        rt.scene().size());
        }
        std::printf("ux_journey_test: all ok (failures=%d last_fail='%s')\n", failures,
                    g_last_fail);
        std::fflush(stdout);
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
}
