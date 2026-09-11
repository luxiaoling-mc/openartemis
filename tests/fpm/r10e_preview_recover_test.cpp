// R10e regression: config speed-slider drag must not strand the sample
// preview (现象B — user authorized engine-side interpretation; NO game data
// changes allowed).
//
// Data facts (ui/config.lua): the preview text (500.z.sample) is printed
// ONLY by config_sampletext, the completion handler of a script-timer
// lytween on the dummy layer 500.z.zz (the single lytween-with-handler in
// the whole game data). config_textex (per-frame p4 of the sl0201/sl0202
// xsliders and page open) clears the sample page (chgmsg+rp) and calls
// config_samplestart, whose lytweendel cancels the running timer round and
// whose `if not flg.config.sample` gate then refuses to rearm while the
// cancelled round's flag is still set (the flag clears only inside the
// round-end handler) — the cycle strands and the preview stays blank.
//
// Engine interpretation (R10e): deleting a lytween that still carries a
// completion handler ENDS the timer round — the pending completion is
// delivered once through the normal completion path, so the data's own
// round-end bookkeeping runs (sample=nil + rearm). Only the sample timer is
// affected in this data (it is the only handler-carrying lytween); visual
// lytweendel cancellations carry no handler and are unchanged.
//
// This test: boot -> story -> F10 config -> page 2 (text settings) -> wait
// for the preview's first print (300 ms round) -> drag the sl0201 thumb
// left while its timer round is armed -> release -> assert the preview
// refills and keeps cycling (>=2 content changes) and stays non-empty.
// Pre-fix the page is rp'd empty on the first drag frame and never prints
// again.
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
bool is_clickable_wait(const oa::runtime::WaitReason* w) {
    using K = oa::runtime::WaitReason::Kind;
    if (!w) return false;
    return w->kind == K::Generic || w->kind == K::Generic0 ||
           w->kind == K::Timed || w->kind == K::Se || w->kind == K::KeyWait;
}
struct Key {
    std::string key;
    int cx = 0, cy = 0;
};
bool find_key(oa::runtime::GameRuntime& rt, const std::string& key, Key* out) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end() || k->second != key) continue;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (rt.scene().world_rect(l->id, w > 0 ? w : 80, hh > 0 ? hh : 40,
                                  &x0, &y0, &rw, &rh)) {
            out->key = key;
            out->cx = int(x0 + rw / 2);
            out->cy = int(y0 + rh / 2);
            return true;
        }
    }
    return false;
}
std::string sample(oa::runtime::GameRuntime& rt) {
    const oa::render::MessageLayer* ml = rt.text().layer("500.z.sample");
    if (!ml) return "-no-layer-";
    for (const auto& u : ml->page)
        if (!u.data.empty()) return u.data.substr(0, 24);
    return "-empty-";
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::fprintf(stderr, "OA_TEST_FPM_PFS not set\n");
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
        bool at_title = false;
        for (size_t f = 0; f < 20000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (!title) continue;
            const auto* w = rt.current_wait();
            if (w && w->kind == oa::runtime::WaitReason::Kind::Stop &&
                w->id.empty()) {
                at_title = true;
                break;
            }
        }
        check(at_title, "boot parked at the title");
        for (size_t f = 0; f < 300; ++f) rt.tick(16, idle); // title settle
        {
            Key start;
            if (find_key(rt, "bt_start", &start)) {
                oa::runtime::FrameInput cl;
                cl.left_click_edge = true;
                cl.left_down = true;
                cl.mouse_x = start.cx;
                cl.mouse_y = start.cy;
                rt.tick(16, cl);
            }
        }
        bool body = false;
        for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (rt.text().page_has_visible_text("1.80.mw.adv_adv") &&
                is_clickable_wait(rt.current_wait())) {
                body = true;
                break;
            }
        }
        check(body, "story body parked");
        for (size_t f = 0; f < 300; ++f) rt.tick(16, idle);
        {
            oa::runtime::FrameInput k;
            k.key_down_edges = {121}; // F10 config
            k.keys_down.insert(121);
            rt.tick(16, k);
        }
        Key unused;
        bool cfg = false;
        for (size_t f = 0; f < 9000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (rt.scene().size() > 150 && find_key(rt, "bt_title", &unused)) {
                cfg = true;
                break;
            }
        }
        check(cfg, "config opened (page 1)");
        for (size_t f = 0; f < 240; ++f) rt.tick(16, idle);
        // page 2 = text settings with the sample preview
        Key p2;
        check(find_key(rt, "page2", &p2), "config page2 tab present");
        if (find_key(rt, "page2", &p2)) {
            std::printf("[r10e] page2 tab @(%d,%d)\n", p2.cx, p2.cy);
            // hover first (config buttons act on btn.cursor), then click
            for (size_t f = 0; f < 12; ++f) {
                oa::runtime::FrameInput mv;
                mv.mouse_x = p2.cx;
                mv.mouse_y = p2.cy;
                rt.tick(16, mv);
            }
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = p2.cx;
            cl.mouse_y = p2.cy;
            rt.tick(16, cl);
        }
        for (size_t f = 0; f < 400; ++f) rt.tick(16, idle);
        {
            Key slk;
            const auto* w = rt.current_wait();
            std::printf("[r10e] after page2: layers=%zu wait=%d sl0201=%d "
                        "zz=%s\n",
                        rt.scene().size(), w ? (int)w->kind : -1,
                        find_key(rt, "sl0201", &slk) ? 1 : 0,
                        rt.scene().find("500.z.zz") ? "y" : "n");
        }
        // wait for the first print (300 ms round after page open)
        bool alive = false;
        for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const std::string s = sample(rt);
            if (s != "-empty-" && s != "-no-layer-") {
                alive = true;
                break;
            }
        }
        check(alive, "preview printed its first sample line");
        std::printf("[r10e] first sample='%s'\n", sample(rt).c_str());
        // mid-first-round: drag the sl0201 thumb left (~90px, 30 frames)
        Key sl;
        if (find_key(rt, "sl0201", &sl)) {
            oa::runtime::FrameInput down;
            down.left_click_edge = true;
            down.left_down = true;
            down.mouse_x = sl.cx;
            down.mouse_y = sl.cy;
            rt.tick(16, down);
            for (int i = 1; i <= 30 && !rt.exit_requested(); ++i) {
                oa::runtime::FrameInput mv;
                mv.left_down = true;
                mv.mouse_x = sl.cx - (i * 90) / 30;
                mv.mouse_y = sl.cy;
                rt.tick(16, mv);
            }
            oa::runtime::FrameInput up;
            up.mouse_x = sl.cx - 90;
            up.mouse_y = sl.cy;
            rt.tick(16, up);
        }
        check(find_key(rt, "sl0201", &sl), "sl0201 present for the drag");
        // post-release: preview must refill and keep cycling
        std::string last = sample(rt);
        int changes = 0;
        bool refilled = false;
        bool ended_empty = true;
        for (size_t f = 0; f < 15000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const std::string s = sample(rt);
            if (s != "-empty-" && s != "-no-layer-") {
                ended_empty = false;
                if (!refilled) {
                    refilled = true;
                    std::printf("[r10e] preview refilled at f=%zu '%s'\n", f,
                                s.c_str());
                } else if (s != last) {
                    ++changes;
                    std::printf("[r10e] cycle change %d at f=%zu '%s'\n",
                                changes, f, s.c_str());
                    last = s;
                }
            } else if (!refilled && s == "-empty-") {
                // still blank immediately after the drag — expected until the
                // delivered round-end rearm prints
            }
            if (refilled && changes >= 2) break;
        }
        std::printf("[r10e] post-drag refilled=%d changes=%d final='%s'\n",
                    refilled ? 1 : 0, changes, sample(rt).c_str());
        check(refilled, "preview refilled after the speed-slider drag");
        check(changes >= 2, "preview kept cycling (>=2 content changes)");
        check(!ended_empty, "preview non-empty at the end");
        std::printf(failures ? "r10e_preview_recover FAIL (%d)\n" : "r10e_preview_recover OK\n",
                    failures);
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
}
