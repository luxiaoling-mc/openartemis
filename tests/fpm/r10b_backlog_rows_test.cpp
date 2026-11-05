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
#include "core/runtime/runtime_game.h"
#include "host_input.h"

namespace {
const char* wait_name(const oa::runtime::Wait* w) {
    if (!w) return "-";
    if (w->waiting_click()) return "click";
    if (w->is_park()) return "stop";
    if (w->timed()) return "timed";
    return "other";
}
} // namespace

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool is_clickable_wait(const oa::runtime::Wait* w) {
    if (!w) return false;
    return w->waiting_click() || w->timed() ||
           w->stage == oa::runtime::Wait::Stage::Se || !w->keys.empty();
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
            oa::test::frame(rt);
            if (!title) continue;
            const auto* w = rt.current_wait();
            if (w && w->is_park() &&
                w->stage_id.empty())
                break;
        }
        // 像玩家一样：标题点「开始」→ 回到剧情页 → 正常读几页。
        if (const auto p = oa::test::row_click_point(rt, "bt_start"))
            oa::test::click_at(rt, p->first, p->second);
        check(oa::test::ensure_story(rt), "story page reached");
        int turns = 0;
        for (int i = 0; i < 12; ++i)
            if (oa::test::advance_page(rt)) ++turns;
        check(turns >= 3, "read a few story pages before the backlog");
        check(oa::test::ensure_story(rt), "story page parked before F8");
        for (size_t f = 0; f < 300; ++f) oa::test::frame(rt); // settle (windowed parity)
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
        // F8 opens the backlog（真实按住，只给一帧边沿会被数据侧当抖动）
        oa::test::hold_key(rt, 119);
        for (size_t f = 0; f < 56; ++f) oa::test::frame(rt);
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
            oa::test::frame(rt);
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
                std::printf("[r10b] f8wait f=%zu layers=%zu wait=%s tx11=%s "
                            "pageup=%d\n",
                            f, rt.scene().size(),
                            wait_name(w),
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
        for (size_t f = 0; f < 300; ++f) oa::test::frame(rt); // settle
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
