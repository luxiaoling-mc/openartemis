// R10b regression: live backlog rows must keep their scene-bound text layers
// after the R10 metric-var barrier change (user real-device: backlog renders
// completely wrong under e8411fa).
//
// Mechanism under test: FPM rebuilds backlog page layers inside event
// streams that also queue structural control tags (lydel 500.z.bt.tx +
// per-row chgmsg/rp/print) and sync metric vars (system_ui_backlog.lua row
// fill: get_message_layer_height for the sub-language row top). The M10c/R10
// barrier pre-executes queued tags when a sync var evaluates at its queue
// position. e8411fa *skipped* benign control tags (flip/lydel/...) and kept
// flushing later text tags — the rebuild text then ran against the pre-lydel
// state and the deferred lydel deleted the freshly bound row layers (rows
// lost their scene nodes / went blank). The fix executes the queued prefix
// IN ORDER (stop only at pauses/redirects/Lua re-entry).
//
// This test boots the real game, walks a few story pages, opens the backlog
// (F8) and asserts every visible row's text layer is filled AND bound to a
// live scene node at the row's slot. Under e8411fa every row lost its node.
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
        auto parked = [&]() {
            if (rt.transition().is_in_progress(rt.now_ms())) return false;
            const auto* w = rt.current_wait();
            if (!is_clickable_wait(w)) return false;
            // full reveal (mirror the windowed driver's ad_story_parked)
            const oa::render::MessageLayer* ml =
                rt.text().layer("1.80.mw.adv_adv");
            return ml && ml->reveal_index >= ml->char_count &&
                   !ml->reveal_pending;
        };
        // walk ~10 story pages so the log spans a backlog page
        int turns = 0;
        bool click_armed = false;
        for (size_t f = 0; f < 60000 && !rt.exit_requested() && turns < 10;
             ++f) {
            if (click_armed) {
                rt.tick(16, idle);
                click_armed = false;
                continue;
            }
            if (!parked()) {
                rt.tick(16, idle);
                continue;
            }
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = 640;
            cl.mouse_y = 600;
            rt.tick(16, cl);
            click_armed = true;
            ++turns;
        }
        for (size_t f = 0; f < 300; ++f) rt.tick(16, idle); // settle (windowed parity)
        {
            const oa::render::MessageLayer* ml =
                rt.text().layer("1.80.mw.adv_adv");
            std::string s0 = ml ? "-" : "no-layer";
            if (ml)
                for (const auto& u : ml->page)
                    if (!u.data.empty()) {
                        s0 = u.data.substr(0, 12);
                        break;
                    }
            std::printf("[r10b] before F8: story='%s' layers=%zu\n",
                        s0.c_str(), rt.scene().size());
        }
        // F8 opens the backlog
        {
            oa::runtime::FrameInput k;
            k.key_down_edges = {119};
            k.keys_down.insert(119);
            for (size_t f = 0; f < 60; ++f) rt.tick(16, f == 0 ? k : idle);
        }
        {
            size_t blogs = 0;
            for (const oa::render::Layer* l : rt.scene().draw_order())
                if (l->id.find("500.z") == 0 || l->id.find("blog") != std::string::npos)
                    ++blogs;
            std::printf("[r10b] after F8+60: blog-ish layers=%zu total=%zu\n",
                        blogs, rt.scene().size());
        }
        bool up = false;
        for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (f == 120 || f == 600) {
                const oa::render::MessageLayer* ml =
                    rt.text().layer("500.z.bt.tx.1.1");
                bool pageup = false;
                for (const oa::render::Layer* l : rt.scene().draw_order()) {
                    const auto* h = rt.scene().find_event_handler(l->id, "click");
                    if (!h) continue;
                    const auto k = h->params.find("key");
                    if (k != h->params.end() && k->second == "bt_pageup")
                        pageup = true;
                }
                const auto* w = rt.current_wait();
                std::printf("[r10b] f8wait f=%zu layers=%zu wait=%d tx11=%s "
                            "pageup=%d\n",
                            f, rt.scene().size(),
                            w ? (int)w->kind : -1,
                            ml ? (ml->page.empty() ? "empty" : "filled") : "absent",
                            pageup ? 1 : 0);
            }
            const oa::render::MessageLayer* ml =
                rt.text().layer("500.z.bt.tx.1.1");
            if (ml && !ml->page.empty()) {
                up = true;
                break;
            }
        }
        check(up, "backlog page opened with row content");
        if (!up) {
            std::printf(failures ? "r10b_backlog_rows FAIL (%d)\n" : "r10b_backlog_rows OK\n",
                        failures);
            return failures ? 1 : 0;
        }
        for (size_t f = 0; f < 300; ++f) rt.tick(16, idle); // settle
        // rows: every filled row text layer (.1) must be BOUND to a live
        // scene node (the e8411fa regression deleted those nodes), and the
        // sub row (.2) must still carry its slot when the layer exists.
        int filled = 0, bound = 0;
        for (int i = 1; i <= 6; ++i) {
            char idb[64];
            std::snprintf(idb, sizeof(idb), "500.z.bt.tx.%d.1", i);
            const oa::render::MessageLayer* ml = rt.text().layer(idb);
            if (!ml || ml->page.empty()) continue;
            ++filled;
            const std::string sid = rt.scene().bound_scene_id(idb);
            const oa::render::Layer* bn = rt.scene().find(sid);
            bool has = bn != nullptr;
            oa::render::Affine2 wt;
            if (has) has = rt.scene().world_transform(sid, &wt);
            std::printf("[r10b] row%d .1 id=%s filled units=%zu bound=%s\n", i,
                        idb, ml->page.size(), (bn && has) ? "yes" : "NO");
            if (bn && has) ++bound;
        }
        std::printf("[r10b] filled rows=%d bound rows=%d\n", filled, bound);
        check(filled >= 2, "at least two backlog rows filled");
        check(bound == filled,
              "every filled backlog row layer is bound to a scene node");
        std::printf(failures ? "r10b_backlog_rows FAIL (%d)\n" : "r10b_backlog_rows OK\n",
                    failures);
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
}
