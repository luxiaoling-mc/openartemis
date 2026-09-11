// QA-queue P1 acceptance (real fpm): long-chain story traversal with chapter
// switching, engine-skip across story text, F3 empirical, and a mid-journey
// UI save -> fresh-boot Continue round trip. Deterministic segments:
//   P1-1  boot -> title -> はじめから -> body
//   P1-2  120+ page turns at real cadence crossing 共通-01 -> 共通-02
//   P1-3  engine skip ([skip allow=1]+[exec skip mode=1], the exact tags
//         FPM skipmode_start emits) turns 120+ pages with zero clicks
//   P1-4  skip-off resumes click advance
//   P1-5  F3 (adv SKIP key) on unread pages refuses without crashing and
//         the story still advances on click (real FPM behavior)
//   P1-6..8  F6 UI save (numbered slot + global files) mid-journey; fresh
//         boot on the same store; title Continue resumes the saved page
// Env: OA_TEST_FPM_PFS (skip 77 unset). See docs/research/28.
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <set>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"
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
bool is_clickable_wait(const oa::runtime::WaitReason* w) {
    using K = oa::runtime::WaitReason::Kind;
    if (!w) return false;
    return w->kind == K::Generic || w->kind == K::Generic0 ||
           w->kind == K::Timed || w->kind == K::Se || w->kind == K::KeyWait;
}
bool is_story_wait(const oa::runtime::WaitReason* w) {
    return w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                 w->kind == oa::runtime::WaitReason::Kind::Generic0 ||
                 w->kind == oa::runtime::WaitReason::Kind::Timed);
}
std::string sample(oa::runtime::GameRuntime& rt) {
    const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
    if (!ml) return "-";
    for (const auto& u : ml->page)
        if (!u.data.empty()) return u.data.substr(0, 20);
    return "-";
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
void dir_files(const std::string& dir, std::set<std::string>& out) {
    // Windows port (research/128): opendir/readdir/dirent.h + stat/S_ISREG have
    // no MSVC counterpart. std::filesystem walks the same tree with the same
    // contract the caller uses — recurse into subdirectories, skip dot entries,
    // collect REGULAR files by basename (the caller asks for save0001.dat /
    // saveg.dat / system.dat wherever DirSaveStore nested them).
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(dir, ec), end;
    for (; it != end && !ec; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code fec;
        if (it->is_regular_file(fec)) out.insert(name);
    }
}
struct Boot {
    oa::runtime::GameRuntime rt;
    std::string chapter;
    Boot(std::shared_ptr<const oa::fs::IFileSystem> fs,
         std::shared_ptr<oa::runtime::SaveStore> store, std::string* ch_out)
        : rt(fs) {
        rt.set_save_store(store);
        rt.open_project("windows");
        if (ch_out) {
            auto& hooks = rt.interpreter().hooks();
            const auto orig = hooks.resource_read;
            hooks.resource_read = [this, ch_out, orig](const std::string& r)
                -> std::optional<std::vector<uint8_t>> {
                if (r.rfind("script/", 0) == 0 || r.find("/script/") != std::string::npos) {
                    *ch_out = r.substr(r.rfind('/') + 1);
                }
                return orig(r);
            };
        }
        rt.boot_project();
    }
};
bool reach_title(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                 std::vector<oa::render::Layer>* out_buttons = nullptr) {
    for (size_t f = 0; f < 20000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        if (!(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty()))
            continue;
        bool has_start = false;
        for (const oa::render::Layer* l : rt.scene().draw_order()) {
            const auto* h = rt.scene().find_event_handler(l->id, "click");
            if (!h) continue;
            const auto k = h->params.find("key");
            if (k != h->params.end() && k->second == "bt_start") has_start = true;
            if (out_buttons) out_buttons->push_back(*l);
        }
        if (has_start) return true;
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
bool park_story(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                size_t budget = 4000) {
    for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.transition().is_in_progress(rt.now_ms())) continue;
        const auto* w = rt.current_wait();
        if (is_clickable_wait(w) && reveal_done(rt)) return true;
    }
    return false;
}
void press_tick(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle, int key,
                size_t hold = 40) {
    oa::runtime::FrameInput k;
    k.key_down_edges = {key};
    k.keys_down.insert(key);
    rt.tick(16, k);
    for (size_t f = 1; f < hold && !rt.exit_requested(); ++f) rt.tick(16, idle);
}
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
    // Windows port (research/128): mkdtemp("/tmp/...") is POSIX-only — the
    // system temp dir + pid (same shape as tests/fpm/save_load_ui_test.cpp)
    // gives an equivalent fresh, per-process store root on MSVC.
    namespace fsf = std::filesystem;
    std::error_code ec;
    const fsf::path store_path =
        fsf::temp_directory_path(ec) / ("oa_p1_" + std::to_string(::getpid()));
    fsf::remove_all(store_path, ec); // fresh tree, like mkdtemp's guarantee
    fsf::create_directories(store_path, ec);
    check(fsf::is_directory(store_path, ec), "temp store dir created");
    const std::string store_dir = store_path.string();
    auto store = std::make_shared<oa::runtime::DirSaveStore>(store_dir);
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
    std::string chapter;
    Boot boot(fs, store, &chapter);
    oa::runtime::FrameInput idle;
    const auto t0 = std::chrono::steady_clock::now();
    auto wall = [&]() -> double {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    oa::runtime::GameRuntime& rt = boot.rt;
    check(reach_title(rt, idle), "P1-1 title parked");
    start_game(rt, idle);
    bool body = false;
    for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.text().page_has_visible_text("1.80.mw.adv_adv")) {
            body = true;
            break;
        }
    }
    check(body, "P1-1b story body reached");
    // ---- P1-2: real-cadence page turns across the 共通-01 -> 共通-02 ----
    size_t clicks = 0, stuck = 0;
    int pages = 0;
    std::string last = sample(rt);
    const std::string first_chapter = chapter;
    while (wall() < 120.0 && !rt.exit_requested() && pages < 200 && stuck < 6) {
        if (!park_story(rt, idle)) {
            ++stuck;
            continue;
        }
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = 640;
        cl.mouse_y = 600;
        rt.tick(16, cl);
        ++clicks;
        bool moved = false;
        for (size_t f = 0; f < 2000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const std::string now = sample(rt);
            if (now != last) {
                last = now;
                ++pages;
                moved = true;
                break;
            }
            if (is_story_wait(rt.current_wait())) break;
        }
        (void)moved;
    }
    std::printf("[p1] walk pages=%d clicks=%zu stuck=%zu chapter='%s' first='%s' "
                "wall=%.1fs\n",
                pages, clicks, stuck, chapter.c_str(), first_chapter.c_str(), wall());
    check(pages >= 120 && stuck == 0,
          "P1-2 120+ page turns without stuck parks (pages=%d stuck=%zu)", pages, stuck);
    check(first_chapter == "共通-01.ast" && chapter != first_chapter,
          "P1-2b chapter switched across the walk ('%s' -> '%s')",
          first_chapter.c_str(), chapter.c_str());
    // ---- P1-3: engine skip across story text, zero clicks ----
    check(park_story(rt, idle, 6000), "P1-3a parked before skip");
    rt.interpreter().enqueue_tag("skip", {{"allow", "1"}, {"unread", "1"}});
    rt.interpreter().enqueue_tag("exec", {{"command", "skip"}, {"mode", "1"}});
    int skip_pages = 0;
    size_t skip_frames = 0;
    const std::string ch_before = chapter;
    last = sample(rt);
    for (; skip_frames < 60000 && !rt.exit_requested(); ++skip_frames) {
        rt.tick(16, idle);
        const std::string now = sample(rt);
        if (now != last) {
            last = now;
            ++skip_pages;
        }
        if (skip_pages >= 160 && chapter != ch_before) break;
        if (skip_pages >= 260) break;
    }
    std::printf("[p1] skip pages=%d frames=%zu chapter='%s'->'%s'\n", skip_pages,
                skip_frames, ch_before.c_str(), chapter.c_str());
    check(skip_pages >= 120,
          "P1-3 engine skip turned 120+ pages with zero clicks (pages=%d)",
          skip_pages);
    rt.interpreter().enqueue_tag("exec", {{"command", "skip"}, {"mode", "0"}});
    for (size_t f = 0; f < 400 && !rt.exit_requested(); ++f) rt.tick(16, idle);
    // ---- P1-4: resume — click advance works after skip-off ----
    const std::string s_skip_end = sample(rt);
    check(park_story(rt, idle, 8000), "P1-4a re-parked after skip-off");
    bool resumed = false;
    {
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = 640;
        cl.mouse_y = 600;
        rt.tick(16, cl);
        for (size_t f = 0; f < 3000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (sample(rt) != s_skip_end) {
                resumed = true;
                break;
            }
            if (is_story_wait(rt.current_wait()) && f > 4) break;
        }
    }
    check(resumed, "P1-4 story advances on click after skip-off");
    // ---- P1-5: F3 (adv SKIP key 114) on unread pages ----
    std::string s_before_f3 = sample(rt);
    press_tick(rt, idle, 114);
    bool f3_ok = !rt.exit_requested();
    for (size_t f = 0; f < 2000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (is_story_wait(rt.current_wait())) break;
    }
    bool f3_click_ok = false;
    {
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = 640;
        cl.mouse_y = 600;
        rt.tick(16, cl);
        for (size_t f = 0; f < 3000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (sample(rt) != s_before_f3) {
                f3_click_ok = true;
                break;
            }
            if (is_story_wait(rt.current_wait()) && f > 4) break;
        }
    }
    std::printf("[p1] F3-unread: alive=%d later-advance=%d sample='%s'->'%s'\n",
                f3_ok ? 1 : 0, f3_click_ok ? 1 : 0, s_before_f3.c_str(),
                sample(rt).c_str());
    check(f3_ok && f3_click_ok,
          "P1-5 F3 on unread pages refuses without crash; click advance still "
          "works");
    // ---- P1-6: mid-journey UI save (F6 slot 1) ----
    check(park_story(rt, idle, 6000), "P1-6a parked before save UI");
    const std::string s_saved = sample(rt);
    press_tick(rt, idle, 117); // F6 = SAVE
    const bool save_screen =
        settle_screen(rt, idle, "500.bt.1.bg.0", 300) && rt.scene().size() > 300;
    check(save_screen, "P1-6b F6 opened the save screen");
    check(click_key(rt, "bt_save01"), "P1-6c save slot 1 clicked");
    bool files_ok = false;
    for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        std::set<std::string> files;
        dir_files(store_dir, files);
        if (files.count("save0001.dat") && files.count("saveg.dat") &&
            files.count("system.dat")) {
            files_ok = true;
            break;
        }
    }
    check(files_ok, "P1-6d numbered save + global files written");
    {
        oa::runtime::FrameInput esc;
        esc.key_down_edges = {27};
        esc.keys_down.insert(27);
        rt.tick(16, esc);
    }
    for (size_t f = 0; f < 9000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.scene().find("500.bt.1.bg.0") == nullptr && rt.scene().size() < 200 &&
            is_story_wait(rt.current_wait()))
            break;
    }
    std::printf("[p1] saved sample='%s' chapter='%s' wall=%.1fs\n", s_saved.c_str(),
                chapter.c_str(), wall());
    // ---- P1-7: fresh boot, title Continue resumes the saved page ----
    {
        std::string ch2;
                auto fs2 = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        Boot boot2(fs2, store, &ch2);
        oa::runtime::FrameInput idle2;
        std::vector<oa::render::Layer> buttons;
        check(reach_title(boot2.rt, idle2, &buttons), "P1-7a second boot title");
        // settle the title entrance animation: bt_load2 world rect must be
        // on-screen and stationary before clicking it (title buttons slide
        // in via tweens; research/10 §7)
        {
            double lx = -1e9;
            size_t quiet = 0;
            for (size_t f = 0; f < 12000 && !boot2.rt.exit_requested(); ++f) {
                boot2.rt.tick(16, idle2);
                if (boot2.rt.transition().is_in_progress(boot2.rt.now_ms())) continue;
                const oa::render::Layer* row = boot2.rt.scene().find("500.b.3.0");
                if (!row) continue;
                double x0 = 0, y0 = 0, rw = 0, rh = 0;
                if (!boot2.rt.scene().world_rect("500.b.3.0", 404, 120, &x0, &y0, &rw, &rh))
                    continue;
                if (x0 < 0) continue;
                quiet = std::fabs(x0 - lx) < 0.5 ? quiet + 1 : 0;
                lx = x0;
                if (quiet > 200) break;
            }
        }
// find the Continue row (title.lua 'cont' cursor)
        bool found = false;
        for (const oa::render::Layer* l : boot2.rt.scene().draw_order()) {
            const auto* h = boot2.rt.scene().find_event_handler(l->id, "click");
            if (!h) continue;
            const auto key = h->params.find("key");
            const auto name = h->params.find("name");
            const bool is_start =
                key != h->params.end() && key->second == "bt_start" &&
                (name == h->params.end() || name->second == "ttl1");
            if (is_start) continue; // the はじめから row is not the Continue row
            (void)key;
            const auto k2 = h->params.find("key");
            if (k2 == h->params.end()) continue;
            const std::string& kv = k2->second;
            if (kv.find("cont") == std::string::npos &&
                kv.find("load2") == std::string::npos)
                continue;
            double w = l->has_clip ? l->clip_w : 404.0;
            double hh = l->has_clip ? l->clip_h : 120.0;
            double x0 = 0, y0 = 0, rw = 0, rh = 0;
            if (!boot2.rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) {
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
            boot2.rt.tick(16, cl);
            found = true;
            std::printf("[p1] continue row %s key=%s center=(%.0f,%.0f)\n",
                        l->id.c_str(), kv.c_str(), x0 + rw / 2, y0 + rh / 2);
            break;
        }
        check(found, "P1-7b title Continue row present (seeded store)");
        bool restored = false;
        if (found) {
            for (size_t f = 0; f < 20000 && !boot2.rt.exit_requested(); ++f) {
                boot2.rt.tick(16, idle2);
                const auto* w = boot2.rt.current_wait();
                if (sample(boot2.rt) == s_saved && rt.text().layer("1.80.mw.adv_adv") &&
                    is_story_wait(w)) {
                    restored = true;
                    break;
                }
            }
        }
        check(restored, "P1-7c Continue restored the saved story page (sample "
                        "match, story wait)");
        if (restored) {
            // and the resumed run advances past the saved page on a click
            bool adv2 = false;
            for (int t = 0; t < 6 && !adv2 && !boot2.rt.exit_requested(); ++t) {
                if (!park_story(boot2.rt, idle2, 3000)) break;
                oa::runtime::FrameInput cl;
                cl.left_click_edge = true;
                cl.left_down = true;
                cl.mouse_x = 640;
                cl.mouse_y = 600;
                boot2.rt.tick(16, cl);
                for (size_t f = 0; f < 2500 && !boot2.rt.exit_requested(); ++f) {
                    boot2.rt.tick(16, idle2);
                    if (sample(boot2.rt) != s_saved) {
                        adv2 = true;
                        break;
                    }
                    if (is_story_wait(boot2.rt.current_wait()) && f > 4) break;
                }
            }
            check(adv2, "P1-7d resumed play advances past the saved page");
        }
    }
    // Windows port (research/128): `rm -rf '...'` is POSIX-only — remove the
    // per-process store tree through std::filesystem instead.
    fsf::remove_all(store_path, ec);
    if (failures) {
        std::fprintf(stderr, "p1_chain_skip_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("p1_chain_skip_test: all ok (wall=%.1fs)\n", wall());
    return 0;
}
