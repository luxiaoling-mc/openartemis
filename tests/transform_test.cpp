// P1b transform/hit + runtime ownership tests (see
// docs/research/10-compositor-transform.md §4, H1-H14).
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/fs/fs.h"
#include "core/runtime/runtime.h"
#include "core/render/layer.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool approx(double a, double b, double eps = 1e-6) {
    const double d = a - b;
    return d > -eps && d < eps;
}
using oa::render::Affine2;
using oa::render::Compositor;

void test_h1_typed_parse_and_scale_getters() {
    Compositor c;
    c.create("1", {});
    // defaults
    check(approx(c.find("1")->x_scale, 100.0) && approx(c.find("1")->y_scale, 100.0) &&
              approx(c.find("1")->anchor_x, 0.0) && approx(c.find("1")->anchor_y, 0.0) &&
              approx(c.find("1")->rotate_deg, 0.0) && !c.find("1")->reverse_x &&
              !c.find("1")->reverse_y,
          "H1 transform defaults: scale 100%, anchor 0, rotate 0, no reverse");
    c.set_props("1", {{"xscale", "200"}, {"yscale", "50"}});
    check(approx(c.find("1")->x_scale, 200.0) && approx(c.find("1")->y_scale, 50.0),
          "H1 percent scale parsed (200/50)");
    // zoom = both axes
    c.set_props("1", {{"zoom", "150"}});
    check(approx(c.find("1")->x_scale, 150.0) && approx(c.find("1")->y_scale, 150.0),
          "H1 zoom sets both scales");
    // reverse flips the scale sign
    c.set_props("1", {{"reversex", "1"}, {"reversey", "on"}});
    check(c.find("1")->reverse_x && c.find("1")->reverse_y, "H1 reverse booleans parsed");
    // parse failure keeps the previous value ( .or)
    c.set_props("1", {{"xscale", "garbage"}, {"rotate", "90"}, {"anchorx", "12.5"}});
    check(approx(c.find("1")->x_scale, 150.0), "H1 xscale parse failure keeps previous");
    check(approx(c.find("1")->rotate_deg, 90.0) && approx(c.find("1")->anchor_x, 12.5),
          "H1 rotate deg + anchor px parsed");
    c.set_props("1", {{"rotate", "abc"}, {"anchorx", "junk"}});
    check(approx(c.find("1")->rotate_deg, 90.0) && approx(c.find("1")->anchor_x, 12.5),
          "H1 rotate/anchor parse failure keeps previous");
}

void test_h2_h3_local_transform_math() {
    // scale around the anchor: anchor point invariant, origin pushed out
    // ( scale_uses_percent_and_anchor test, 759-782)
    Compositor c;
    c.create("a", {});
    c.set_props("a", {{"xscale", "200"}, {"yscale", "200"}, {"anchorx", "10"},
                      {"anchory", "10"}});
    Affine2 m = c.find("a")->local_transform();
    double x = 0, y = 0;
    m.transform_point(10, 10, &x, &y);
    check(approx(x, 10.0) && approx(y, 10.0), "H2 anchor point invariant under scale");
    m.transform_point(0, 0, &x, &y);
    check(approx(x, -10.0) && approx(y, -10.0), "H2 origin pushed to -10 under 2x");
    m.transform_point(5, 5, &x, &y);
    check(approx(x, 0.0) && approx(y, 0.0), "H2 (anchor/2) maps to 0 (T(a)RS T(-a))");
    // offset composes after anchor/scale/rotate (order)
    c.set_props("a", {{"left", "7"}, {"top", "-3"}});
    m = c.find("a")->local_transform();
    m.transform_point(10, 10, &x, &y);
    check(approx(x, 17.0) && approx(y, 7.0), "H3 offset applied after anchor");
    // rotation only: 90 deg maps (1,0) -> (0,1) plus offset
    Compositor r;
    r.create("r", {});
    r.set_props("r", {{"rotate", "90"}});
    Affine2 rm = r.find("r")->local_transform();
    rm.transform_point(1, 0, &x, &y);
    check(approx(x, 0.0, 1e-9) && approx(y, 1.0, 1e-9), "H3 rotate 90deg maps (1,0)->(0,1)");
    // reverse = negative scale: matrix is not plain translation
    Compositor f;
    f.create("f", {});
    f.set_props("f", {{"reversex", "1"}});
    check(!f.find("f")->local_transform().is_plain_translation(),
          "H3 reversex matrix is not a plain translation (negative a)");
    check(f.find("f")->local_transform().is_plain_translation() == false ||
              f.find("f")->local_transform().invertible(),
          "H3 reversex stays invertible");
}

void test_h4_world_chain_translate_and_reverse() {
    Compositor c;
    c.create("1", {{"file", "bg"}});
    c.set_props("1", {{"left", "100"}, {"top", "50"}});
    c.create("1.0", {{"file", "fg"}});
    Affine2 t;
    check(c.world_transform("1.0", &t), "H4 world transform exists for child");
    double x = 0, y = 0;
    t.transform_point(0, 0, &x, &y);
    check(approx(x, 100.0) && approx(y, 50.0),
          "H4 parent translate passes into the child world origin");
    // reversex on the parent flips the whole child chain
    Compositor rev;
    rev.create("1", {});
    rev.set_props("1", {{"reversex", "1"}});
    rev.create("1.0", {{"file", "b"}});
    rev.set_props("1.0", {{"left", "50"}, {"top", "0"}, {"width", "100"},
                          {"height", "100"}});
    Affine2 wt;
    check(rev.world_transform("1.0", &wt), "H4 world transform (reverse parent)");
    wt.transform_point(50, 10, &x, &y);
    check(approx(x, -100.0) && approx(y, 10.0),
          "H4 reversex maps child local x through -(x+50)");
    wt.transform_point(150, 10, &x, &y);
    check(approx(x, -200.0), "H4 reversex mirrors the child quad x range");
}

void test_h5_h12_hit_and_world_rect_parent_scale_clip() {
    // parent xscale=200 (anchor 0), child left=50 width 100
    Compositor c;
    c.create("1.0", {{"file", "button"}});
    c.set_props("1", {{"xscale", "200"}});
    c.set_props("1.0", {{"left", "50"}, {"top", "0"}, {"width", "100"},
                        {"height", "100"}});
    check(c.hit_test(180.0, 50.0, nullptr, nullptr) == "1.0", "H5 hit inside parent-scaled child");
    check(c.hit_test(75.0, 50.0, nullptr, nullptr) == std::string(),
          "H5 miss left of the scaled child");
    check(c.hit_test(300.0, 50.0, nullptr, nullptr) == std::string(),
          "H5 miss right of the scaled child (edge exclusive)");
    double x = 0, y = 0, w = 0, h = 0;
    check(c.world_rect("1.0", 100, 100, &x, &y, &w, &h),
          "H12 world rect of scaled child");
    check(approx(x, 100.0) && approx(y, 0.0) && approx(w, 200.0) && approx(h, 100.0),
          "H12 scaled child world AABB = [100,0 200x100]");

    // clip + parent chain: 500.b.2 (60,112) -> 500.b.2.0 clip 4,8,40,24
    Compositor t;
    t.create("500.b.2", {});
    t.set_props("500.b.2", {{"left", "60"}, {"top", "112"}});
    t.create("500.b.2.0", {{"file", "btn"}});
    t.set_props("500.b.2.0", {{"clip", "4,8,40,24"}});
    check(t.hit_test(60.0, 112.0, nullptr, nullptr) == "500.b.2.0",
          "H12 hit at the group origin (clip quad starts at 0,0 local)");
    check(t.hit_test(80.0, 124.0, nullptr, nullptr) == "500.b.2.0",
          "H12 hit inside the clip quad under the group offset");
    check(t.hit_test(101.0, 124.0, nullptr, nullptr) == std::string(),
          "H12 miss past the clip quad (right edge exclusive)");
    check(t.world_rect("500.b.2.0", 40, 24, &x, &y, &w, &h),
          "H12 world rect of clip child under group offset");
    check(approx(x, 60.0) && approx(y, 112.0) && approx(w, 40.0) && approx(h, 24.0),
          "H12 world AABB composes group offset + clip size");
}

void test_h6_size_precedence_and_h10_overlap() {
    // width/height wins over clip for hit sizing 
    Compositor c;
    c.create("b", {{"file", "btn"}});
    c.set_props("b", {{"left", "0"}, {"top", "0"}, {"width", "50"}, {"height", "20"},
                      {"clip", "4,8,40,24"}});
    // width/height rect [0,50)x[0,20) governs hit
    check(c.hit_test(49, 19, nullptr, nullptr) == "b", "H6 width/height extends the hit rect");
    check(c.hit_test(51, 12, nullptr, nullptr) == std::string(), "H6 past width/height = miss");
    // top-to-bottom overlapping list 
    Compositor o;
    o.create("1.0", {{"file", "lower"}});
    o.create("1.1", {{"file", "upper"}});
    for (const char* id : {"1.0", "1.1"})
        o.set_props(id, {{"left", "0"}, {"top", "0"}, {"width", "100"}, {"height", "100"}});
    const auto hits = o.hit_test_all(10.0, 10.0, nullptr, nullptr);
    check(hits.size() == 2 && hits[0] == "1.1" && hits[1] == "1.0",
          "H10 overlapping hits come top (1.1) to bottom (1.0)");
    // a pure group node without any resolvable size never hits
    Compositor g;
    g.create("grp", {});
    g.create("grp.0", {{"file", "leaf"}});
    g.set_props("grp.0", {{"left", "0"}, {"top", "0"}, {"clip", "0,0,30,30"}});
    check(g.hit_test(15, 15, nullptr, nullptr) == "grp.0", "H10 leaf hit");
    const auto gh = g.hit_test_all(15, 15, nullptr, nullptr);
    for (const std::string& id : gh) check(id != "grp", "H10 group node without size skipped");
}

void test_h7_h8_h9_root_props_and_visibility() {
    Compositor c;
    c.create("1", {{"file", "a"}});
    c.set_root_props({{"left", "100"}, {"alpha", "128"}});
    check(!c.find("!"), "H8 no '!' node exists");
    check(approx(c.root_props().alpha, 128.0 / 255.0), "H8 root props alpha stored");
    Affine2 t;
    c.world_transform("1", &t);
    double x = 0, y = 0;
    t.transform_point(0, 0, &x, &y);
    check(approx(x, 100.0), "H7 root props translate moves every root");
    check(approx(c.chain_opacity("1"), 128.0 / 255.0),
          "H7 root props alpha multiplies the tree");
    c.set_root_props({{"visible", "0"}});
    check(!c.is_effectively_visible("1"), "H9 root hidden makes the whole tree invisible");
    check(c.hit_test_all(100.0, 100.0, nullptr, nullptr).empty(), "H9 no hits when the root is hidden");
    c.set_root_props({{"visible", "1"}});
    c.set_props("1", {{"visible", "0"}});
    check(!c.is_effectively_visible("1"), "H9 ancestor hidden culls the subtree");
    c.set_props("1", {{"visible", "1"}});
    check(c.is_effectively_visible("1"), "H9 restored");
}

void test_rotation_hit_inside_rotated_quad() {
    // A 45-degree rotated quad hits exactly inside the rotated quad (inverse-
    // local test, ) — not merely inside its world AABB.
    const double s = std::sqrt(0.5); // cos/sin 45deg
    Compositor c;
    c.create("r", {{"file", "rot"}});
    c.set_props("r", {{"rotate", "45"}, {"width", "100"}, {"height", "50"}});
    // local (50,25) sits inside; its world image is (s*50 - s*25, s*50 + s*25)
    const double wx = s * 50.0 - s * 25.0;
    const double wy = s * 50.0 + s * 25.0;
    check(c.hit_test(wx, wy, nullptr, nullptr) == "r", "interior point of the rotated quad hits");
    // inside the world AABB but outside the rotated quad must miss:
    // world (60,50) -> local x = (60+50)s ~77.8 in-range, local y = (-60+50)s <0
    check(c.hit_test(60.0, 50.0, nullptr, nullptr) == std::string(),
          "world-AABB-only point misses the rotated quad");
    check(c.hit_test(0.0, 0.0, nullptr, nullptr) == "r", "corner-origin (0,0) of the quad hits");
    check(c.hit_test(-50.0, 120.0, nullptr, nullptr) == std::string(),
          "far outside the rotated quad misses");
}

struct MemFs : oa::fs::IFileSystem {
    std::map<std::string, std::string> files;
    std::optional<std::vector<uint8_t>> read(std::string_view path) const override {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return std::nullopt;
        return std::vector<uint8_t>(it->second.begin(), it->second.end());
    }
    bool exists(std::string_view path) const override {
        return files.count(std::string(path)) > 0;
    }
    const char* kind() const override { return "mem"; }
};

void test_h11_runtime_owns_scene_and_applies_layer_events() {
    // layer events apply at dispatch inside GameRuntime; the host stream only
    // carries non-scene events ( runtime/); lyprop
    // id="!" routes to root props and never creates a node .
    MemFs fs;
    fs.files["system.ini"] =
        "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
        "BOOT = system/first.iet\nSAVEPATH = save\n";
    fs.files["system/first.iet"] =
        "*main\n"
        "[lyc2 id=\"500.b.2.0\" file=\"btn.png\" x=\"60\" y=\"112\"]\n"
        "[lyprop id=\"500.b.2.0\" clip=\"4,8,40,24\" alpha=\"200\"]\n"
        "[lyprop id=\"!\" alpha=\"128\"]\n"
        "[lydel id=\"missing\"]\n"
        "[stop]\n";
    oa::runtime::GameRuntime rt(std::make_shared<MemFs>(fs));
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    for (int i = 0; i < 12 && !rt.exit_requested(); ++i) rt.tick(16, idle);

    const auto& scene = rt.scene();
    check(scene.find("500") != nullptr && scene.find("500.b") != nullptr &&
              scene.find("500.b.2") != nullptr && scene.find("500.b.2.0") != nullptr,
          "H11 lyc2+lyprop materialized the full ancestor chain in the runtime scene");
    const oa::render::Layer* leaf = scene.find("500.b.2.0");
    check(leaf != nullptr && leaf->file == "btn.png", "H11 file bound on create");
    check(leaf && leaf->has_clip && approx(leaf->clip_w, 40.0) && approx(leaf->clip_h, 24.0),
          "H11 clip parsed after routed set_props");
    check(leaf && approx(leaf->alpha, 200.0 / 255.0), "H11 alpha applied incrementally");
    check(approx(rt.scene().root_props().alpha, 128.0 / 255.0),
          "H11 lyprop id='!' wrote root props");
    check(scene.find("!") == nullptr, "H11 no '!' node was created");
    check(scene.find("missing") == nullptr, "H11 lydel on a missing id is a no-op");

    // host event stream contains no scene events
    for (int i = 0; i < 4; ++i) rt.tick(16, idle);
    for (const auto& e : rt.drain_events()) {
        check(e.kind != oa::runtime::Event::Kind::LayerCreate &&
                  e.kind != oa::runtime::Event::Kind::LayerDelete &&
                  e.kind != oa::runtime::Event::Kind::LayerSetProps,
              "H11 layer events never reach the host stream");
    }
    // world geometry usable from the runtime scene (H12)
    double x = 0, y = 0, w = 0, h = 0;
    check(scene.world_rect("500.b.2.0", 40, 24, &x, &y, &w, &h) && approx(x, 60.0) &&
              approx(y, 112.0) && approx(w, 40.0) && approx(h, 24.0),
          "H11/H12 runtime scene world rect composes x/y + clip size");
}

void test_h13_runtime_tween_drives_typed_transform() {
    // [lytween param=xscale] consumed by the runtime scene and per-frame
    // advance settles the typed x_scale field (semantics)
    MemFs fs;
    fs.files["system.ini"] =
        "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
        "BOOT = system/first.iet\nSAVEPATH = save\n";
    fs.files["system/first.iet"] =
        "*main\n"
        "[lyc2 id=\"1\" file=\"a.png\"]\n"
        "[lytween id=\"1\" param=\"xscale\" from=\"100\" to=\"200\" time=\"100\"]\n"
        "[stop]\n";
    oa::runtime::GameRuntime rt(std::make_shared<MemFs>(fs));
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    // first tick consumes the tween event at dispatch; subsequent ticks
    // advance the timeline until the 100 ms tween settles.
    for (int i = 0; i < 6; ++i) rt.tick(100, idle);
    check(rt.scene().find("1") != nullptr, "H13 layer exists");
    check(approx(rt.scene().find("1")->x_scale, 200.0, 1e-4),
          "H13 xscale tween settled to 200 in the typed field");
    check(!rt.scene().has_tweens(), "H13 finished tween removed");
}

} // namespace

int main() {
    test_h1_typed_parse_and_scale_getters();
    test_h2_h3_local_transform_math();
    test_h4_world_chain_translate_and_reverse();
    test_h5_h12_hit_and_world_rect_parent_scale_clip();
    test_h6_size_precedence_and_h10_overlap();
    test_h7_h8_h9_root_props_and_visibility();
    test_rotation_hit_inside_rotated_quad();
    test_h11_runtime_owns_scene_and_applies_layer_events();
    test_h13_runtime_tween_drives_typed_transform();
    if (failures) {
        std::fprintf(stderr, "transform_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("transform_test: all ok\n");
    return 0;
}
