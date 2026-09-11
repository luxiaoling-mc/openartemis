// M5: GameRuntime frame test — real fpm/root.pfs boots inside the runtime
// wait-state machine. Stable invariants for the headless stage: hundreds of
// frames with and without input run without Lua errors; waits fire; physical
// clicks never release [stop]-class waits (decide edges do). Env-gated.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

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
        oa::runtime::FrameInput no_input;
        oa::runtime::FrameInput click;
        click.left_click_edge = true;

        size_t wait_events = 0;
        int stop_waits = 0;
        int last_kind = -99;
        std::string pos_summary;
        for (int i = 0; i < 500 && !rt.exit_requested(); ++i) {
            rt.tick(16, no_input);
            const oa::runtime::WaitReason* w = rt.current_wait();
            if (w) {
                ++wait_events;
                if (w->kind == oa::runtime::WaitReason::Kind::Stop) ++stop_waits;
            }
            const int k = w ? (int)w->kind : -1;
            if (k != last_kind) {
                last_kind = k;
                pos_summary += "[" + std::to_string(i) + ":" + std::to_string(k) + "@" +
                               (rt.interpreter().current_script()
                                    ? std::string(*rt.interpreter().current_script())
                                    : std::string("(none)")) +
                               ":" + std::to_string(rt.interpreter().current_line()) + "] ";
            }
        }
        // physical clicks must not release a plain [stop] (reason empty).
        // Stop{reason:"trans"} waits (P2a) release on the transition timeline:
        // in the headless runtime nothing renders, so a trans wait resolves on
        // the next tick without input (visual-host model, research/09 §10.4).
        bool released_plain_stop_by_click = false;
        for (int i = 0; i < 10; ++i) {
            const oa::runtime::WaitReason* before = rt.current_wait();
            const bool plain_stop_before =
                before && before->kind == oa::runtime::WaitReason::Kind::Stop &&
                before->id.empty();
            rt.tick(16, click);
            if (plain_stop_before && !rt.waiting_stop()) released_plain_stop_by_click = true;
        }
        // decide edges keep the machine alive without errors
        for (int i = 0; i < 120; ++i) {
            if (i % 30 == 0) rt.apply_override(124, 32);
            if (i % 30 == 15) rt.apply_override(124, 0);
            rt.tick(16, no_input);
            if (rt.exit_requested()) break;
        }

        check(!rt.exit_requested(), "never requested exit");
        check(wait_events >= 20, "many wait events over 500 idle frames");
        // A [stop] may be skipped when a queued jump overrides the position
        // before the stop row executes (queue semantics); when one
        // parks us, physical clicks must never release it.
        if (stop_waits > 0)
            check(!released_plain_stop_by_click, "clicks never release plain stop waits");
        check(rt.interpreter().current_script() != nullptr, "interpreter positioned");
        std::printf("[rt] idle transitions: %s\n", pos_summary.c_str());
        std::printf("[rt] waits=%zu stops=%d final=%s:%zu\n", wait_events, stop_waits,
                    rt.interpreter().current_script()
                        ? rt.interpreter().current_script()->c_str()
                        : "(none)",
                    rt.interpreter().current_line());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "runtime_frames_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("runtime_frames_test: all ok\n");
    return 0;
}
