// P1a layer-semantics tests (see docs/research/08-layer-semantics.md §9,
// A1-A24).
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "core/render/layer.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
template <typename A, typename B>
bool approx(A a, B b) {
    const double d = double(a) - double(b);
    return d > -1e-9 && d < 1e-9;
}
std::vector<std::string> ids_of(const std::vector<const oa::render::Layer*>& v) {
    std::vector<std::string> out;
    for (const auto* l : v) out.push_back(l->id);
    return out;
}
} // namespace

int main() {
    { // A1: create auto-builds ancestors; file lands on the leaf only
        oa::render::Compositor c;
        c.create("1.0.-1", {{"file", "black"}});
        check(c.find("1") != nullptr, "ancestor 1 materialized");
        check(c.find("1.0") != nullptr, "ancestor 1.0 materialized");
        check(c.find("1.0.-1") && c.find("1.0.-1")->file == "black", "leaf file bound");
        check(c.find("1")->file.empty(), "ancestor is an empty group node");
        check(c.size() == 3, "three nodes total");
    }
    { // A3/A4: delete removes whole subtree; missing id is a no-op
        oa::render::Compositor c;
        c.create("1", {{"file", "a"}});
        c.create("1.0", {{"file", "b"}});
        c.create("1.0.0", {{"file", "c"}});
        c.create("1.0.1", {{"file", "d"}});
        c.create("2", {{"file", "e"}});
        c.remove("1");
        check(c.find("1") == nullptr && c.find("1.0") == nullptr &&
                  c.find("1.0.0") == nullptr && c.find("1.0.1") == nullptr,
              "subtree cascade removed");
        check(c.find("2") != nullptr, "sibling subtree untouched");
        const size_t n = c.size();
        c.remove("7.7.7");
        check(c.size() == n, "remove of missing id is a no-op");
    }
    { // remove keeps unrelated prefix neighbours ("1x" is not "1.*")
        oa::render::Compositor c;
        c.create("1", {});
        c.create("1x", {});
        c.create("1.0", {});
        c.remove("1");
        check(c.find("1x") != nullptr, "id with same start but not id.* survives");
    }
    { // A5/A6: set_props is incremental and autovivifies
        oa::render::Compositor c;
        c.set_props("5", {{"left", "10"}, {"alpha", "255"}});
        check(c.find("5") != nullptr, "set_props materializes missing id (A6)");
        check(approx(c.find("5")->left, 10.0) && approx(c.find("5")->alpha, 1.0),
              "left=10 alpha=255 applied");
        c.set_props("5", {{"alpha", "0"}});
        check(approx(c.find("5")->left, 10.0) && approx(c.find("5")->alpha, 0.0),
              "second batch only touches alpha (incremental, A5)");
    }
    { // A7: x/left and y/top are alias keys over one coordinate; f32 allowed
        oa::render::Compositor c;
        c.create("k", {});
        c.set_props("k", {{"x", "100"}, {"top", "-50"}});
        check(approx(c.find("k")->left, 100.0) && approx(c.find("k")->top, -50.0),
              "x -> left, top alias");
        c.set_props("k", {{"left", "12.5"}, {"y", "7"}});
        check(approx(c.find("k")->left, 12.5) && approx(c.find("k")->top, 7.0),
              "left overrides x when both present; fractional pixels kept");
        c.set_props("k", {{"left", "oops"}});
        check(approx(c.find("k")->left, 12.5), "left parse failure keeps previous");
    }
    { // A8/A9: alpha is 0-255 u8 with float fallback; failure keeps value
        oa::render::Compositor c;
        c.create("a", {});
        c.set_props("a", {{"alpha", "128"}});
        check(approx(c.find("a")->alpha, 128.0 / 255.0), "alpha 128 -> 128/255");
        c.set_props("a", {{"alpha", "255.0"}});
        check(approx(c.find("a")->alpha, 1.0), "alpha float fallback 255.0 -> 255");
        c.set_props("a", {{"alpha", "-5"}});
        check(approx(c.find("a")->alpha, 0.0), "alpha negative clamps to 0");
        c.set_props("a", {{"alpha", "300"}});
        check(approx(c.find("a")->alpha, 1.0), "alpha overflow clamps to 255");
        c.set_props("a", {{"alpha", "junk"}});
        check(approx(c.find("a")->alpha, 1.0), "alpha unparseable keeps previous");
        const double fresh = [] {
            oa::render::Compositor c2;
            c2.create("f", {});
            return c2.find("f")->alpha;
        }();
        check(approx(fresh, 1.0), "default alpha is 255 -> 1.0");
    }
    { // A10: visible parse table; invalid strings keep previous; default true
        oa::render::Compositor c;
        c.create("v", {});
        check(c.find("v")->visible == true, "default visible true");
        for (const char* t : {"1", "on", "true", "yes"}) {
            c.set_props("v", {{"visible", t}});
            check(c.find("v")->visible == true, "truthy visible accepted");
        }
        for (const char* f : {"0", "off", "false", "no"}) {
            c.set_props("v", {{"visible", f}});
            check(c.find("v")->visible == false, "falsy visible accepted");
        }
        c.set_props("v", {{"visible", "2"}});
        check(c.find("v")->visible == false, "invalid visible keeps previous (2)");
        c.set_props("v", {{"visible", "1"}});
        c.set_props("v", {{"visible", ""}});
        check(c.find("v")->visible == true, "empty visible keeps previous (true)");
    }
    { // A11/A12: clip typed source rect; unknown keys verbatim in custom
        oa::render::Compositor c;
        c.create("cl", {});
        c.set_props("cl", {{"clip", "0,0,100,100"}, {"draggable", "1"}});
        const auto* l = c.find("cl");
        check(l->has_clip && approx(l->clip_w, 100) && approx(l->clip_h, 100),
              "clip parsed to typed rect");
        check(l->props.at("draggable") == "1", "unknown key kept verbatim");
        l = nullptr;
        c.set_props("cl", {{"clip", "5,6,30,40"}});
        c.set_props("cl", {{"clip", "0,0,100"}});
        check(c.find("cl")->has_clip && approx(c.find("cl")->clip_x, 5) &&
                  approx(c.find("cl")->clip_w, 30),
              "short clip parse failure keeps previous rect");
        c.set_props("cl", {{"clip", "1,2,3,4,ignored"}});
        check(approx(c.find("cl")->clip_x, 1) && approx(c.find("cl")->clip_h, 4),
              "extra clip components ignored");
        oa::render::Compositor c2;
        c2.create("c2", {});
        check(c2.find("c2")->has_clip == false, "clip absent by default");
        c2.set_props("c2", {{"clip", "a,b,c,d"}});
        check(c2.find("c2")->has_clip == false, "non-numeric clip rejected");
    }
    { // A13: "file" key is ignored by set_props (bound only at create)
        oa::render::Compositor c;
        c.create("x", {{"file", "bg"}, {"path", ":bg/"}});
        c.set_props("x", {{"file", "other"}, {"visible", "1"}});
        check(c.find("x")->file == "bg", "set_props does not rebind file");
    }
    { // A14/A15: lyc extras (one-shot create props): color switches solid mode
        // (file unbound), width/height parsed; plain create keeps group node
        oa::render::Compositor c;
        c.create("5", {{"width", "100"}, {"height", "50"}, {"color", "FF00FF"}});
        const auto* l = c.find("5");
        check(l->has_color && l->solid_rgba[0] == 255 && l->solid_rgba[1] == 0 &&
                  l->solid_rgba[2] == 255 && l->solid_rgba[3] == 255,
              "RRGGBB color -> solid rgba [255,0,255,255]");
        check(l->file.empty(), "solid mode unbinds file");
        check(approx(l->width, 100) && approx(l->height, 50), "width/height parsed");
        l = nullptr;
        c.create("6", {{"file", "img"}});
        check(c.find("6")->file == "img" && !c.find("6")->has_color,
              "file create stays a texture layer");
        // AARRGGBB: alpha taken from the high byte
        c.set_props("5", {{"color", "80FF0000"}});
        check(c.find("5")->solid_rgba[0] == 255 && c.find("5")->solid_rgba[3] == 0x80,
              "AARRGGBB color keeps its alpha");
    }
    { // mask field ( mask routing) incl. empty clears
        oa::render::Compositor c;
        c.create("m", {{"file", "fg"}});
        c.set_props("m", {{"mask", "fgmask"}});
        check(c.find("m")->mask == "fgmask", "mask stored");
        c.set_props("m", {{"mask", ""}});
        check(c.find("m")->mask.empty(), "empty mask clears");
    }
    { // A21: dotted ids are verbatim strings, never numericised
        oa::render::Compositor c;
        c.create("1.80", {{"file", "cg"}});
        check(c.find("1.80") != nullptr && c.find("1.80")->id == "1.80",
              "trailing zero preserved (1.80 != 1.8)");
        check(c.find("1.8") == nullptr, "1.8 is a distinct id");
        c.create("500.b.2.0", {{"file", "btn"}});
        check(c.find("500.b.2.0") != nullptr && c.find("500.b.2.0")->id == "500.b.2.0",
              "non-numeric segments preserved verbatim");
        check(c.find("500.b.20") == nullptr, "500.b.20 is a distinct id");
        c.remove("1.80");
        check(c.find("1") != nullptr, "removing a child keeps its ancestor group");
    }
    { // A22: draw-order comparator — pure segment comparison (L2 M7: the old
        // message-prefix "message-last" spelling branch is gone; independent-
        // message slots order is structural (message_slots, creation order)
        // and never flows through compare_ids). All ids here are ordinary
        // scene ids: numeric segments first, then strings by ASCII, shorter
        // dotted prefixes first. "@openartemis-message-x" is a plain string root
        // ('@' = 0x40 < 'a'), "zzz" sorts after "abc".
        oa::render::Compositor c;
        c.create("10", {});
        c.create("2", {});
        c.create("1.0.0", {});
        c.create("1.0.-1", {});
        c.create("1.0.-2", {});
        c.create("abc", {});
        c.create("@openartemis-message-x", {});
        c.create("zzz", {});
        check(ids_of(c.draw_order()) ==
                  (std::vector<std::string>{"1", "1.0", "1.0.-2", "1.0.-1", "1.0.0", "2",
                                            "10", "@openartemis-message-x", "abc", "zzz"}),
              "numeric-segment sort: -2 < -1 < 0, 2 < 10, strings after numbers "
              "(ASCII order), no message-prefix branch");
    }
    { // visibility + opacity inheritance along the dotted ancestor chain
        oa::render::Compositor c;
        c.create("1", {{"file", "mw"}});
        c.create("1.mw.adv", {{"file", "adv"}});
        check(c.is_effectively_visible("1.mw.adv"), "default visible down the chain");
        c.set_props("1", {{"visible", "0"}});
        check(!c.is_effectively_visible("1.mw.adv"), "hidden ancestor hides the leaf");
        check(!c.is_effectively_visible("1"), "parent itself hidden");
        c.set_props("1", {{"visible", "1"}});
        check(c.is_effectively_visible("1.mw.adv"), "restored parent reveals the leaf");
        c.set_props("1.mw.adv", {{"visible", "0"}});
        check(!c.is_effectively_visible("1.mw.adv"), "leaf hidden by itself");
        check(!c.is_effectively_visible("9.9.9"), "missing layer is not visible");
        c.set_props("1.mw.adv", {{"visible", "1"}, {"alpha", "128"}});
        check(approx(c.chain_opacity("1.mw.adv"), 128.0 / 255.0), "chain opacity = own alpha");
        c.set_props("1", {{"alpha", "200"}});
        check(approx(c.chain_opacity("1.mw.adv"), 200.0 / 255.0 * 128.0 / 255.0),
              "chain opacity multiplies ancestor alphas");
        check(approx(c.chain_opacity("1"), 200.0 / 255.0), "root chain opacity is its own");
    }

    if (failures) {
        std::fprintf(stderr, "layer_semantics_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("layer_semantics_test: all ok\n");
    return 0;
}
