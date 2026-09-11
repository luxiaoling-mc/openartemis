// M4c: headless FPM boot driver — runs the real fpm/root.pfs boot chain
// (system/first.iet: system_initlua -> includes -> dataloading -> initialize
// -> starting -> game_start -> title_init -> *title [stop]). Env-gated.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "core/fs/physfs_fs.h"
#include "core/fs/project.h"
#include "core/runtime/runtime_iet.h"

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
    size_t steps = 0;
    size_t waits = 0;
    bool saw_initlua = false;
    bool saw_starting = false;
    bool saw_title_init = false;
    bool stop_after_title = false;
    std::vector<std::string> unknown_tags;
    std::string interim_error;
    try {
                oa::fs::PhysFileSystem fs(pfs_path, false);
        const auto project = oa::fs::Project::open(fs, "windows");

        oa::runtime::Interpreter::Config cfg;
        cfg.charset = project.config.charset;
        cfg.platform = project.config.platform;
        cfg.stage_width = project.config.stage_width;
        cfg.stage_height = project.config.stage_height;
        cfg.fps = project.config.fps;
        cfg.env = project.config.env;
        oa::runtime::Interpreter it(cfg);
        it.variables().platform = cfg.platform;
        it.set_variable("s.savepath", oa::runtime::Value::make_string("save"));
        it.hooks().file_loader = [&fs, &it](const std::string& name)
            -> std::optional<std::vector<uint8_t>> {
            return fs.read(it.resolve_magic_path(name));
        };

        (void)0; // counters live at function scope so the catch can see them

        it.on_step = [&](const std::string& script, size_t /*line*/,
                         const oa::runtime::Instruction& ins) {
            (void)script;
            ++steps;
            if (const std::string* fn = ins.get("function")) {
                if (*fn == "system_initlua") saw_initlua = true;
                if (*fn == "system_starting") saw_starting = true;
                if (*fn == "title_init") saw_title_init = true;
            }
            if (ins.tag == "stop") {
                if (saw_title_init) stop_after_title = true;
            }
        };
        it.set_callback([&](const oa::runtime::Event& e) {
            if (e.kind == oa::runtime::Event::Kind::Custom) {
                bool seen = false;
                for (const auto& t : unknown_tags)
                    if (t == e.tag) seen = true;
                if (!seen) {
                    unknown_tags.push_back(e.tag);
                    std::printf("[boot] custom/unknown tag: %s\n", e.tag.c_str());
                }
                return oa::runtime::CallbackResult::Continue;
            }
            if (e.kind == oa::runtime::Event::Kind::Wait_) {
                ++waits;
                return oa::runtime::CallbackResult::Pause;
            }
            return oa::runtime::CallbackResult::Continue;
        });

        it.boot(project.config.boot_script);
        std::printf("[boot] charset=%s stage=%dx%d\n", cfg.charset.c_str(),
                    cfg.stage_width, cfg.stage_height);
        for (;;) {
            const oa::runtime::ExecutionResult r = it.run();
            if (r == oa::runtime::ExecutionResult::Completed) break;
            it.next_line();
            if (steps > 500000) {
                std::printf("[boot] runaway step limit\n");
                break;
            }
        }
        std::printf("[boot] steps=%zu waits=%zu unknown=%zu pos=%s:%zu\n", steps, waits,
                    unknown_tags.size(),
                    it.current_script() ? it.current_script()->c_str() : "(none)",
                    it.current_line());
        // M4c interim pin: the whole include/dataload/initialize/starting chain
        // runs clean. Reaching the visible *title additionally needs the M5
        // native layer/text tags (lyc/print/... currently Custom no-ops).
        check(saw_initlua, "system_initlua ran");
        check(saw_starting, "system_starting ran");
        check(waits >= 5, "expected wait yields across the boot chain");
        std::printf("[boot] note: title_init=%s stop_after_title=%s (needs M5 native tags)\n",
                    saw_title_init ? "yes" : "no", stop_after_title ? "yes" : "no");
    } catch (const std::exception& e) {
        if (!saw_starting) {
            std::fprintf(stderr, "BOOT EXCEPTION before system_starting: %s\n", e.what());
            return 1;
        }
        // Interim M4c pin: the boot chain completes through system_starting;
        // later-stage Lua activity needs M5 native tags (currently no-ops).
        interim_error = e.what();
    }
    std::printf("[boot] steps=%zu waits=%zu unknown=%zu\n", steps, waits,
                unknown_tags.size());
    std::printf("[boot] note: title_init=%s stop_after_title=%s%s%s\n",
                saw_title_init ? "yes" : "no", stop_after_title ? "yes" : "no",
                interim_error.empty() ? "" : " interim_error=", interim_error.c_str());
    check(saw_initlua, "system_initlua ran");
    check(saw_starting, "system_starting ran");
    check(saw_title_init, "title_init ran (boot chain reached the title)");
    check(stop_after_title, "chain stopped at the *title [stop]");
    check(waits >= 5, "expected wait yields across the boot chain");
    if (failures) {
        std::fprintf(stderr, "boot_fpm_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("boot_fpm_test: all ok (interim M4c pin)\n");
    return 0;
}
