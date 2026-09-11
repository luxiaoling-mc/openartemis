// QA-queue U3 (global save verdict) + P2 (load UI closure), real fpm:
// drive the actual save/load screens with a real DirSaveStore.
//
// U3: FPM's "global save" is the g./s. engine domain (sys/gscr/conf pluto
// tables -> g.* variables) persisted to saveg.dat/system.dat by syssave,
// which the engine appends to every numbered save (research/17 S5). This
// test proves the real UI chain: clicking a slot on the F6 save screen
// writes the numbered save AND the global files (saveg.dat carries the
// config/script/system tables), and a fresh runtime booting the same store
// sysloads them back (g.script present).
//
// P2: the total-acceptance leftover "F7 load click never prints loaded"
// (research/21 §3c) — the load slot click opens a CONFIRM DIALOG
// (csv.dlg.load def=0) that the old acceptance runs never answered. With
// the dialog confirmed the engine [load] fires and the story resumes at the
// saved page: page sample restored, story wait park, layer count back to
// the story range.
#include <cstdarg> // va_list/va_start (was pulled in transitively via media/players.h)
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_save.h"
#include "core/runtime/runtime_save.h"

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
/// Click the center of the scene layer whose click row carries `key`.
bool click_key(oa::runtime::GameRuntime& rt, const std::string& key) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end() || k->second != key) continue;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? double(l->width) : 404.0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? double(l->height) : 120.0);
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
        return true;
    }
    return false;
}
bool reach_title(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
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
bool reach_story_page(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.text().page_has_visible_text("1.80.mw.adv_adv")) break;
    }
    for (int t = 0; t < 6 && !rt.exit_requested(); ++t) {
        if (!park_story(rt, idle, 2500)) break;
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = 640;
        cl.mouse_y = 600;
        rt.tick(16, cl);
        (void)park_story(rt, idle, 2500);
    }
    return park_story(rt, idle, 2500);
}
void press_tick(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle, int key) {
    oa::runtime::FrameInput k;
    k.key_down_edges = {key};
    k.keys_down.insert(key);
    rt.tick(16, k);
    for (size_t f = 1; f < 40 && !rt.exit_requested(); ++f) rt.tick(16, idle);
}
/// Wait for a UI screen to finish opening: its button node present while a
/// wait park holds, then `settle` extra idle frames (the FPM uiopenanime
/// slides the window in — clicking mid-animation hits the wrong slot).
bool settle_screen(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                   const char* node, size_t settle = 300) {
    size_t held = 0;
    for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        if (w && rt.scene().find(node) != nullptr) {
            if (++held > settle) return true;
        } else {
            held = 0;
        }
    }
    return false;
}
/// Wait for the yes/no dialog to finish opening, then press Enter (YES).
void confirm_dialog(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    size_t held = 0;
    for (size_t f = 0; f < 6000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        if (w && rt.scene().find("600.1.bt.1.0") != nullptr) {
            if (++held > 120) break;
        } else {
            held = 0;
        }
    }
    oa::runtime::FrameInput en;
    en.key_down_edges = {13};
    en.keys_down.insert(13);
    for (size_t f = 0; f < 60 && !rt.exit_requested(); ++f) rt.tick(16, f == 0 ? en : idle);
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_FPM_PFS unset; skipping\n");
        return 77;
    }
    // Windows port: mkdtemp("/tmp/...") is POSIX-only — use the system temp
    // dir + pid (same shape as misc_b_test) so the real-fpm save/load chain
    // runs on MSVC too.
    namespace fsf = std::filesystem;
    std::error_code ec;
    const fsf::path store_dir =
        fsf::temp_directory_path(ec) / ("oa_saveload_" + std::to_string(::getpid()));
    fsf::remove_all(store_dir, ec);
    auto store = std::make_shared<oa::runtime::DirSaveStore>(store_dir.string());

        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    check(reach_title(rt, idle), "P2-0 title parked");
    start_game(rt, idle);
    check(reach_story_page(rt, idle), "P2-1 story body parked");
    const std::string s0 = sample(rt);
    check(s0 != "-" && !s0.empty(), "P2-2 saved page sample captured");
    std::printf("[sl] park sample='%s' wait=%s layers=%zu\n", s0.c_str(), ws(rt.current_wait()),
                rt.scene().size());

    // ---- U3: F6 save screen -> slot 1 (new save, csv dlg.save def=1 auto) --
    press_tick(rt, idle, 117); // F6 = SAVE
    const bool save_screen =
        settle_screen(rt, idle, "500.bt.1.bg.0", 300) && rt.scene().size() > 300;
    check(save_screen, "U3-1 F6 opened the save screen (settled)");
    check(click_key(rt, "bt_save01"), "U3-2 save slot 1 row present and clicked");
    // wait for the numbered save + global files to land in the store, then
    // let the save flow finish (savenext -> saveload_reload refresh) before
    // closing the screen — Esc during the save tail is swallowed by the flow.
    bool slot_file = false, saveg = false, systemd = false;
    size_t done = 0;
    for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        slot_file = store->exists("savedata_cn/save0001.dat");
        saveg = store->exists("savedata_cn/saveg.dat");
        systemd = store->exists("savedata_cn/system.dat");
        if (slot_file && saveg && systemd && ++done > 400) break;
    }
    check(slot_file, "U3-3 UI slot save wrote the numbered save file");
    check(saveg && systemd, "U3-4 UI slot save wrote the global files (saveg/system)");
    if (saveg) {
        // FPM's global tables ride the g. domain: saveg.dat = one binary
        // domain-map document (research/64) with the config/script/system
        // pluto tables (research/17 §0, §3).
        const auto bytes = store->read("savedata_cn/saveg.dat");
        bool keys_ok = false;
        if (bytes) {
            try {
                const auto map = oa::runtime::decode_domain_map(
                    std::string(bytes->begin(), bytes->end()));
                keys_ok = map.count("config") == 1 && map.count("script") == 1 &&
                          map.count("system") == 1;
            } catch (...) {
                keys_ok = false;
            }
        }
        check(keys_ok, "U3-5 saveg.dat carries the FPM global tables "
                       "(config/script/system)");
    }
    // close the save screen (Esc = ui EXIT), then settle back at the story
    {
        oa::runtime::FrameInput esc;
        esc.key_down_edges = {27};
        esc.keys_down.insert(27);
        rt.tick(16, esc);
    }
    bool closed = false;
    for (size_t f = 0; f < 9000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        if (rt.scene().find("500.bt.1.bg.0") == nullptr && rt.scene().size() < 200 &&
            is_story_wait(w)) {
            closed = true;
            break;
        }
    }
    check(closed, "P2-3 save screen closed and the story wait returned");
    // ---- P2: advance the story past the saved page ----
    std::string s1 = s0;
    bool advanced = false;
    for (int t = 0; t < 8 && !rt.exit_requested(); ++t) {
        if (!park_story(rt, idle, 2500)) break;
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = 640;
        cl.mouse_y = 600;
        rt.tick(16, cl);
        bool moved = false;
        for (size_t f = 0; f < 2500 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const std::string nv = sample(rt);
            if (nv != s0) {
                s1 = nv;
                moved = true;
                break;
            }
            const auto* w = rt.current_wait();
            const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
            if (is_story_wait(w) && ml && ml->reveal_index >= ml->char_count &&
                !ml->reveal_pending && f > 2)
                break;
        }
        if (moved) {
            advanced = true;
            break;
        }
    }
    check(advanced, "P2-4 story advanced past the saved page (sample changed)");
    std::printf("[sl] advanced sample='%s'\n", s1.c_str());
    // ---- P2: F7 load screen -> slot 1 -> CONFIRM dialog -> Enter ----
    // let the page-turn cascade settle back at the [@] park before F7
    // (the sample change fires mid-cascade; F7 during the timed estag rows
    // is swallowed by the framework's per-page key state).
    check(park_story(rt, idle, 5000), "P2-4b story re-parked after the advance");
    press_tick(rt, idle, 118); // F7 = LOAD
    const bool load_screen =
        settle_screen(rt, idle, "500.bt.1.bg.0", 300) && rt.scene().size() > 300;
    check(load_screen, "P2-5 F7 opened the load screen (settled)");
    check(click_key(rt, "bt_save01"), "P2-6 load slot 1 row present and clicked");
    bool dlg = false;
    for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.scene().find("600.1.bt.1.0") != nullptr) {
            dlg = true;
            break;
        }
    }
    check(dlg, "P2-7 load confirmation dialog opened (csv.dlg.load def=0)");
    // confirm (Enter = CLICK on the auto-cursor YES)
    bool restored = false;
    {
        confirm_dialog(rt, idle);
        for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const auto* w = rt.current_wait();
            const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
            if (sample(rt) == s0 && rt.scene().find("600.1.bt.1.0") == nullptr &&
                rt.scene().size() < 200 && is_story_wait(w) && ml &&
                ml->reveal_index >= ml->char_count && !ml->reveal_pending) {
                restored = true;
                break;
            }
        }
    }
    check(restored,
          "P2-8 load UI closure: story restored to the saved page (sample back, "
          "parked, layers back)");
    std::printf("[sl] post-load sample='%s' wait=%s layers=%zu script=%s:%zu\n",
                sample(rt).c_str(), ws(rt.current_wait()), rt.scene().size(),
                rt.interpreter().current_script()
                    ? rt.interpreter().current_script()->c_str()
                    : "-",
                rt.interpreter().current_line());

    // ---- U3: cross-restart global round trip (boot #2, same store) ----
    {
                auto fs2 = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rt2(fs2);
        rt2.set_save_store(store);
        rt2.open_project("windows");
        rt2.boot_project();
        // sysload at boot restored the persisted g. domain; the FPM boot
        // then read it (dataloading) into its own tables. The engine-side
        // marker: the g.script variable exists in the interpreter store.
        const bool gscript =
            rt2.interpreter().variables().get("g.script").has_value() ||
            rt2.interpreter().variables().get("g.system").has_value();
        check(gscript, "U3-6 restart sysload restored the FPM global domain "
                       "(g.script/g.system present)");
    }
    std::error_code ec2;
    fsf::remove_all(store_dir, ec2);
    if (failures) {
        std::fprintf(stderr, "save_load_ui_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("save_load_ui_test: all ok\n");
    return 0;
}
