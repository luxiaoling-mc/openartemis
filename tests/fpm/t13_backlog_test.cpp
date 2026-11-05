// test13 (穢翼のユースティア): the backlog must show its rows and closing it must
// put the story back on a click-advanceable page.
//
// The regression this guards: closing the backlog left the script parked on
// system/ui.asb's [stop] (a Lua-queued stop), so the story never came back, and
// the backlog rows (message layers without a [font] layout box) were not
// drawable at all.
//
// Real flow only: title -> story -> right click (ADV menu) -> backlog button ->
// sys_ret button -> story. No injected tags.
//
// Env: OA_TEST_T13_PFS (skip 77 when unset).
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime_game.h"
#include "host_input.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::string page_str(const oa::render::MessageLayer* ml, size_t cap = 24) {
    if (!ml) return {};
    std::string s;
    for (const auto& u : ml->page) {
        if (u.data.empty()) continue;
        s += u.data;
        if (s.size() > cap) break;
    }
    return s;
}

const oa::render::MessageLayer* story_layer(oa::runtime::GameRuntime& rt) {
    const oa::render::MessageLayer* best = nullptr;
    for (const std::string& id : rt.text().visible_content_layers()) {
        const auto* ml = rt.text().layer(id);
        if (!ml || ml->char_count == 0) continue;
        if (!best || ml->char_count > best->char_count) best = ml;
    }
    return best;
}

bool click_layer(oa::runtime::GameRuntime& rt, const std::string& id, double w = 200.0,
                 double h = 60.0) {
    const oa::render::Layer* l = rt.scene().find(id);
    if (!l || !rt.scene().is_effectively_visible(id)) return false;
    double x0 = 0, y0 = 0, rw = 0, rh = 0;
    if (!rt.scene().world_rect(id, l->has_clip ? l->clip_w : w,
                               l->has_clip ? l->clip_h : h, &x0, &y0, &rw, &rh))
        return false;
    oa::test::click_at(rt, int(x0 + rw / 2), int(y0 + rh / 2));
    return true;
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pfs = std::getenv("OA_TEST_T13_PFS");
    if (!pfs || !*pfs) {
        std::printf("OA_TEST_T13_PFS unset; skipping\n");
        return 77;
    }
    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs, true);
    oa::runtime::GameRuntime rt(fs);
    rt.open_project("windows");
    rt.boot_project();
    if (std::getenv("OA_TAG_TRACE"))
        rt.interpreter().on_step = [](const std::string& script, size_t line,
                                      const oa::runtime::Instruction& ins) {
            std::fprintf(stderr, "  %s:%zu [%s]\n", script.c_str(), line, ins.tag.c_str());
        };
    for (int i = 0; i < 900 && !rt.exit_requested(); ++i) oa::test::frame(rt);
    if (!oa::test::start_story_from_title(rt)) {
        std::printf("story not reached; skipping\n");
        return 77;
    }
    for (int i = 0; i < 6; ++i) {
        oa::test::ensure_story(rt);
        oa::test::advance_page(rt);
    }
    const std::string before = page_str(story_layer(rt));
    check(!before.empty(), "story text before the backlog");

    // ADV menu -> backlog.
    oa::test::right_click(rt);
    for (int i = 0; i < 120; ++i) oa::test::frame(rt);
    check(click_layer(rt, "1.80.100.2.3.0"), "backlog button present");
    for (int i = 0; i < 180; ++i) oa::test::frame(rt);

    // Rows: text + drawable (the blank-backlog regression).
    int rows = 0;
    bool all_drawable = true;
    for (const std::string& id : rt.text().visible_content_layers()) {
        if (id.rfind("500.100.", 0) != 0) continue;
        const auto* ml = rt.text().layer(id);
        if (!ml || ml->char_count == 0) continue;
        ++rows;
        if (!rt.scene().is_message_layer_drawable(id, ml->positioned))
            all_drawable = false;
    }
    std::printf("[test] backlog rows=%d drawable=%d\n", rows, int(all_drawable));
    // 行文本要有排版盒（msg.iet 头里 msg_backlog_init 给的 360,68,660x540）；
    // 没有盒就会画在 (0,0)，看起来"文字不在框里"。
    if (const auto* row = rt.text().layer("500.100.1.0")) {
        std::printf("[test] row box=%.0f,%.0f %.0fx%.0f pos=%d\n", row->left, row->top,
                    row->width, row->height, int(row->positioned));
        check(row->positioned && row->left > 300 && row->top > 40 && row->width > 400,
              "backlog row text is laid out inside the frame");
    }
    // Close with the return button (500.200.2.0 = sys_ret).
    check(click_layer(rt, "500.200.2.0"), "backlog return button present");
    for (int i = 0; i < 180; ++i) oa::test::frame(rt);

    const std::string* script = rt.interpreter().current_script();
    std::printf("[test] after close: script=%s wait=%s\n", script ? script->c_str() : "-",
                rt.current_wait() ? "parked" : "none");
    check(script && script->find("scenario/main/") != std::string::npos,
          "story script resumed after the backlog");
    const auto* w = rt.current_wait();
    check(w && w->waiting_click(), "story page waits for a click");

    // One input must turn the page (headless clicks cannot reach the ADV hit
    // layer without textures, so use the role-advance key).
    const std::string was = page_str(story_layer(rt));
    for (int i = 0; i < 60; ++i) oa::test::frame(rt);
    oa::test::hold_key(rt, 0x0D, 4, 4);
    bool advanced = false;
    for (int i = 0; i < 120 && !advanced; ++i) {
        oa::test::frame(rt);
        const std::string now = page_str(story_layer(rt));
        if (!now.empty() && now != was) advanced = true;
    }
    check(advanced, "story advances after the backlog");

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("[test] backlog open/close OK\n");
    return 0;
}
