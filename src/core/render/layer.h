#pragma once
// oa::render — Artemis layer tree.
// Layer ids are dotted strings kept verbatim ("1", "1.0", "500.b.2.0",
// "1.0.-1"); they are never numericised. Props are stored verbatim in
// `props`; the keys below are parsed on demand with these semantics:
//   - left/top are pixel offsets relative to the parent; x/left and y/top
//     are alias keys writing the same field
//   - alpha is 0-255 u8: integer first, float fallback clamped to 0..255,
//     parse failure keeps the previous value
//   - visible only accepts 1/on/true/yes vs 0/off/false/no; anything else
//     keeps the previous value; default true
//   - clip=[x,y,w,h] is a texture-pixel SOURCE sub-rect (sprite-atlas crop)
//     and defines the drawn quad size w x h. It is not a clickable area
//     and not a mask.
//   - width/height size only lyc solid-color quads
//   - color (lyc solid mode, clears file) and mask (lyc mask path) are
//     tracked as fields
// Ancestors of an id are auto-created as empty group nodes on create and
// set_props (Scene::ensure_path); set_props on a missing
// id therefore materializes the node. remove deletes
// the whole descendant subtree. "file" is ignored by
// set_props (it is only bound at create time).
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::render {
/// Overlay slot id: it IS the host-uploaded texture key too (single
/// source; the slot draws whatever texture is uploaded under this name).
static constexpr const char* kOverlayNodeId = "@video_overlay";


// 消息层场景槽不再使用字符串前缀编码(旧的 kMessagePrefix
// "openartemis-<hex(message id)>" 命名已删除):layered=0 独立消息的槽节点
// 由 Compositor 显式分配(见 ensure_message_scene_node / message_slots),
// 顺序由结构承担,id 只是诊断名。compare_ids 因此不再有消息前缀拼写分支。

// ---------------------------------------------------------------------------
// 2D affine (double precision), column-vector convention: a point (x,y) maps
// to x' = a*x + c*y + e, y' = b*x + d*y + f. Multiplication (M * N) applies N
// first, then M — column-vector composition (local_transform,
// world = parent * local). The layer quad lives in local
// layer coordinates ([0..w] x [0..h]); left/top (and any ancestor offset) is
// part of the transform, NOT an extra quad origin.
struct Affine2 {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    static Affine2 identity() { return {}; }
    static Affine2 translation(double tx, double ty) {
        Affine2 m;
        m.e = tx;
        m.f = ty;
        return m;
    }
    static Affine2 scaling(double sx, double sy) {
        Affine2 m;
        m.a = sx;
        m.d = sy;
        return m;
    }
    /// Rotation by `rad` (counter-clockwise in screen coords where y grows
    /// down; same handedness as the framework's from_angle).
    static Affine2 rotation(double rad) {
        Affine2 m;
        m.a = std::cos(rad);
        m.b = std::sin(rad);
        m.c = -std::sin(rad);
        m.d = std::cos(rad);
        return m;
    }
    /// (M * N): N applied to a point first, then M.
    Affine2 operator*(const Affine2& n) const {
        Affine2 m;
        m.a = a * n.a + c * n.b;
        m.b = b * n.a + d * n.b;
        m.c = a * n.c + c * n.d;
        m.d = b * n.c + d * n.d;
        m.e = a * n.e + c * n.f + e;
        m.f = b * n.e + d * n.f + f;
        return m;
    }
    void transform_point(double x, double y, double* ox, double* oy) const {
        *ox = a * x + c * y + e;
        *oy = b * x + d * y + f;
    }
    bool invertible() const { return a * d - b * c != 0.0; }
    Affine2 inverse() const {
        const double det = a * d - b * c;
        Affine2 m;
        m.a = d / det;
        m.b = -b / det;
        m.c = -c / det;
        m.d = a / det;
        m.e = -(m.a * e + m.c * f);
        m.f = -(m.b * e + m.d * f);
        return m;
    }
    /// Pure axis-aligned translation with a positive unit scale (common case
    /// for most Artemis UI): the fast SDL dst-rect path applies.
    bool is_plain_translation(double eps = 1e-9) const {
        return std::fabs(a - 1.0) < eps && std::fabs(d - 1.0) < eps &&
               std::fabs(b) < eps && std::fabs(c) < eps;
    }
};

// ---------------------------------------------------------------------------
// handler rows (LayerEventHandler / InputHandler).
// The engine stores registrations verbatim (params) and only dispatches them;
// it never interprets game function names.
// ---------------------------------------------------------------------------

/// One [lyevent] registration row on a layer for one event type.
struct LayerEventHandler {
    bool enabled = true;          // mode=disable only suspends (params kept)
    bool penetration = false;     // overlapping events reach this lower layer
    std::string handler;          // inline tag executed on hit ("calllua"); "" none
    std::string file;             // jump target file (inline-frame work deferred)
    std::string label;            // jump target label (inline-frame work deferred)
    bool call = false;            // call vs jump (deferred with inline frames)
    /// Registration extras (function/name/key/se/click/over/out …), passed to
    /// the Lua callback table verbatim (tags/).
    std::map<std::string, std::string> params;
    /// Full registration params for e:setEventFilter (extra + id/type/mode/
    /// file/label/handler + call/penetration=1 flags).
    std::map<std::string, std::string> filter_params;
};

/// One global input-handler row, keyed (event_name, key); [setonpush] etc.
struct InputHandler {
    std::string handler; // "calllua" (only tag FPM registers)
    std::string file;    // deferred with inline frames
    std::string label;   // deferred with inline frames
    bool call = false;
    std::map<std::string, std::string> params;        // key/adv/ui/btn/function…
    std::map<std::string, std::string> filter_params; // for e:setEventFilter
};

// ---------------------------------------------------------------------------
// 统一 invalidate
// 簿记的方面(方面粒度)。位掩码语义:同一 (id, aspect) 同帧
// 去重(集合);未列键(未知/自定义 verbatim)按 Content 兜底。区域脏区不在
// 本层;方面粒度只被簿记消费(角色分发才按方面)。
// ---------------------------------------------------------------------------
enum class DirtyAspect : uint16_t {
    None = 0,             // 无方面(note_content_change 的活动帧写不记哨兵
                          // —— 簿记不记,见 mark_dirty 注释)
    Content = 1 << 0,     // 文件/几何(宽高)/滤镜/color/mask 内容写
    Transform = 1 << 1,   // left/top/anchor/scale/rotate/reverse 变换写
    Clip = 1 << 2,        // clip 写
    Visibility = 1 << 3,  // visible 写
    Opacity = 1 << 4,     // alpha 写
    Handlers = 1 << 5,    // [lyevent] 行写(命中/拖拽行为面)
    TextBinding = 1 << 6, // 文本绑定/槽写(预留;文本域仍走 revision 项)
    HostFrame = 1 << 7,   // 宿主上传帧/画布内容写(video pump;emote 泵同面)
};

// ---------------------------------------------------------------------------
// 层内容来源 = "谁在提供这一层的内容"。
//
// 旧模型把内容种类编码进 file 字符串的保留命名空间(`__video_layer__:<id>` /
// `__emote_layer__:<id>`),分类(kind_of)与纹理缓存都靠前缀嗅探;现改由
// **写入口显式设置**的本字段承担 —— file 只是资源名,"怎么读这个资源"由该
// 状态投影出的内容角色实例决定(content_role.h 的 texture_key/content_quad,
// 缓存按 TextureKey 域分桶)。
//
// 只有"字符串表达不了"的宿主供帧面进本状态:资源文件(file 非空 → Image)、
// 纯色(color + w/h → Solid)、容器(空 file + 子/intermediate → Group)仍由
// 既有 typed 字段纯投影导出 —— 不给它们冗余状态,避免双份真值失同步。
//
// 生命周期(旧的拼写语义逐位保持):
//   - [video] 播放绑定 → VideoFrame(file/color/mask/path 清空;视频期间该层
//     画宿主上传帧);finish_video 解绑 → Unbound(层回到空态,不回滚旧图)。
//   - emote 物化绑定 → EmoteCanvas(盒 = PSB 设计盒,apply_emote_static 设置)。
//   - 场景重新绑定过的层不被视频收尾清掉(unbind_video_layer 只在仍是
//     VideoFrame 时生效 —— 旧 clear_layer_file_if_matches 的匹配语义)。
//   - 存档面零改动:本字段不进存档格式(存档只有 id+verbatim props+
//     handler 行),宿主供帧绑定由 [video]/emote 事件重放重建,与旧实现
//     "绑定从不进 props、读档后由事件重绑"逐位一致。
// ---------------------------------------------------------------------------
enum class LayerContent : uint8_t {
    Unbound,     // 无宿主供帧绑定:file 非空 = 资源文件;否则纯色/容器/空
    VideoFrame,  // 绑定到该层视频通道的宿主上传解码帧
    EmoteCanvas, // 绑定到该层 emote 画布的宿主上传帧(GPU 合成或 CPU 上传)
};

struct Layer {
    std::string id;                           // verbatim dotted id
    std::map<std::string, std::string> props; // verbatim Artemis params
    /// 内容来源(写入口显式设置 —— 分类不看 file 拼写,见上方枚举注释)。
    LayerContent content = LayerContent::Unbound;
    // ---- typed parse results (only written when parsing succeeds) ----
    bool visible = true;                      // default true (is_visible)
    /// 节点被脚本【创建】（[lyc]/[lyc2]/csvbtn 行,经 Compositor::create;
    /// 存档恢复的 create 同样置位）。消息绑定自动物化（ensure_path,chgmsg
    /// 绑定）以及仅经 [lyprop] 改写（Compositor::set_props —— 悬停/居中/显隐
    /// 之类的锚定改写）都【不】置位：文本绘制门槛 is_message_layer_drawable
    /// 只认"脚本创建出来的节点"或 [font] 排版盒,孤儿/无盒 overlay 打印
    /// （NekoMiko config.lua conf_mwsample 500.p03 数值读出、snll config
    /// uihelp 悬停 tip 500.z.help —— 数据没有给它们任何字表盒）不得仅凭
    /// 物化+锚定 lyprop 而获得绘制位。
    bool script_created = false;
    double left = 0;                          // px; alias keys: left|x
    double top = 0;                           // px; alias keys: top|y
    double width = 0;                         // 0 = not specified (solid quads only)
    double height = 0;
    double alpha = 1.0;                       // normalized 0..1 (parsed with u8 0-255 rules)
    // ---- typed transforms (semantics summarized in the file header) ----
    double anchor_x = 0;                      // px; default 0 = top-left
    double anchor_y = 0;
    double x_scale = 100.0;                   // percent; 100 = 1.0
    double y_scale = 100.0;
    double rotate_deg = 0;                    // degrees (radians at use)
    bool reverse_x = false;                   // negative scale sign
    bool reverse_y = false;
    std::string file;                         // bound texture name (Create only)
    std::string path;                         // oa magic-path prefix (cache key)
    bool has_clip = false;                    // texture source sub-rect [x,y,w,h]
    double clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
    bool has_color = false;                   // lyc solid-color mode (binding file cleared)
    uint8_t solid_rgba[4] = {0, 0, 0, 0};     // [r, g, b, a]
    std::string mask;                         // lyc mask path (compositing is separate)

    // ------------------------------------------------------------------
    // event handlers ([lyevent] rows; LayerEventHandler)
    // ------------------------------------------------------------------
    /// [lyevent] 注册的处理器，按事件类型键控（"click"/"rollover"/"rollout"/
    /// "dragin"/"drag"/"dragout"/"" …）。一次注册一行；mode reset/disable/enable
    /// 只增删改对应键。该行内容
    /// 对应 LayerEventHandler。
    std::map<std::string, LayerEventHandler> event_handlers;

    /// Merge one lyprop-style batch: verbatim-copy all keys, then parse the
    /// known geometry/visibility keys with the header semantics. Incremental:
    /// only keys present in `p` change; parse failures keep previous values.
    void set_props(const std::map<std::string, std::string>& p);

    /// "把 file 写成资源名" 的单一维护点 —— 内容来源状态
    /// (content)与 file 字段是两份真值,凡是由资源面写 file 的路径(lyc
    /// Create / set_layer_file / [anime] 帧直写)都必须同时解除宿主供帧绑定,
    /// 否则宿主供帧层被资源写覆盖后 kind_of 仍按 Video/Emote 走(旧实现靠
    /// file 拼写天然成立:写 file 即离开保留命名空间)。宿主供帧绑定自身
    /// (bind_video_layer/bind_emote_layer)不走本方法 —— 它们清空 file。
    void set_resource_file(std::string f) {
        file = std::move(f);
        content = LayerContent::Unbound;
    }

    /// Local affine of this layer relative to its parent: translate(left,top) *
    /// translate(anchor) * rotate * scale * translate(-anchor), scale carrying
    /// the reverse sign.
    Affine2 local_transform() const {
        const double sx = (x_scale / 100.0) * (reverse_x ? -1.0 : 1.0);
        const double sy = (y_scale / 100.0) * (reverse_y ? -1.0 : 1.0);
        Affine2 m = Affine2::translation(left, top);
        m = m * Affine2::translation(anchor_x, anchor_y);
        m = m * Affine2::rotation(rotate_deg * 3.14159265358979323846 / 180.0);
        m = m * Affine2::scaling(sx, sy);
        m = m * Affine2::translation(-anchor_x, -anchor_y);
        return m;
    }
};

/// Compare two dotted layer ids for draw order (compare_layer_id /
/// compare_id_part): segments split on '.', numeric segments
/// (i64 parse, optional sign) compare numerically, numeric sorts before
/// string, otherwise lexicographic; shorter prefixes first. Pure
/// segment comparison — the old message-prefix (message-last) spelling branch
/// is gone; independent-message slot ordering is structural (message_slots,
/// creation order), not a naming property.
int compare_ids(std::string_view a, std::string_view b);

/// 显式层树节点(替代旧扁平存储的 "id." 前缀扫描模拟树)。层本体 =
/// layer(layer.id 同时是 Compositor 内部索引键,一经创建不变);parent/children
/// 构成显式父子链,任何现存节点的全部祖先必在树中(ensure_path 不变量)。
/// 所有权在 Compositor::nodes_;存活节点地址稳定(渲染/诊断可在帧内持裸
/// 指针;场景突变后旧指针失效,与 draw_order 快照同规则)。字段仅由
/// Compositor 写,外部(渲染前序 DFS、诊断)只读。
struct SceneNode {
    Layer layer;
    SceneNode* parent = nullptr;   // nullptr = 根;非拥有
    /// 直接子节点,compare_ids 升序(前序 DFS == 全局 compare_ids 序);
    /// 非拥有(所有权在 Compositor)。
    std::vector<SceneNode*> children;
    /// 引擎消息槽标记 —— layered=0 独立消息的槽节点(位于消息槽区
    /// message_slots,不在场景 roots 树;渲染/顺序输出恒在场景区之后)。
    bool message_slot = false;
};

/// [lytween] 完成回执：tween 完成处理器的公开**名字**（定义 = render 内部面
/// core/render/render_internal.h）。Compositor 的 tween/anime
/// 方法只是把它按值传出；需要读写其字段的调用方（runtime 实现面与
/// tests/transition_test.cpp）自行 include render_internal.h。
struct TweenDone;

class Compositor {
public:
    /// 构造/析构外联（layer.cpp）：动画态经不透明 AnimStore 持有。
    /// unique_ptr 成员
    /// 使 Compositor 不再可拷贝（全仓无拷贝点：只按值持有/默认构造）。
    Compositor();
    ~Compositor();

    /// lyc/lyc2 equivalent: ensure id and ancestors exist, bind `file`/`path`
    /// (Create), then apply the remaining props (SetProperties pass; a color
    /// key switches the node to solid mode and unbinds the file).
    void create(const std::string& id, const std::map<std::string, std::string>& props);
    /// Overlay-video slot: a structural TOPMOST layer that is NOT part of
    /// the script layer tree (compare_ids is irrelevant to it). Bind `file`
    /// (a host-uploaded texture name) to draw the full-stage overlay above
    /// every script and message layer; empty file hides the slot. Managed by
    /// the runtime while an overlay video plays.
    void set_overlay_file();
    void clear_overlay_file();
   /// Overlay slot node when it exists and is bound+visible (renderers draw
    /// it structurally last, after the message-slot region).
    const SceneNode* overlay_node() const;
    /// Load-time reset: drop the whole layer tree plus tweens/anime/
    /// handler registries/message bindings and the "!" root props holder.
    void clear_scene();
    /// Save-restore of one [lyevent] row (scene snapshots carry the
    /// layer event registrations; Layer.event_handlers is part of
    /// the serialized Scene). No-op when the layer does not exist.
    void restore_event_handler_row(const std::string& id, const std::string& event_type,
                                   LayerEventHandler row);
    /// Save-restore of one global input row (setonpush family). The global
    /// input registry travels with scene snapshots like layer handler rows
    /// do: save/load restores the engine input state as of the save moment
    /// (quickload used to leave every push row lost).
    void restore_input_row(const std::string& event_name, const std::string& key,
                           InputHandler row);
    /// The global input registry (save snapshots iterate it; keyed by
    /// (event_name, key)).
    const std::map<std::pair<std::string, std::string>, InputHandler>&
    input_handlers() const {
        return input_handlers_;
    }
    /// lydel: remove id and its whole descendant subtree; no-op on missing id.
    /// Active tweens of the removed subtree are dropped and, when any of them
    /// belonged to a [tweenset] group, every other member of that group is
    /// cascade-removed across all layers.
    void remove(const std::string& id);
    /// lyprop: incremental merge; a missing id is materialized (with missing
    /// ancestors) before merging.
    void set_props(const std::string& id, const std::map<std::string, std::string>& props);
    /// 资源文件绑定(lyc Create 之后的再绑定面;file = 资源路径,Asset 域):
    /// 清除任何宿主供帧绑定(LayerContent::Unbound)与 solid/mask 状态,
    /// path 前缀一并有主。`allow_auto` 同 bind_video_layer 的物化门槛。
    bool set_layer_file(const std::string& id, const std::string& file,
                        bool allow_auto = false);
    /// [video] 播放绑定(取代旧的
    /// `set_layer_file(id, "__video_layer__:"+id)` 拼写绑定):该层内容 =
    /// 其视频通道的宿主上传解码帧(通道 id == 层 id);file/path/color/mask
    /// 全部让位(层不再画旧图),解绑后层回到空态 —— 与旧拼写绑定逐位等价。
    /// 缺失载体的物化门槛与旧 set_layer_file 完全一致:系统 overlay 区
    /// ("500." id 族)+ 调用方 allow_auto(稀疏环路门),
    /// 其余缺失即 no-op(false)。
    bool bind_video_layer(const std::string& id, bool allow_auto = false);
    /// [video] 解绑(取代旧的
    /// `clear_layer_file_if_matches(id, layer_texture_name(id))`):只在层
    /// 内容仍是该视频帧时解绑 —— 视频起停/EOF 收尾不得清掉场景期间重新
    /// 绑定过的层(匹配语义与旧实现一致,判定从 file 字符串比对升级为内容
    /// 来源状态比对)。返回是否实际解绑。
    bool unbind_video_layer(const std::string& id);
    /// emote 画布绑定(apply_emote_static:该层内容 = emote 画布,盒由调用方
    /// 先行 set_props 设置)。物化门槛与 bind_video_layer 同款。
    bool bind_emote_layer(const std::string& id, bool allow_auto = false);

    const Layer* find(const std::string& id) const;
    /// 只读节点句柄（分类权威 kind_of 的结构上下文与层角色派生用）。
    /// 返回 nullptr 当 id 不存在。只读,
    /// 不参与任何现有语义;调用方按 draw_order 快照同规则持用（存活期地址
    /// 稳定,场景突变后失效）。
    const SceneNode* scene_node(const std::string& id) const {
        return find_node(id);
    }

    // ------------------------------------------------------------------
    // 统一 invalidate
    // 簿记 — 先簿记后启用,双轨并存。mark_dirty(id, aspect) = 统一无效化
    // 入口(本层由 Compositor 写入口与宿主泵调用;同一 (id, aspect) 同帧
    // 集合去重,aspect 位掩码 OR 合并)。每帧决策点 consume_dirty_report()
    // 取走并清空(宿主仍按旧计数器渲染 — 输出未接;OA_DIRTY_AB 同进程 A/B
    // 差异记录)。簿记内容 = 实际发生的场景写(事件级与结构性写),
    // 动画活动帧的每帧值写不记(由 animated_now/prev 帧项覆盖)。
    // 簿记是**观察通道**(不改语义态;mutable —— 宿主渲染路径
    // 只有 const scene 句柄,仍须能上报 HostFrame 泵写),非 const 调用方
    // (Compositor 内部写入口)同样可用。
    // ------------------------------------------------------------------
    /// 统一 invalidate 入口:记录一次 (id, aspect) 写。伪 id:"" = 根 props
    /// holder(!,Layer-shaped,非节点),"*" = 全场景结构操作(clear_scene)。
    void mark_dirty(const std::string& id, DirtyAspect aspect) const;
    /// 自上次 consume 以来的脏报告(id → 方面位掩码;同帧去重后的集合)。
    const std::map<std::string, uint16_t>& dirty_report() const {
        return dirty_frame_;
    }
    /// 取走并清空本帧脏报告(每帧决策点恰好一次)。
    std::map<std::string, uint16_t> consume_dirty_report() const;
    /// 簿记上限丢弃计数(消费者缺失时的有界保护,诊断用;正常宿主每帧消费,
    /// 恒 0)。
    size_t dirty_ledger_dropped() const { return dirty_ledger_dropped_; }
    /// 显式树根节点表(compare_ids 升序;只读)。渲染/诊断前序 DFS
    /// 入口:替代旧的 draw_order+前缀重建(每帧一次,免逐帧前缀扫描)。
    const std::vector<SceneNode*>& roots() const { return roots_; }
    /// 引擎消息槽节点表(创建序;只读)。layered=0 独立消息的槽节点,
    /// 恒在场景区之后绘制/输出(旧 message-last 排序垫底的显式化)。
    /// 渲染 DFS 在遍历 roots 后再遍历本表。
    const std::vector<SceneNode*>& message_slots() const { return message_slots_; }
    /// Back-to-front draw order: 场景区显式树前序 + 消息槽区创建序(每层
    /// 兄弟有序 ⇒ 场景区前序 == compare_ids sorted-roots DFS == 旧全局
    /// stable_sort 序;槽区不再参与 id 排序)。
    std::vector<const Layer*> draw_order() const;
    size_t size() const { return nodes_.size(); }

    /// Scene::is_effectively_visible equivalent: the layer
    /// exists and neither it nor any of its (existing) dotted ancestors — nor
    /// the root props (!) layer — is hidden. Used to cull whole subtrees in
    /// draw/hit.
    bool is_effectively_visible(const std::string& id) const;
    /// Product of opacities along the dotted ancestor chain including the
    /// root props opacity and the layer itself
    /// (root_opacity * parent_opacity * opacity).
    double chain_opacity(const std::string& id) const;

    // ------------------------------------------------------------------
    // transform chain + hit geometry
    // ------------------------------------------------------------------

    /// [lyprop id="!"]: root-props merge. Never creates a
    /// "!" node. Its transform/opacity/visibility apply to the whole tree.
    void set_root_props(const std::map<std::string, std::string>& props);
    /// The root props holder (Layer-shaped props only; id empty, not a node).
    const Layer& root_props() const { return root_props_; }

    /// World affine of `id`: root-props local transform composed with every
    /// dotted ancestor from the top down and the layer itself.
    /// False when the layer does not exist.
    bool world_transform(const std::string& id, Affine2* out) const;
    /// World AABB of the layer's local content rect [0..w] x [0..h] under its
    /// full ancestor chain (transform_rect). False when the
    /// layer is missing.
    bool world_rect(const std::string& id, double w, double h, double* ox, double* oy,
                    double* ow, double* oh) const;

    // ------------------------------------------------------------------
    // message-layer registry (~ binding;
    // ensure_message_scene_node 槽位分配取代 id 侧编码)
    // ------------------------------------------------------------------

    /// chgmsg 切换/创建消息层时登记 message_id → scene_id 绑定，使
    /// [lyprop id="~xxx"] 解析到对应场景图层。
    void set_message_layer_binding(const std::string& message_id,
                                   const std::string& scene_id);
    /// 显式 chgmsg 重新选择消息层时脱离"随父删除失效"状态。
    void revive_message_layer(const std::string& message_id);
    /// 默认消息层（[lyprop id="~"] 用；未设置时忽略该操作）。
    void set_default_message_layer(const std::optional<std::string>& message_id);
    /// 解析 layer 事件里的特殊 ID：`~xxx` → 绑定表映射（未登记按 xxx 直查）；
    /// `~` → 默认消息层（无默认返回 nullopt = 忽略）；其余原样返回。
    /// `!` 由调用方在此之前单独分派。
    std::optional<std::string> resolve_message_target(const std::string& raw) const;
    /// 消息层当前绑定的场景目标（未登记返回消息层 id 自身；宿主文本摆位用）。
    std::string bound_scene_id(const std::string& message_id) const;
    /// 消息层当前是否有效可见（文本注入判定；宿主树规则，
    /// 实现见 layer.cpp 的规则注释）：显式删除
    /// 标记 ⇒ 不可见；绑定目标存在但不可有效可见 ⇒ 不可见；绑定目标是消息
    /// 槽节点或目标缺失 ⇒ 消息 id 宿主树锚点活性；layered identity 且
    /// 节点有效可见 ⇒ 可见。
    bool is_message_layer_visible(const std::string& message_id) const;
    /// 消息层当前是否可绘制（渲染器文本注入判定；is_message_layer_visible
    /// 之上叠加"绘制盒"门槛）：有效可见 且 绑定目标存在 且（文本层自带由
    /// [font] 排版键（left/top/width/height）配置的绘制盒 或 绑定目标节点是
    /// 脚本显式创建/改写的 script_created 节点）。未配置绘制盒的文本若只绑
    /// 在消息绑定自动物化的节点上（孤儿打印——游戏数据残留的未挂载
    /// ui_message 数值打印等）,不绘制:它没有任何脚本指定的落点,真机
    /// Artemis 对此类打印无可依附文本盒即不显示。
    bool is_message_layer_drawable(const std::string& message_id,
                                   bool text_positioned) const;
    /// 返回消息层的场景目标并确保节点存在（替代旧
    /// "openartemis-<hex>" 字符串编码）：
    ///  - layered=1 → message_id 自身（宿主树节点，ensure_path 物化于场景区，
    ///    按 id 正常排序）；
    ///  - layered=0 → 引擎消息槽节点（消息槽区，创建序，恒在场景区之后绘制；
    ///    槽 id 不随消息 id 变化——同名消息复用既有槽）。调用方随后照常
    ///    set_message_layer_binding/revive。
    std::string ensure_message_scene_node(const std::string& message_id,
                                          bool layered);
    /// id 是否引擎消息槽节点（槽区/结构判定，替代旧前缀字符串检查；
    /// 序列化快照跳过槽、宿主诊断用）。
    bool is_message_slot(const std::string& id) const;

    /// Host-side texture-size fallback for hit testing:
    /// layers without width/height or clip need their texture size. The
    /// resolver returns the texture's logical (stage) pixel size, or nullopt
    /// when the texture is not yet known.
    using QuadSizeFn = std::function<std::optional<std::pair<double, double>>(void*, const Layer&)>;
    /// Host-side pixel-alpha sampler for clickablethreshold:
    /// returns the texture alpha at texture pixel (tx,ty), or nullopt when the
    /// layer has no sampleable texture / the pixel is out of range (the caller
    /// then falls back to the layer alpha). Texture coords already include the
    /// clip offset; the host resolves the file.
    using AlphaSamplerFn =
        std::function<std::optional<uint8_t>(void*, const Layer&, int tx, int ty)>;
    /// Top-to-bottom list of layers at stage point (mx,my) that are
    /// effectively visible and whose hit quad (width/height > clip > texture)
    /// contains the point. A pure group node without a
    /// resolvable size never hits. Layers with a `clickablethreshold` are
    /// additionally tested against the injected alpha sampler (default
    /// sampler = unavailable -> layer-alpha fallback).
    std::vector<std::string> hit_test_all(double mx, double my, const QuadSizeFn& size,
        void* userdata, const AlphaSamplerFn& alpha = {}) const;
    /// Topmost hit; "" when none.
    std::string hit_test(double mx, double my, const QuadSizeFn& size,
        void* userdata, const AlphaSamplerFn& alpha = {}) const;

    // ------------------------------------------------------------------
    // event registries + draggable/dragarea
    // ------------------------------------------------------------------

    /// [lyevent] registration apply
    /// from verbatim tag params. `raw` must contain the full tag parameter map
    /// (id/type/mode/file/label/call/handler/penetration + extras). A row with
    /// no `type` is expanded the way the Artemis Lua global `lyevent` does
    /// (FPM glue, button.lua:125-128 / func.lua:776-786): one typed row per
    /// present click/over/out(/dragin/drag/dragout) function key.
    void apply_lyevent(const std::string& id, const std::map<std::string, std::string>& raw);
    /// Legacy layer-event tags [setonclick]/[setondrag]/… = lyevent with
    /// mode=init; [delonclick]/… = mode=reset under `event_type`.
    void apply_legacy_layer_event(const std::string& id, const std::string& event_type,
                                  bool reset, const std::map<std::string, std::string>& raw);
    /// Global input registry ([setonpush] family).
    void set_input_handler(const std::string& event_name,
                           const std::map<std::string, std::string>& raw);
    /// delonpush family: key "" clears the whole event name, otherwise the key.
    void del_input_handler(const std::string& event_name, const std::string& key);
    const InputHandler* get_input_handler(const std::string& event_name,
                                          const std::string& key) const;
    /// Any layer with an enabled handler row (hit-test interaction gate,
    /// applied at dispatch time in the runtime).
    bool has_any_enabled_handler(const std::string& id) const;
    const LayerEventHandler* find_event_handler(const std::string& id,
                                                const std::string& event_type) const;

    /// draggable=1 & dragarea clamp helpers.
    bool is_layer_draggable(const std::string& id) const;
    /// Layer (left, top) when it exists (layer_offset).
    std::optional<std::pair<double, double>> layer_offset(const std::string& id) const;
    /// drag_layer_to: clamp origin+delta into dragarea (default unbounded),
    /// write left/top props back, return the new position when the layer
    /// exists.
    bool drag_layer_to(const std::string& id, double origin_left, double origin_top,
                       double dx, double dy, double* out_left, double* out_top);

    /// clickablethreshold test at local (unscaled) layer coords. Sampler
    /// missing / layer without a sampleable texture
    /// falls back to the layer alpha.
    bool is_pointer_transparent(const Layer& l, double local_x, double local_y,
                                const AlphaSamplerFn& alpha, void* userdata) const;

    // ------------------------------------------------------------------
    // minimal animation (FPM transition path)
    // ------------------------------------------------------------------

    /// [lytween]: full Artemis param semantics. Params come from
    /// the tag verbatim (id/param/from/to/ease/time/delay/loop/yoyo/loopdelay/
    /// sync/delete/file/label/handler + extras). `to` parse failure ignores
    /// the request; missing `from` uses the layer's current
    /// property value. Inside an open [tweenset] the request is collected and
    /// only scheduled at [/tweenset].
    void apply_lytween(const std::map<std::string, std::string>& params, uint64_t clock_ms);
    /// [lytweendel]: settle every tween of `id` to its final value, clear the
    /// list, and cascade-remove same-group members on other layers without
    /// settling them.
    /// A removed tween that still carries a completion handler (the
    /// FPM script-timer idiom: lytween + handler=calllua on a dummy layer)
    /// has its completion DELIVERED once — deletion cancels the animation
    /// but ends the timer round, so the data's own completion bookkeeping
    /// (which clears the armed flag and rearms) cannot strand. Returns the
    /// delivered completions (empty when nothing pending was removed).
    std::vector<TweenDone> apply_lytweendel(const std::string& id);
    /// [tweenset] open/close: collected tweens are started
    /// sequentially — each entry's start = previous (delay + time) offset —
    /// and tagged with one shared set id.
    void tweenset_start();
    void tweenset_end(uint64_t clock_ms);

    /// [anime] event (verbatim [anime] tag params). Modes init/add/end map to
    /// apply_anime_event: init ensures the layer, binds its
    /// file/mask/merges its props and seeds a single-frame AnimeState (loop
    /// default -1); add appends one frame to the existing state; end sorts the
    /// frames by time, sets the total duration and starts playback from the
    /// current clock. Frame props only keep the AnimeHandler whitelist keys.
    void apply_anime(const std::map<std::string, std::string>& params, uint64_t clock_ms);

    /// Advance every active tween to `now_ms` and apply the current values to
    /// the target layers (typed fields + verbatim props;
    /// resolved_props). Tweens that finished are settled to
    /// their final value and removed; delete_on_finish removes the layer
    /// (cascading its subtree and group members). Returns the completion
    /// handlers the host should dispatch. Also advances active [anime] frame
    /// animations to `now_ms`.
    std::vector<TweenDone> advance_tweens(uint64_t now_ms);
    bool has_tweens() const;
    /// Whether a [anime] playback state exists (static-frame skip: an active
    /// animation makes every tick visually dirty).
    bool has_anime() const;
    /// Per-layer [anime] activity (anime_states_ buckets are dropped when the
    /// animation finishes; presence == running). Motion-gate granularity for
    /// [trans].
    bool layer_anime_active(const std::string& id) const;
    /// Sync-tween wait release condition: the layer is gone or holds no
    /// tween any more (sync_tween_finished, runtime/).
    bool layer_tweens_finished(const std::string& id) const;

private:
    // 内容状态写统一
    // 收口。值语义角色形态下 kind/content 投影在读取点即时派生、无缓存
    // 可刷,"重派生"无需动作;本方法 = 内容写后的统一簿记出口 + 状态角色
    // 升级时的唯一重派生插桩点(届时"上述方法尾部各加一次派生调用
    // 即可,不新增旁路写")。调用契约见定义处注释。
    void note_content_change(const std::string& id, DirtyAspect aspects);
    Layer* find_mut(const std::string& id);
    /// 宿主供帧绑定(视频/emote)的载体解析:已存在 → 直接用;缺失时按
    /// "500." 系统 overlay 区或 allow_auto 物化(物化门槛的单一出处,
    /// 语义 = 旧 set_layer_file 内联门槛)。
    Layer* resolve_host_bind_carrier(const std::string& id, bool allow_auto);
    void ensure_path(const std::string& id); // autovivify
    SceneNode* find_node(const std::string& id);
    const SceneNode* find_node(const std::string& id) const;
    /// 建一个新空节点挂到 parent(或 roots_，parent==nullptr)，兄弟保持
    /// compare_ids 升序；节点入 nodes_（唯一所有权，存活期地址稳定）。
    SceneNode* make_node(SceneNode* parent, const std::string& id);
    /// 分配一个引擎消息槽节点（单段诊断名 "@msg<n>"，不随消息 id
    /// 变化；不进 roots_，按创建序进 message_slots_）。
    SceneNode* make_message_slot();
    /// 从父（或 roots_）摘除 n；节点仍归 nodes_ 所有，由调用方 erase 释放。
    void detach_node(SceneNode* n);
    /// 子树（含自身）前序收集 id；remove 的级联清理（tween 桶 / 消息注册表）
    /// 由 "id. 前缀扫描" 改为子树集合判定（O(subtree) 直操作）。
    void collect_subtree(const SceneNode* n, std::vector<std::string>* out) const;
    /// 前序追加（draw_order / 渲染 DFS）。
    void append_preorder(const SceneNode* n, std::vector<const Layer*>* out) const;
    /// 宿主树锚点活性（is_message_layer_visible 内部辅助）：
    /// 沿 message_id 祖先链取最近现存节点的有效可见性作为消息文本活性
    /// （消息文本生命周期 = 宿主窗口树生命周期）；链上无任何现存
    /// 节点时：多点消息 id ⇒ false（宿主树已死），单段消息 id（引擎自管
    /// overlay，无宿主树概念）⇒ true。
    bool host_tree_anchor_visible(const std::string& message_id) const;
    /// Drop tweens of the subtree (子树 id 集合) and cascade-remove every
    /// [tweenset] group touched by them across all layers.
    void clear_tweens_of_subtree(const std::set<std::string>& subtree_ids);
    void apply_one_tween(const std::map<std::string, std::string>& params,
                         const std::optional<uint64_t>& delay_override,
                         std::optional<uint64_t> set_id, uint64_t clock_ms);
    // 显式层树（替代旧的扁平 vector<Layer> + "id." 前缀扫描模拟树）。
    // 所有权与索引合一：id → 节点；roots_ 只存无父节点指针。map 元素地址在
    // 节点存活期间稳定——渲染/诊断帧内持裸 const Layer* 安全（突变后失效，
    // 与 draw_order 快照同规则）。
    std::map<std::string, std::unique_ptr<SceneNode>> nodes_;
    // 无父节点（根），compare_ids 升序
    std::vector<SceneNode*> roots_;
    // 引擎消息槽区 — layered=0 独立消息的槽节点（创建序;恒在场景区
    // 之后绘制/输出;节点带 message_slot 标记,不进 roots_）。
    std::vector<SceneNode*> message_slots_;
    /// 消息 → 槽 id（独立于 message_layer_bindings_：绑定随宿主子树
    /// 删除被清，槽是引擎资源，映射活到 clear_scene / 槽被
    /// remove —— 保证宿主树删光后继续文本不反复建新槽）。
    std::map<std::string, std::string> message_slot_of_;
    uint64_t next_message_slot_serial_ = 0;
    // [lyprop id="!"] root-props holder (not a scene node; root_props)
    Layer root_props_;
    // 动画态（per-layer tween 桶 + [tweenset] 收集器 + [anime] 回放
    // 状态）收进不透明持有：anim/transition 类型不再出现在公开头。
    // 定义见 layer.cpp（struct Compositor::AnimStore）；
    // 成员声明顺序 = 原顺序 ⇒ 构造/析构顺序逐位不变。
    struct AnimStore;
    std::unique_ptr<AnimStore> anim_;
    // global input registry (input_handlers), keyed
    // (event_name, key)
    std::map<std::pair<std::string, std::string>, InputHandler> input_handlers_;
    // message-layer binding registry + default + deleted markers
    std::map<std::string, std::string> message_layer_bindings_;
    std::optional<std::string> default_message_layer_;
    std::set<std::string> deleted_message_layers_;
    // 统一 invalidate 簿记(本帧累积;id → 方面位掩码)。
    // 每帧 consume_dirty_report() 取走;消费者缺失时按上限丢弃并计数。
    // mutable:簿记是观察通道(不改语义态),宿主渲染
    // 路径仅持 const scene 句柄。
    mutable std::map<std::string, uint16_t> dirty_frame_;
    static constexpr size_t kDirtyLedgerCap = 4096;
    mutable size_t dirty_ledger_dropped_ = 0;
};

inline void Compositor::mark_dirty(const std::string& id, DirtyAspect aspect) const {
    // 有界簿记:宿主每帧消费(容量远够一帧写);无消费者的核心测试进程按上限
    // 丢弃并计数,防无限增长(丢弃只损失簿记证据,不影响任何行为)。
    if (dirty_frame_.size() >= kDirtyLedgerCap) {
        dirty_frame_.clear();
        ++dirty_ledger_dropped_;
    }
    uint16_t& bits = dirty_frame_[id];
    bits = uint16_t(bits | uint16_t(aspect));
}

inline std::map<std::string, uint16_t> Compositor::consume_dirty_report() const {
    std::map<std::string, uint16_t> out;
    out.swap(dirty_frame_);
    return out;
}

inline void Compositor::note_content_change(const std::string& id,
                                            DirtyAspect aspects) {
    // 内容状态落定后的统一收口 —— 逐位等价各写
    // 入口的既有簿记记法(create/set_props/绑定族/remove/clear_scene/…),
    // 簿记输出与收口前逐位一致(OA_DIRTY_AB 汇总同口径)。aspects == None =
    // 活动帧写(advance_tweens 值写/anime 帧写):簿记不记 —— 由
    // animated_now/prev 帧项覆盖,若记会与 legacy 失配(A/B 类差异)。
    if (aspects == DirtyAspect::None) return;
    mark_dirty(id, aspects);
}


} // namespace oa::render
