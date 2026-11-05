// test5 (B42 prologue): after the opening recap montage the reported input
// sequence "right click, then left click" left the story unable to advance.
//
// Root cause this guards: a click handler that queues tags while the story is
// parked (here the right-click message-off path jumps into system/ui.asb and
// returns) needs a return anchor. Without it the ui script's [return] ends
// with an empty call stack and the story never resumes.
//
// The test walks the opening like a player: click the title start, and for
// each story page perform right click -> left click -> try to advance. A
// montage segment has no text for a while, so a script-position change counts
// as progress as well.
//
// Env: OA_TEST_T5_PFS = test5 root.pfs (self-skips with 77 when unset).
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime_game.h"
#include "host_input.h"

namespace {

int failures = 0;
void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

std::string where(oa::runtime::GameRuntime& rt) {
    const std::string* s = rt.interpreter().current_script();
    return (s ? *s : std::string("-")) + ":" +
           std::to_string(rt.interpreter().current_line());
}

std::string page_text(oa::runtime::GameRuntime& rt) {
    const auto* ml = oa::test::story_layer(rt);
    return ml ? oa::test::page_text(ml) : std::string();
}

/// Wait for the reveal to finish, then right click -> left click -> try to
/// advance. Returns false when the story stopped responding.
bool survives_rmb_lmb(oa::runtime::GameRuntime& rt, const std::string& text) {
    for (int i = 0; i < 200 && !oa::test::reveal_done(rt); ++i) oa::test::frame(rt);
    const std::string before = where(rt);
    oa::test::right_click(rt);
    for (int i = 0; i < 40 && !rt.exit_requested(); ++i) oa::test::frame(rt);
    if (const auto p = oa::test::screen_click_point(rt))
        oa::test::click_at(rt, p->first, p->second);
    for (int i = 0; i < 40 && !rt.exit_requested(); ++i) oa::test::frame(rt);

    int moved = 0;
    for (int i = 0; i < 900 && !rt.exit_requested(); ++i) {
        oa::test::frame(rt);
        const std::string now = page_text(rt);
        if (!now.empty() && now != text) return true;
        if (where(rt) != before && ++moved >= 3) return true; // montage / cut
        if (oa::test::is_page_wait(rt.current_wait()) && oa::test::reveal_done(rt) &&
            i % 15 == 14) {
            if (const auto p = oa::test::screen_click_point(rt))
                oa::test::click_at(rt, p->first, p->second);
        }
    }
    return false;
}

} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_T5_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_T5_PFS unset; skipping\n");
        return 77;
    }
    try {
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        rt.boot_project();

        // Title: B42 uses btn01 (はじめから); bt_start covers the other builds.
        bool started = false;
        for (int i = 0; i < 4000 && !rt.exit_requested() && !started; ++i) {
            oa::test::frame(rt);
            auto p = oa::test::row_click_point(rt, "bt_start");
            if (!p) p = oa::test::row_click_point(rt, "btn01");
            if (p) {
                oa::test::click_at(rt, p->first, p->second);
                started = true;
            }
        }
        check(started, "title start button reachable");
        if (!started) return 1;

        int pages = 0;
        std::string last;
        for (int f = 0; f < 200000 && !rt.exit_requested() && pages < 40; ++f) {
            oa::test::frame(rt);
            const std::string now = page_text(rt);
            if (now.empty() || now == last) {
                const auto* w = rt.current_wait();
                if (oa::test::is_page_wait(w) && oa::test::reveal_done(rt)) {
                    if (const auto p = oa::test::screen_click_point(rt))
                        oa::test::click_at(rt, p->first, p->second);
                }
                continue;
            }
            last = now;
            ++pages;
            if (pages > 40) break;
            std::printf("[t5] page %d '%s'\n", pages, now.substr(0, 16).c_str());
            if (!survives_rmb_lmb(rt, now)) {
                check(false, "page " + std::to_string(pages) + " advances after right+left click ('" +
                                 now.substr(0, 20) + "')");
                break;
            }
        }
        check(pages >= 3, "reached the opening pages");
        std::printf(failures ? "t5_montage_input FAIL (%d)\n" : "t5_montage_input OK\n", failures);
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
}
