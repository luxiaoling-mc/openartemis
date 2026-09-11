// layer-model S3 (research/106; design docs 104 §4.2/§4.3, 105 §6):
// ContentRole 家族的矩阵断言组 ——
//  1) 投影一致性:content_role_of(状态) 的 kind == kind_of(状态)(内容态/全量);
//  2) 面方法 vs 原公式参考实现(测试内保留 renderer/runtime 原公式 ——
//     Release 也生效的"断言组",替代 105 §6 清单 1 建议的 NDEBUG 断言组:
//     角色方法体 = 逐字搬移,这里用参考实现对照锁死);
//  3) 内容 quad 矩阵:clip 前置/解码自然尺寸/上传盒/上传自然尺寸/纯色/无,
//     与 quad_for_layer(renderer.cpp:891-943)的参考实现逐字段相等;
//  4) 角色性质表:host_pumped/decodable/mask/sampleable/dirty_kind 缺省表。
// 断言 = NDEBUG 编译即关(仓库单测惯例);零行为面 —— 全部纯新增。
#include <cstdio>
#include <map>
#include <string>
#include <utility>

#include "core/render/content_role.h"
#include "core/render/layer.h"
#include "core/render/layer_kind.h"

namespace {

int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using oa::render::Compositor;
using oa::render::ContentQuad;
using oa::render::ContentRole;
using oa::render::ContentSizeSource;
using oa::render::DirtyKind;
using oa::render::Layer;
using oa::render::LayerKind;
using oa::render::SceneNode;
using oa::render::TextureKey;

// ---------------------------------------------------------------------------
// Mock 尺寸源:S5 起按 TextureKey 域分派(Asset → decoded 表;宿主供帧域 →
// uploaded 表;两表查询互斥,与 RenderEngine 的域门控同构)
// ---------------------------------------------------------------------------
struct MockSizeSource : public ContentSizeSource {
    std::map<std::string, std::pair<double, double>> decoded;  // Asset 域
    std::map<std::string, std::pair<double, double>> uploaded; // 宿主供帧域
    std::optional<std::pair<double, double>> decoded_size(const TextureKey& k) const override {
        if (!k.is_asset()) return std::nullopt;
        const auto it = decoded.find(k.name);
        return it == decoded.end() ? std::nullopt
                                   : std::optional<std::pair<double, double>>(it->second);
    }
    std::optional<std::pair<double, double>> uploaded_size(const TextureKey& k) const override {
        if (k.is_asset()) return std::nullopt;
        const auto it = uploaded.find(k.name);
        return it == uploaded.end() ? std::nullopt
                                    : std::optional<std::pair<double, double>>(it->second);
    }
};

// ---------------------------------------------------------------------------
// 参考实现:现状 quad_for_layer(renderer.cpp:891-943)的逐字公式 —— 测试内
// 保留原逻辑(clip → 解码自然尺寸 → 上传盒/自然尺寸 → 纯色),角色
// content_quad 必须逐字段相等。键由调用方给出(角色读取域键;S5 起 quad 与
// 键是两面,键自身由 texture_key 断言组单独钉住)。
// ---------------------------------------------------------------------------
ContentQuad reference_quad_for_layer(const Layer& l, const TextureKey& key,
                                     const ContentSizeSource& s) {
    ContentQuad q;
    if (l.has_clip) {
        q.w = l.clip_w;
        q.h = l.clip_h;
        q.src_x = l.clip_x;
        q.src_y = l.clip_y;
        q.src_w = l.clip_w;
        q.src_h = l.clip_h;
        q.has_src = true;
        q.has = true;
    } else if (!l.file.empty() || !key.is_asset()) {
        if (const auto di = s.decoded_size(key)) {
            q.w = di->first;
            q.h = di->second;
            q.has = true;
        } else if (const auto up = s.uploaded_size(key)) {
            if (l.width > 0 && l.height > 0) {
                q.w = l.width;
                q.h = l.height;
                q.has = true;
            } else {
                q.w = up->first;
                q.h = up->second;
                q.has = true;
            }
        }
    } else if (l.has_color && l.width > 0 && l.height > 0) {
        q.solid = true;
        q.w = l.width;
        q.h = l.height;
        q.has = true;
    }
    return q;
}

bool quad_eq(const ContentQuad& a, const ContentQuad& b) {
    return a.has == b.has && a.w == b.w && a.h == b.h && a.solid == b.solid &&
           a.has_src == b.has_src && a.src_x == b.src_x && a.src_y == b.src_y &&
           a.src_w == b.src_w && a.src_h == b.src_h;
}

// ---------------------------------------------------------------------------
// 1. 投影一致性 + 角色身份
// ---------------------------------------------------------------------------
void test_projection_identity() {
    // 内容态状态矩阵 → 角色 kind == kind_of
    {
        Layer l;
        l.id = "a";
        check(oa::render::content_role_of(l).kind() == oa::render::kind_of(l),
              "empty layer projection == kind_of");
    }
    {
        Layer l;
        l.id = "img";
        l.file = "bg/x";
        check(oa::render::content_role_of(l).kind() == LayerKind::Image,
              "plain file -> ImageContent");
    }
    {
        Layer l;
        l.id = "v";
        l.content = oa::render::LayerContent::VideoFrame;
        check(oa::render::content_role_of(l).kind() == LayerKind::Video,
              "content=VideoFrame -> VideoContent");
    }
    {
        Layer l;
        l.id = "e";
        l.content = oa::render::LayerContent::EmoteCanvas;
        check(oa::render::content_role_of(l).kind() == LayerKind::Emote,
              "content=EmoteCanvas -> EmoteContent");
    }
    {
        Layer l;
        l.id = "s";
        l.has_color = true;
        l.width = 10;
        l.height = 20;
        check(oa::render::content_role_of(l).kind() == LayerKind::Solid,
              "color + w/h -> SolidContent");
    }
    {
        Layer l;
        l.id = "g";
        l.props["intermediate_render"] = "1";
        check(oa::render::content_role_of(l).kind() == LayerKind::Group,
              "intermediate shell -> GroupContent");
    }
    {
        Layer l;
        l.id = oa::render::kOverlayNodeId;
        check(oa::render::content_role_of(l).kind() == LayerKind::Overlay,
              "overlay slot id -> OverlayContent");
    }
    // 结构行(node 投影):MessageSlot → EmptyContent;纯子容器 → GroupContent
    {
        Layer l;
        l.id = "@msg0";
        SceneNode n;
        n.layer = l;
        n.message_slot = true;
        const ContentRole& r = oa::render::content_role_of(n);
        check(r.kind() == LayerKind::None,
              "message slot node content role = EmptyContent");
    }
    {
        Layer l;
        l.id = "cont";
        SceneNode n;
        n.layer = l;
        SceneNode c;
        n.children.push_back(&c);
        check(oa::render::content_role_of(n).kind() == LayerKind::Group,
              "pure container (children) node projection = GroupContent");
    }
    // 内容态投影看不到子节点(单参 = 内容视图;空层仍 Empty)
    {
        Layer l;
        l.id = "cont";
        check(oa::render::content_role_of(l).kind() == LayerKind::None,
              "single-arg projection is the content view (children invisible)");
    }
}

// ---------------------------------------------------------------------------
// 2. 角色性质表(缺省 dirty_kind / 纹理面归属)
// ---------------------------------------------------------------------------
void test_role_properties() {
    for (int i = 0; i <= int(LayerKind::MessageSlot); ++i) {
        const LayerKind k = LayerKind(i);
        if (k == LayerKind::MessageSlot) continue; // 无内容角色类
        const ContentRole& r = oa::render::content_role_of(k);
        check(r.kind() == k, "role kind == mapped kind");
        check(r.dirty_kind() == oa::render::dirty_kind_of(k),
              "role dirty_kind == dirty_kind_of table");
    }
    // 纹理面归属
    check(!oa::render::content_role_of(LayerKind::Image).host_pumped() &&
              oa::render::content_role_of(LayerKind::Image).decodable_asset() &&
              oa::render::content_role_of(LayerKind::Image).mask_composited() &&
              oa::render::content_role_of(LayerKind::Image).sampleable(),
          "ImageContent = decodable asset face (mask/sample reachable)");
    for (const LayerKind k : {LayerKind::Video, LayerKind::Emote, LayerKind::Overlay}) {
        const ContentRole& r = oa::render::content_role_of(k);
        check(r.host_pumped() && !r.decodable_asset() && !r.mask_composited() &&
                  !r.sampleable(),
              "host-pumped role: upload face, no decoded face");
    }
    for (const LayerKind k : {LayerKind::None, LayerKind::Group, LayerKind::Solid}) {
        const ContentRole& r = oa::render::content_role_of(k);
        check(!r.host_pumped() && !r.decodable_asset() && !r.mask_composited() &&
                  !r.sampleable(),
              "content-less/solid role has no texture face");
    }
}

// ---------------------------------------------------------------------------
// 3. 面方法 vs 原公式(状态矩阵;经角色分发取值)
// ---------------------------------------------------------------------------
void test_role_gates() {
    const ContentRole& img = oa::render::content_role_of(LayerKind::Image);
    const ContentRole& solid = oa::render::content_role_of(LayerKind::Solid);
    const ContentRole& empty = oa::render::content_role_of(LayerKind::None);
    {
        Layer l;
        l.id = "x";
        // textured_content == !file.empty();solid_content == 原准入;content
        // _present == 运动门原条件;clip 二义两门 == 原条件
        check(!img.textured_content(l) && !empty.textured_content(l),
              "empty file -> no textured content");
        check(!solid.solid_content(l) && !empty.solid_content(l) && !img.solid_content(l),
              "no color/w-h -> no solid content");
        check(!img.content_present(l) && !solid.content_present(l),
              "file-less color-less -> no content presence");
        check(!img.clip_texture_source(l) && !img.clip_crop_window(l),
              "no clip -> both clip semantics off");
        l.file = "bg/x";
        check(img.textured_content(l) && !img.solid_content(l) && img.content_present(l),
              "file layer textured + present");
        check(!img.clip_texture_source(l) && !img.clip_crop_window(l),
              "file layer without clip: no clip semantics");
        l.has_clip = true;
        l.clip_w = 10;
        l.clip_h = 5;
        check(img.clip_texture_source(l) && !img.clip_crop_window(l),
              "file+clip still texture source");
    }
    {
        // 无文件容器:clip = 内容裁剪窗(组路径的 93/99 面)
        Layer l;
        l.id = "cont";
        l.has_clip = true;
        const ContentRole& r = oa::render::content_role_of(l);
        check(r.clip_crop_window(l) && !r.clip_texture_source(l),
              "file-less + clip = crop window (container semantics)");
        check(!r.textured_content(l) && !r.solid_content(l) && !r.content_present(l),
              "file-less + color-less container: no content, no presence");
        check(r.intermediate_mode(l) == 0, "no intermediate flag -> mode 0");
    }
    {
        // 纯色:has_color + w/h(参考公式同款)
        Layer l;
        l.id = "s";
        l.has_color = true;
        l.width = 30;
        l.height = 40;
        const ContentRole& r = oa::render::content_role_of(l);
        check(r.solid_content(l) && !r.textured_content(l) && r.content_present(l),
              "solid layer: solid content + presence (has_color)");
        check(!r.clip_crop_window(l) && !r.clip_texture_source(l),
              "solid file-less without clip: no clip semantics");
    }
    // intermediate_mode 解析(verbatim props;失败 = 0)与 renderer 同语义
    {
        Layer l;
        l.id = "g";
        l.props["intermediate_render"] = "2";
        const ContentRole& r = oa::render::content_role_of(l);
        check(r.intermediate_mode(l) == 2, "intermediate_render=2 parsed");
        l.props["intermediate_render"] = "abc";
        check(r.intermediate_mode(l) == 0, "parse failure -> 0");
        l.props.erase("intermediate_render");
        check(r.intermediate_mode(l) == 0, "absent -> 0");
    }
    // 资源读取域键(S5,research/108:角色实例决定怎么读一层的资源 ——
    // Asset 域 = path 前缀 + file 原公式;宿主供帧域 = 层身份)。
    {
        Layer l;
        l.id = "k";
        l.file = "fg/x";
        check(oa::render::content_role_of(l).texture_key(l) ==
                  TextureKey{TextureKey::Domain::Asset, "fg/x"},
              "image texture_key = Asset(path+file)");
        l.path = ":fg/";
        check(oa::render::content_role_of(l).texture_key(l) ==
                  TextureKey{TextureKey::Domain::Asset, ":fg/fg/x"},
              "image texture_key keeps the path prefix");
        check(oa::render::content_role_of(LayerKind::Image).texture_key(l).is_asset(),
              "Image role reads the Asset domain");
    }
    // 宿主供帧域键:名 = 层身份(上传方与读取方同源单点)
    {
        Layer v;
        v.id = "500.z.mv";
        v.content = oa::render::LayerContent::VideoFrame;
        check(oa::render::content_role_of(v).texture_key(v) ==
                  TextureKey{TextureKey::Domain::VideoFrame, "500.z.mv"},
              "video texture_key = VideoFrame(layer id)");
        check(oa::render::VideoContent::frame_key("500.z.mv") ==
                  oa::render::content_role_of(v).texture_key(v),
              "VideoContent::frame_key == role key (single source)");
        Layer e;
        e.id = "2.5";
        e.content = oa::render::LayerContent::EmoteCanvas;
        check(oa::render::content_role_of(e).texture_key(e) ==
                  TextureKey{TextureKey::Domain::EmoteCanvas, "2.5"},
              "emote texture_key = EmoteCanvas(layer id)");
        Layer o;
        o.id = oa::render::kOverlayNodeId;
        o.file = oa::render::kOverlayNodeId;
        check(oa::render::content_role_of(o).texture_key(o) ==
                  oa::render::OverlayContent::frame_key(),
              "overlay texture_key = OverlayFrame slot key");
        // 域隔离:同一名字在不同域不是同一个键(旧命名空间约定的替代面)
        check(TextureKey{TextureKey::Domain::VideoFrame, "x"} !=
                  TextureKey{TextureKey::Domain::EmoteCanvas, "x"},
              "same name in different domains is a different key");
        check(TextureKey{TextureKey::Domain::Asset, "x"} !=
                  TextureKey{TextureKey::Domain::VideoFrame, "x"},
              "asset and host-frame domains never collide");
    }
}

// ---------------------------------------------------------------------------
// 4. content_quad 矩阵 vs quad_for_layer 参考实现(逐字段)
// ---------------------------------------------------------------------------
void test_content_quad_matrix() {
    MockSizeSource s;
    s.decoded["bg/x"] = {1920.0, 1080.0};
    s.uploaded["v"] = {1280.0, 720.0};   // VideoFrame 域,名 = 层 id
    s.uploaded["e"] = {640.0, 900.0};    // EmoteCanvas 域
    s.uploaded[oa::render::kOverlayNodeId] = {1280.0, 720.0}; // OverlayFrame 域

    auto run_case = [&](const char* what, Layer l) {
        const ContentRole& role = oa::render::content_role_of(l);
        const TextureKey key = role.texture_key(l);
        const ContentQuad ref = reference_quad_for_layer(l, key, s);
        const ContentQuad got = role.content_quad(l, s);
        if (!quad_eq(ref, got)) {
            std::fprintf(stderr,
                         "FAIL: quad mismatch [%s]: ref{has=%d w=%.1f h=%.1f "
                         "solid=%d src=%d (%.0f,%.0f %.0fx%.0f)} got{has=%d "
                         "w=%.1f h=%.1f solid=%d src=%d (%.0f,%.0f %.0fx%.0f)}\n",
                         what, (int)ref.has, ref.w, ref.h, (int)ref.solid,
                         (int)ref.has_src, ref.src_x, ref.src_y, ref.src_w,
                         ref.src_h, (int)got.has, got.w, got.h, (int)got.solid,
                         (int)got.has_src, got.src_x, got.src_y, got.src_w,
                         got.src_h);
            ++failures;
        }
    };

    // Image:解码自然尺寸(decoded 命中/未命中)
    {
        Layer l;
        l.id = "img";
        l.file = "bg/x";
        run_case("image decoded", l);
        l.id = "img2";
        l.file = "missing.png";
        run_case("image not decoded", l);
    }
    // Image + path 前缀键
    {
        Layer l;
        l.id = "imgp";
        l.file = "x";
        l.path = ":fg/";
        run_case("image path key", l);
    }
    // Image + clip = 源子矩形(前置)
    {
        Layer l;
        l.id = "imgc";
        l.file = "bg/x";
        l.has_clip = true;
        l.clip_x = 10;
        l.clip_y = 20;
        l.clip_w = 100;
        l.clip_h = 200;
        run_case("image clip", l);
    }
    // Video/Emote/Overlay:decoded miss → 上传纹理(盒优先 / 自然尺寸)。
    // S5: 宿主供帧域键名 = 层 id(生产不变量:通道 id == 层 id),故这些用例
    // 的 id 与上传表项同名。
    {
        Layer l;
        l.id = "v";
        l.content = oa::render::LayerContent::VideoFrame;
        run_case("video natural", l);
        Layer w;
        w.id = "v"; // 同一通道(盒优先于上传纹理自然尺寸)
        w.content = oa::render::LayerContent::VideoFrame;
        w.width = 800;
        w.height = 450;
        run_case("video box", w);
        Layer e;
        e.id = "e";
        e.content = oa::render::LayerContent::EmoteCanvas;
        e.width = 320;
        e.height = 450; // emote 设计盒
        run_case("emote box", e);
        Layer o;
        o.id = oa::render::kOverlayNodeId;
        o.file = oa::render::kOverlayNodeId;
        run_case("overlay natural", o);
        Layer v3;
        v3.id = "v3";
        v3.content = oa::render::LayerContent::VideoFrame;
        run_case("video not uploaded", v3); // 无盒无上传:无 quad(draw 跳过)
    }
    // Solid:纯色 quad;clip+color 仍 clip 前置(与现状分支次序一致)
    {
        Layer l;
        l.id = "s";
        l.has_color = true;
        l.solid_rgba[0] = 255;
        l.width = 50;
        l.height = 60;
        run_case("solid", l);
        Layer c;
        c.id = "sc";
        c.has_color = true;
        c.width = 50;
        c.height = 60;
        c.has_clip = true;
        c.clip_x = 1;
        c.clip_y = 2;
        c.clip_w = 30;
        c.clip_h = 40;
        run_case("solid+clip (clip first)", c);
    }
    // 空态/中间态:clip 前置 / w-h 缺省 color 无 quad
    {
        Layer l;
        l.id = "e";
        run_case("empty", l);
        Layer mid;
        mid.id = "mid";
        mid.has_color = true;
        run_case("color w/o w/h intermediate", mid);
        Layer cont;
        cont.id = "cont";
        cont.has_clip = true;
        cont.clip_x = 0;
        cont.clip_y = 0;
        cont.clip_w = 800;
        cont.clip_h = 200;
        run_case("container clip (crop window layer)", cont);
        // 中间组壳(intermediate + clip):quad 面 = clip(组路径消费裁剪窗)
        Layer g;
        g.id = "g";
        g.props["intermediate_render"] = "1";
        g.has_clip = true;
        g.clip_x = 0;
        g.clip_y = 0;
        g.clip_w = 800;
        g.clip_h = 200;
        run_case("intermediate group with clip", g);
    }
}

// ---------------------------------------------------------------------------
// 5. Compositor 可达写入序列上的投影(角色随状态往返;同 layer_kind_test 序列)
// ---------------------------------------------------------------------------
void test_compositor_projection() {
    Compositor c;
    auto role_kind = [&](const std::string& id) {
        const oa::render::SceneNode* n = c.scene_node(id);
        return n ? oa::render::content_role_of(*n).kind() : LayerKind::None;
    };
    c.create("1", {{"file", "bg001_a"}, {"path", ":bg/"}});
    check(role_kind("1") == LayerKind::Image, "lyc file -> Image role");
    c.create("2", {{"color", "#ffffffff"}, {"width", "100"}, {"height", "50"}});
    check(role_kind("2") == LayerKind::Solid, "lyc color + w/h -> Solid role");
    c.create("3", {{"color", "#80000000"}});
    check(role_kind("3") == LayerKind::None, "color w/o w/h -> Empty role");
    c.set_props("3", {{"width", "640"}, {"height", "360"}});
    check(role_kind("3") == LayerKind::Solid, "w/h later -> Solid role");
    // 视频绑定 → 解绑 → 静态图 → emote(角色往返)
    c.bind_video_layer("3");
    check(role_kind("3") == LayerKind::Video, "video bind -> Video role");
    c.unbind_video_layer("3");
    check(role_kind("3") == LayerKind::None, "video unbind -> Empty role");
    c.set_layer_file("3", "sakura.png");
    check(role_kind("3") == LayerKind::Image, "still rebind -> Image role");
    c.set_props("3", {{"width", "300"}, {"height", "300"}});
    c.bind_emote_layer("3");
    check(role_kind("3") == LayerKind::Emote, "emote bind -> Emote role");
    // 结构行:中间组壳/纯子容器/槽/overlay
    c.create("4", {{"intermediate_render", "1"}, {"clip", "0,0,800,200"}});
    c.create("4.1", {{"file", "face/row.png"}});
    check(role_kind("4") == LayerKind::Group, "intermediate container -> Group role");
    c.set_props("500.z.help", {{"visible", "1"}});
    check(role_kind("500") == LayerKind::Group, "autovivified ancestor -> Group role");
    check(role_kind("500.z.help") == LayerKind::None,
          "autovivified empty carrier -> Empty role");
    const std::string slot = c.ensure_message_scene_node("msg0", false);
    check(role_kind(slot) == LayerKind::None, "message slot -> Empty content role");
    c.set_overlay_file();
    check(role_kind(oa::render::kOverlayNodeId) == LayerKind::Overlay,
          "overlay slot -> Overlay role");
    c.remove("3");
    c.clear_scene();
}

// ---------------------------------------------------------------------------
// S4 (research/107):角色 dirty_kind_for(活动态修正)与
// dirty_kind_effective(kind, active) 全角色一致性
// ---------------------------------------------------------------------------
void test_dirty_kind_for() {
    using oa::render::ActivePlane;
    const auto r = [](LayerKind k) -> const oa::render::ContentRole& {
        return oa::render::content_role_of(k);
    };
    // 内容态角色(None..Overlay:角色 kind 与入参一致):dirty_kind_for =
    // effective(自身 kind, active)(角色 kind 固定 ⇒ 表驱动;S5 若把修正
    // 下沉到具体角色类再覆盖本方法)。
    for (int i = 0; i <= int(LayerKind::Overlay); ++i) {
        const LayerKind k = LayerKind(i);
        const oa::render::ContentRole& role = r(k);
        check(role.kind() == k, "content role projection kind identity");
        for (int j = 0; j <= int(ActivePlane::Fade); ++j) {
            const ActivePlane a = ActivePlane(j);
            check(role.dirty_kind_for(a) ==
                      oa::render::dirty_kind_effective(k, a),
                  "dirty_kind_for(active) == effective(kind, active)");
        }
    }
    // MessageSlot 内容面 = EmptyContent(kind None,106 §2.1/§5):角色面性质
    // 按 None 查表;结构行(槽节点 = 字形承载)的 TextBound 修正是全量分类
    // 面 —— dirty_kind_effective(LayerKind::MessageSlot, ...) 由
    // layer_kind_test 钉住;S5 时钟注册用 node 版全量分类取结构行。
    check(r(LayerKind::MessageSlot).kind() == LayerKind::None,
          "MessageSlot content face projects EmptyContent");
    // 文档锚:修正表要点(与 layer_kind_test 同规则)
    check(r(LayerKind::Image).dirty_kind_for(ActivePlane::PropAnim) ==
              DirtyKind::PropAnimated,
          "ImageContent tween/anime -> PropAnimated");
    check(r(LayerKind::Solid).dirty_kind_for(ActivePlane::PropAnim) ==
              DirtyKind::PropAnimated,
          "SolidContent tween/anime -> PropAnimated");
    check(r(LayerKind::Video).dirty_kind_for(ActivePlane::PropAnim) ==
              DirtyKind::HostPump,
          "VideoContent tween keeps HostPump (pump dominates)");
    check(r(LayerKind::Video).dirty_kind_for(ActivePlane::Trans) ==
              DirtyKind::PerFrame,
          "VideoContent trans -> PerFrame");
    check(r(LayerKind::Emote).dirty_kind_for(ActivePlane::Fade) ==
              DirtyKind::PerFrame,
          "EmoteContent fade -> PerFrame");
}

} // namespace

int main() {
    test_projection_identity();
    test_role_properties();
    test_role_gates();
    test_content_quad_matrix();
    test_compositor_projection();
    test_dirty_kind_for();
    if (failures == 0) {
        std::printf("content_role_test: all ok\n");
        return 0;
    }
    std::printf("content_role_test: %d failures\n", failures);
    return 1;
}
