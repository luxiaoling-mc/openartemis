// so90_face_probe — headless scene-discovery probe for the 天选庶民的真命之选
// (Select Oblige, r10-era fpm framework, 1280x720) "background turns black
// when the mw face avatar shows / backlog opens" defect (research/90).
//
// Boots the real archive, parks the title (or loads the given save), walks
// story pages and reports, at every parked page: the wait, a text sample and
// the inventory of VISIBLE layers carrying an intermediate_render_mask or an
// intermediate_render prop (the engine offscreen-group path the avatar /
// backlog row faces use). Not registered as a CTest test (diagnostic probe).
//
// Env:
//   OA_TEST_SO_PFS   root.pfs of the title (also positional argv[1])
//   OA_SO_SAVEDIR    save-store root dir (copy of the game savedata/)
//   OA_SO_LOAD=1     load save0001.dat at the title instead of bt_start
//   OA_SO_PAGES=N    page-walk budget (default 300)
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_save.h"

namespace {
const char* wk(const oa::runtime::WaitReason* w) {
    using K = oa::runtime::WaitReason::Kind;
    if (!w) return "?";
    switch (w->kind) {
        case K::Generic: return "generic";
        case K::Generic0: return "wt0";
        case K::Timed: return "timed";
        case K::Stop: return "stop";
        case K::Se: return "se";
        case K::VideoLayer: return "video";
        case K::ScenarioTween: return "scenario-tween";
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
std::string page_sample(const oa::runtime::GameRuntime& rt) {
    const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
    if (!ml) return std::string("-");
    for (const auto& u : ml->page)
        if (!u.data.empty()) return u.data.substr(0, 14);
    return std::string("-");
}
/// Every VISIBLE layer that would take the offscreen-group path or carries a
/// mask — printed as "id alpha clip file props". The constant stage-root
/// "1.0" mask (no intermediate_render) is skipped: it is not a group.
void dump_mask_layers(oa::runtime::GameRuntime& rt, const char* tag) {
    int n = 0;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        if (!l) continue;
        const auto im = l->props.find("intermediate_render");
        const auto mk = l->props.find("intermediate_render_mask");
        if (im == l->props.end() && mk == l->props.end()) continue;
        if (l->id == "1.0" && im == l->props.end()) continue;
        std::string clip;
        const auto cv = l->props.find("clip");
        if (cv != l->props.end()) clip = cv->second;
        std::string all;
        for (const auto& kv : l->props) {
            if (all.size() >= 260) break;
            if (!all.empty()) all += " ";
            all += kv.first + "=" + kv.second;
        }
        std::printf("[%s] mask %-28s alpha=%.2f clip='%s' file='%s' props={%s}\n",
                    tag, l->id.c_str(), l->alpha, clip.c_str(), l->file.c_str(),
                    all.c_str());
        ++n;
    }
    if (n == 0) std::printf("[%s] mask (none)\n", tag);
}
void click_at(oa::runtime::GameRuntime& rt, int x, int y) {
    oa::runtime::FrameInput cl;
    cl.left_click_edge = true;
    cl.left_down = true;
    cl.mouse_x = x;
    cl.mouse_y = y;
    rt.tick(16, cl);
    oa::runtime::FrameInput idle;
    for (size_t f = 0; f < 8; ++f) rt.tick(16, idle);
}
} // namespace

int main(int argc, char** argv) {
    const char* pfs_path = std::getenv("OA_TEST_SO_PFS");
    if ((!pfs_path || !*pfs_path) && argc > 1) pfs_path = argv[1];
    if (!pfs_path || !*pfs_path) {
        std::fprintf(stderr, "OA_TEST_SO_PFS not set\n");
        return 77;
    }
    const char* sd = std::getenv("OA_SO_SAVEDIR");
    const bool do_load = std::getenv("OA_SO_LOAD") != nullptr;
    const int page_budget = [] {
        const char* v = std::getenv("OA_SO_PAGES");
        return v && *v ? std::atoi(v) : 300;
    }();
    try {
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rt(fs);
        if (sd && *sd) {
            auto store = std::make_shared<oa::runtime::DirSaveStore>(sd);
            rt.set_save_store(store);
        }
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
        for (size_t f = 0; f < 60000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (!title) continue;
            const auto* w = rt.current_wait();
            if (w && w->kind == oa::runtime::WaitReason::Kind::Stop &&
                w->id.empty()) {
                at_title = true;
                break;
            }
        }
        std::printf("[so90] title parked: %d\n", (int)at_title);
        for (size_t f = 0; f < 120; ++f) rt.tick(16, idle);

        if (do_load) {
            std::printf("[so90] loading save0001.dat\n");
            const bool ok = rt.load_game_from("save0001.dat", -1);
            std::printf("[so90] load ok=%d\n", (int)ok);
        } else {
            Key start;
            if (find_key(rt, "bt_start", &start)) {
                oa::runtime::FrameInput cl;
                cl.left_click_edge = true;
                cl.left_down = true;
                cl.mouse_x = start.cx;
                cl.mouse_y = start.cy;
                rt.tick(16, cl);
                std::printf("[so90] clicked bt_start at (%d,%d)\n", start.cx,
                            start.cy);
            } else {
                std::printf("[so90] bt_start not found\n");
            }
        }

        // walk pages until the budget runs out
        int pages = 0;
        int turns = 0; // parked-page turns recorded
        bool parked_prev = false;
        bool dumped = false; // one full visible-layer inventory per page
        for (size_t f = 0; f < 2000000 && pages < page_budget &&
                             !rt.exit_requested();
             ++f) {
            rt.tick(16, idle);
            const auto* w = rt.current_wait();
            const bool parked = is_clickable_wait(w) &&
                rt.text().page_has_visible_text("1.80.mw.adv_adv");
            if (parked && !parked_prev) {
                std::printf("[so90] page %4d wait=%s sample='%s' layers=%zu\n",
                            pages + 1, wk(w),
                            page_sample(rt).c_str(), rt.scene().size());
                dump_mask_layers(rt, "page");
                dumped = false;
                ++pages;
                click_at(rt, 640, 600);
                parked_prev = true;
                ++turns;
            } else if (!parked) {
                parked_prev = false;
            }
            // On avatar pages, one full visible-layer inventory (id/file/
            // alpha) shows whether scene bg/fg content exists under the
            // black backdrop the user reports.
            bool avatar_now = false;
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                if (l->id.find(".fa") != std::string::npos) avatar_now = true;
            }
            if (avatar_now && !dumped) {
                dumped = true;
                std::printf("[so90] -- visible inventory (avatar page) --\n");
                size_t cnt = 0;
                for (const oa::render::Layer* l : rt.scene().draw_order()) {
                    if (cnt++ >= 400) break;
                    std::string clip;
                    const auto cv = l->props.find("clip");
                    if (cv != l->props.end()) clip = cv->second;
                    std::printf("[so90]   %-30s a=%.2f clip='%s' file='%s'\n",
                                l->id.c_str(), l->alpha, clip.c_str(),
                                l->file.c_str());
                }
                std::printf("[so90] -- end inventory --\n");
            }
        }
        std::printf("[so90] DONE pages=%d turns=%d exit=%d\n", pages, turns,
                    (int)rt.exit_requested());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "so90 probe exception: %s\n", e.what());
        return 1;
    }
}
