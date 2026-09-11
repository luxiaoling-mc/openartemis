// M5b compositor + P1a layer-model alignment: create/remove cascade, prop
// parse, ancestor autovivification, draw order. Semantics notes: docs/research/08.
#include <cstdio>
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
std::vector<std::string> ids_of(const std::vector<const oa::render::Layer*>& v) {
    std::vector<std::string> out;
    for (const auto* l : v) out.push_back(l->id);
    return out;
}
} // namespace

int main() {
    oa::render::Compositor c;
    c.create("1", {{"file", "zbg001_a"}, {"path", ":bg/"}});
    c.create("1.0", {{"file", "hiy_noa0510"}, {"path", ":fg/hiy/no/"}});
    c.create("2", {});
    c.create("4.30", {});
    c.create("500.tr", {});
    c.create("@openartemis-message-x", {});
    // dotted ids autovivify their ancestors as empty group nodes ("4", "500")
    // ( ensure_path;  test create_auto_builds_ancestors)
    check(c.size() == 8, "six explicit + ancestors 4/500");

    auto order = c.draw_order();
    check(ids_of(order) == (std::vector<std::string>{"1", "1.0", "2", "4", "4.30",
                                                     "500", "500.tr", "@openartemis-message-x"}),
          "numeric-segment draw order (strings after numbers; @openartemis-message-x "
          "is the only string root here — no message-prefix branch, L2 M7)");

    // parent delete removes the whole subtree (id + id.*)
    c.remove("1");
    check(c.size() == 6 && c.find("1") == nullptr && c.find("1.0") == nullptr,
          "delete cascades to id.*");
    c.remove("9.9.9"); // missing id: no-op
    check(c.size() == 6, "delete of missing id is a no-op");

    // props: alpha 128 -> ~0.502 (u8 semantics), visible off, left via alias
    c.set_props("2", {{"alpha", "128"}, {"visible", "0"}, {"x", "640"}});
    const oa::render::Layer* l2 = c.find("2");
    check(l2 && l2->visible == false, "visible parsed");
    check(l2 && l2->alpha > 0.50 && l2->alpha < 0.51, "alpha 128/255");
    check(l2 && l2->left == 640, "x alias parses into left");
    check(l2 && l2->props.at("alpha") == "128", "verbatim prop kept");
    c.set_props("2", {{"alpha", "255.0"}});
    check(c.find("2")->alpha == 1.0, "alpha float fallback 255.0 -> 255");
    c.set_props("2", {{"alpha", "abc"}});
    check(c.find("2")->alpha == 1.0, "alpha parse failure keeps previous value");

    // lyprop on a missing id materializes the layer ( set_props
    // autovivify); it has no file -> pure group node
    c.set_props("nope", {{"visible", "1"}});
    check(c.size() == 7 && c.find("nope") != nullptr && c.find("nope")->file.empty(),
          "set_props materializes missing id");
    check(c.find("nope")->visible == true, "visible=1 parsed on autovivified layer");

    // clip = texture source sub-rect [x,y,w,h] (sprite-atlas crop), not a
    // clickable offset
    c.set_props("2", {{"clip", "4,8,160,48"}});
    const oa::render::Layer* l2b = c.find("2");
    check(l2b && l2b->has_clip && l2b->clip_x == 4 && l2b->clip_y == 8 &&
              l2b->clip_w == 160 && l2b->clip_h == 48,
          "clip parsed as source rect");
    c.set_props("2", {{"clip", "1,2,3"}});
    check(c.find("2")->has_clip && c.find("2")->clip_w == 160,
          "short clip keeps previous value");
    c.set_props("2", {{"visible", "1"}});
    check(c.find("2")->visible == true, "visible=1 restores");

    // ---- U16 (research/32): overlay-message text visibility follows the
    // host window tree. FPM story text ("1.80.mw.adv_adv") materializes as an
    // engine root overlay node; when msg_hide hides the 1.80 tree (R5) or
    // [alldelete] deletes it (image.lua lydel id=1), the text must disappear
    // with it — a fully deleted tree previously left the text alive over the
    // black transition page (title return / exit residue).
    {
        oa::render::Compositor sc;
        sc.create("1", {});
        sc.create("1.80", {});
        sc.create("1.80.mw", {});
        // M7: layered=0 消息的槽节点由引擎显式分配(结构标记 + 槽区,无 hex
        // 编码,id 不随消息 id 变);测试以真实消息槽为绑定目标。
        const std::string slot =
            sc.ensure_message_scene_node("1.80.mw.adv_adv", false);
        check(slot != "1.80.mw.adv_adv" && sc.is_message_slot(slot) &&
                  sc.find(slot) != nullptr,
              "M7 story message got an engine message slot");
        check(sc.ensure_message_scene_node("1.80.mw.adv_adv", false) == slot,
              "M7 slot reused for the same message (not content-derived)");
        sc.set_message_layer_binding("1.80.mw.adv_adv", slot);
        check(sc.is_message_layer_visible("1.80.mw.adv_adv"),
              "U16 overlay text visible while the host window tree lives");
        sc.set_props("1.80", {{"visible", "0"}});
        check(!sc.is_message_layer_visible("1.80.mw.adv_adv"),
              "U16 (R5) overlay text hidden when the host tree is hidden");
        sc.set_props("1.80", {{"visible", "1"}});
        sc.remove("1"); // [alldelete] lydel-style: whole host tree dies
        check(!sc.is_message_layer_visible("1.80.mw.adv_adv"),
              "U16 overlay text dies when the host window tree is deleted");
        // a root-level engine-only overlay (single segment, no host tree)
        // keeps its own visibility semantics
        sc.create("znotify", {});
        check(sc.is_message_layer_visible("znotify"),
              "U16 single-segment engine overlay unaffected");
        sc.set_props("znotify", {{"visible", "0"}});
        check(!sc.is_message_layer_visible("znotify"),
              "U16 single-segment overlay hidden by its own visibility");
        // ---- M4 (research/33): converged rule surface pins — semantics
        // identical to the pre-M4 code (pure refactor); these asserts spell
        // out the explicit rule the duplicated ancestor-chain lambdas
        // collapsed into (host_tree_anchor_visible).
        // remove("1") cleared the binding but the slot itself stays in the
        // message-slot area; rebinding revives it: the slot node is alive and
        // visible, yet the host tree is gone -> overlay text stays dead.
        sc.set_message_layer_binding("1.80.mw.adv_adv", slot);
        check(sc.find(slot) != nullptr,
              "M4 message slot node itself still exists");
        check(!sc.is_message_layer_visible("1.80.mw.adv_adv"),
              "M4 slot node alive but host tree gone -> invisible");
        // Host tree re-created: the nearest live anchor decides again.
        sc.create("1", {});
        sc.create("1.80", {});
        sc.create("1.80.mw", {});
        check(sc.is_message_layer_visible("1.80.mw.adv_adv"),
              "M4 host tree re-created -> overlay text visible again");
        sc.set_props("1.80.mw", {{"visible", "0"}});
        check(!sc.is_message_layer_visible("1.80.mw.adv_adv"),
              "M4 nearest live host anchor hidden -> invisible");
        sc.set_props("1.80.mw", {{"visible", "1"}});
        sc.remove("1");
        check(!sc.is_message_layer_visible("1.80.mw.adv_adv"),
              "M4 host tree deleted again -> invisible");
        // Layered identity (bound scene target == message id): the node's own
        // effective visibility decides, no host-tree anchoring involved.
        sc.create("9.9.adv", {});
        sc.set_message_layer_binding("9.9.adv", "9.9.adv");
        check(sc.is_message_layer_visible("9.9.adv"),
              "M4 layered identity node visible -> visible");
        sc.set_props("9.9", {{"visible", "0"}});
        check(!sc.is_message_layer_visible("9.9.adv"),
              "M4 layered identity node under a hidden ancestor -> invisible");
        sc.set_props("9.9", {{"visible", "1"}});
        sc.remove("9");
        check(!sc.is_message_layer_visible("9.9.adv"),
              "M4 layered binding dropped with its subtree -> invisible");
        // ---- M7: 槽区输出 = 场景区之后按创建序(无 message-last 拼写分支)。
        const std::string slot2 = sc.ensure_message_scene_node("m2", false);
        check(sc.is_message_slot(slot2) && slot2 != slot,
              "M7 second message gets its own slot");
        const auto order7 = sc.draw_order();
        check(order7.size() >= 2 &&
                  order7[order7.size() - 2]->id == slot &&
                  order7[order7.size() - 1]->id == slot2,
              "M7 slots sit at the draw tail in creation order");
        sc.remove(slot2);
        check(!sc.find(slot2) && sc.draw_order().back()->id == slot,
              "M7 removing a slot detaches it from the slot area");
    }
    // ---- 孤儿消息绘制门槛 (research/63): is_message_layer_drawable ——
    // 有效可见之外还要求"绘制盒":文本层 [font] 排版键配置过 或 绑定节点被
    // 脚本显式创建/改写(script_created)。既无盒、节点又只是 chgmsg 绑定
    // ensure_path 自动物化的孤儿打印(游戏数据残留的未挂载 ui_message 数值
    // 打印)不绘制 —— 场景从未给它位置,绘制只会落在继承来的外来几何上。
    {
        oa::render::Compositor sc;
        sc.create("500", {}); // authored UI group (NekoMiko config 500)
        const std::string node =
            sc.ensure_message_scene_node("500.p03", true); // chgmsg materializes
        check(node == "500.p03" && sc.find(node) != nullptr,
              "63 orphan layered message node auto-materialized (not authored)");
        check(sc.find(node) && !sc.find(node)->script_created,
              "63 auto-materialized node is NOT script_created");
        check(!sc.is_message_layer_drawable("500.p03", false),
              "63 unboxed orphan text not drawable");
        check(sc.is_message_layer_visible("500.p03"),
              "63 orphan stays registry-visible (scene semantics unchanged)");
        check(sc.is_message_layer_drawable("500.p03", true),
              "63 text box ([font] layout keys) makes it drawable");
        // research/71 (user-corrected rule, supersedes 63's lyprop anchor):
        // [lyprop] on a binding-materialized node does NOT give unboxed text
        // a paint seat — snll config uihelp hover tips (500.z.help) print
        // with no [font] row; the uihelp_over lyprop only anchors/centers the
        // overlay, so the tip must not draw (FPM's boxed uihelp keeps
        // drawing). Only [lyc]/[lyc2] creation authors a node.
        sc.set_props("500.p03", {{"visible", "1"}});
        check(sc.find("500.p03") && !sc.find("500.p03")->script_created,
              "71 lyprop does NOT author the materialized node");
        check(!sc.is_message_layer_drawable("500.p03", false),
              "71 unboxed text on an lyprop-only node is NOT drawable");
        sc.create("500.p03", {}); // [lyc2]-style authored node
        check(sc.find("500.p03") && sc.find("500.p03")->script_created,
              "71 lyc2-created node is script_created");
        check(sc.is_message_layer_drawable("500.p03", false),
              "71 created (authored) node makes the message drawable");
        // 隐藏祖先仍然压过一切。
        sc.set_props("500", {{"visible", "0"}});
        check(!sc.is_message_layer_drawable("500.p03", true),
              "63 hidden authored ancestor still blocks drawing");
        sc.set_props("500", {{"visible", "1"}});
        // layered=0 槽消息:槽节点从不 script_created → 只靠文本盒。
        sc.create("1.80", {});
        sc.create("1.80.mw", {});
        const std::string slot =
            sc.ensure_message_scene_node("1.80.mw.adv", false);
        sc.set_message_layer_binding("1.80.mw.adv", slot);
        check(sc.is_message_layer_visible("1.80.mw.adv"),
              "63 slot message visible through the host tree anchor");
        check(!sc.is_message_layer_drawable("1.80.mw.adv", false),
              "63 unboxed slot message not drawable");
        check(sc.is_message_layer_drawable("1.80.mw.adv", true),
              "63 boxed slot message drawable");
    }
    // ---- layer-video 绑定物化门槛 (research/73;layer-model S5 起入口 =
    // bind_video_layer):只对系统覆盖区 ("500." id 族,标题花瓣 500.z.mv 的
    // [video] 行先于载体创建)自动物化缺失载体;剧情内 layer 视频(载体归 ADV
    // 场景系统所有、稍后才建)必须绑到已存在的脚本层,缺失即拒绝 —— 65 的
    // 无条件物化把 snll 回忆蒙太奇的近白噪音条(ノイズa.ogv,7 帧 88-100% 白
    // 循环)整屏绑上去,画面被白色闪烁盖没(研究/73)。
    {
        oa::render::Compositor sc;
        const size_t base = sc.size();
        check(!sc.bind_video_layer("3"),
              "73 missing in-story carrier: bind refused");
        check(sc.find("3") == nullptr && sc.size() == base,
              "73 refused bind does NOT materialize the in-story layer");
        // research/96: the runtime may explicitly allow auto-materialization
        // for looping sparse-overlay effect strips (btjy weather loops) —
        // their framework binding never authors the carrier leaf, so the
        // strict rule made the whole story layer-video family invisible.
        check(sc.bind_video_layer("1.0.bx.by.bs.8.bg.8.t.y.x.p.m.a.a.b", true) &&
                  sc.find("1.0.bx.by.bs.8.bg.8.t.y.x.p.m.a.a.b")->content ==
                      oa::render::LayerContent::VideoFrame,
              "96 allowed missing in-story carrier materializes + binds");
        check(sc.find("1.0") != nullptr && sc.find("1.0.bx.by.bs.8.bg.8") != nullptr,
              "96 materialized in-story carrier ancestors (ensure_path)");
        check(!sc.bind_video_layer("9.bg.9.leaf", false) &&
                  sc.find("9.bg.9.leaf") == nullptr,
              "96 allow_auto=false keeps the strict 73 refusal");
        sc.create("7", {});
        check(sc.bind_video_layer("7") &&
                  sc.find("7")->content == oa::render::LayerContent::VideoFrame,
              "73 existing carrier binds (any id)");
        check(sc.bind_video_layer("500.z.mv"),
              "73 overlay-zone (500.) missing carrier is materialized");
        const oa::render::Layer* mv = sc.find("500.z.mv");
        check(mv && mv->content == oa::render::LayerContent::VideoFrame &&
                  mv->file.empty(),
              "73 500.z.mv bound to its video channel (no name spelling in file)");
        check(sc.find("500") != nullptr && sc.find("500.z") != nullptr,
              "73 500.z.mv ancestors materialized (ensure_path)");
        // 解绑匹配语义(旧 clear_layer_file_if_matches):重绑成静态图后,
        // 视频收尾解绑不得清掉该层。
        check(sc.set_layer_file("500.z.mv", "sakura.png"), "rebind to a still image");
        check(!sc.unbind_video_layer("500.z.mv") &&
                  sc.find("500.z.mv")->file == "sakura.png",
              "unbind refuses on a layer re-bound meanwhile");
        check(sc.bind_video_layer("500.z.mv") && sc.unbind_video_layer("500.z.mv") &&
                  sc.find("500.z.mv")->file.empty(),
              "bind -> unbind returns the carrier to the empty state");
    }

    if (failures) {
        std::fprintf(stderr, "compositor_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("compositor_test: all ok\n");
    return 0;
}
