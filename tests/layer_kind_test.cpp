// layer-model S1/S2 (research/105; design doc research/104):
//  - S1 分类权威: LayerKind/kind_of 纯函数对 §2.2 语义表的断言组 —— 现有
//    真实旅程/测试可达的层状态组合(经 Compositor API 复现:lyc 文件/纯色、
//    [video]/emote 保留命名空间绑定与解绑往返、ensure_path 物化中间节点、
//    intermediate_render 中间组、clip 容器窗、消息槽/overlay 结构槽) +
//    文档点名边界(命名空间带冒号前缀、无文件+clip 内容窗、has_color+w/h=
//    Solid vs w/h 缺省=Empty 中间态)。
//  - S2 簿记: DirtyAspect/mark_dirty/consume 集合语义 + 写入口标记面
//    (create/set_props/remove/set_layer_file/lyevent/lytween/anime/root
//    props/clear_scene/overlay 转场)。
// 断言 = NDEBUG 编译即关(仓库单测惯例);零行为面 —— 全部纯新增。
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "core/render/layer.h"
#include "core/render/layer_kind.h"
// research/111: advance_tweens/apply_lytweendel 的 std::vector<TweenDone>
// 返回值需要 render 内部头里的元素类型定义（公开头只前向声明）。
#include "core/render/render_internal.h"

namespace {

int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using oa::render::Compositor;
using oa::render::DirtyAspect;
using oa::render::Layer;
using oa::render::LayerContent;
using oa::render::LayerKind;
using oa::render::SceneNode;

// ---------------------------------------------------------------------------
// S1a: 纯内容态分类(手搓 Layer 字段;无树上下文)
// ---------------------------------------------------------------------------
void test_content_kinds() {
    // Image: 常规 file
    {
        Layer l;
        l.id = "a.b";
        l.file = "pc/ja/mw/dialog";
        check(oa::render::kind_of(l) == LayerKind::Image, "plain file -> Image");
    }
    // Video/Emote: 内容来源状态(写入口显式设置。S5,research/108:保留命名空间
    // `__video_layer__:`/`__emote_layer__:` 拼写判定已整体删除 —— file 只剩
    // 资源名,"怎么读这个资源"由内容角色实例决定)
    {
        Layer l;
        l.id = "500.z.mv";
        l.content = LayerContent::VideoFrame;
        check(oa::render::kind_of(l) == LayerKind::Video,
              "content=VideoFrame -> Video");
        Layer e;
        e.id = "2.5";
        e.content = LayerContent::EmoteCanvas;
        check(oa::render::kind_of(e) == LayerKind::Emote,
              "content=EmoteCanvas -> Emote");
        // file 字段与内容来源无关:宿主供帧层 file 空(绑定清资源位)
        check(l.file.empty() && e.file.empty(),
              "host-frame bindings carry no file name");
    }
    // 边界:旧的保留命名空间拼写如今只是普通资源名(引擎不再认识它)——
    // 拼写分类面已删除的直接断言
    {
        Layer l;
        l.id = "x";
        l.file = "__video_layer__:x";
        check(oa::render::kind_of(l) == LayerKind::Image,
              "the retired __video_layer__ spelling is a plain image name");
        Layer m;
        m.id = "y";
        m.file = "__emote_layer__:y";
        check(oa::render::kind_of(m) == LayerKind::Image,
              "the retired __emote_layer__ spelling is a plain image name");
    }
    // Solid: has_color && w/h > 0(§2.2 Solid 行;file 必空的互斥路由)
    {
        Layer l;
        l.id = "win";
        l.has_color = true;
        l.solid_rgba[0] = 255;
        l.width = 100;
        l.height = 50;
        check(oa::render::kind_of(l) == LayerKind::Solid,
              "has_color + w/h -> Solid");
    }
    // 边界:has_color 但 w/h 缺省 = 中间态(draw_one 不画)→ None/Empty
    {
        Layer l;
        l.id = "mid";
        l.has_color = true; // width/height 0
        check(oa::render::kind_of(l) == LayerKind::None,
              "has_color without w/h is an intermediate Empty (None)");
        Layer h;
        h.id = "mid2";
        h.has_color = true;
        h.width = 100; // height 缺省
        check(oa::render::kind_of(h) == LayerKind::None,
              "has_color with w only stays None");
    }
    // 中间组(intermediate_render != 0,verbatim props):无文件即 Group
    {
        Layer l;
        l.id = "g";
        l.props["intermediate_render"] = "1";
        check(oa::render::kind_of(l) == LayerKind::Group,
              "intermediate_render=1 file-less -> Group (no tree ctx)");
        Layer m;
        m.id = "g2";
        m.props["intermediate_render"] = "2";
        check(oa::render::kind_of(m) == LayerKind::Group,
              "intermediate_render=2 -> Group");
        Layer n;
        n.id = "g3";
        n.props["intermediate_render"] = "0";
        check(oa::render::kind_of(n) == LayerKind::None,
              "intermediate_render=0 -> not a group");
        Layer o;
        o.id = "g4";
        o.props["intermediate_render"] = "abc"; // parse failure = 0
        check(oa::render::kind_of(o) == LayerKind::None,
              "intermediate_render parse failure -> not a group");
    }
    // 组语义与内容正交:intermediate 文件层仍按内容归类(doc §2.2 Group 行注:
    // "文件层也可带";draw_one 内容分支优先)
    {
        Layer l;
        l.id = "img.g";
        l.file = "bg/x";
        l.props["intermediate_render"] = "1";
        check(oa::render::kind_of(l) == LayerKind::Image,
              "file + intermediate_render -> Image (group semantics orthogonal)");
        check(oa::render::intermediate_render_nonzero(l),
              "intermediate_render_nonzero sees the verbatim flag");
    }
    // 空态:file/color 空、无子、非中间组
    {
        Layer l;
        l.id = "orp";
        check(oa::render::kind_of(l) == LayerKind::None, "empty layer -> None");
    }
    // overlay 结构槽:id 判定(与绑定状态无关)
    {
        Layer l;
        l.id = oa::render::kOverlayNodeId;
        check(oa::render::kind_of(l) == LayerKind::Overlay,
              "overlay slot id -> Overlay");
        Layer e;
        e.id = oa::render::kOverlayNodeId;
        e.file = oa::render::kOverlayNodeId; // 绑定态
        check(oa::render::kind_of(e) == LayerKind::Overlay,
              "bound overlay slot still Overlay");
    }
}

// ---------------------------------------------------------------------------
// S1b: 结构行(SceneNode 上下文:children / message_slot)
// ---------------------------------------------------------------------------
void test_node_kinds() {
    // 纯子容器:file 空 + 子节点 → Group(§2.2 Group 行;无 intermediate 时组
    // 语义不激活,角色仍是容器)
    {
        Layer l;
        l.id = "g";
        SceneNode n;
        n.layer = l;
        check(oa::render::kind_of(n) == LayerKind::None,
              "file-less node without children -> None");
        SceneNode c1, c2;
        n.children.push_back(&c1);
        n.children.push_back(&c2);
        check(oa::render::kind_of(n) == LayerKind::Group,
              "file-less node with children -> Group (pure container)");
    }
    // 边界:无文件 + clip = 内容裁剪窗的承载者(93/99)—— 有子即 Group;
    // 无子无中间组时 clip 无绘制语义 → None
    {
        Layer l;
        l.id = "row";
        l.has_clip = true;
        l.clip_x = 0;
        l.clip_y = 0;
        l.clip_w = 800;
        l.clip_h = 200;
        SceneNode n;
        n.layer = l;
        check(oa::render::kind_of(n) == LayerKind::None,
              "file-less + clip alone (no children) -> None");
        SceneNode c;
        n.children.push_back(&c);
        check(oa::render::kind_of(n) == LayerKind::Group,
              "file-less + clip + children -> Group (crop-window container)");
    }
    // 有内容节点的子容器判定不受子影响
    {
        Layer l;
        l.id = "img";
        l.file = "bg/x";
        SceneNode n;
        n.layer = l;
        SceneNode c;
        n.children.push_back(&c);
        check(oa::render::kind_of(n) == LayerKind::Image,
              "file + children stays Image");
    }
    // 消息槽结构行
    {
        Layer l;
        l.id = "@msg0";
        SceneNode n;
        n.layer = l;
        n.message_slot = true;
        check(oa::render::kind_of(n) == LayerKind::MessageSlot,
              "message_slot node -> MessageSlot");
    }
    // 空节点 + intermediate(无子)= Group
    {
        Layer l;
        l.id = "shell";
        l.props["intermediate_render"] = "1";
        SceneNode n;
        n.layer = l;
        check(oa::render::kind_of(n) == LayerKind::Group,
              "intermediate file-less shell -> Group");
    }
    // dirty_kind 缺省映射表(future 接口预留;只锁表不锁语义演进)
    {
        using oa::render::DirtyKind;
        check(oa::render::dirty_kind_of(LayerKind::None) == DirtyKind::Static,
              "None default dirty kind Static");
        check(oa::render::dirty_kind_of(LayerKind::Group) == DirtyKind::Grouped,
              "Group default dirty kind Grouped");
        check(oa::render::dirty_kind_of(LayerKind::Solid) == DirtyKind::Static,
              "Solid default dirty kind Static");
        check(oa::render::dirty_kind_of(LayerKind::Image) == DirtyKind::Static,
              "Image default dirty kind Static");
        check(oa::render::dirty_kind_of(LayerKind::Video) == DirtyKind::HostPump,
              "Video default dirty kind HostPump");
        check(oa::render::dirty_kind_of(LayerKind::Emote) == DirtyKind::HostPump,
              "Emote default dirty kind HostPump");
        check(oa::render::dirty_kind_of(LayerKind::Overlay) == DirtyKind::HostPump,
              "Overlay default dirty kind HostPump");
        check(oa::render::dirty_kind_of(LayerKind::MessageSlot) ==
                  DirtyKind::TextBound,
              "MessageSlot default dirty kind TextBound");
        for (int i = 0; i <= int(LayerKind::MessageSlot); ++i) {
            const auto k = LayerKind(i);
            check(oa::render::layer_kind_name(k) != nullptr &&
                      oa::render::layer_kind_name(k)[0] != '?',
                  "layer_kind_name covers every LayerKind");
        }
        for (int i = 0; i <= int(DirtyKind::PerFrame); ++i) {
            const auto k = DirtyKind(i);
            check(oa::render::dirty_kind_name(k) != nullptr &&
                      oa::render::dirty_kind_name(k)[0] != '?',
                  "dirty_kind_name covers every DirtyKind");
        }
    }
}

// ---------------------------------------------------------------------------
// S1c: 真实 Compositor 可达状态(旅程/测试同款写入序列)上复述分类
// ---------------------------------------------------------------------------
void test_compositor_reachable() {
    Compositor c;
    // lyc 文件背景 → Image
    c.create("1", {{"file", "bg001_a"}, {"path", ":bg/"}});
    check(c.scene_node("1") != nullptr &&
              oa::render::kind_of(*c.scene_node("1")) == LayerKind::Image,
          "lyc file -> Image (compositor)");
    // lyc2 纯色 + w/h → Solid(color 键路由清 file)
    c.create("2", {{"color", "#ffffffff"}, {"width", "100"}, {"height", "50"}});
    check(c.scene_node("2") &&
              oa::render::kind_of(*c.scene_node("2")) == LayerKind::Solid,
          "lyc color + w/h -> Solid (compositor)");
    // 纯色 w/h 缺省(数据常见:只有 color,后续 lyprop 补 w/h)→ 中间态 None
    c.create("3", {{"color", "#80000000"}});
    check(c.scene_node("3") &&
              oa::render::kind_of(*c.scene_node("3")) == LayerKind::None,
          "lyc color without w/h -> None intermediate (compositor)");
    // lyprop 补 w/h → 角色升级 Solid
    c.set_props("3", {{"width", "640"}, {"height", "360"}});
    check(c.scene_node("3") &&
              oa::render::kind_of(*c.scene_node("3")) == LayerKind::Solid,
          "color + later w/h -> Solid (role follows state)");
    // ensure_path 物化中间节点(create 祖先/lyprop 缺省 id)→ None
    c.set_props("500.z.help", {{"visible", "1"}}); // 物化 500/500.z/500.z.help
    check(c.scene_node("500") && !c.scene_node("500")->children.empty() &&
              oa::render::kind_of(*c.scene_node("500")) == LayerKind::Group,
          "autovivified ancestor with children -> Group");
    check(c.scene_node("500.z.help") &&
              oa::render::kind_of(*c.scene_node("500.z.help")) ==
                  LayerKind::None,
          "autovivified carrier (empty leaf) -> None");
    // [video] 绑定:内容来源状态 → Video;物化载体验证(500. 区)
    const bool bound = c.bind_video_layer("500.z.mv");
    check(bound && c.scene_node("500.z.mv") &&
              oa::render::kind_of(*c.scene_node("500.z.mv")) == LayerKind::Video,
          "bind_video_layer -> Video");
    check(c.find("500.z.mv") && c.find("500.z.mv")->file.empty(),
          "video bind writes no magic name into file");
    // 视频结束解绑(finish_video 同款)→ 回到空态
    check(c.unbind_video_layer("500.z.mv"), "unbind_video_layer matched");
    check(c.scene_node("500.z.mv") &&
              oa::render::kind_of(*c.scene_node("500.z.mv")) == LayerKind::None,
          "video unbind -> carrier back to None");
    // 再绑常规图 → Image(同一节点角色往返:video → None → Image)
    c.set_layer_file("500.z.mv", "sakura.png");
    check(c.scene_node("500.z.mv") &&
              oa::render::kind_of(*c.scene_node("500.z.mv")) == LayerKind::Image,
          "carrier rebound to a still image -> Image");
    // emote 绑定(apply_emote_static 同款:set_props 盒 + bind_emote_layer)
    c.set_props("500.z.mv", {{"width", "300"}, {"height", "300"}});
    c.bind_emote_layer("500.z.mv");
    check(c.scene_node("500.z.mv") &&
              oa::render::kind_of(*c.scene_node("500.z.mv")) == LayerKind::Emote,
          "bind_emote_layer -> Emote");
    // emote 绑定后 has_color 被清(绑定语义),Image 静态帧被替换
    check(c.find("500.z.mv") && !c.find("500.z.mv")->has_color,
          "layer-file binding dropped solid state");
    // intermediate 组(90/93/99 面):文件空 + intermediate + clip + 子 = Group
    c.create("4", {{"intermediate_render", "1"}, {"clip", "0,0,800,200"}});
    c.create("4.1", {{"file", "face/row.png"}});
    check(c.scene_node("4") &&
              oa::render::kind_of(*c.scene_node("4")) == LayerKind::Group,
          "intermediate container with clip + children -> Group");
    // 消息槽(ensure_message_scene_node layered=0)
    const std::string slot = c.ensure_message_scene_node("msg0", false);
    const SceneNode* sn = c.scene_node(slot);
    check(sn && sn->message_slot &&
              oa::render::kind_of(*sn) == LayerKind::MessageSlot,
          "layered=0 message slot node -> MessageSlot");
    // overlay 槽
    c.set_overlay_file();
    check(c.scene_node(oa::render::kOverlayNodeId) &&
              oa::render::kind_of(*c.scene_node(oa::render::kOverlayNodeId)) ==
                  LayerKind::Overlay,
          "overlay slot -> Overlay");
    // 大树删除 → 角色级联消失(remove 级联不破坏 kind 判定)
    c.remove("1");
    check(c.find("1") == nullptr, "remove cascade still works with ledger on");
    c.clear_scene();
    check(c.size() == 0, "clear_scene empties tree (ledger still bounded)");
}

// ---------------------------------------------------------------------------
// S2: 簿记集合语义 + 写入口标记面
// ---------------------------------------------------------------------------
void test_ledger_semantics() {
    Compositor c;
    // 空报告
    check(c.dirty_report().empty(), "fresh ledger empty");
    check(c.consume_dirty_report().empty(), "consume on empty ledger empty");
    // mark 集合语义:同一 (id, aspect) 同帧去重;aspect 位 OR 合并
    c.mark_dirty("a", DirtyAspect::Content);
    c.mark_dirty("a", DirtyAspect::Content);
    c.mark_dirty("a", DirtyAspect::Visibility);
    c.mark_dirty("b", DirtyAspect::Transform);
    const auto r1 = c.dirty_report();
    check(r1.size() == 2, "two ids after dedupe");
    check(r1.at("a") == uint16_t(uint16_t(DirtyAspect::Content) |
                                 uint16_t(DirtyAspect::Visibility)),
          "same-id aspects OR-merged");
    check(r1.at("b") == uint16_t(DirtyAspect::Transform), "id b aspect kept");
    // consume 取走并清空
    const auto c1 = c.consume_dirty_report();
    check(c1.size() == 2 && c.dirty_report().empty(),
          "consume returns and clears the frame report");
    // 下一帧新写(跨帧不串)
    c.mark_dirty("b", DirtyAspect::Opacity);
    const auto r2 = c.dirty_report();
    check(r2.size() == 1 && !r2.count("a"), "next frame only holds new marks");
    check(r2.at("b") == uint16_t(DirtyAspect::Opacity),
          "same id next frame replaces bits");
    (void)c.consume_dirty_report();
}

void test_ledger_write_entries() {
    Compositor c;
    auto report = [&c]() {
        auto m = c.consume_dirty_report();
        return m;
    };
    // create → 目标 id 记 Content|…(这里批只有 file → Content)
    c.create("1", {{"file", "bg/x"}});
    {
        auto m = report();
        check(m.count("1") && m.at("1") != 0, "create marks the layer");
    }
    // lyprop visible → Visibility 位
    c.set_props("1", {{"visible", "0"}});
    {
        auto m = report();
        check(m.count("1") && (m.at("1") & uint16_t(DirtyAspect::Visibility)),
              "set_props visible marks Visibility");
    }
    // lyprop left/top → Transform 位
    c.set_props("1", {{"left", "10"}, {"top", "20"}});
    {
        auto m = report();
        check(m.count("1") && (m.at("1") & uint16_t(DirtyAspect::Transform)),
              "set_props left/top marks Transform");
    }
    // 根 props(!) → 伪 id ""
    c.set_root_props({{"alpha", "128"}});
    {
        auto m = report();
        check(m.count("") && (m.at("") & uint16_t(DirtyAspect::Opacity)),
              "root props mark pseudo id '' with Opacity");
    }
    // [lyevent] → Handlers
    c.apply_lyevent("1", {{"type", "click"}, {"mode", "init"},
                          {"handler", "calllua"}, {"click", "fn"}});
    {
        auto m = report();
        check(m.count("1") && (m.at("1") & uint16_t(DirtyAspect::Handlers)),
              "lyevent marks Handlers");
    }
    // lytween(left) → 目标 id Transform;活动期帧写不记(见 advance 后空报告)
    c.apply_lytween({{"id", "1"}, {"param", "left"}, {"from", "0"},
                     {"to", "100"}, {"time", "80"}},
                    0);
    {
        auto m = report();
        check(m.count("1") && (m.at("1") & uint16_t(DirtyAspect::Transform)),
              "lytween on left marks Transform on the target");
    }
    // 推进一帧(未结束:只做值写,不记簿记——由 animated 帧项覆盖)
    c.advance_tweens(40);
    check(report().empty(), "tween per-frame value writes are not bookkept");
    // 推进到结束帧:settle 也不记(由 s_prev_animated 覆盖);delete_on_finish
    // 的 remove 记(结构性写)
    c.advance_tweens(100);
    check(report().empty(), "settle frame not bookkept (animated-prev term)");
    // lytween delete=1 结束 → remove() 级联标记
    c.apply_lytween({{"id", "1"}, {"param", "left"}, {"from", "0"},
                     {"to", "50"}, {"time", "40"}, {"delete", "1"}},
                    0);
    (void)report(); // 注册帧
    c.advance_tweens(100);
    {
        auto m = report();
        check(m.count("1"), "delete_on_finish removal marks the subtree");
    }
    // [lytweendel] 实际 settle → 记;无 tween 的 del 不记
    c.apply_lytween({{"id", "1"}, {"param", "alpha"}, {"from", "1"},
                     {"to", "0.5"}, {"time", "500"}},
                    0);
    (void)report();
    c.apply_lytweendel("1");
    {
        auto m = report();
        check(m.count("1") && (m.at("1") & uint16_t(DirtyAspect::Opacity)),
              "lytweendel settle marks Opacity on the target");
    }
    c.apply_lytweendel("1"); // 无 tween:no-op
    check(report().empty(), "lytweendel with no tweens leaves no mark");
    // 视频绑定成功记 Content;解绑只在仍绑着视频帧时才记(匹配语义)
    check(c.bind_video_layer("1"), "video bind on an existing carrier");
    check(report().count("1") == 1, "video bind marks Content");
    // 场景期间重绑成静态图后,视频收尾解绑不匹配(旧文件名比对语义)
    c.set_layer_file("1", "other.png");
    (void)report();
    check(!c.unbind_video_layer("1") && report().empty(),
          "unmatched unbind leaves no mark");
    check(c.bind_video_layer("1") && report().count("1") == 1,
          "re-bind to the video frame marks Content");
    check(c.unbind_video_layer("1") && report().count("1") == 1,
          "matched unbind marks Content");
    // anime init/add/end 记 Content;未知 mode 不记
    c.apply_anime({{"id", "9"}, {"mode", "init"}, {"file", "an/f0.png"}}, 0);
    check(report().count("9") == 1, "anime init marks the layer");
    c.apply_anime({{"id", "9"}, {"mode", "add"}, {"file", "an/f1.png"},
                   {"time", "100"}},
                  0);
    check(report().count("9") == 1, "anime add marks the layer");
    c.apply_anime({{"id", "9"}, {"mode", "end"}, {"time", "100"}}, 0);
    check(report().count("9") == 1, "anime end marks the layer");
    c.apply_anime({{"id", "9"}, {"mode", "bogus"}}, 0);
    check(report().empty(), "unknown anime mode leaves no mark");
    // remove 子树逐 id 记
    c.create("2", {});
    c.create("2.1", {{"file", "fg/x"}});
    c.create("2.1.1", {});
    (void)report(); // 创建帧
    c.remove("2");
    {
        auto m = report();
        check(m.count("2") && m.count("2.1") && m.count("2.1.1"),
              "remove marks every subtree id");
    }
    c.remove("nope"); // 缺失 id no-op
    check(report().empty(), "remove of a missing id leaves no mark");
    // overlay 转场才记(幂等重设不记)
    c.set_overlay_file();
    check(report().count(oa::render::kOverlayNodeId) == 1,
          "overlay bind transition marks the slot");
    c.set_overlay_file(); // 幂等恢复:无变化
    check(report().empty(), "idempotent overlay re-bind leaves no mark");
    c.clear_overlay_file();
    check(report().count(oa::render::kOverlayNodeId) == 1,
          "overlay unbind transition marks the slot");
    c.clear_overlay_file(); // 空态重复
    check(report().empty(), "idempotent overlay clear leaves no mark");
    // clear_scene → "*"
    c.clear_scene();
    {
        auto m = report();
        check(m.count("*") == 1, "clear_scene marks pseudo '*'");
    }
}

// ---------------------------------------------------------------------------
// S4 (research/107): dirty_kind 活动态修正(105 §6 清单 2)—— ActivePlane
// 位语义 + dirty_kind_effective 修正矩阵。接口预留扩展,测试钉住规则表。
// ---------------------------------------------------------------------------
void test_dirty_kind_active_correction() {
    using oa::render::ActivePlane;
    using oa::render::DirtyKind;
    // 位组合语义
    check(oa::render::has_plane(ActivePlane::PropAnim, ActivePlane::PropAnim),
          "has_plane(single bit, same bit)");
    check(!oa::render::has_plane(ActivePlane::None, ActivePlane::None),
          "None has no set bit (intersection is empty)");
    const ActivePlane pa = ActivePlane::PropAnim;
    const ActivePlane combo = pa | ActivePlane::Trans | ActivePlane::Fade;
    check(oa::render::has_plane(combo, ActivePlane::PropAnim) &&
              oa::render::has_plane(combo, ActivePlane::Trans) &&
              oa::render::has_plane(combo, ActivePlane::Fade),
          "ActivePlane OR keeps every bit");
    check(!oa::render::has_plane(ActivePlane::Trans, ActivePlane::Fade),
          "ActivePlane bits are independent");
    // 修正矩阵:8 角色 × {无活动 / PropAnim / Trans / Fade / 全活动}
    const auto expect = [](LayerKind k, ActivePlane a, DirtyKind want,
                           const char* what) {
        check(oa::render::dirty_kind_effective(k, a) == want, what);
    };
    const ActivePlane all = ActivePlane::PropAnim | ActivePlane::Trans |
                            ActivePlane::Fade;
    for (int i = 0; i <= int(LayerKind::MessageSlot); ++i) {
        const LayerKind k = LayerKind(i);
        const DirtyKind base = oa::render::dirty_kind_of(k);
        // 无活动面 = 缺省表(逐位一致)
        expect(k, ActivePlane::None, base, "no active plane keeps default");
        // Trans/Fade(单独或叠加)覆盖为 PerFrame
        expect(k, ActivePlane::Trans, DirtyKind::PerFrame,
               "trans active -> PerFrame overrides default");
        expect(k, ActivePlane::Fade, DirtyKind::PerFrame,
               "fade active -> PerFrame overrides default");
        expect(k, all, DirtyKind::PerFrame,
               "trans+fade dominate any combined plane");
        // PropAnim:仅缺省 Static(None/Solid/Image)升级 PropAnimated
        const DirtyKind want =
            base == DirtyKind::Static ? DirtyKind::PropAnimated : base;
        expect(k, ActivePlane::PropAnim, want,
               "PropAnim upgrades only Static defaults");
    }
    // 抽几个具名断言作文档锚(与上表同规则)
    expect(LayerKind::Image, ActivePlane::PropAnim, DirtyKind::PropAnimated,
           "Image + active tween/anime -> PropAnimated");
    expect(LayerKind::Solid, ActivePlane::PropAnim, DirtyKind::PropAnimated,
           "Solid + active tween/anime -> PropAnimated");
    expect(LayerKind::Video, ActivePlane::PropAnim, DirtyKind::HostPump,
           "Video + tween keeps HostPump (pump dominates)");
    expect(LayerKind::Group, ActivePlane::PropAnim, DirtyKind::Grouped,
           "Group + tween keeps Grouped (bake dominates)");
    expect(LayerKind::MessageSlot, ActivePlane::PropAnim,
           DirtyKind::TextBound, "MessageSlot + tween keeps TextBound");
    expect(LayerKind::Video, ActivePlane::Trans, DirtyKind::PerFrame,
           "Video + trans -> PerFrame");
}

} // namespace

int main() {
    test_content_kinds();
    test_node_kinds();
    test_compositor_reachable();
    test_ledger_semantics();
    test_ledger_write_entries();
    test_dirty_kind_active_correction();
    if (failures == 0) {
        std::printf("layer_kind_test: all ok\n");
        return 0;
    }
    std::printf("layer_kind_test: %d failures\n", failures);
    return 1;
}
