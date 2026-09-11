// Diagnostic probe (gt game): drive boot -> title -> はじめから -> story body,
// then dump the message-layer font params and layout geometry so the text
// renderer can be compared against the game's expected font table
// (adv01: size=35 left=360 top=550 width=740, color 0xffffff, outline+shadow).
// Env: OA_TEST_FPM_PFS (skip code 77 when unset).
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"
#include "core/render/font.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool approx(double a, double b, double eps = 1e-9) {
    const double d = a - b;
    return d > -eps && d < eps;
}
const char* wait_str(const oa::runtime::WaitReason* w) {
    if (!w) return "none";
    using K = oa::runtime::WaitReason::Kind;
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
        bool saw_title = false;
        rt.interpreter().on_step = [&](const std::string&, size_t,
                                       const oa::runtime::Instruction& i) {
            const std::string* f = i.get("function");
            if (f && *f == "title_init") saw_title = true;
        };
        std::string target;
        // boot to title
        for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            if ((f % 1200) == 0) {
                std::printf("[probe] f=%zu title_init=%s wait=%s layers=%zu\n", f,
                            saw_title ? "yes" : "no", wait_str(rt.current_wait()),
                            rt.scene().size());
            }
            if (!saw_title) continue;
            const auto* w = rt.current_wait();
            if (!(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty())) continue;
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                const auto* h = rt.scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto k = h->params.find("key");
                if (k == h->params.end() || k->second != "bt_start") continue;
                target = l->id;
                break;
            }
            if (!target.empty()) break;
        }
        std::printf("[probe] title target=%s\n", target.c_str());
        if (target.empty()) {
            std::printf("[probe] verdict: title start button not found\n");
            return 1;
        }
        const oa::render::Layer* pl = rt.scene().find(target);
        double w = pl && pl->has_clip ? pl->clip_w : 0;
        double h = pl && pl->has_clip ? pl->clip_h : 0;
        if ((w <= 0 || h <= 0) && pl) {
            w = pl->width > 0 ? pl->width : w;
            h = pl->height > 0 ? pl->height : h;
        }
        if (w <= 0 || h <= 0) { w = 160; h = 48; }
        auto center = [&](int* cx, int* cy) -> bool {
            double x0 = 0, y0 = 0, rw = 0, rh = 0;
            if (!rt.scene().world_rect(target, w, h, &x0, &y0, &rw, &rh)) return false;
            *cx = int(x0 + rw / 2);
            *cy = int(y0 + rh / 2);
            return true;
        };
        // settle
        double last_x = -1e9;
        size_t quiet = 0;
        for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const auto* wq = rt.current_wait();
            if (!(wq && wq->kind == oa::runtime::WaitReason::Kind::Stop && wq->id.empty())) continue;
            if (rt.transition().is_in_progress(rt.now_ms())) continue;
            double x0 = 0, y0 = 0, rw = 0, rh = 0;
            if (!rt.scene().world_rect(target, w, h, &x0, &y0, &rw, &rh)) continue;
            if (x0 < 0) { last_x = x0; quiet = 0; continue; }
            quiet = (x0 - last_x < 0.5 && last_x - x0 < 0.5) ? quiet + 1 : 0;
            last_x = x0;
            if (quiet >= 150) break;
        }
        // click start
        bool first = true;
        bool title_gone = false;
        for (size_t f = 0; f < 800 && !rt.exit_requested(); ++f) {
            oa::runtime::FrameInput in;
            if (first) {
                (void)center(&in.mouse_x, &in.mouse_y);
                in.left_click_edge = true;
                in.left_down = true;
                first = false;
            }
            rt.tick(16, in);
            if (rt.scene().find("500") == nullptr) title_gone = true;
            if (title_gone) break;
        }
        std::printf("[probe] title_gone=%s\n", title_gone ? "yes" : "no");
        // wait for body text
        bool reached = false;
        std::string layer_id;
        for (size_t f = 0; f < 6000 && !rt.exit_requested() && !reached; ++f) {
            rt.tick(16, idle);
            for (const std::string& id : rt.text().visible_content_layers()) {
                if (!rt.text().page_has_visible_text(id)) continue;
                // skip the name/measure layers: want the main adv layer
                layer_id = id;
                reached = true;
                break;
            }
        }
        if (!reached) {
            std::printf("[probe] verdict: body text not reached\n");
            return 1;
        }
        // dump every content layer's font + geometry + layout
        const oa::render::TextEngine& te = rt.text();
        std::printf("[probe] ===== text state dump =====\n");
        for (const std::string& id : te.visible_content_layers()) {
            const oa::render::MessageLayer* ml = te.layer(id);
            if (!ml || ml->page.empty()) continue;
            std::printf("[probe] layer id=%s layered=%d left=%.1f top=%.1f width=%.1f height=%.1f "
                        "reveal=%llu chars=%zu\n",
                        id.c_str(), ml->layered ? 1 : 0, ml->left, ml->top, ml->width, ml->height,
                        (unsigned long long)ml->reveal_index, ml->char_count);
            std::string sample;
            for (const auto& u : ml->page)
                if (!u.data.empty()) { sample = u.data.substr(0, 24); break; }
            std::printf("[probe]   sample='%s'\n", sample.c_str());
            // page-unit font params (first text unit)
            for (const auto& u : ml->page) {
                if (u.kind == oa::render::PageUnit::Kind::Newline || u.data.empty()) continue;
                std::printf("[probe]   unit font raw:\n");
                for (const auto& [k, v] : u.font.raw)
                    std::printf("[probe]     %s = '%s'\n", k.c_str(), v.c_str());
                break;
            }
        }
        std::printf("[probe] default font raw:\n");
        for (const auto& [k, v] : te.default_font().raw)
            std::printf("[probe]   %s = '%s'\n", k.c_str(), v.c_str());

        // ------------------------------------------------------------------
        // Hard font-table assertions: the FPM adv01 message box applies its
        // font-table configuration (header comment: size=35 left=360 top=550
        // width=740 color 0xffffff outline+shadow) through the engine's font
        // pipeline. Anchor on the *applied result*, not internal mechanics:
        // (a) the body adv layer carries the table's font params verbatim in
        // the page-unit font raw map, (b) the engine parsed size numerically
        // and routed left/top/width/height into the message-layer geometry,
        // (c) the boot-time default face resolved to the real font file.
        // ------------------------------------------------------------------
        std::string adv_id;
        const oa::render::MessageLayer* adv = nullptr;
        const oa::render::PageUnit* adv_unit = nullptr;
        for (const std::string& id : te.visible_content_layers()) {
            const oa::render::MessageLayer* ml = te.layer(id);
            if (!ml || ml->page.empty()) continue;
            for (const auto& u : ml->page) {
                if (u.kind == oa::render::PageUnit::Kind::Newline || u.data.empty()) continue;
                auto raw_val = [&](const char* k) -> const std::string* {
                    const auto it = u.font.raw.find(k);
                    return it == u.font.raw.end() ? nullptr : &it->second;
                };
                const std::string* size = raw_val("size");
                const std::string* width = raw_val("width");
                if (size && *size == "35" && width && *width == "740") {
                    adv_id = id;
                    adv = ml;
                    adv_unit = &u;
                }
                break; // first text unit per layer only
            }
            if (adv) break;
        }
        std::printf("[probe] adv01 layer id=%s\n", adv_id.empty() ? "(none)" : adv_id.c_str());
        check(!adv_id.empty(), "F1 body adv01 layer found (font-table size=35/width=740)");
        if (adv && adv_unit) {
            const auto raw_of = [&](const char* k) -> std::string {
                const auto it = adv_unit->font.raw.find(k);
                return it == adv_unit->font.raw.end() ? std::string() : it->second;
            };
            check(raw_of("size") == "35", "F2 adv01 font size param 35 preserved");
            check(raw_of("left") == "360" && raw_of("top") == "550" &&
                      raw_of("width") == "740",
                  "F3 adv01 window geometry params preserved (left=360 top=550 width=740)");
            check(raw_of("color") == "16777215",
                  "F4 adv01 text color 0xffffff (16777215) preserved");
            check(raw_of("style") == "outline,shadow",
                  "F5 adv01 outline+shadow style preserved");
            const std::string face = raw_of("face");
            constexpr const char* kFaceFile = "font/sourcehansans-medium.otf";
            constexpr size_t kFaceSuffix = 24; // "sourcehansans-medium.otf"
            check(face == kFaceFile ||
                      (face.size() >= kFaceSuffix &&
                       face.compare(face.size() - kFaceSuffix, kFaceSuffix,
                                    kFaceFile + 5) == 0),
                  "F6 adv01 face resolved to sourcehansans-medium.otf");
            check(approx(adv_unit->font.size(), 35.0), "F7 engine parsed size 35 numerically");
            check(approx(adv->left, 360.0) && approx(adv->top, 550.0) &&
                      approx(adv->width, 740.0) && approx(adv->height, 500.0),
                  "F8 font left/top/width/height routed into the message-layer geometry");
            check(adv_unit->font.raw.count("outlinecolor") == 1 &&
                      adv_unit->font.raw.count("shadowcolor") == 1,
                  "F9 outline/shadow colors carried for the style pass");
        }
        const auto& dface = te.default_font().raw.find("face");
        constexpr const char* kFaceFile = "font/sourcehansans-medium.otf";
        constexpr size_t kFaceSuffix = 24; // "sourcehansans-medium.otf"
        check(dface != te.default_font().raw.end() &&
                  (dface->second == kFaceFile ||
                   (dface->second.size() >= kFaceSuffix &&
                    dface->second.compare(dface->second.size() - kFaceSuffix,
                                          kFaceSuffix, kFaceFile + 5) == 0)),
              "F10 boot default font face resolved to sourcehansans-medium.otf");
        // ------------------------------------------------------------------
        // Real-font metric locks (R3 visual fix, research/22 §3): the engine
        // must rasterize/measure at the  ab_glyph-equivalent scale —
        // ppem(size) = round(size·upem/(hhea_asc−hhea_desc)); for this font
        // (upem=1000, asc=963, desc=−347 → H=1310) size 35 → ppem 27 and a
        // full-width CJK advance ≈ 27 px (the pre-fix engine measured at
        // nominal ppem 35 + kerning −2 → 33 px, which is what the real
        // machine showed as oversized text). Glyph vertical placement = the
        // ascent/bearing formula used by draw (offset_y = ascent−bitmap_top).
        // ------------------------------------------------------------------
        if (adv && adv_unit) {
            oa::render::FontSystem fsys(fs.get(), &rt);
            const oa::render::FontDesc& af = adv_unit->font;
            const uint32_t cp_a = 0x3042; // あ
            const oa::render::FontSystem::GlyphMeasure gm = fsys.measure_glyph(af, cp_a);
            std::printf("[probe] adv glyph measure ppem=%.0f advance=%.2f ink=%.1fx%.1f "
                        "ascent=%.2f off=%.1f/%.1f\n",
                        gm.ppem, gm.advance, gm.ink_w, gm.ink_h, gm.ascent, gm.offset_x,
                        gm.offset_y);
            check(gm.ppem == 27.0,
                  "F11 raster ppem = round(35*1000/1310) = 27 (ab_glyph ratio scale)");
            check(gm.advance >= 26.0 && gm.advance <= 27.6,
                  "F12 full-width CJK advance ≈ 27 px at size 35 (was ~35+kerning)");
            check(gm.ink_w >= 20.0 && gm.ink_h >= 20.0, "F12b kana ink box present");
            check(gm.ascent >= 25.0 && gm.ascent <= 26.6,
                  "F12c scaled ascent = size*963/1310 ≈ 25.7 (FT ascender@ppem27)");
            check(gm.offset_y >= 0.0 && gm.offset_y <= gm.ascent,
                  "F12d glyph row offset = ascent − bitmap_top in [0, ascent]");
            // 布局行高 = 字体表字段和（text_line_metrics）；adv01 表 →
            // 0 + 14 − 12 + 35 − 6 = 31
            const double expected_lh = af.spacetop() + std::max(0.0, af.rubysize()) +
                                       af.spacemiddle() + af.size() + af.spacebottom();
            const oa::render::LaidPage page = oa::render::layout_page(
                *adv,
                [](void* u, const oa::render::FontDesc& f, uint32_t cp)
                    -> oa::render::CharMetrics {
                    return static_cast<oa::render::FontSystem*>(u)->metrics_for(f, cp);
                },
                rt.text().layout_config(), &fsys);
            std::printf("[probe] adv layout line_height=%.2f expected=%.2f glyphs=%zu\n",
                        page.line_height, expected_lh, page.glyphs.size());
            check(page.line_height > 0 &&
                      approx(page.line_height, expected_lh, 1e-9) &&
                      approx(expected_lh, 31.0, 1e-9),
                  "F13 adv01 line height = spacetop+ruby+spacemiddle+size+spacebottom = 31");
            // 页内 CJK 字形步进走真实比例度量（<30px；修复前为 ~35）
            if (page.glyphs.size() >= 2) {
                bool cjk_ok = true;
                double max_adv = 0;
                for (const auto& g : page.glyphs) {
                    if (g.newline) continue;
                    if (g.advance > max_adv) max_adv = g.advance;
                    if (g.advance <= 0 || g.advance >= 30.0) cjk_ok = false;
                }
                check(cjk_ok && max_adv > 20.0,
                      "F14 real-metric layout advances in (20,30) px (not ~35/33)");
            }
        }
        std::printf("[probe] verdict: dumped layer=%s\n", layer_id.c_str());
        if (failures) {
            std::fprintf(stderr, "text_layout_probe: %d failure(s)\n", failures);
            return 1;
        }
        std::printf("text_layout_probe: all ok\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
}
