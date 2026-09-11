// R7 acceptance (QA-queue U1/U2 re-verification): drive the real fpm/root.pfs
// headless deep into the story — title -> start -> 55+ page turns — and lock
// the standing-figure (fg) and message-window face-icon (avatar, mw.100)
// geometry to the coordinates FPM derives from the PNG "pos" comments
// (e:loadPngComments; image_fg.lua getfgfilepos). Before the R7 fix the
// engine did not implement that e: call, so every figure/face part fell back
// to world (0,0) / group origin — the "立绘/头像位置不对" class of defects.
// Env: OA_TEST_FPM_PFS (skip 77 when unset). See docs/research/26.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>

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
std::string sample(oa::runtime::GameRuntime& rt, const std::string& id) {
    const oa::render::MessageLayer* ml = rt.text().layer(id);
    if (!ml) return "-";
    for (const auto& u : ml->page)
        if (!u.data.empty()) return u.data.substr(0, 20);
    return "-";
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
bool is_fg_file(const std::string& file) {
    return file.rfind(":fg/", 0) == 0; // standing-figure part files
}
bool is_mw_face_part_file(const std::string& file) {
    return file.rfind(":fa/", 0) == 0; // mw face-icon ("fa") part files
}
// layer id -> world origin of its local (0,0)
bool world_origin(oa::runtime::GameRuntime& rt, const std::string& id,
                  double* ox, double* oy) {
    oa::render::Affine2 t;
    if (!rt.scene().world_transform(id, &t)) return false;
    t.transform_point(0, 0, ox, oy);
    return true;
}
bool near(double a, double b) { return std::fabs(a - b) < 0.6; }
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
        for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (!title) continue;
            const auto* w = rt.current_wait();
            if (w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty()) break;
        }
        check(title, "R7-1 boot parked at the title");
        {
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
        bool body = false;
        for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if (rt.text().page_has_visible_text("1.80.mw.adv_adv")) {
                body = true;
                break;
            }
        }
        check(body, "R7-2 story body text reached after the start click");

        auto find_any = [&](bool (*pred)(const std::string&),
                            const std::string& skip_prefix, std::string* id,
                            std::string* file) {
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                if (l->file.empty() || !pred(l->file)) continue;
                if (!skip_prefix.empty() && l->id.rfind(skip_prefix, 0) == 0) continue;
                *id = l->id;
                *file = l->file;
                return true;
            }
            return false;
        };
        size_t clicks = 0;
        int pages_changed = 0;
        std::string last_page;
        bool fg_seen = false, av_seen = false;
        std::string fg_leaf_id, fg_face_id;
        std::string fg_file_base, fg_file_face;
        std::string av_base_id, av_face_id, av_file_base, av_file_face;
        double fg_base_x = 0, fg_base_y = 0, fg_face_x = 0, fg_face_y = 0;
        double av_base_x = 0, av_base_y = 0, av_face_x = 0, av_face_y = 0;
        bool nameplate_seen = false;
        double np_x = 0, np_y = 0;
        size_t stuck = 0;
        std::set<std::string> fg_bases, av_bases;
        // ---- deep walk: clickable parks, click to turn pages ----
        while (clicks < 300 && !rt.exit_requested()) {
            bool parked = false;
            for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
                rt.tick(16, idle);
                if (rt.transition().is_in_progress(rt.now_ms())) continue;
                const auto* w = rt.current_wait();
                if (is_clickable_wait(w) && reveal_done(rt)) {
                    parked = true;
                    break;
                }
            }
            if (!parked) {
                if (++stuck >= 5) break;
                continue;
            }
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                if (l->file.empty()) continue;
                if (is_fg_file(l->file) && l->file.find("/no/") != std::string::npos &&
                    l->id.rfind("1.80.mw.", 0) != 0)
                    fg_bases.insert(l->file);
                if (is_mw_face_part_file(l->file)) av_bases.insert(l->file);
            }
            if (!fg_seen) {
                std::string id2, f2;
                if (find_any(is_fg_file, "1.80.mw.", &id2, &f2)) {
                    fg_seen = true;
                    fg_leaf_id = id2;
                    fg_file_base = f2;
                    // base leaf = last "...b.N" node; its own png carries the
                    // figure's stage pos comment ("pos,438,69,.." for
                    // hiy_nob0500); a sibling part (the face) usually exists.
                    world_origin(rt, id2, &fg_base_x, &fg_base_y);
                    const size_t dot = id2.rfind('.');
                    const std::string parts_root = id2.substr(0, dot);
                    for (const oa::render::Layer* l : rt.scene().draw_order()) {
                        if (l->id.rfind(parts_root + ".", 0) != 0) continue;
                        if (l->file == fg_file_base) continue;
                        fg_face_id = l->id;
                        fg_file_face = l->file;
                        world_origin(rt, l->id, &fg_face_x, &fg_face_y);
                        break;
                    }
                    std::printf("[r7] FG leaf %s '%s' at (%.1f,%.1f) face %s '%s' "
                                "at (%.1f,%.1f)\n",
                                fg_leaf_id.c_str(), fg_file_base.c_str(), fg_base_x,
                                fg_base_y, fg_face_id.c_str(), fg_file_face.c_str(),
                                fg_face_x, fg_face_y);
                }
            }
            if (!av_seen) {
                std::string id2, f2;
                if (find_any(is_mw_face_part_file, "", &id2, &f2)) {
                    av_seen = true;
                    av_base_id = id2;
                    av_file_base = f2;
                    world_origin(rt, id2, &av_base_x, &av_base_y);
                    const size_t dot = id2.rfind('.');
                    const std::string parts_root = id2.substr(0, dot);
                    for (const oa::render::Layer* l : rt.scene().draw_order()) {
                        if (l->id.rfind(parts_root + ".", 0) != 0) continue;
                        if (l->file == av_file_base) continue;
                        av_face_id = l->id;
                        av_file_face = l->file;
                        world_origin(rt, l->id, &av_face_x, &av_face_y);
                        break;
                    }
                    std::printf("[r7] AV base %s '%s' at (%.1f,%.1f) face %s '%s' "
                                "at (%.1f,%.1f)\n",
                                av_base_id.c_str(), av_file_base.c_str(), av_base_x,
                                av_base_y, av_face_id.c_str(), av_file_face.c_str(),
                                av_face_x, av_face_y);
                    // nameplate image layer on the same parked page
                    for (const oa::render::Layer* l : rt.scene().draw_order()) {
                        if (l->id == "1.80.mw.1" && !l->file.empty()) {
                            nameplate_seen = true;
                            world_origin(rt, l->id, &np_x, &np_y);
                            break;
                        }
                    }
                }
            }
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = 640;
            cl.mouse_y = 600;
            rt.tick(16, cl);
            ++clicks;
            const std::string cur = sample(rt, "1.80.mw.adv_adv");
            if (cur != last_page) {
                last_page = cur;
                ++pages_changed;
            }
            if (pages_changed >= 55 && clicks > 30 && fg_seen && av_seen) break;
        }
        check(fg_seen, "R7-3 standing-figure layers appear in the story");
        check(av_seen, "R7-4 message-window face icon appears in the story");
        check(pages_changed >= 55 && stuck == 0,
              "R7-5 deep page walk (55+ turns) with no stuck park "
              "(pages=%d clicks=%zu stuck=%zu)",
              pages_changed, clicks, stuck);
        std::printf("[r7] walk: clicks=%zu pages_changed=%d stuck=%zu fg_bases=%zu "
                    "av_bases=%zu sample='%s'\n",
                    clicks, pages_changed, stuck, fg_bases.size(), av_bases.size(),
                    sample(rt, "1.80.mw.adv_adv").c_str());
        // ---- geometry locks (data == FPM png comment placement) ----
        // fg hiy_nob0500 base: comment "pos,438,69,400,1144" (image_fg.lua
        // fg: base group at z.x,z.y); face b0055 comment (579,176).
        check(fg_file_base.find("hiy_nob0500") != std::string::npos &&
                  near(fg_base_x, 438.0) && near(fg_base_y, 69.0),
              "R7-6 figure base placed at its PNG-comment pos (438,69), got "
              "'%s' (%.1f,%.1f)",
              fg_file_base.c_str(), fg_base_x, fg_base_y);
        check(!fg_face_id.empty() && near(fg_face_x, 579.0) && near(fg_face_y, 176.0),
              "R7-7 figure face part placed at its own comment pos (579,176), got "
              "'%s' (%.1f,%.1f)",
              fg_file_face.c_str(), fg_face_x, fg_face_y);
        // mw face icon: composite group 1.80.mw.100 at csv.mw.face (0,420);
        // part hiy_noa0500 fa comment (39,34) -> world (39,454); a0035 at
        // (101,153) -> (101,573).
        check(av_file_base.find("hiy_noa0500") != std::string::npos &&
                  near(av_base_x, 39.0) && near(av_base_y, 454.0),
              "R7-8 avatar base part at icon-comment pos (39,454), got '%s' "
              "(%.1f,%.1f)",
              av_file_base.c_str(), av_base_x, av_base_y);
        check(!av_face_id.empty() && near(av_face_x, 101.0) && near(av_face_y, 573.0),
              "R7-9 avatar face part at (101,573), got '%s' (%.1f,%.1f)",
              av_file_face.c_str(), av_face_x, av_face_y);
        // nameplate (name images under pc/ja/mw/name) next to the icon box
        check(nameplate_seen && near(np_x, 326.0) && near(np_y, 507.0),
              "R7-10 nameplate image at csv.mw.name (326,507), got (%.1f,%.1f)",
              np_x, np_y);
        check(fg_bases.size() >= 5 && av_bases.size() >= 5,
              "R7-11 expression/outfit switching observed over the walk "
              "(figure=%zu avatar=%zu)",
              fg_bases.size(), av_bases.size());
        std::printf("char_visual_probe: all ok (failures=%d last_fail='%s')\n",
                    failures, g_last_fail);
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
}
