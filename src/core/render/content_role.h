#pragma once
// oa::render — ContentRole 家族 = 层消费面
// "按内容状态分叉"读侧判定的等价委托层。
//
// 形态：角色 = **无状态单例**,派生 = 内容态 kind_of
// 纯函数(layer 单参视图)的直接投影(content_role_of)。节点不内嵌角色指针
// ("值语义轻量方案"):读取点即时派生,无缓存即无陈旧,
// 节点地址稳定零触碰。kind_of 保持唯一分类权威 —— 本头文件不新增
// 任何分类规则,只做 投影 kind → 角色类。
//
// **资源读取由角色实例决定** —— texture_key 返回
// TextureKey{域,名}(Asset 域 = path+file 资源文件,按 magic path 解码;
// VideoFrame/EmoteCanvas/OverlayFrame 域 = 宿主供帧,按层身份查上传纹理缓存),
// content_quad/命中/采样/尺寸探测全部经它。旧的保留命名空间约定
// (`__video_layer__:<id>` / `__emote_layer__:<id>`,分类与缓存都靠 file 拼写)
// 随之整体删除:分类改读 Layer::content(显式内容来源状态),缓存按域分桶。
//
// 等价纪律:每个方法体 = 现状分支条件的**逐字搬移**(注释给出处函数名);
// 不可达怪异态(如消息槽节点带 file)的内容面差异属理论差,可达面由
// tests/content_role_test.cpp 的矩阵断言(测试内参考实现对照,Release 同样
// 生效)与旅程 REF/NEW 交叉锁定。任何判定调整必须同步矩阵测试。
//
// 预留:角色 dirty_kind() = dirty_kind_of 缺省表;活动态
// 修正(anime/tween→PropAnimated、trans/fade→PerFrame)在统一时钟注册时
// 于角色类上覆盖。若升级为"节点内嵌有状态角色",写入口收敛清单在
// content_role 派生点(全部 Layer 内容写在 Compositor 方法内,单点挂接)。
#include <optional>
#include <string>
#include <utility>

#include "core/render/layer.h"
#include "core/render/layer_kind.h"
#include "core/render/texture_key.h"

namespace oa::render {

// ---------------------------------------------------------------------------
// 角色读取"纹理自然尺寸"的宿主面。同一查询原本散在 renderer 的 decoded /
// textures 缓存与 backend texture_size;RenderEngine 实现本接口后,角色
// 内容 quad/命中/采样共享同一读数面。
// 注:上传纹理的"存在"与"自然尺寸可得"在引擎内等价 —— 引擎创建纹理时尺寸
// 必知(create_texture w/h),backend texture_size 恒可得。
//
// 读数按 TextureKey 域分派 —— decoded 只服务 Asset 域,
// 宿主供帧域(video/emote/overlay)只走 textures。旧注释"保留命名空间永不在
// decoded"由域判定结构化承担,不再依赖任何名字拼写。
// ---------------------------------------------------------------------------
class ContentSizeSource {
public:
    virtual ~ContentSizeSource() = default;
    /// decoded 缓存里的解码资产尺寸(Asset 域;非 Asset 域恒 nullopt ——
    /// 宿主供帧不是可解码资产,从不进 decoded)。
    virtual std::optional<std::pair<double, double>> decoded_size(const TextureKey& key) const = 0;
    /// textures 缓存里的宿主供帧纹理自然尺寸(VideoFrame/EmoteCanvas/
    /// OverlayFrame 域;Asset 域恒 nullopt;无此纹理 = nullopt)。
    virtual std::optional<std::pair<double, double>> uploaded_size(const TextureKey& key) const = 0;
};

/// 内容 quad 的场景侧描述(renderer LocalQuad 的字段面;
/// renderer.cpp 在 quad_for_layer 出口转换 —— double→float 逐字段同现状)。
struct ContentQuad {
    bool has = false;
    double w = 0, h = 0;
    bool solid = false;  // 纯色 quad,无纹理
    bool has_src = false;
    double src_x = 0, src_y = 0, src_w = 0, src_h = 0;  // clip 纹理源子矩形
};

// ---------------------------------------------------------------------------
// 现状片段的共享辅助(renderer.cpp 各分支的逐字搬移;角色方法经它们组合)
// ---------------------------------------------------------------------------
namespace content_detail {

/// quad_for_layer 的 clip 前置片段原样:typed clip
/// 优先,任意角色同语义(与现状分支次序一致 —— clip 先于 file/color)。
inline bool clip_quad(const Layer& l, ContentQuad* q) {
    if (!l.has_clip) return false;
    q->w = l.clip_w;
    q->h = l.clip_h;
    q->src_x = l.clip_x;
    q->src_y = l.clip_y;
    q->src_w = l.clip_w;
    q->src_h = l.clip_h;
    q->has_src = true;
    q->has = true;
    return true;
}

/// 纹理内容 quad 片段(quad_for_layer 的 file 分支原样):
/// 解码资产自然尺寸(decoded 命中才 has)→ 上传纹理:层盒(width/height,
/// emote 设计盒/宿主绑定盒)优先,否则上传纹理自然尺寸。
/// 原注:host-uploaded frame textures(the reserved
/// `__emote_layer__:<id>` / `__video_layer__:<id>` namespaces — 现为
/// TextureKey 的 VideoFrame/EmoteCanvas 域)live only in the `textures`
/// cache — never in `decoded`(they are not decodable assets),so draw_one
/// used to skip the layer entirely(emote figures invisible in the window:
/// the 立绘 hide/show window diff was empty).Size them from the layer's own
/// width/height design box when set(the emote carrier is created with
/// width/height = the psb design size and the half-res figure texture
/// stretches across that box — only through the box do the ground-anchor
/// rows land at their stage rows),otherwise from the uploaded texture's
/// natural size.
///
/// key = 角色读取域键(调用方 = role.texture_key(l);Asset 域查 decoded,
/// 宿主供帧域查 textures —— 两域查询互斥,顺序与旧单键查表逐位等价)。
inline void textured_quad(const Layer& l, const ContentSizeSource& s,
                          const TextureKey& key, ContentQuad* q) {
    if (const auto di = s.decoded_size(key)) {
        q->w = di->first;
        q->h = di->second;
        q->has = true;
    } else if (const auto up = s.uploaded_size(key)) {
        if (l.width > 0 && l.height > 0) {
            q->w = l.width;
            q->h = l.height;
            q->has = true;
        } else {
            q->w = up->first;
            q->h = up->second;
            q->has = true;
        }
    }
}

/// 纯色 quad 片段(quad_for_layer 的 color 分支原样)。
inline void solid_quad(const Layer& l, ContentQuad* q) {
    q->solid = true;
    q->w = l.width;
    q->h = l.height;
    q->has = true;
}

} // namespace content_detail

// ---------------------------------------------------------------------------
// ContentRole:内容角色 = 读侧分叉的归属点。
// 共享公式(现状对所有角色同语义的判定)在基类默认实现,逐角色可达域不同的
// 面由子类覆盖(kind/dirty_kind/content_quad/纹理面归属)。
// ---------------------------------------------------------------------------
class ContentRole {
public:
    virtual ~ContentRole() = default;

    // ---- 身份/性质 ----
    virtual LayerKind kind() const = 0;
    /// dirty_kind 缺省表(dirty_kind_of);活动态修正见
    /// dirty_kind_for。
    virtual DirtyKind dirty_kind() const = 0;
    /// 缺省性质 + 运行态活动面(ActivePlane)下的生效
    /// dirty_kind —— 统一查 dirty_kind_effective(kind(), active)(活动态
    /// 修正:anime/tween→PropAnimated、trans/fade→PerFrame)。
    /// 角色 = 无状态单例、kind 固定 ⇒ 表驱动;若把修正下沉到具体角色类
    /// 再覆盖本方法(覆盖点预留)。只作簿记/接口扩展,不接渲染门。
    virtual DirtyKind dirty_kind_for(ActivePlane active) const {
        return dirty_kind_effective(kind(), active);
    }

    // ---- 内容形态准入(draw_one 分支门) ----
    /// 纹理内容形态:file 非空(draw_one 的 file 分支门)。宿主供帧角色
    /// (Video/Emote)以内容来源状态覆盖 —— 它们的资源不在 file 里。
    virtual bool textured_content(const Layer& l) const { return !l.file.empty(); }
    /// 纯色内容形态:file 空 && has_color && w/h>0(draw_one 纯色准入)。
    virtual bool solid_content(const Layer& l) const {
        return l.file.empty() && l.has_color && l.width > 0 && l.height > 0;
    }

    // ---- clip 双义:有文件层 = 纹理源子矩形;无文件容器 = 内容
    // 裁剪窗。二义解析入口归角色(数据仍在 typed clip 字段)。 ----
    /// clip 的纹理源语义(有文件层:quad src / 命中尺寸 / 采样偏移用)。
    virtual bool clip_texture_source(const Layer& l) const {
        return !l.file.empty() && l.has_clip;
    }
    /// clip 的内容裁剪窗语义(无文件容器;组烘焙裁剪窗判定
    /// group_needs_offscreen 的门)。
    virtual bool clip_crop_window(const Layer& l) const {
        return l.file.empty() && l.has_clip;
    }

    // ---- 内容 quad(clip 前置 = content_detail::clip_quad,
    // 其后接本角色的内容片段:纹理(Image/Video/Emote/Overlay)、纯色(Solid)、
    // 无(Group/Empty))----
    virtual ContentQuad content_quad(const Layer& l, const ContentSizeSource& s) const = 0;

    // ---- 资源读取域键:角色实例决定"怎么读这一层的资源"。
    // 现状重复四处:layer_file_key / quad_for_layer / hit_size /
    // alpha_sampler / runtime bound_image_size
    // → 收敛为角色一面,并由字符串升级为域键(Asset = path+file 资源文件;
    // 宿主供帧域 = 层身份;上传方/读取方同源单点)。 ----
    virtual TextureKey texture_key(const Layer& l) const {
        return asset_key(l.path.empty() ? l.file : l.path + l.file); // 原样公式
    }

    // ---- 纹理面归属(现状由"能否进 decoded"隐式承担 → 显式化)----
    /// 宿主上传帧/画布命名空间(视频流帧/emote 画布/overlay;不进 decoded,
    /// 渲染时走 textures 缓存)。
    virtual bool host_pumped() const = 0;
    /// 解码资产面(常规 file 命名空间;decoded 缓存 + mask 合成面可达)。
    virtual bool decodable_asset() const = 0;
    /// mask 合成面(texture_for_masked;`_m` 掩码组合
    /// 只对解码资产面可及 —— 上传绑定清 mask)。
    virtual bool mask_composited() const { return decodable_asset(); }
    /// 解码采样可行性(clickablethreshold/alpha 采样只走 decoded,上传纹理
    /// 回退层 alpha)。
    virtual bool sampleable() const { return decodable_asset(); }

    // ---- 组语义开关(与角色正交的性质;任何角色可带 intermediate_render;
    // intermediate_mode 的逐字搬移)----
    virtual int intermediate_mode(const Layer& l) const {
        const auto it = l.props.find("intermediate_render");
        if (it == l.props.end()) return 0;
        try {
            return std::stoi(it->second);
        } catch (...) {
            return 0;
        }
    }

    // ---- 内容在场(capture_transition_source 运动门:
    // 画面里有"运动图形"(有效可见的带内容层上有运行中 tween/[anime])时
    // [trans] 不生成 overlay —— 该门跳过无内容层,翻转条件逐字搬移)----
    virtual bool content_present(const Layer& l) const {
        return !l.file.empty() || l.has_color;
    }
};

/// 空/孤儿(ensure_path 物化、等待重绑的载体;含 has_color 但 w/h 缺省中间态
/// —— draw_one 不画)。无自内容;无组壳。
class EmptyContent final : public ContentRole {
public:
    LayerKind kind() const override { return LayerKind::None; }
    DirtyKind dirty_kind() const override { return DirtyKind::Static; }
    bool host_pumped() const override { return false; }
    bool decodable_asset() const override { return false; }
    ContentQuad content_quad(const Layer& l, const ContentSizeSource&) const override {
        ContentQuad q;
        content_detail::clip_quad(l, &q); // 与现状 quad_for_layer 的 clip 前置一致
        return q;                         // 无自内容尺寸(中间态 color 无 w/h 不画)
    }
};

/// 容器/中间组:intermediate_render != 0(组语义,文件层也可带但内容判定在前;
/// 文件空 + intermediate = 中间组壳)或 file 空 + 子节点(纯组织容器)。
/// 无文件 + clip = 内容裁剪窗。
class GroupContent final : public ContentRole {
public:
    LayerKind kind() const override { return LayerKind::Group; }
    DirtyKind dirty_kind() const override { return DirtyKind::Grouped; }
    bool host_pumped() const override { return false; }
    bool decodable_asset() const override { return false; }
    ContentQuad content_quad(const Layer& l, const ContentSizeSource&) const override {
        ContentQuad q;
        content_detail::clip_quad(l, &q); // 裁剪窗容器的 clip 面(组路径自行消费)
        return q;                         // 无自内容(子树即"内容")
    }
};

/// lyc 纯色:has_color && width>0 && height>0(file 必空 —— color 键路由清
/// file,set_layer_file 清 color)。tween 可驱动 w/h/alpha。
class SolidContent final : public ContentRole {
public:
    LayerKind kind() const override { return LayerKind::Solid; }
    DirtyKind dirty_kind() const override { return DirtyKind::Static; }
    bool host_pumped() const override { return false; }
    bool decodable_asset() const override { return false; }
    ContentQuad content_quad(const Layer& l, const ContentSizeSource&) const override {
        ContentQuad q;
        if (content_detail::clip_quad(l, &q)) return q;
        // 纯色准入(draw_one 同门;可达态恒真,保留逐字判据)
        if (l.has_color && l.width > 0 && l.height > 0)
            content_detail::solid_quad(l, &q);
        return q;
    }
};

/// 静态/动画图:file 非空、非保留命名空间(mask/clip/滤镜走纹理面;anime 帧
/// 播放不改角色)。纹理面 = decoded 解码资产;mask 合成可达。
class ImageContent final : public ContentRole {
public:
    LayerKind kind() const override { return LayerKind::Image; }
    DirtyKind dirty_kind() const override { return DirtyKind::Static; }
    bool host_pumped() const override { return false; }
    bool decodable_asset() const override { return true; }
    ContentQuad content_quad(const Layer& l, const ContentSizeSource& s) const override {
        ContentQuad q;
        if (content_detail::clip_quad(l, &q)) return q;
        content_detail::textured_quad(l, s, texture_key(l), &q); // 解码自然尺寸(上传面不可达)
        return q;
    }
};

/// 视频层:宿主上传的该层视频通道解码帧(由层的内容来源状态判定,
/// 不再嗅探 file 拼写;绑定可随时装上/摘下,同一节点视频/静态可往返 ——
/// 角色随绑定变化)。
class VideoContent final : public ContentRole {
public:
    LayerKind kind() const override { return LayerKind::Video; }
    DirtyKind dirty_kind() const override { return DirtyKind::HostPump; }
    bool host_pumped() const override { return true; }
    bool decodable_asset() const override { return false; }
    /// 视频通道帧键(通道 id = 层 id;上传方 app 与读取方渲染同源单点)。
    static TextureKey frame_key(const std::string& layer_id) {
        return TextureKey{TextureKey::Domain::VideoFrame, layer_id};
    }
    TextureKey texture_key(const Layer& l) const override { return frame_key(l.id); }
    /// 内容形态/在场:宿主供帧层的内容不在 file 里 —— 判定 = 内容来源状态
    /// (旧实现"file 非空"因绑定把保留命名空间写进 file 而隐式成立)。
    bool textured_content(const Layer& l) const override {
        return l.content == LayerContent::VideoFrame;
    }
    bool content_present(const Layer& l) const override {
        return l.content == LayerContent::VideoFrame;
    }
    ContentQuad content_quad(const Layer& l, const ContentSizeSource& s) const override {
        ContentQuad q;
        if (content_detail::clip_quad(l, &q)) return q;
        // 纹理面:decoded 恒 miss → 上传纹理盒/自然尺寸(原顺序逐条:
        // decoded 先、上传后,与现状一致)
        content_detail::textured_quad(l, s, texture_key(l), &q);
        return q;
    }
};

/// emote 层:宿主画布/上传帧(盒 = PSB 设计盒,apply_emote_static 设置)。
class EmoteContent final : public ContentRole {
public:
    LayerKind kind() const override { return LayerKind::Emote; }
    DirtyKind dirty_kind() const override { return DirtyKind::HostPump; }
    bool host_pumped() const override { return true; }
    bool decodable_asset() const override { return false; }
    /// emote 画布键(层 id 即画布身份;CPU 上传帧与 GPU 合成目标同名互换)。
    static TextureKey canvas_key(const std::string& layer_id) {
        return TextureKey{TextureKey::Domain::EmoteCanvas, layer_id};
    }
    TextureKey texture_key(const Layer& l) const override { return canvas_key(l.id); }
    /// 内容形态/在场:同 VideoContent —— 宿主供帧层的内容不在 file 里。
    bool textured_content(const Layer& l) const override {
        return l.content == LayerContent::EmoteCanvas;
    }
    bool content_present(const Layer& l) const override {
        return l.content == LayerContent::EmoteCanvas;
    }
    ContentQuad content_quad(const Layer& l, const ContentSizeSource& s) const override {
        ContentQuad q;
        if (content_detail::clip_quad(l, &q)) return q;
        content_detail::textured_quad(l, s, texture_key(l), &q);
        return q;
    }
};

/// overlay 结构槽:id == kOverlayNodeId(结构顶层,非脚本树;绑定与否都是它)。
/// 全舞台纹理(域键名 = kOverlayNodeId);忽略根 props 变换是 draw_scene 的
/// 结构特例,不进角色。
class OverlayContent final : public ContentRole {
public:
    LayerKind kind() const override { return LayerKind::Overlay; }
    DirtyKind dirty_kind() const override { return DirtyKind::HostPump; }
    bool host_pumped() const override { return true; }
    bool decodable_asset() const override { return false; }
    /// 全舞台 overlay 帧键(宿主内部面;名 = 槽 id)。
    static TextureKey frame_key() {
        return TextureKey{TextureKey::Domain::OverlayFrame, kOverlayNodeId};
    }
    TextureKey texture_key(const Layer&) const override { return frame_key(); }
    ContentQuad content_quad(const Layer& l, const ContentSizeSource& s) const override {
        ContentQuad q;
        if (content_detail::clip_quad(l, &q)) return q;
        content_detail::textured_quad(l, s, texture_key(l), &q); // 上传全舞台帧自然尺寸
        return q;
    }
};

// ---------------------------------------------------------------------------
// 派生投影(统一入口;值语义形态下写入口无需
// 动作,角色即 kind_of 内容视图的即时投影)
// ---------------------------------------------------------------------------
inline const ContentRole& content_role_of(LayerKind k) {
    static const EmptyContent kEmpty;
    static const GroupContent kGroup;
    static const SolidContent kSolid;
    static const ImageContent kImage;
    static const VideoContent kVideo;
    static const EmoteContent kEmote;
    static const OverlayContent kOverlay;
    switch (k) {
        case LayerKind::Group: return kGroup;
        case LayerKind::Solid: return kSolid;
        case LayerKind::Image: return kImage;
        case LayerKind::Video: return kVideo;
        case LayerKind::Emote: return kEmote;
        case LayerKind::Overlay: return kOverlay;
        case LayerKind::None: return kEmpty;
        case LayerKind::MessageSlot:
            break; // 结构行;内容面 = Empty(见类头)
    }
    return kEmpty;
}

/// 内容态投影(layer 单参视图;与 kind_of(layer) 同判据)。
inline const ContentRole& content_role_of(const Layer& l) {
    return content_role_of(kind_of(l));
}

/// 全量投影(含结构行:kind_of(node) 的 Group=纯容器/中间组与 MessageSlot 均
/// 有结构上下文;MessageSlot 槽节点的内容面 = EmptyContent —— 字形在
/// TextEngine,槽节点无自有内容)。
inline const ContentRole& content_role_of(const SceneNode& n) {
    const LayerKind k = kind_of(n);
    return k == LayerKind::MessageSlot ? content_role_of(LayerKind::None)
                                       : content_role_of(k);
}

} // namespace oa::render
