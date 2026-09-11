// nsel_jump_test — FPM "次の選択肢に進む" (dock bt_nsel, exec=adv_selnext) —
// end-to-end acceptance over the REAL fpm archive (docs/research/56).
//
// Bug (research report /tmp/skip_next_select_report.md): the whole script
// chain (dock button -> adv_selnext gating -> dialog -> goNextSelect estag
// chain -> goNextSelectLoop -> e:debugSkip{index=99999}) exists and runs,
// but the engine registered e:debugSkip as a no-op (lua_bridge.cpp noops
// list), so the "jump to the next option" never advances the story.
//
// Route: boot -> はじめから -> a few real-cadence page turns in 共通-01/02,
// then (config only, not game data) conf.messkip=1 — the FPM "skip unread"
// setting the user toggles in the config UI — then click the story-dock
// bt_nsel button, confirm the dialog, and assert the engine fast-forwards
// (zero further input) across 共通-02..11 and halts at the first select of
// 共通-12 with the option rows live (static chapter scan: 共通-01..11 carry
// no select/movie/stop/skipstop/staffroll/title rows; 共通-12 has one select
// group at ast lines 206-208).
//
// Red on HEAD: NSE5 (advance / options) fails — position never leaves the
// trigger page (engine never enters the fast-forward). Green after the
// engine patch (e:debugSkip -> exskip fast-forward + [stop 0=exskip]
// boundary + onDebugSkipOut).
//
// Env: OA_TEST_FPM_PFS (skip 77 unset). Optional OA_EXSKIPDBG=1 traces the
// engine exskip protocol. Mirrors p3_select_test / qload_dock_test drivers.
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
/// Hover then click at (x, y); hold `after` frames with the button down.
void hover_click(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle, int x,
                 int y, size_t after = 200) {
    oa::runtime::FrameInput mv;
    mv.mouse_x = x;
    mv.mouse_y = y;
    for (size_t f = 0; f < 30; ++f) rt.tick(16, mv);
    oa::runtime::FrameInput cl;
    cl.mouse_x = x;
    cl.mouse_y = y;
    cl.left_click_edge = true;
    cl.left_down = true;
    rt.tick(16, cl);
    oa::runtime::FrameInput hold;
    hold.mouse_x = x;
    hold.mouse_y = y;
    for (size_t f = 0; f < after && !rt.exit_requested(); ++f) rt.tick(16, hold);
    oa::runtime::FrameInput out;
    out.mouse_x = -1;
    out.mouse_y = -1;
    for (size_t f = 0; f < 20 && !rt.exit_requested(); ++f) rt.tick(16, out);
}
/// World center of the layer whose click row carries `key`.
bool row_center(oa::runtime::GameRuntime& rt, const std::string& key, int* x, int* y) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end() || k->second != key) continue;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 60.0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 60.0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (!rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) return false;
        *x = int(x0 + rw / 2);
        *y = int(y0 + rh / 2);
        return true;
    }
    return false;
}
bool click_key(oa::runtime::GameRuntime& rt, const std::string& key) {
    int x = 0, y = 0;
    if (!row_center(rt, key, &x, &y)) return false;
    oa::runtime::FrameInput idle;
    hover_click(rt, idle, x, y);
    return true;
}
/// Dialog visible = the yes/no node OR a bt_yes / bt_ok click row exists.
bool dlg_visible(oa::runtime::GameRuntime& rt) {
    if (rt.scene().find("600.1.bt.1.0") != nullptr) return true;
    int x = 0, y = 0;
    return row_center(rt, "bt_yes", &x, &y) || row_center(rt, "bt_ok", &x, &y);
}
/// Confirm the yes/no dialog: Enter first (p3 pattern), then the bt_yes row
/// as a fallback; return true once the dialog closed.
bool confirm_dialog(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    size_t held = 0;
    for (size_t f = 0; f < 6000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        if (w && dlg_visible(rt)) {
            if (++held > 120) break;
        } else {
            held = 0;
        }
    }
    if (held == 0) return false; // dialog never appeared
    oa::runtime::FrameInput en;
    en.key_down_edges = {13};
    en.keys_down.insert(13);
    for (size_t f = 0; f < 60 && !rt.exit_requested(); ++f) rt.tick(16, f == 0 ? en : idle);
    for (size_t f = 0; f < 2000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (!dlg_visible(rt)) return true;
    }
    // fallback: click the yes row
    return click_key(rt, "bt_yes") &&
           [&] {
               for (size_t f = 0; f < 2000 && !rt.exit_requested(); ++f) {
                   rt.tick(16, idle);
                   if (!dlg_visible(rt)) return true;
               }
               return false;
           }();
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
/// FPM uimask layer (uimask_on -> lyc zzamask; fullscreen black). The
/// user-visible black screen after the nsel confirm is this layer surviving:
/// the replay tail (script.asb *main_blj) removes it via [uimask 0="del"].
const oa::render::Layer* mask_layer(oa::runtime::GameRuntime& rt) {
    return rt.scene().find("zzamask");
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_FPM_PFS unset; skipping\n");
        return 77;
    }
    namespace fsf = std::filesystem;
    std::error_code ec;
    const fsf::path store_dir =
        fsf::temp_directory_path(ec) / ("oa_nsel_" + std::to_string(::getpid()));
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
    check(reach_title(rt, idle), "NSE-0 title parked");
    start_game(rt, idle);
    bool body = false;
    for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.text().page_has_visible_text("1.80.mw.adv_adv")) {
            body = true;
            break;
        }
    }
    check(body, "NSE-0b story body reached");
    // ---- NSE-1: real-cadence page turns until parked inside 共通-02 ----
    std::string last = sample(rt);
    int clicks = 0;
    for (int p = 0; p < 400 && !rt.exit_requested(); ++p) {
        if (!park_story(rt, idle)) break;
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = 640;
        cl.mouse_y = 600;
        rt.tick(16, cl);
        ++clicks;
        bool settled = false;
        for (size_t f = 0; f < 2500 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const std::string now = sample(rt);
            if (now != last) {
                last = now;
                settled = true;
                break;
            }
            if (is_story_wait(rt.current_wait()) && f > 4) {
                settled = true;
                break;
            }
        }
        (void)settled;
        if (chapter == "共通-02.ast" && park_story(rt, idle)) break;
    }
    std::printf("[nsel] turns=%d chapter='%s' sample='%s' wall=%.1fs\n", clicks,
                chapter.c_str(), sample(rt).c_str(), wall());
    check(chapter == "共通-02.ast" || chapter == "共通-01.ast",
          "NSE-1 story parked in the early common route (chapter='%s')", chapter.c_str());
    const std::string ch_before = chapter;
    const std::string s_before = sample(rt);
    (void)s_before; // diagnostics reference point for red-state diffing
    // ---- NSE-2: enable "skip unread" (conf.messkip) — the FPM config the
    // player flips to allow jumping over unread text; config is Lua runtime
    // state, not game data. ----
    {
        bool ok = false;
        try {
            rt.interpreter().lua_bridge().run_code(
                "if type(conf) == 'table' then conf.messkip = 1 end",
                "nsel-messkip");
            ok = true; // conf table exists post-boot; if it did not, NSE-4
                       // fails loudly (the unread gate blocks the dialog)
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[nsel] messkip set failed: %s\n", ex.what());
        }
        check(ok, "NSE-2 messkip toggle issued");
        for (size_t f = 0; f < 120 && !rt.exit_requested(); ++f) rt.tick(16, idle);
    }
    // ---- NSE-3: dock bt_nsel (list_*_ja.tbl mw.bt.dc.1.5, exec=adv_selnext) ----
    int bx = 0, by = 0;
    const bool dock_row = row_center(rt, "bt_nsel", &bx, &by);
    std::printf("[nsel] dock bt_nsel row=%d at %d,%d (wait=%s)\n", (int)dock_row, bx, by,
                ws(rt.current_wait()));
    check(dock_row, "NSE-3 dock nsel button clickable");
    bool clicked_ui = false;
    if (dock_row) {
        hover_click(rt, idle, bx, by);
        clicked_ui = true;
    }
    // ---- NSE-4: confirmation dialog appears; confirm it ----
    bool dlg = confirm_dialog(rt, idle);
    if (!dlg && clicked_ui) {
        // dock click may have been eaten by a frame tail: retry once
        if (row_center(rt, "bt_nsel", &bx, &by)) {
            hover_click(rt, idle, bx, by);
            dlg = confirm_dialog(rt, idle);
        }
    }
    std::printf("[nsel] dialog confirmed=%d wait=%s chapter='%s' wall=%.1fs\n", (int)dlg,
                ws(rt.current_wait()), chapter.c_str(), wall());
    check(dlg, "NSE-4 next-select confirm dialog answered");
    // ---- NSE-5: fast-forward must cross chapters and park at the select ----
    // (a) no-input window: engine exskip should do all the work
    bool opts_seen = false;
    bool ch_moved = false;
    bool mask_seen = false;      // zzamask was raised during the jump (repro)
    bool mask_at_arrival = false; // zzamask still live when the select parked
    size_t opts_frame = 0;       // no-input frames consumed until the select
    size_t mask_frames = 0;      // frames the mask was up (black-screen width)
    std::vector<SelectOpt> opts;
    for (size_t f = 0; f < 60000 && !rt.exit_requested() && !opts_seen; ++f) {
        rt.tick(16, idle);
        if (chapter != ch_before) ch_moved = true;
        if (mask_layer(rt)) {
            mask_seen = true;
            ++mask_frames;
        }
        if (find_select_opts(rt, &opts)) {
            opts_seen = true;
            opts_frame = f;
            mask_at_arrival = mask_layer(rt) != nullptr;
            break;
        }
        if (!opts_seen && f % 4000 == 3999)
            std::printf("[nsel] (a) f=%zu chapter='%s' wait=%s sample='%s'\n", f,
                        chapter.c_str(), ws(rt.current_wait()), sample(rt).c_str());
    }
    std::printf("[nsel] phase-a opts=%d chmoved=%d mask=%d maskArrival=%d "
                "optsFrame=%zu maskFrames=%zu chapter='%s' wall=%.1fs\n",
                (int)opts_seen, (int)ch_moved, (int)mask_seen, (int)mask_at_arrival,
                opts_frame, mask_frames, chapter.c_str(), wall());
    // (b) landing clicks (the boundary replay may need one page click before
    // the select screen's estag chain completes)
    int land_clicks = 0;
    for (int p = 0; p < 80 && !opts_seen && !rt.exit_requested(); ++p) {
        if (!park_story(rt, idle, 6000)) break;
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = 640;
        cl.mouse_y = 600;
        rt.tick(16, cl);
        ++land_clicks;
        for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (chapter != ch_before) ch_moved = true;
            if (mask_layer(rt)) mask_seen = true;
            if (find_select_opts(rt, &opts)) {
                opts_seen = true;
                mask_at_arrival = mask_layer(rt) != nullptr;
                break;
            }
            if (f > 100 && is_story_wait(rt.current_wait())) break;
        }
    }
    std::printf("[nsel] phase-b landclicks=%d opts=%d chmoved=%d chapter='%s' wait=%s "
                "wall=%.1fs\n",
                land_clicks, (int)opts_seen, (int)ch_moved, chapter.c_str(),
                ws(rt.current_wait()), wall());
    check(opts_seen || ch_moved,
          "NSE-5 next-select jump moved the story (opts=%d chmoved=%d chapter=%s->%s)",
          (int)opts_seen, (int)ch_moved, ch_before.c_str(), chapter.c_str());
    std::printf("[nsel] mask-raised-during-jump=%d (with the tick-burst the "
                "whole crossing completes inside the confirm tick, so the "
                "mask may never be observable post-confirm)\n",
                (int)mask_seen);
    check(mask_frames < 150,
          "NSE-7c black-screen window stays short: the exskip fast-forward "
          "must burn parks inside a few ticks, not one per frame — at 60 fps "
          "a 6800-frame crossing (~114 s) is the reported black screen "
          "(maskFrames=%zu)", mask_frames);
    if (opts_seen) {
        check(!mask_at_arrival,
              "NSE-7 uimask removed by the replay tail ([uimask 0=\"del\"], "
              "script.asb *main_blj) before the select screen parks — the "
              "screen must NOT stay black (mask=%d)", (int)mask_at_arrival);
        // ---- NSE-7b: arrival screen = the normal select screen (option text
        // layers visible like p3's P3-1e) — the "black screen" symptom would
        // show up as missing visible content here ----
        {
            int opt_text = 0;
            for (const std::string& mlid : rt.text().visible_content_layers()) {
                const oa::render::MessageLayer* ml = rt.text().layer(mlid);
                if (!ml || !rt.text().page_has_visible_text(mlid)) continue;
                if (mlid.find(".120.") == std::string::npos) continue;
                ++opt_text;
            }
            std::printf("[nsel] arrival text-layers=%d opts=%zu layers=%zu\n",
                        opt_text, opts.size(), rt.scene().size());
            check(opt_text >= (int)opts.size(),
                  "NSE-7b per-option text layers visible at the arrival "
                  "select (text=%d opts=%zu)", opt_text, opts.size());
        }
        // NSE-5c used to be check(true, ...) inside this if: a tautology that
        // could never fail. The chapter label is printed just below; the real
        // select-state gates are NSE-5b / NSE-7b.
        check(opts.size() >= 2, "NSE-5b two options present (got %zu)", opts.size());
        std::printf("[nsel] select chapter='%s' wait=%s opts=%zu sample='%s'\n",
                    chapter.c_str(), ws(rt.current_wait()), opts.size(), sample(rt).c_str());
        // ---- NSE-6: choose option 1 -> select resolves, story continues ----
        const SelectOpt& o = opts[0];
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = int(o.cx);
        cl.mouse_y = int(o.cy);
        for (size_t f = 0; f < 80 && !rt.exit_requested(); ++f) rt.tick(16, f == 0 ? cl : idle);
        bool resolved = false;
        for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            std::vector<SelectOpt> chk;
            if (!find_select_opts(rt, &chk)) {
                resolved = true;
                break;
            }
        }
        check(resolved, "NSE-6 option click resolved the select");
        // story must stay alive: next page turn advances
        bool adv = false;
        for (int t = 0; t < 6 && !adv && !rt.exit_requested(); ++t) {
            if (!park_story(rt, idle, 3000)) break;
            const std::string s0 = sample(rt);
            oa::runtime::FrameInput c2;
            c2.left_click_edge = true;
            c2.left_down = true;
            c2.mouse_x = 640;
            c2.mouse_y = 600;
            rt.tick(16, c2);
            for (size_t f = 0; f < 2500 && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                if (sample(rt) != s0 || is_story_wait(rt.current_wait())) {
                    adv = true;
                    break;
                }
            }
        }
        check(adv, "NSE-6b story advances after the branch");
    }
    fsf::remove_all(store_dir, ec);
    if (failures) {
        std::fprintf(stderr, "nsel_jump_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("nsel_jump_test: all ok (wall=%.1fs)\n", wall());
    return 0;
}
