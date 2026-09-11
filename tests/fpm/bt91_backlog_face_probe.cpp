// bt91_face_probe — backlog-row "小人物头像" geometry probe for the
// openartemis backlog-face-size report (btjy: backlog speaker icons drawn
// oversized vs fpm reference). Headless: boots the real archive, walks story
// pages, opens the backlog (F8) and dumps, per row, the row subtree layers
// (id/file/props/natural size) plus world AABBs of the drawn quads — the
// engine-side ground truth for actual icon pixel size per game.
//
// Usage: bt91_face_probe <root.pfs|game dir> [opts]
//   OA_BT91_SAVE=<dir>   save-store root (default: _bt91_save next to cwd)
//   OA_BT91_PAGES=N      parked pages before F8 (default 24)
//   OA_BT91_WALK=1       only walk pages (no backlog) — title/debug use
//   OA_BT91_KEY=<name>   click this click-key at title instead of bt_start
//   OA_BT91_NOF8=1       do not attempt backlog
//   OA_BT91_F8TICKS=N    ticks to wait for the backlog to open (default 900)
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <optional>

#include "core/fs/fs.h"
#include "core/fs/physfs_fs.h"
#include "core/fs/project.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_save.h"
#include "core/render/layer.h"
#include "core/render/text.h"

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

bool clickable(const oa::runtime::WaitReason* w) {
    using K = oa::runtime::WaitReason::Kind;
    if (!w) return false;
    return w->kind == K::Generic || w->kind == K::Generic0 ||
           w->kind == K::Timed || w->kind == K::Se || w->kind == K::KeyWait;
}

struct Key { std::string key; int cx = 0, cy = 0; double w = 0, h = 0; };
std::vector<Key> list_keys(oa::runtime::GameRuntime& rt) {
    std::vector<Key> out;
    std::set<std::string> seen;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end()) continue;
        if (!seen.insert(k->second).second) continue;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (rt.scene().world_rect(l->id, w > 0 ? w : 80, hh > 0 ? hh : 40,
                                  &x0, &y0, &rw, &rh)) {
            out.push_back({k->second, int(x0 + rw / 2), int(y0 + rh / 2), rw,
                           rh});
        } else {
            out.push_back({k->second, 0, 0, 0, 0});
        }
    }
    return out;
}

bool click_key(oa::runtime::GameRuntime& rt, const std::string& key) {
    for (const auto& v : list_keys(rt))
        if (v.key == key) {
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = v.cx;
            cl.mouse_y = v.cy;
            rt.tick(16, cl);
            for (size_t f = 0; f < 10; ++f) rt.tick(16, {});
            std::printf("[bt91] clicked '%s' at (%d,%d)\n", key.c_str(), v.cx,
                        v.cy);
            return true;
        }
    std::printf("[bt91] key '%s' NOT clickable\n", key.c_str());
    return false;
}

// --- natural image size of a layer file (mirrors RenderEngine::resolve_image
// naming: resolved path, then +.png, then +.jpg) ---------------------------
bool sniff_png(const std::vector<uint8_t>& d, double* w, double* h) {
    if (d.size() < 24) return false;
    if (d[0] != 0x89 || d[1] != 'P' || d[2] != 'N' || d[3] != 'G') return false;
    if (d[12] != 'I' || d[13] != 'H' || d[14] != 'D' || d[15] != 'R')
        return false;
    auto rd = [&](size_t i) {
        return (uint32_t(d[i]) << 24) | (uint32_t(d[i + 1]) << 16) |
               (uint32_t(d[i + 2]) << 8) | uint32_t(d[i + 3]);
    };
    *w = rd(16);
    *h = rd(20);
    return *w > 0 && *h > 0;
}
bool sniff_jpg(const std::vector<uint8_t>& d, double* w, double* h) {
    if (d.size() < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    size_t i = 2;
    while (i + 9 < d.size()) {
        if (d[i] != 0xFF) { ++i; continue; }
        const uint8_t m = d[i + 1];
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        if (m == 0xD9 || m == 0xDA) return false;
        if (i + 4 > d.size()) return false;
        const size_t len = (size_t(d[i + 2]) << 8) | d[i + 3];
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (i + 9 > d.size()) return false;
            *h = double((d[i + 5] << 8) | d[i + 6]);
            *w = double((d[i + 7] << 8) | d[i + 8]);
            return *w > 0 && *h > 0;
        }
        i += 2 + len;
    }
    return false;
}

struct Ctx {
    oa::runtime::GameRuntime* rt = nullptr;
    std::shared_ptr<const oa::fs::IFileSystem> fs;
    double stage_w = 1280, stage_h = 720;
};

bool layer_natural_size(Ctx& c, const oa::render::Layer& l, double* w, double* h) {
    if (l.file.empty()) return false;
    std::string resolved = c.rt->interpreter().resolve_magic_path(l.file);
    for (const std::string& cand :
         {resolved, resolved + ".png", resolved + ".jpg"}) {
        const auto b = c.fs->read(cand);
        if (!b) continue;
        if (sniff_png(*b, w, h) || sniff_jpg(*b, w, h)) return true;
    }
    return false;
}

// local quad of a layer the way draw_one would size it
bool layer_quad(Ctx& c, const oa::render::Layer& l, double* w, double* h,
                bool* cropped) {
    *cropped = false;
    if (l.has_clip) {
        *w = l.clip_w;
        *h = l.clip_h;
        *cropped = true;
        return *w > 0 && *h > 0;
    }
    if (!l.file.empty()) return layer_natural_size(c, l, w, h);
    if (l.width > 0 && l.height > 0) { *w = l.width; *h = l.height; return true; }
    return false;
}

std::string layer_sig(const oa::render::Layer& l) {
    char buf[512];
    const char* clip = l.has_clip ? "clip" : "";
    std::snprintf(buf, sizeof(buf),
                  "pos=(%.0f,%.0f) sz=(%.0fx%.0f) xy=(%.1f,%.1f) "
                  "clip=%s(%.0f,%.0f,%.0f,%.0f) vis=%d a=%.2f",
                  l.left, l.top, l.width, l.height, l.x_scale, l.y_scale,
                  clip, l.clip_x, l.clip_y, l.clip_w, l.clip_h,
                  (int)l.visible, l.alpha);
    std::string s = buf;
    for (const char* k :
         {"left", "top", "width", "height", "clip", "xscale", "yscale",
          "zoom", "anchorx", "anchory", "alpha", "intermediate_render",
          "intermediate_render_mask", "visible", "file"}) {
        const auto it = l.props.find(k);
        if (it != l.props.end()) s += " " + std::string(k) + "=" + it->second;
    }
    if (!l.file.empty()) s += " file='" + l.file + "'";
    if (!l.mask.empty()) s += " mask='" + l.mask + "'";
    return s;
}

/// Dump every layer under `prefix` (draw_order scan, cheap for small sets)
/// with world AABB of its drawn quad.
void dump_subtree(Ctx& c, const std::string& tag, const std::string& prefix,
                  bool all_visible_only) {
    for (const oa::render::Layer* l : c.rt->scene().draw_order()) {
        if (prefix.size() > l->id.size()) continue;
        if (l->id.compare(0, prefix.size(), prefix) != 0) continue;
        if (all_visible_only &&
            !c.rt->scene().is_effectively_visible(l->id))
            continue;
        double w = 0, h = 0, x0 = 0, y0 = 0, rw = 0, rh = 0;
        bool cr = false;
        char wb[160] = "";
        if (layer_quad(c, *l, &w, &h, &cr) &&
            c.rt->scene().world_rect(l->id, w, h, &x0, &y0, &rw, &rh)) {
            std::snprintf(wb, sizeof(wb),
                          "WORLD=(%.0f,%.0f %gx%g)%s", x0, y0, rw, rh,
                          cr ? " [clip-quad]" : "");
        }
        double nw = 0, nh = 0;
        char nb[96] = "";
        if (!l->file.empty() && layer_natural_size(c, *l, &nw, &nh))
            std::snprintf(nb, sizeof(nb), "NAT=%gx%g ", nw, nh);
        std::printf("[%s] %-40s %s%s %s\n", tag.c_str(), l->id.c_str(), nb,
                    layer_sig(*l).c_str(), wb);
    }
}

std::string story_sample(Ctx& c) {
    std::string r = "-";
    for (const std::string& id : c.rt->text().visible_content_layers()) {
        const oa::render::MessageLayer* ml = c.rt->text().layer(id);
        if (!ml || ml->page.empty()) continue;
        if (ml->reveal_pending) continue;
        std::string s;
        for (const auto& u : ml->page)
            if (!u.data.empty()) { s = u.data.substr(0, 18); break; }
        if (!s.empty()) { r = id + ":'" + s + "'"; return r; }
    }
    return r;
}

bool any_story_text(Ctx& c, const std::string& avoid) {
    for (const std::string& id : c.rt->text().visible_content_layers()) {
        if (avoid.size() && id.find(avoid) != std::string::npos) continue;
        const oa::render::MessageLayer* ml = c.rt->text().layer(id);
        if (!ml || ml->char_count == 0) continue;
        if (ml->reveal_pending) continue;
        if (ml->reveal_index >= ml->char_count) return true;
    }
    return false;
}

void dump_blog(Ctx& c, const char* tag) {
    // per row: text row subtree, row button layers and everything carrying
    // face/mask/intermediate props under the 500 root
    for (int i = 1; i <= 4; ++i) {
        const std::string tx = "500.z.bt.tx." + std::to_string(i);
        const std::string bt = "500.z.bt." + std::to_string(i);
        const std::string bt2 = "500.bt." + std::to_string(i);
        bool any = false;
        for (const oa::render::Layer* l : c.rt->scene().draw_order()) {
            if (l->id.find(tx) == 0 || l->id.find(bt) == 0 ||
                l->id.find(bt2) == 0)
                any = true;
        }
        if (!any) break;
        std::printf("[%s] === row %d ===\n", tag, i);
        for (const std::string& p : {tx, bt, bt2})
            dump_subtree(c, tag, p, true);
    }
    // general visible inventory of mask/intermediate face-bearing layers
    dump_subtree(c, tag, "500.", false);
}

bool is_blog_open(oa::runtime::GameRuntime& rt) {
    for (const oa::render::Layer* l : rt.scene().draw_order())
        if (l->id == "500.0.drag" || l->id == "500.z.bt.tx.1") return true;
    return false;
}

} // namespace

int main(int argc, char** argv) {
    const char* pfs = std::getenv("OA_BT91_PFS");
    if ((!pfs || !*pfs) && argc > 1) pfs = argv[1];
    if (!pfs || !*pfs) {
        std::fprintf(stderr, "usage: bt91_face_probe <root.pfs|dir>\n");
        return 77;
    }
    const char* sv = std::getenv("OA_BT91_SAVE");
    const char* kw = std::getenv("OA_BT91_KEY");
    const int pages_budget = [] {
        const char* v = std::getenv("OA_BT91_PAGES");
        return v && *v ? std::atoi(v) : 24;
    }();
    const int f8_ticks = [] {
        const char* v = std::getenv("OA_BT91_F8TICKS");
        return v && *v ? std::atoi(v) : 1200;
    }();
    const bool no_f8 = std::getenv("OA_BT91_NOF8") != nullptr;
    const bool no_f8_after = no_f8;
    (void)no_f8_after;
    try {
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs, false);
        oa::runtime::GameRuntime rt(fs);
        Ctx c;
        c.rt = &rt;
        c.fs = fs;
        if (sv && *sv) {
            auto store = std::make_shared<oa::runtime::DirSaveStore>(sv);
            rt.set_save_store(store);
            std::printf("[bt91] save store %s\n", sv);
        }
        rt.open_project("windows");
        rt.boot_project();
        c.stage_w = rt.project_.config.stage_width;
        c.stage_h = rt.project_.config.stage_height;
        std::printf("[bt91] stage %.0fx%.0f\n", c.stage_w, c.stage_h);

        oa::runtime::FrameInput idle;
        bool title = false;
        rt.interpreter().on_step = [&](const std::string&, size_t,
                                       const oa::runtime::Instruction& i) {
            const std::string* f = i.get("function");
            if (f && *f == "title_init") title = true;
        };
        bool at_title = false;
        for (size_t f = 0; f < 80000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (!title) continue;
            const auto* w = rt.current_wait();
            if (w && w->kind == oa::runtime::WaitReason::Kind::Stop &&
                w->id.empty()) {
                at_title = true;
                break;
            }
        }
        std::printf("[bt91] title parked: %d (exit=%d)\n", (int)at_title,
                    (int)rt.exit_requested());
        for (size_t f = 0; f < 60; ++f) rt.tick(16, idle);
        if (!at_title) {
            auto ks = list_keys(rt);
            std::printf("[bt91] pre-title screen; click keys: %zu\n",
                        ks.size());
            for (const auto& k : ks)
                std::printf("[bt91]   key '%s' center=(%d,%d) %gx%g\n",
                            k.key.c_str(), k.cx, k.cy, k.w, k.h);
        }

        const char* load_name = std::getenv("OA_BT91_LOAD");
        const char* prekeys = std::getenv("OA_BT91_PRE");
        auto nav_once = [&](const std::string& key, int) {
            for (int f = 0; f < 4000; ++f) {
                rt.tick(16, idle);
                if (rt.exit_requested()) return false;
                for (const auto& v : list_keys(rt))
                    if (v.key == key) return click_key(rt, key);
            }
            return false;
        };
        if (prekeys && *prekeys) {
            std::string buf = prekeys;
            size_t a = 0;
            for (;;) {
                size_t b = buf.find(',', a);
                std::string k = buf.substr(a, b == std::string::npos ? buf.size() - a : b - a);
                if (!k.empty()) {
                    std::printf("[bt91] nav click '%s'\n", k.c_str());
                    nav_once(k, 200);
                }
                if (b == std::string::npos) break;
                a = b + 1;
            }
        }
        bool started = false;
        if (load_name && *load_name) {
            const bool ok = rt.load_game_from(load_name, -1);
            std::printf("[bt91] load %s ok=%d\n", load_name, (int)ok);
            started = ok;
            if (ok && std::getenv("OA_BT91_LUABLOG_IMMED")) {
                for (size_t q = 0; q < 240; ++q) rt.tick(16, idle);
                std::printf("[bt91] lua adv_backlog (immed)\n");
                rt.interpreter().lua_bridge().call_plain("adv_backlog");
                for (size_t q = 0; q < 120; ++q) rt.tick(16, idle);
                if (is_blog_open(rt)) {
                    dump_blog(c, "blog");
                    std::printf("[bt91] DONE after immed blog dump\n");
                    return 0;
                }
                std::printf("[bt91] immed backlog not open; walk on\n");
                started = false;
            }
        } else {
            const char* start_key = kw ? kw : "bt_start";
            started = click_key(rt, start_key);
            if (started && kw) {
                for (size_t f = 0; f < 240; ++f) rt.tick(16, idle);
                auto ks = list_keys(rt);
                std::printf("[bt91] keys after '%s':\n", start_key);
                for (const auto& k : ks)
                    std::printf("[bt91]   key '%s' center=(%d,%d)\n", k.key.c_str(), k.cx, k.cy);
                if (std::getenv("OA_BT91_DBGKEYS")) return 0;
            }
            for (size_t f = 0; f < 300 && !started; ++f) {
                rt.tick(16, idle);
                if (any_story_text(c, "")) started = true;
            }
        }
        if (!started) {
            auto ks = list_keys(rt);
            std::printf("[bt91] no start/load; available:\n");
            for (const auto& k : ks)
                std::printf("[bt91]   key '%s' center=(%d,%d)\n", k.key.c_str(), k.cx, k.cy);
            return 0;
        }
        // ---- story walk ----
        std::string sample = "-";
        std::string sample_prev = "-";
        int parks = 0;
        bool parked_prev = false;
        bool f8_sent = false;
        bool blog_dumped = false;
        for (size_t f = 0; f < 4000000 && parks < pages_budget &&
                            !rt.exit_requested();
             ++f) {
            rt.tick(16, idle);
            if (f8_sent && !blog_dumped) {
                if (is_blog_open(rt)) {
                    std::printf("[bt91] BACKLOG OPEN f=%zu\n", f);
                    for (size_t s = 0; s < 30; ++s) rt.tick(16, idle);
                    dump_blog(c, "blog");
                    blog_dumped = true;
                    std::printf("[bt91] DONE after blog dump\n");
                    return 0;
                }
                if (f8_sent && f > f8_sent + size_t(f8_ticks)) {
                    std::printf("[bt91] backlog did not open in %d ticks\n",
                                f8_ticks);
                    // fall through: keep walking
                    f8_sent = 0;
                }
            }
            const auto* w = rt.current_wait();
            const bool text_now = any_story_text(c, "");
            const bool parked = clickable(w) && text_now && sample_prev ==
                                sample; // stable page text while revealing
            (void)parked;
            // park detection: clickable wait + complete text, sample stable
            std::string smp = story_sample(c);
            const bool stable_text = smp != "-" && smp == sample;
            sample = smp;
            bool full = false;
            for (const std::string& id :
                 rt.text().visible_content_layers()) {
                const oa::render::MessageLayer* ml = rt.text().layer(id);
                if (ml && ml->char_count > 0 && !ml->reveal_pending &&
                    ml->reveal_index >= ml->char_count)
                    full = true;
            }
            const bool page_park = clickable(w) && full && stable_text &&
                                   sample != "-";
            if (page_park && !parked_prev) {
                sample_prev = sample;
                ++parks;
                std::printf("[bt91] park %2d wait=%s sample=%s\n", parks,
                            wk(w), sample.c_str());
                if (parks == pages_budget) {
                    if (std::getenv("OA_BT91_LUABLOG")) {
                        std::printf("[bt91] lua adv_backlog at f=%zu\n", f);
                        rt.interpreter().lua_bridge().call_plain("adv_backlog");
                        for (size_t q = 0; q < 40; ++q) rt.tick(16, idle);
                    } else {
                        std::printf("[bt91] sending F8 (backlog) f=%zu\n", f);
                        oa::runtime::FrameInput k;
                        k.key_down_edges.push_back(119);
                        rt.tick(16, k);
                    }
                    for (size_t q = 0; q < 60; ++q) rt.tick(16, idle);
                    bool opened = is_blog_open(rt);
                    std::printf("[bt91] backlog opened=%d\n", (int)opened);
                    if (opened) {
                        for (size_t q = 0; q < 60; ++q) rt.tick(16, idle);
                        dump_blog(c, "blog");
                        std::printf("[bt91] DONE after blog dump\n");
                        return 0;
                    }
                }
                // mw-face (story avatar) inventory: layers carrying a face
                // mask / face container ids (mw.bb.* so-class / *.fa ids)
                bool av = false;
                for (const oa::render::Layer* l : rt.scene().draw_order()) {
                    const auto mk = l->props.find("intermediate_render_mask");
                    const bool masked = mk != l->props.end() &&
                                        mk->second.find("maskface") != std::string::npos;
                    if (l->id.find(".mw.bb.") != std::string::npos || masked ||
                        (l->id.find(".fa") != std::string::npos &&
                         l->id.find(".fa.") != std::string::npos)) {
                        if (!av) {
                            av = true;
                            std::printf("[bt91] -- mw avatar page --\n");
                        }
                        dump_subtree(c, "mwface", l->id, true);
                    }
                }
                if (av) std::printf("[bt91] -- end avatar --\n");
                parked_prev = true;
                if (parks >= pages_budget) continue;
                const double cx = c.stage_w / 2.0;
                const double cy = c.stage_h - 150.0;
                oa::runtime::FrameInput cl;
                cl.left_click_edge = true;
                cl.left_down = true;
                cl.mouse_x = int(cx);
                cl.mouse_y = int(cy);
                rt.tick(16, cl);
                for (size_t q = 0; q < 8; ++q) rt.tick(16, idle);
            } else if (!page_park) {
                parked_prev = false;
            }
            if (parks >= 3 && f % 12000 == 0) {
                const auto* w2 = rt.current_wait();
                std::printf("[bt91] tick %zu wait=%s sample='%s' parks=%d\n",
                            f, wk(w2), sample.c_str(), parks);
            }
        }
        std::printf("[bt91] FIN parks=%d exit=%d\n", parks,
                    (int)rt.exit_requested());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[bt91] exception: %s\n", e.what());
        return 1;
    }
}
