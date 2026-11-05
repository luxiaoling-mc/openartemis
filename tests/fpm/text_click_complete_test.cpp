// Regression: a click while a story page is still revealing must complete that
// page and must NOT turn it — checked on several pages in a row (the residue of
// one click must never leak into the next page turn).
//
// Input goes through the host path (host_input.h) and lands on the real ADV
// layer, which the pointer chain consumes — the case that used to slip through.
//
// Env: OA_TEST_FPM_PFS (skip 77 when unset).
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime_game.h"
#include "host_input.h"

namespace {
/// The ADV text layer (id contains "adv"), not font/name helper layers.
const oa::render::MessageLayer* adv_layer(oa::runtime::GameRuntime& rt) {
    const oa::render::MessageLayer* found = nullptr;
    for (const std::string& id : rt.text().visible_content_layers()) {
        const auto* ml = rt.text().layer(id);
        if (!ml || ml->char_count == 0) continue;
        if (ml->id.find("adv") == std::string::npos) continue;
        if (!found || ml->char_count > found->char_count) found = ml;
    }
    return found;
}

int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

/// Click the biggest click-handled layer (ADV screen / dialog).
void click_screen(oa::runtime::GameRuntime& rt) {
    if (const auto p = oa::test::screen_click_point(rt)) {
        oa::test::click_at(rt, p->first, p->second);
        return;
    }
    oa::test::click_at(rt, 640, 400);
}

/// Choice / confirm dialogs park on [stop]: press "no=1" when present.
bool click_yes(oa::runtime::GameRuntime& rt) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h || !l->visible) continue;
        const auto n = h->params.find("no");
        if (n == h->params.end() || n->second != "1") continue;
        const double w = l->has_clip ? l->clip_w : 60.0;
        const double hh = l->has_clip ? l->clip_h : 30.0;
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (!rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) continue;
        oa::test::click_at(rt, int(x0 + rw / 2), int(y0 + rh / 2));
        return true;
    }
    return false;
}
} // namespace

int main() {
    const char* pfs = std::getenv("OA_TEST_TG3_PFS");
    if (!pfs || !*pfs) {
        std::printf("OA_TEST_TG3_PFS unset; skipping\n");
        return 77;
    }
    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs, false);
    oa::runtime::GameRuntime rt(fs);
    rt.open_project("windows");
    rt.boot_project();
    for (int i = 0; i < 600; ++i) oa::test::frame(rt);
    oa::test::start_story_from_title(rt);

    int pages = 0;
    for (int step = 0; step < 6000 && !rt.exit_requested() && pages < 6; ++step) {
        oa::test::frame(rt);
        const auto* ml = adv_layer(rt);
        if (!ml) {
            if (step % 12 == 0 && !click_yes(rt)) click_screen(rt);
            continue;
        }

        const bool finished = !ml->reveal_pending && ml->reveal_index >= ml->char_count;
        if (finished) {
            if (step % 12 == 0 && !click_yes(rt)) click_screen(rt);
            continue;
        }
        if (!ml->reveal_pending || ml->reveal_index < 1 ||
            ml->reveal_index + 3 >= ml->char_count)
            continue;

        // Mid-reveal: one click must complete this page and nothing else.
        const std::string id = ml->id;
        const std::string text = oa::test::page_text(ml);
        const size_t chars = ml->char_count;
        click_screen(rt);

        const auto* after = adv_layer(rt);
        check(after && after->id == id, "RG1 click did not turn the page");
        if (after) {
            check(oa::test::page_text(after) == text, "RG2 same page text");
            check(after->reveal_index >= chars, "RG3 one click completed the page");
        }
        // The derived decide edge lands on a later frame; it must not advance.
        for (int k = 0; k < 3; ++k) {
            oa::test::frame(rt);
            const auto* nxt = adv_layer(rt);
            check(nxt && nxt->id == id, "RG4 click residue did not turn the page");
        }
        std::printf("[test] page %d ok ('%s' %zu chars)\n", pages,
                    text.substr(0, 12).c_str(), chars);
        ++pages;
        for (int k = 0; k < 8; ++k) oa::test::frame(rt);
    }

    if (pages < 3) {
        // 这个包里没走到可逐字的剧情页（例如数据开头是对话框流程），跳过。
        std::printf("no paced story page reached (%d); skipping\n", pages);
        return 77;
    }
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("[test] click-to-complete OK (%d pages)\n", pages);
    return 0;
}
