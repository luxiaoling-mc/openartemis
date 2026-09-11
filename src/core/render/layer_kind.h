#pragma once
// oa::render — 分类权威 layer_kind.h。
// LayerKind 枚举 + kind_of() 纯函数 = 单一分类权威
// （"种类 = 内容角色,不是生命周期类型"）。
//
// 本头文件只含新代码（纯枚举 + 纯函数,零行为面）;不修改 layer.h 现有字段,
// 不被 layer.h 反向包含。消费方:kind_of 被 content_role.h 与测试消费;
// 任何判定调整必须同步 layer_kind 的分类断言组。
//
// DirtyKind = 统一 invalidate 的 dirty_kind 性质**接口预留**
// （Static/PropAnimated/TextBound/HostPump/Grouped/PerFrame）;
// 目前只交付枚举 + 默认映射表,没有任何消费者,标注 future。
// 区域脏区（AABB）不在本工程范围。

#include <cstdint>
#include <string>
#include <string_view>

#include "core/render/layer.h"

namespace oa::render {

// ---------------------------------------------------------------------------
// 保留纹理命名空间前缀 (`__video_layer__:` / `__emote_layer__:`)已整体删除 ——
// 视频/emote 层不再靠 file 拼写判定,而由 Layer::content(写入口显式设置的
// 内容来源状态)判定;"怎么读这一层的资源文件"由该状态投影出的内容角色实例
// 决定(content_role.h 的 texture_key:Asset 域按路径解码,宿主供帧域查上传
// 纹理)。纹理缓存按 TextureKey 域分桶,撞名在结构上不可能,不再需要命名空间
// 约定。
// ---------------------------------------------------------------------------
/// 层角色/种类 = 内容状态纯函数导出（值序即判定优先序,见 kind_of 注释）:
enum class LayerKind : uint8_t {
    None,        // 空/孤儿:无内容无子（ensure_path 物化、等待重绑的载体;
                 // 含 has_color 但 w/h 缺省的"中间态"——draw_one 不画）
    Group,       // 容器/中间组:intermediate_render != 0（组语义,文件层也可
                 // 带但内容判定在前）或 file 空 + 有子节点（纯组织/顺序容器;
                 // 无文件 + clip = 内容裁剪窗语义）
    Solid,       // lyc 纯色:has_color && width>0 && height>0（file 必空——
                 // color 键路由清 file,set_layer_file 清 color）
    Image,       // 静态/动画图:file 非空、非宿主供帧（mask/clip/滤镜走
                 // 纹理面;anime 帧播放不改角色）
    Video,       // 内容来源 = LayerContent::VideoFrame（宿主上传流帧）
    Emote,       // 内容来源 = LayerContent::EmoteCanvas（宿主画布/上传帧）
    Overlay,     // 结构槽:id == kOverlayNodeId（@video_overlay;结构顶层,
                 // 非脚本树;绑定与否都是它）
    MessageSlot, // 结构槽:node.message_slot（layered=0 独立消息槽节点）
};

/// dirty_kind 性质（**接口预留,future**）:
/// 每类角色的"内容变化需要重绘/重烘焙"缺省性质。真实逐帧判定还依赖运行态
/// （活动 tween/anime → PropAnimated、活动视频/淡出 → PerFrame、组烘焙参与
/// → Grouped）,因此 dirty_kind_of 只给"无活动态时的默认性质"表;区域脏区
/// 不在本阶段。统一时钟注册/粒度化落地前不得有消费者。
enum class DirtyKind : uint8_t {
    Static,      // 内容只随显式事件变化（Image/Solid/None 缺省）
    PropAnimated,// 属性动画轨驱动（tween/anime 写回;横切服务,按活动态取）
    TextBound,   // 字形内容随 reveal/翻页/绑定变化（MessageSlot 缺省）
    HostPump,    // 宿主上传帧/画布 revision 泵（Video/Emote/Overlay 缺省）
    Grouped,     // 子树进烘焙缓存,任何子内容变化整体重烘焙（Group 缺省）
    PerFrame,    // 活动期每帧红（trans/fade/连续视频;运行态性质）
};

inline const char* layer_kind_name(LayerKind k) {
    switch (k) {
        case LayerKind::None: return "None";
        case LayerKind::Group: return "Group";
        case LayerKind::Solid: return "Solid";
        case LayerKind::Image: return "Image";
        case LayerKind::Video: return "Video";
        case LayerKind::Emote: return "Emote";
        case LayerKind::Overlay: return "Overlay";
        case LayerKind::MessageSlot: return "MessageSlot";
    }
    return "?";
}
inline const char* dirty_kind_name(DirtyKind k) {
    switch (k) {
        case DirtyKind::Static: return "Static";
        case DirtyKind::PropAnimated: return "PropAnimated";
        case DirtyKind::TextBound: return "TextBound";
        case DirtyKind::HostPump: return "HostPump";
        case DirtyKind::Grouped: return "Grouped";
        case DirtyKind::PerFrame: return "PerFrame";
    }
    return "?";
}

/// FUTURE 接口预留（统一时钟注册前无消费者）:角色缺省 dirty_kind 表
/// （各角色的"可裁剪粒度"与"脏区来源"缺省性质;
/// 活动态修正见 dirty_kind_of 头注释）。
inline DirtyKind dirty_kind_of(LayerKind k) {
    switch (k) {
        case LayerKind::None: return DirtyKind::Static;
        case LayerKind::Group: return DirtyKind::Grouped;
        case LayerKind::Solid: return DirtyKind::Static;
        case LayerKind::Image: return DirtyKind::Static;
        case LayerKind::Video: return DirtyKind::HostPump;
        case LayerKind::Emote: return DirtyKind::HostPump;
        case LayerKind::Overlay: return DirtyKind::HostPump;
        case LayerKind::MessageSlot: return DirtyKind::TextBound;
    }
    return DirtyKind::Static;
}

// ---------------------------------------------------------------------------
// 运行态活动面 —— dirty_kind 活动态修正的输入。面名按"每帧更新入口/时钟面"
// 命名:值 = 位标志,同一帧可多面同时活动。
// ---------------------------------------------------------------------------
enum class ActivePlane : uint8_t {
    None = 0,       // 无活动面
    PropAnim = 1,   // 属性动画轨活动:目标层有运行中 tween/[anime](tick 面 2;
                    // Compositor has_tweens/has_anime/layer_tweens_finished/
                    // layer_anime_active 查询)
    Trans = 2,      // [trans] 活动(等待机/渲染;transition().is_in_progress)
    Fade = 4,       // 全局淡出活动([alldelete] fade < 1.0;tick pre 面)
};

inline ActivePlane operator|(ActivePlane a, ActivePlane b) {
    return ActivePlane(uint8_t(a) | uint8_t(b));
}
inline bool has_plane(ActivePlane flags, ActivePlane bit) {
    return (uint8_t(flags) & uint8_t(bit)) != 0;
}

/// dirty_kind 活动态修正:缺省性质 + 运行态活动面 → 生效性质。纯函数,
/// **只作簿记/接口预留扩展**(测试钉住;统一时钟注册/粒度化前无消费者,
/// 不接渲染门)。修正规则:
///
///   Trans/Fade 活动 → PerFrame(任意缺省性质:活动期每帧红,过渡/淡出行
///   —— 覆盖一切)
///   PropAnim 活动 → 仅缺省 Static(None/Solid/Image:内容只随显式事件变化,
///   活动属性动画轨使其逐帧变)升级 PropAnimated;TextBound/HostPump/Grouped
///   的缺省性质已覆盖自身内容更新源(字形/泵帧/烘焙),叠加的属性动画不改变
///   主导性质 —— 该叠加以粒度化裁定为准,本表不预判
///   无活动面 → 缺省表(与 dirty_kind_of 逐位一致)
inline DirtyKind dirty_kind_effective(LayerKind k, ActivePlane active) {
    if (has_plane(active, ActivePlane::Trans) ||
        has_plane(active, ActivePlane::Fade))
        return DirtyKind::PerFrame;
    if (has_plane(active, ActivePlane::PropAnim) &&
        dirty_kind_of(k) == DirtyKind::Static)
        return DirtyKind::PropAnimated;
    return dirty_kind_of(k);
}

/// intermediate_render 组语义是否激活（verbatim props 键;与角色正交——
/// 任何角色节点都可带组语义,组路径/烘焙按它判定 renderer.cpp 的
/// group_path_required/group_needs_offscreen;文件空 + 有 clip 的中间组
/// = 内容裁剪窗,见 renderer.cpp 注释）。
inline bool intermediate_render_nonzero(const Layer& l) {
    const auto it = l.props.find("intermediate_render");
    if (it == l.props.end()) return false;
    // 与 renderer.cpp RenderEngine::intermediate_mode 同语义（stoi,失败=0）。
    // 注意:该处读取的是 verbatim props —— 与 typed 轨无关,原样复制判定。
    try {
        return std::stoi(it->second) != 0;
    } catch (...) {
        return false;
    }
}

/// 分类权威纯函数（内容态判定;自上而下首个命中即终值）:
///
///   1. node 为消息槽节点                → MessageSlot（结构行）
///   2. layer.id == kOverlayNodeId       → Overlay（结构行;id 即判定）
///   3. content == VideoFrame            → Video（显式内容来源状态）
///   4. content == EmoteCanvas           → Emote（显式内容来源状态）
///   5. file 非空(其余)                  → Image（资源纹理;与 draw_one
///                                          file 先于 color 的分支一致）
///   6. has_color && width>0 && height>0 → Solid（color 键清 file ⇒ 与 5
///                                          互斥;w/h 缺省 = 中间态,落 8）
///   7. file 空 && !Solid && (intermediate_render != 0 || 有子节点)
///                                      → Group（中间组 / 纯组织容器;
///                                          无文件 + clip 内容窗的承载者）
///   8. 其余                             → None/Empty（含 w/h 缺省的
///                                          has_color 中间态:draw_one
///                                          `!has_color || w<=0 || h<=0` 不画）
///
/// 该判定源 = Layer::content(写入口显式设置的内容来源状态),不再是 file
/// 拼写。—— "怎么读这一层的资源文件"是内容角色实例的事
/// (content_role.h:Asset 域按路径解码,VideoFrame/EmoteCanvas/OverlayFrame
/// 域查宿主上传纹理缓存),分类只需知道"谁在提供内容"。
/// 该判定与渲染分支输入条件的关系: 3/4/5 ↔ draw_one 的
/// file 分支与 quad_for_layer 的纹理域,6 ↔ draw_one 纯色分支,
/// 7 ↔ group_path_required/中间组语义与"文件空容器"裁剪窗,2 ↔ draw_scene
/// overlay 槽结构分支,1 ↔ 消息槽区结构绘制。
inline LayerKind kind_of(const Layer& layer, const SceneNode* node) {
    if (node && node->message_slot) return LayerKind::MessageSlot;
    if (layer.id == kOverlayNodeId) return LayerKind::Overlay;
    if (layer.content == LayerContent::VideoFrame) return LayerKind::Video;
    if (layer.content == LayerContent::EmoteCanvas) return LayerKind::Emote;
    if (!layer.file.empty()) return LayerKind::Image;
    if (layer.has_color && layer.width > 0 && layer.height > 0) return LayerKind::Solid;
    if (layer.file.empty() &&
        (intermediate_render_nonzero(layer) ||
         (node && !node->children.empty()))) return LayerKind::Group;
    return LayerKind::None;
}

/// 内容态分类（无树上下文;结构行 MessageSlot 与"纯子容器 Group"不可判定,
/// 按 node=nullptr 保守处理:children 视为空）。overlay（id 判定）、
/// 保留命名空间、solid、intermediate 组、空态全部可判。
inline LayerKind kind_of(const Layer& layer) { return kind_of(layer, nullptr); }

/// 全量分类（含结构行;node 不得为 nullptr）。
inline LayerKind kind_of(const SceneNode& node) { return kind_of(node.layer, &node); }

} // namespace oa::render
