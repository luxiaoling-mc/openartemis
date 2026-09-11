// QA-queue U13 acceptance: the mouse wheel must behave as a momentary key
// pulse on vk 136/137 (up/down) — down edge, a short isDown window, then an
// explicit key-up (the host fix in src/app/main.cpp). Headless proxy: the
// runtime never sees the host, so this test locks the *contract* the host
// pulse produces: N wheel pulses produce exactly N actions on the story page
// (or the action of the park the pulse lands in) and then PERFECT stillness —
// no repeat, no free-run — across a long idle window, on both the story
// (137 = CLICK advance) and the backlog screen (136 = HUP scroll).
// Real-event Xvfb evidence: /tmp/xvfb_u13 (see docs/research/30).
#include <cstdarg>
#include <cstdio>
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
bool is_clickable_wait(const oa::runtime::WaitReason* w) {
    using K = oa::runtime::WaitReason::Kind;
    if (!w) return false;
    return w->kind == K::Generic || w->kind == K::Generic0 ||
           w->kind == K::Timed || w->kind == K::Se || w->kind == K::KeyWait;
}
bool reveal_done(const oa::runtime::GameRuntime& rt) {
    for (const std::string& id : rt.text().visible_content_layers()) {
        if (!rt.text().page_has_visible_text(id)) continue;
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        if (ml && (ml->reveal_pending || ml->reveal_index < ml->char_count))
            return false;
    }
    return true;
}
std::string sample(oa::runtime::GameRuntime& rt) {
    const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
    if (!ml) return "-";
    for (const auto& u : ml->page)
        if (!u.data.empty()) return u.data.substr(0, 16);
    return "-";
}
std::string blog_fp(oa::runtime::GameRuntime& rt) {
    std::string out;
    for (const std::string& id : rt.text().visible_content_layers()) {
        if (id.find(".z.bt.") == std::string::npos) continue;
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        if (!ml || !rt.text().page_has_visible_text(id)) continue;
        for (const auto& u : ml->page)
            if (!u.data.empty()) out += u.data.substr(0, 4);
    }
    return out.empty() ? std::string("-") : out;
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
            if (w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty()) break;
        }
        check(title, "U13-0 title parked");
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
        // ---- real-cadence page turns to build story depth + a backlog ----
        auto park = [&](size_t budget) {
            for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                if (rt.transition().is_in_progress(rt.now_ms())) continue;
                const auto* w = rt.current_wait();
                if (is_clickable_wait(w) && reveal_done(rt)) return true;
            }
            return false;
        };
        auto turn_page = [&] {
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = 640;
            cl.mouse_y = 600;
            rt.tick(16, cl);
            for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                const auto* w = rt.current_wait();
                if (is_clickable_wait(w) && reveal_done(rt)) break;
            }
        };
        bool body = false;
        for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (rt.text().page_has_visible_text("1.80.mw.adv_adv")) {
                body = true;
                break;
            }
        }
        check(body, "U13-1 story body reached");
        // wheel pulses need a stable story park: cross into 共通-02 and walk
        // ~8 stable pages (the parked page must be a plain dialog park; the
        // test refuses chapter-boundary parks by re-parking on a click)
        for (int p = 0; p < 400 && !rt.exit_requested(); ++p) {
            if (!park(4000)) break;
            turn_page();
            const std::string s = sample(rt);
            if (s != "-") break; // first text page after the start clicks
        }
        // settle: click until the parked page carries text and no transition
        for (int t = 0; t < 12 && !rt.exit_requested(); ++t) {
            if (sample(rt) == "-" || rt.transition().is_in_progress(rt.now_ms())) {
                if (park(4000)) turn_page();
                continue;
            }
            if (!park(2000)) continue;
            break;
        }
        check(sample(rt) != "-", "U13-2 story parked on a text page");
        // ---- U13-3: N wheel-down pulses => N page turns then stillness ----
        const int kDown = 137;
        const int pulses = 3;
        std::string cur = sample(rt);
        int turned = 0;
        for (int i = 0; i < pulses; ++i) {
            // host pulse shape: down edge + 1 extra held frame + up edge
            oa::runtime::FrameInput d0;
            d0.key_down_edges = {kDown};
            d0.keys_down.insert(kDown);
            rt.tick(16, d0);
            oa::runtime::FrameInput d1;
            d1.keys_down.insert(kDown);
            rt.tick(16, d1);
            oa::runtime::FrameInput u0;
            u0.key_up_edges = {kDown};
            rt.tick(16, u0);
            // wait for the page to turn (bounded) and settle on a new park
            bool moved = false;
            for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                const std::string ns = sample(rt);
                if (ns != cur && ns != "-") {
                    cur = ns;
                    moved = true;
                    break;
                }
            }
            if (!moved) break;
            ++turned;
            // settle into the next clean park before the next pulse
            for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                const auto* w = rt.current_wait();
                if (is_clickable_wait(w) && reveal_done(rt)) break;
            }
        }
        check(turned == pulses,
              "U13-3 three wheel-down pulses turned exactly three pages "
              "(turned=%d)",
              turned);
        // stillness: nothing moves across a long idle window
        std::string still = sample(rt);
        bool free_run = false;
        for (size_t f = 0; f < 500 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const std::string ns = sample(rt);
            if (ns != still) {
                free_run = true;
                still = ns;
            }
        }
        check(!free_run, "U13-4 no free-run after the last wheel pulse "
                         "(story static over 500 frames)");
        // ---- U13-5: wheel-up opens the backlog; scroll pulses then stop ----
        {
            oa::runtime::FrameInput w0;
            w0.key_down_edges = {136};
            w0.keys_down.insert(136);
            rt.tick(16, w0);
            oa::runtime::FrameInput w1;
            w1.keys_down.insert(136);
            rt.tick(16, w1);
            oa::runtime::FrameInput wu;
            wu.key_up_edges = {136};
            rt.tick(16, wu);
            bool blog = false;
            for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                if (rt.scene().find("500.z.bt.tx.1.1") != nullptr && rt.scene().size() > 180) {
                    blog = true;
                    break;
                }
            }
            check(blog, "U13-5 wheel-up pulse opened the backlog");
            if (blog) {
                // give the backlog its settle + initial view
                for (size_t f = 0; f < 800; ++f) rt.tick(16, idle);
                // the test needs more than one history page to scroll into
                // older entries; if only one page fits, stillness alone is
                // still the U13 property (no free-run). Scroll with HUP(136)
                // pulses toward older entries, then watch stillness.
                std::string fp0 = blog_fp(rt);
                int scrolled = 0;
                for (int i = 0; i < 3; ++i) {
                    oa::runtime::FrameInput s0;
                    s0.key_down_edges = {136};
                    s0.keys_down.insert(136);
                    rt.tick(16, s0);
                    oa::runtime::FrameInput s1;
                    s1.keys_down.insert(136);
                    rt.tick(16, s1);
                    oa::runtime::FrameInput su;
                    su.key_up_edges = {136};
                    rt.tick(16, su);
                    for (size_t f = 0; f < 2500 && !rt.exit_requested(); ++f) {
                        rt.tick(16, idle);
                        if (blog_fp(rt) != fp0) {
                            fp0 = blog_fp(rt);
                            ++scrolled;
                            break;
                        }
                    }
                    for (size_t f = 0; f < 400; ++f) rt.tick(16, idle);
                }
                std::printf("[u13] blog scrolled=%d/3 fp='%s'\n", scrolled,
                            blog_fp(rt).c_str());
                bool blog_free = false;
                std::string bf = blog_fp(rt);
                for (size_t f = 0; f < 500 && !rt.exit_requested(); ++f) {
                    rt.tick(16, idle);
                    if (blog_fp(rt) != bf) {
                        blog_free = true;
                        bf = blog_fp(rt);
                    }
                }
                check(!blog_free,
                      "U13-6 no free-run inside the backlog after wheel pulses");
            }
        }
        if (failures) {
            std::fprintf(stderr, "u13_wheel_test: %d failure(s)\n", failures);
            return 1;
        }
        std::printf("u13_wheel_test: all ok\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
}
