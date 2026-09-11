// QA-queue P3 acceptance (real fpm): the in-story choice (select) screen.
// Route: boot -> はじめから -> ~250 real-cadence page turns (共通-01/02), then
// engine skip (the exact tags FPM skipmode_start emits) across chapters until
// the story halts at the first select (共通-12) — engine-skip stops at the
// select's stop-wait, exactly like FPM's own skip. Then:
//   P3-1  select screen: >=2 option click rows (name=select), geometry on the
//         csv.mw.select row centers (640,321)/(640,405), and per-option text
//         message layers carry the option text
//   P3-2  hover + click option 1 -> options removed, branch chapter 共通-12a
//         entered, story continues into 共通-13
//   P3-3  branch-state save/load: F6 save at the branch page; F7 load +
//         confirm -> the saved branch page is restored (sample equality)
// Env: OA_TEST_FPM_PFS (skip 77 unset). See docs/research/28.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
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
bool reach_title(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    for (size_t f = 0; f < 20000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        if (!(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty()))
            continue;
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
struct SelectOpt {
    std::string layer;
    std::string no;
    double cx = 0, cy = 0;
};
bool find_select_opts(oa::runtime::GameRuntime& rt, std::vector<SelectOpt>* out) {
    std::map<int, SelectOpt> by_no;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto it = h->params.find("name");
        if (it == h->params.end() || it->second != "select") continue;
        SelectOpt o;
        o.layer = l->id;
        const auto no = h->params.find("no");
        if (no != h->params.end()) o.no = no->second;
        double w = l->has_clip ? l->clip_w : 560.0;
        double hh = l->has_clip ? l->clip_h : 74.0;
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) {
            o.cx = x0 + rw / 2;
            o.cy = y0 + rh / 2;
        }
        by_no[std::atoi(o.no.c_str())] = o;
    }
    for (auto& [k, v] : by_no) out->push_back(v);
    return !out->empty();
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_FPM_PFS unset; skipping\n");
        return 77;
    }
    // Windows port: mkdtemp("/tmp/...") is POSIX-only — use the system temp
    // dir + pid (same shape as misc_b_test / save_load_ui_test) so the
    // real-fpm select chain runs on MSVC too.
    namespace fsf = std::filesystem;
    std::error_code ec;
    const fsf::path store_dir =
        fsf::temp_directory_path(ec) / ("oa_p3select_" + std::to_string(::getpid()));
    fsf::remove_all(store_dir, ec);
    auto store = std::make_shared<oa::runtime::DirSaveStore>(store_dir.string());
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
    std::string chapter;
    Boot boot(fs, store, &chapter);
    oa::runtime::GameRuntime& rt = boot.rt;
    oa::runtime::FrameInput idle;
    const auto t0 = std::chrono::steady_clock::now();
    auto wall = [&]() -> double {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    check(reach_title(rt, idle), "P3-0 title parked");
    start_game(rt, idle);
    bool body = false;
    for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.text().page_has_visible_text("1.80.mw.adv_adv")) {
            body = true;
            break;
        }
    }
    check(body, "P3-0b story body reached");
    // ---- 250 real-cadence page turns (past 共通-02), then engine skip to
    //      the first select: engine skip halts on the select stop-wait ----
    size_t clicks = 0;
    std::string last = sample(rt);
    for (int p = 0; p < 250 && !rt.exit_requested(); ++p) {
        if (!park_story(rt, idle)) break;
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = 640;
        cl.mouse_y = 600;
        rt.tick(16, cl);
        ++clicks;
        for (size_t f = 0; f < 2500 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const std::string now = sample(rt);
            if (now != last) {
                last = now;
                break;
            }
            if (is_story_wait(rt.current_wait())) break;
        }
    }
    std::printf("[p3] pre-skip pages=%zu chapter='%s' wall=%.1fs\n", clicks,
                chapter.c_str(), wall());
    rt.interpreter().enqueue_tag("skip", {{"allow", "1"}, {"unread", "1"}});
    rt.interpreter().enqueue_tag("exec", {{"command", "skip"}, {"mode", "1"}});
    size_t skip_frames = 0;
    for (; skip_frames < 120000 && !rt.exit_requested(); ++skip_frames) {
        rt.tick(16, idle);
        std::vector<SelectOpt> chk;
        if (find_select_opts(rt, &chk)) break;
    }
    std::printf("[p3] skip frames=%zu chapter='%s' wall=%.1fs\n", skip_frames,
                chapter.c_str(), wall());
    // stop the skip engine (the select park halts it anyway)
    rt.interpreter().enqueue_tag("exec", {{"command", "skip"}, {"mode", "0"}});
    for (size_t f = 0; f < 200 && !rt.exit_requested(); ++f) rt.tick(16, idle);
    // ---- P3-1 select screen checks ----
    std::vector<SelectOpt> opts;
    const bool sel = find_select_opts(rt, &opts);
    check(sel, "P3-1 select screen reached (共通-12)");
    std::printf("[p3] select chapter='%s' wait=%s opts=%zu\n", chapter.c_str(),
                ws(rt.current_wait()), opts.size());
    int opt_text_layers = 0;
    if (sel) {
        check(opts.size() >= 2, "P3-1b two options present (got %zu)", opts.size());
        check(std::fabs(opts[0].cx - 640.0) < 25 && std::fabs(opts[0].cy - 321.0) < 25,
              "P3-1c option 1 on the csv.mw.select row center (640,321), got "
              "(%.0f,%.0f)",
              opts[0].cx, opts[0].cy);
        if (opts.size() >= 2)
            check(std::fabs(opts[1].cy - 405.0) < 25,
                  "P3-1d option 2 row at y=405, got %.0f", opts[1].cy);
        for (const std::string& mlid : rt.text().visible_content_layers()) {
            const oa::render::MessageLayer* ml = rt.text().layer(mlid);
            if (!ml || !rt.text().page_has_visible_text(mlid)) continue;
            if (mlid.find(".120.") == std::string::npos) continue;
            ++opt_text_layers;
            std::printf("[p3] opt-ml %s left=%.0f top=%.0f chars=%zu sample='%s'\n",
                        mlid.c_str(), ml->left, ml->top, ml->char_count,
                        sample(rt).c_str());
        }
        check(opt_text_layers >= (int)opts.size(),
              "P3-1e per-option text layers visible (opts=%zu text=%d)",
              opts.size(), opt_text_layers);
        // ---- P3-2 choose option 1: hover then click ----
        const SelectOpt& o = opts[0];
        bool hovered = false;
        rt.set_pointer_observer([&](const oa::runtime::GameRuntime::PointerDispatch& dd) {
            if (dd.kind == oa::runtime::GameRuntime::PointerDispatch::Kind::HoverIn &&
                (dd.layer == o.layer || dd.layer.rfind(o.layer, 0) == 0))
                hovered = true;
        });
        for (size_t f = 0; f < 400 && !rt.exit_requested() && !hovered; ++f) {
            oa::runtime::FrameInput mv;
            mv.mouse_x = int(o.cx);
            mv.mouse_y = int(o.cy);
            rt.tick(16, mv);
        }
        rt.set_pointer_observer(nullptr);
        check(hovered, "P3-2a option hover dispatched (select_over ran)");
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = int(o.cx);
        cl.mouse_y = int(o.cy);
        for (size_t f = 0; f < 80 && !rt.exit_requested(); ++f) rt.tick(16, f == 0 ? cl : idle);
        // wait for the select to resolve (options removed)
        bool resolved = false;
        std::string branch;
        for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            std::vector<SelectOpt> chk;
            if (!find_select_opts(rt, &chk)) {
                resolved = true;
                break;
            }
        }
        check(resolved, "P3-2b select resolved after the choice");
        // branch: story continues into the chosen sub-chapter 共通-12a then
        // back onto the trunk 共通-13; keep walking deep into 共通-13 so the
        // page-turn tail (skip-stop key reset, select-exit animation) fully
        // settles and the mw is back to its normal dialog state
        bool saw_branch = false, saw_next = false;
        int ch13_pages = 0;
        for (int p = 0; p < 350 && !rt.exit_requested(); ++p) {
            if (!park_story(rt, idle, 3000)) break;
            if (chapter.find("共通-12a") != std::string::npos) saw_branch = true;
            if (chapter == "共通-13.ast") {
                saw_next = true;
                ++ch13_pages;
            }
            if (saw_branch && saw_next && ch13_pages >= 40) break;
            oa::runtime::FrameInput c2;
            c2.left_click_edge = true;
            c2.left_down = true;
            c2.mouse_x = 640;
            c2.mouse_y = 600;
            rt.tick(16, c2);
        }
        std::printf("[p3] branch12a=%d next13=%d ch13pages=%d chapter='%s'\n",
                    saw_branch ? 1 : 0, saw_next ? 1 : 0, ch13_pages,
                    chapter.c_str());
        check(saw_branch, "P3-2c chosen branch 共通-12a entered after option 1");
        check(saw_next && ch13_pages >= 20,
              "P3-2d story continues deep into the trunk chapter 共通-13 "
              "(pages=%d)",
              ch13_pages);
    }
    // ---- P3-3 branch-state save/load ----
    check(park_story(rt, idle, 6000), "P3-3a parked on the branch page");
    // settle a little after the park (framework per-page key tails)
    for (size_t f = 0; f < 200 && !rt.exit_requested(); ++f) rt.tick(16, idle);
    const std::string s_branch = sample(rt);
    check(s_branch != "-" && !s_branch.empty(),
          "P3-3a2 branch save page carries text (sample='%s')", s_branch.c_str());
    press_tick(rt, idle, 117); // F6 save
    check(settle_screen(rt, idle, "500.bt.1.bg.0", 300) && rt.scene().size() > 300,
          "P3-3b save screen opened");
    check(click_key(rt, "bt_save01"), "P3-3c branch save slot 1 clicked");
    // wait for the numbered save + global files (p1/save_load_ui pattern),
    // so the Esc lands in the clean UI state (not the save tail)
    bool files_ok = false;
    for (size_t f = 0; f < 15000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        auto* storep = store.get();
        (void)storep;
        files_ok = store->exists("savedata_cn/save0001.dat") &&
                   store->exists("savedata_cn/saveg.dat") &&
                   store->exists("savedata_cn/system.dat");
        if (files_ok) break;
    }
    check(files_ok, "P3-3c2 numbered save + globals written");
    // close the save screen; a single Esc may land on a framework tail and
    // be swallowed, so re-issue it while the save node is still around
    bool closed = false;
    for (int esc_try = 0; esc_try < 4 && !closed && !rt.exit_requested(); ++esc_try) {
        if (esc_try > 0) {
            for (size_t f = 0; f < 300 && !rt.exit_requested(); ++f) rt.tick(16, idle);
        }
        oa::runtime::FrameInput esc;
        esc.key_down_edges = {27};
        esc.keys_down.insert(27);
        rt.tick(16, esc);
        for (size_t f = 0; f < 9000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const auto* w = rt.current_wait();
            const bool ui_gone = rt.scene().find("500.bt.1.bg.0") == nullptr;
            const bool parkedish =
                w && (is_story_wait(w) || w->kind == oa::runtime::WaitReason::Kind::Stop);
            if (ui_gone && parkedish && sample(rt) == s_branch) {
                closed = true;
                break;
            }
        }
    }
    std::printf("[p3] save-close closed=%d wait=%s layers=%zu sample='%s'\n",
                closed ? 1 : 0, ws(rt.current_wait()), rt.scene().size(),
                sample(rt).c_str());
    check(closed, "P3-3d save screen closed, branch page restored");
    // ---- P3-3e..g: fresh boot + title Continue -> branch page restored
    // (cross-restart branch persistence; the F7 load-UI leg itself is
    // covered by save_load_ui_test P2 / p1_chain_skip P1-7) ----
    {
        std::string ch2;
                auto fs2 = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        Boot boot2(fs2, store, &ch2);
        oa::runtime::FrameInput idle2;
        check(reach_title(boot2.rt, idle2), "P3-3e second boot title parked");
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
        bool clicked = false;
        for (const oa::render::Layer* l : boot2.rt.scene().draw_order()) {
            const auto* h = boot2.rt.scene().find_event_handler(l->id, "click");
            if (!h) continue;
            const auto key = h->params.find("key");
            if (key == h->params.end() || key->second != "bt_load2") continue;
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
            clicked = true;
            break;
        }
        check(clicked, "P3-3f title Continue row clicked");
        bool restored = false;
        if (clicked) {
            for (size_t f = 0; f < 25000 && !boot2.rt.exit_requested(); ++f) {
                boot2.rt.tick(16, idle2);
                const auto* w = boot2.rt.current_wait();
                if (sample(boot2.rt) == s_branch && is_story_wait(w)) {
                    restored = true;
                    break;
                }
            }
        }
        std::printf("[p3] boot2-continue restored=%d chapter='%s' sample='%s'\n",
                    restored ? 1 : 0, ch2.c_str(), sample(boot2.rt).c_str());
        check(restored,
              "P3-3g fresh-boot Continue restored the branch-A page (sample, "
              "wait)");
        // branch story still advances after the cross-restart load
        bool adv = false;
        for (int t = 0; t < 6 && !adv && !boot2.rt.exit_requested(); ++t) {
            if (!park_story(boot2.rt, idle2, 3000)) break;
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = 640;
            cl.mouse_y = 600;
            boot2.rt.tick(16, cl);
            for (size_t f = 0; f < 2500 && !boot2.rt.exit_requested(); ++f) {
                boot2.rt.tick(16, idle2);
                if (sample(boot2.rt) != s_branch) {
                    adv = true;
                    break;
                }
                if (is_story_wait(boot2.rt.current_wait()) && f > 4) break;
            }
        }
        check(adv, "P3-3h branch story advances after the load");
    }
    fsf::remove_all(store_dir, ec);
    if (failures) {
        std::fprintf(stderr, "p3_select_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("p3_select_test: all ok (wall=%.1fs)\n", wall());
    return 0;
}
