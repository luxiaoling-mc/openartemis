#include "core/render/layer.h"
#include "core/render/render_internal.h" // tween/[anime]/[trans] 语义类型（render 内部面）
#include "core/render/content_role.h" // 角色委托: draw_order overlay-live 读侧判定

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <charconv> // §8: format_tween_param 的 std::to_chars
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>

namespace oa::render {

// ---------------------------------------------------------------------------
// 动画态（tween 桶 + [tweenset] 收集器 + [anime] 回放）
// 由 layer.h 的 5 个具名成员收进本不透明结构。成员声明顺序 = 原顺序 ⇒
// 构造/析构顺序逐位不变；AnimStore 只在本 TU 可见（layer.h 只前向声明），
// anim/transition 类型因此不再出现在任何公开头。
// ---------------------------------------------------------------------------
struct Compositor::AnimStore {
    // tween 桶（per-layer active tweens, tweenset collector）
    std::map<std::string, std::vector<Tween>> tweens_;
    bool collecting_set_ = false;
    std::vector<std::map<std::string, std::string>> set_pending_;
    uint64_t next_tween_set_id_ = 1;
    // [anime] 帧动画回放状态（anime_states）
    std::map<std::string, AnimeState> anime_states_;
};

Compositor::Compositor() : anim_(std::make_unique<AnimStore>()) {}
Compositor::~Compositor() = default;

// layer.h 内的内联访问器外联化（同一表达式，仅动画态改经 anim_）。
bool Compositor::has_tweens() const { return !anim_->tweens_.empty(); }
bool Compositor::has_anime() const { return !anim_->anime_states_.empty(); }
bool Compositor::layer_anime_active(const std::string& id) const {
    return anim_->anime_states_.count(id) > 0;
}

namespace {

// ---------------------------------------------------------------------------
// 写入口 → 方面位的键扫描。只按"批里出现
// 哪些键"归类(set_props/lytween param 同语义);未知/自定义键按 Content
// 兜底(intermediate_render/colormultiply/layermode 等滤镜面都是 Content)。
// 归类只被统一 invalidate 簿记消费;方面清单见 layer.h DirtyAspect。
// ---------------------------------------------------------------------------
DirtyAspect props_aspect(const std::map<std::string, std::string>& p) {
    DirtyAspect a = DirtyAspect::Content; // 几何(宽高)/文件/color/mask/滤镜/未知
    for (const auto& [k, v] : p) {
        (void)v;
        if (k == "left" || k == "x" || k == "top" || k == "y" ||
            k == "anchorx" || k == "anchory" || k == "xscale" ||
            k == "yscale" || k == "zoom" || k == "rotate" ||
            k == "reversex" || k == "reversey") {
            a = DirtyAspect(uint16_t(a) | uint16_t(DirtyAspect::Transform));
        } else if (k == "visible") {
            a = DirtyAspect(uint16_t(a) | uint16_t(DirtyAspect::Visibility));
        } else if (k == "alpha") {
            a = DirtyAspect(uint16_t(a) | uint16_t(DirtyAspect::Opacity));
        } else if (k == "clip") {
            a = DirtyAspect(uint16_t(a) | uint16_t(DirtyAspect::Clip));
        } else {
            // Content 兜底(含 width/height/color/mask/file/自定义键)
        }
    }
    return a;
}

std::string_view trim_view(std::string_view v) {
    while (!v.empty() && std::isspace((unsigned char)v.front())) v.remove_prefix(1);
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.remove_suffix(1);
    return v;
}

std::string ascii_lower(std::string_view v) {
    std::string s(v);
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

/// Full-string f64-style parse; empty / trailing garbage / non-finite fail.
bool parse_f64(std::string_view v, double* out) {
    v = trim_view(v);
    if (v.empty()) return false;
    const std::string s(v);
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    if (!std::isfinite(d)) return false; // inf/nan never appear in real scripts
    *out = d;
    return true;
}

/// alpha parse: integer u8 first, then
/// float fallback clamped to 0..=255; failure -> nullopt (keep previous value).
std::optional<int> parse_alpha_u8(std::string_view v) {
    v = trim_view(v);
    if (v.empty()) return std::nullopt;
    const std::string s(v);
    // integer path (u8 range)
    char* iend = nullptr;
    errno = 0;
    const long long iv = std::strtoll(s.c_str(), &iend, 10);
    const bool int_ok = errno != ERANGE && iend != s.c_str() && *iend == '\0';
    if (int_ok && iv >= 0 && iv <= 255) return (int)iv;
    // float fallback: any f32-parseable value is clamped to 0..=255
    double fv = 0;
    if (parse_f64(s, &fv) && !std::isnan(fv)) {
        if (fv < 0) fv = 0;
        if (fv > 255) fv = 255;
        return (int)fv;
    }
    return std::nullopt;
}

/// visible parse: only explicit
/// true/false spellings succeed; anything else keeps the previous value.
std::optional<bool> parse_bool(std::string_view v) {
    const std::string s = ascii_lower(trim_view(v));
    if (s == "1" || s == "on" || s == "true" || s == "yes") return true;
    if (s == "0" || s == "off" || s == "false" || s == "no") return false;
    return std::nullopt;
}

/// clip parse: the first four
/// comma-separated numbers (each trimmed) must parse; fewer than four
/// components is invalid; extras are ignored.
std::optional<std::array<double, 4>> parse_clip_rect(std::string_view v) {
    std::array<double, 4> out{};
    for (int i = 0; i < 4; ++i) {
        const size_t comma = v.find(',');
        const std::string_view part = trim_view(v.substr(0, comma));
        if (!parse_f64(part, &out[(size_t)i])) return std::nullopt;
        if (i < 3) {
            if (comma == std::string_view::npos) return std::nullopt; // too few
            v.remove_prefix(comma + 1);
        }
    }
    return out;
}

/// hex color parse: optional
/// #/0x prefix, RRGGBB (A=255) or AARRGGBB; returns [a, r, g, b].
std::optional<std::array<uint8_t, 4>> parse_hex_color(std::string_view v) {
    v = trim_view(v);
    if (v.size() > 1 && (v[0] == '#')) v.remove_prefix(1);
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v.remove_prefix(2);
    if (v.size() != 6 && v.size() != 8) return std::nullopt;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::array<uint8_t, 4> out{};
    for (size_t i = 0; i < v.size(); ++i) {
        const int h = hex(v[i]);
        if (h < 0) return std::nullopt;
        if (i % 2 == 0)
            out[i / 2] = (uint8_t)(h << 4);
        else
            out[i / 2] = (uint8_t)(out[i / 2] | h);
    }
    if (v.size() == 6) {
        const uint8_t r = out[0], g = out[1], b = out[2];
        out[0] = 255;
        out[1] = r;
        out[2] = g;
        out[3] = b;
    }
    return out;
}

// Compare one id segment the way an i64 parse would classify it.
bool i64_segment(std::string_view seg, long long* value) {
    if (seg.empty()) return false;
    size_t i = 0;
    if (seg[0] == '+' || seg[0] == '-') {
        if (seg.size() == 1) return false;
        i = 1;
    }
    for (; i < seg.size(); ++i)
        if (!std::isdigit((unsigned char)seg[i])) return false;
    const std::string s(seg);
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    *value = v;
    return true;
}

} // namespace

void Layer::set_props(const std::map<std::string, std::string>& p) {
    for (const auto& [k, v] : p) props[k] = v; // verbatim, all keys (incl. custom)
    // typed pass: only keys present in this batch; std::map iteration is
    // sorted, so when both alias spellings appear the later key (x over left,
    // y over top) wins deterministically (a hash-map order is
    // unspecified for that degenerate case; scripts never send both).
    for (const auto& [k, v] : p) {
        double d = 0;
        if ((k == "left" || k == "x") && parse_f64(v, &d)) {
            left = d;
        } else if ((k == "top" || k == "y") && parse_f64(v, &d)) {
            top = d;
        } else if (k == "width" && parse_f64(v, &d)) {
            width = d;
        } else if (k == "height" && parse_f64(v, &d)) {
            height = d;
        } else if (k == "anchorx" && parse_f64(v, &d)) {
            anchor_x = d;
        } else if (k == "anchory" && parse_f64(v, &d)) {
            anchor_y = d;
        } else if (k == "xscale" && parse_f64(v, &d)) {
            x_scale = d; // percent; 100 = 1.0
        } else if (k == "yscale" && parse_f64(v, &d)) {
            y_scale = d;
        } else if (k == "zoom") {
            // shorthand that sets both scale axes at once
            if (parse_f64(v, &d)) {
                x_scale = d;
                y_scale = d;
            }
        } else if (k == "rotate" && parse_f64(v, &d)) {
            rotate_deg = d; // degrees
        } else if (k == "reversex") {
            if (const auto b = parse_bool(v)) reverse_x = *b;
        } else if (k == "reversey") {
            if (const auto b = parse_bool(v)) reverse_y = *b;
        } else if (k == "alpha") {
            if (const auto a = parse_alpha_u8(v)) alpha = double(*a) / 255.0;
        } else if (k == "visible") {
            if (const auto b = parse_bool(v)) visible = *b;
        } else if (k == "clip") {
            if (const auto r = parse_clip_rect(v)) {
                has_clip = true;
                clip_x = (*r)[0];
                clip_y = (*r)[1];
                clip_w = (*r)[2];
                clip_h = (*r)[3];
            }
            // parse failure keeps the previous clip (`.or`)
        } else if (k == "color") {
            // lyc solid mode: routed color unbinds file
            if (const auto rgba = parse_hex_color(v)) {
                has_color = true;
                solid_rgba[0] = (*rgba)[1]; // r
                solid_rgba[1] = (*rgba)[2]; // g
                solid_rgba[2] = (*rgba)[3]; // b
                solid_rgba[3] = (*rgba)[0]; // a
                file.clear();
                // color 路由同时解除宿主供帧绑定 —— 旧实现
                // 靠 file 清空隐式降级(Solid),内容来源状态必须同步(纯色层
                // 不再是视频/emote 层)。
                content = LayerContent::Unbound;
            }
        } else if (k == "mask") {
            mask = v; // set_mask: empty string clears
        }
        // unknown keys and "file" stay verbatim-only
    }
}

int compare_ids(std::string_view a, std::string_view b) {
    // 纯段比较 — 旧消息前缀(message-last)拼写分支已删:独立消息槽
    // 的顺序是结构性的(message_slots 创建序),不由 id 承担。
    size_t ia = 0, ib = 0;
    for (;;) {
        const size_t da = a.find('.', ia);
        const size_t db = b.find('.', ib);
        const std::string_view sa =
            a.substr(ia, da == std::string_view::npos ? a.size() - ia : da - ia);
        const std::string_view sb =
            b.substr(ib, db == std::string_view::npos ? b.size() - ib : db - ib);
        long long na = 0, nb = 0;
        const bool an = i64_segment(sa, &na);
        const bool bn = i64_segment(sb, &nb);
        if (an || bn) {
            if (an && bn) {
                if (na != nb) return na < nb ? -1 : 1;
            } else {
                // numeric segment sorts before a string one
                return an ? -1 : 1;
            }
        } else {
            const int cmp = sa.compare(sb);
            if (cmp != 0) return cmp < 0 ? -1 : 1;
        }
        const bool a_end = da == std::string_view::npos;
        const bool b_end = db == std::string_view::npos;
        if (a_end && b_end) return 0;
        if (a_end) return -1; // shorter prefix first
        if (b_end) return 1;
        ia = da + 1;
        ib = db + 1;
    }
}

namespace {

// ---------------------------------------------------------------------------
// tween helpers
// ---------------------------------------------------------------------------

bool parse_u64(std::string_view v, uint64_t* out) {
    v = trim_view(v);
    if (v.empty()) return false;
    const std::string s(v);
    char* end = nullptr;
    errno = 0;
    const unsigned long long x = std::strtoull(s.c_str(), &end, 10);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    *out = (uint64_t)x;
    return true;
}

bool parse_i32(std::string_view v, int* out) {
    v = trim_view(v);
    if (v.empty()) return false;
    const std::string s(v);
    char* end = nullptr;
    errno = 0;
    const long x = std::strtol(s.c_str(), &end, 10);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    *out = (int)x;
    return true;
}

// current property value in *script units* (alpha 0-255, coordinates px,
// xscale/yscale percent, rotate degrees).
double current_param_value(const Layer& l, const std::string& param) {
    if (param == "left" || param == "x") return l.left;
    if (param == "top" || param == "y") return l.top;
    if (param == "alpha") return l.alpha * 255.0;
    if (param == "width") return l.width;
    if (param == "height") return l.height;
    if (param == "xscale") return l.x_scale;
    if (param == "yscale") return l.y_scale;
    if (param == "zoom") return l.x_scale; // zoom 别名 = xscale
    if (param == "rotate") return l.rotate_deg;
    if (param == "anchorx") return l.anchor_x;
    if (param == "anchory") return l.anchor_y;
    if (param == "xscale" || param == "yscale" || param == "zoom") return 100.0;
    if (param == "alpha") return 255.0;
    return 0.0;
}

} // namespace

namespace {

// 兄弟表（roots_ / 父 children）按 compare_ids 升序的二分定位。
// 同父节点的首段已共享同一前缀，比较整 id 等价于比较末段。
std::vector<SceneNode*>::iterator sorted_slot(std::vector<SceneNode*>& v,
                                              const std::string& id) {
    return std::lower_bound(v.begin(), v.end(), id,
                            [](const SceneNode* n, const std::string& x) {
                                return compare_ids(n->layer.id, x) < 0;
                            });
}

} // namespace

SceneNode* Compositor::find_node(const std::string& id) {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : it->second.get();
}

const SceneNode* Compositor::find_node(const std::string& id) const {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : it->second.get();
}

Layer* Compositor::find_mut(const std::string& id) {
    SceneNode* n = find_node(id);
    return n ? &n->layer : nullptr;
}

const Layer* Compositor::find(const std::string& id) const {
    const SceneNode* n = find_node(id);
    return n ? &n->layer : nullptr;
}

SceneNode* Compositor::make_node(SceneNode* parent, const std::string& id) {
    auto node = std::make_unique<SceneNode>();
    node->layer.id = id;
    node->parent = parent;
    SceneNode* raw = node.get();
    nodes_.emplace(id, std::move(node)); // 所有权+索引；map 元素地址稳定
    std::vector<SceneNode*>& sib = parent ? parent->children : roots_;
    sib.insert(sorted_slot(sib, id), raw);
    return raw;
}

void Compositor::detach_node(SceneNode* n) {
    std::vector<SceneNode*>& sib = n->parent ? n->parent->children : roots_;
    sib.erase(std::remove(sib.begin(), sib.end(), n), sib.end());
    n->parent = nullptr; // 节点即将随 nodes_.erase 释放
}

void Compositor::collect_subtree(const SceneNode* n,
                                 std::vector<std::string>* out) const {
    out->push_back(n->layer.id);
    for (const SceneNode* c : n->children) collect_subtree(c, out);
}

void Compositor::append_preorder(const SceneNode* n,
                                 std::vector<const Layer*>* out) const {
    out->push_back(&n->layer);
    for (const SceneNode* c : n->children) append_preorder(c, out);
}

/// Scene::ensure_path: materialize id and all ancestors
/// as empty group nodes, keeping the ancestor invariant intact. 沿
/// 显式树逐段下行（现有链直接走指针），缺失段就地补建（兄弟保持序位）；
/// 不再递归前缀重建。
void Compositor::ensure_path(const std::string& id) {
    if (find_node(id)) return;
    SceneNode* parent = nullptr;
    for (size_t i = 0;;) {
        const size_t dot = id.find('.', i);
        const std::string pref =
            dot == std::string::npos ? id : id.substr(0, dot);
        std::vector<SceneNode*>& sib =
            parent ? parent->children : roots_;
        const auto it = sorted_slot(sib, pref);
        SceneNode* cur = nullptr;
        if (it != sib.end() && compare_ids((*it)->layer.id, pref) == 0)
            cur = *it;
        if (!cur) cur = make_node(parent, pref);
        parent = cur;
        if (dot == std::string::npos) break;
        i = dot + 1;
    }
}

void Compositor::clear_scene() {
    // load-time reset: drop every layer (tree/typed props/tweens/anime/
    // handler registries/message bindings) and the "!" root props.
    nodes_.clear();
    roots_.clear();
    message_slots_.clear();
    message_slot_of_.clear();
    next_message_slot_serial_ = 0;
    root_props_ = Layer{};
    anim_->tweens_.clear();
    anim_->collecting_set_ = false;
    anim_->set_pending_.clear();
    anim_->anime_states_.clear();
    input_handlers_.clear();
    message_layer_bindings_.clear();
    default_message_layer_.reset();
    deleted_message_layers_.clear();
    // 簿记: 全场景结构性清空 = "*" 内容写
    // (reset/load 面;现状 legacy 无对应计数)。
    // 全节点内容消亡 = 内容写,经统一收口
    // (角色随节点消亡,无重派生;簿记位不变)。
    note_content_change("*", DirtyAspect::Content);
}

void Compositor::restore_event_handler_row(const std::string& id,
                                          const std::string& event_type,
                                          LayerEventHandler row) {
    Layer* layer = find_mut(id);
    if (!layer) return;
    layer->event_handlers[event_type] = std::move(row);
}

void Compositor::restore_input_row(const std::string& event_name, const std::string& key,
                                   InputHandler row) {
    input_handlers_[{event_name, key}] = std::move(row);
}

void Compositor::create(const std::string& id,
                        const std::map<std::string, std::string>& props) {
    ensure_path(id);
    Layer* l = find_mut(id);
    // 脚本显式创建（[lyc]/[lyc2] 事件;消息绑定 ensure_path 物化不走这里）。
    l->script_created = true;
    // Scene::create: bind the file (non-empty
    // clears solid mode) — like the lyc Create event that precedes the
    // SetProperties pass.
    if (const auto it = props.find("file"); it != props.end()) {
        // lyc 的 file 键是资源路径(Asset),Create 绑定
        // 解除任何宿主供帧状态(存档恢复:宿主供帧绑定从不进 props,
        // 读档后由 [video]/emote 事件重放重建 —— 与旧实现一致)。
        l->set_resource_file(it->second);
        if (!it->second.empty()) l->has_color = false;
    }
    if (const auto it = props.find("path"); it != props.end()) l->path = it->second;
    // then the SetProperties pass (lyc extras / lyc2 alpha); color may
    // re-enter solid mode and unbind the file (reduced order).
    l->set_props(props);
    // 统一 invalidate 簿记(节点创建/重绑定 = 内容写,
    // 经统一收口 note_content_change —— 写入口收敛表行
    // "create": script_created/file/path + set_props 全键,存档恢复也走它)
    note_content_change(id, DirtyAspect(uint16_t(DirtyAspect::Content) |
                                        uint16_t(props_aspect(props))));
}

/// 删除逻辑图层子树时，同步清理落在该子树上的消息层绑定与默认消息层。
/// inside 判定由 "deleted_id / deleted_id." 前缀扫描改为显式子树 id
/// 集合（remove 已沿树收集，O(#bindings) 直查，语义与旧前缀闭包等价）。
static void cleanup_message_layers_of_subtree(
    const std::set<std::string>& subtree_ids,
    std::map<std::string, std::string>* bindings,
    std::optional<std::string>* default_message_layer,
    std::set<std::string>* deleted_markers) {
    auto inside = [&](const std::string& id) {
        return subtree_ids.count(id) != 0;
    };
    std::vector<std::string> drop;
    for (const auto& [message_id, scene_id] : *bindings) {
        if (inside(message_id) || inside(scene_id)) drop.push_back(message_id);
    }
    for (const std::string& message_id : drop) {
        bindings->erase(message_id);
        deleted_markers->insert(message_id);
        // 默认消息层若是被移除绑定/子树中的消息层 id 也一并清除。
        if (default_message_layer->has_value() &&
            **default_message_layer == message_id) {
            default_message_layer->reset();
        }
    }
    if (default_message_layer->has_value() &&
        inside(**default_message_layer)) {
        default_message_layer->reset();
    }
}

void Compositor::remove(const std::string& id) {
    SceneNode* n = find_node(id);
    if (!n) return; // scene.delete on missing id -> 0, no-op
    // 子树 = 以 n 为根的显式子表（沿树收集，免 "id." 前缀扫描）。
    std::vector<std::string> sub;
    collect_subtree(n, &sub);
    const std::set<std::string> subtree_ids(sub.begin(), sub.end());
    // tweenset cascade on layer deletion: dropping tweens
    // of the removed subtree also clears same-group members elsewhere.
    clear_tweens_of_subtree(subtree_ids);
    // message-layer registry cleanup
    cleanup_message_layers_of_subtree(subtree_ids, &message_layer_bindings_,
                                      &default_message_layer_,
                                      &deleted_message_layers_);
    // 摘除并释放整棵子树（nodes_ 是唯一所有者；erase 即析构）。
    if (n->message_slot) {
        // 槽节点不在 roots 树;从消息槽区摘除并清消息→槽映射
        // (防悬垂)。
        message_slots_.erase(std::remove(message_slots_.begin(),
                                         message_slots_.end(), n),
                             message_slots_.end());
        for (auto it = message_slot_of_.begin(); it != message_slot_of_.end();) {
            if (it->second == id)
                it = message_slot_of_.erase(it);
            else
                ++it;
        }
    } else {
        detach_node(n);
    }
    for (const std::string& s : sub) {
        // 簿记: 整棵被删子树逐 id 记内容写(节点不
        // 存在时的 lydel no-op 不记 —— legacy 仍保守计数);
        // 统一收口(角色随节点消亡,无重派生 —— 收敛表行 remove)
        note_content_change(s, DirtyAspect::Content);
        nodes_.erase(s);
    }
}

void Compositor::clear_tweens_of_subtree(const std::set<std::string>& subtree_ids) {
    std::set<uint64_t> touched_sets;
    for (auto it = anim_->tweens_.begin(); it != anim_->tweens_.end();) {
        if (subtree_ids.count(it->first)) {
            for (const Tween& t : it->second)
                if (t.set_id) touched_sets.insert(*t.set_id);
            it = anim_->tweens_.erase(it);
        } else {
            ++it;
        }
    }
    if (touched_sets.empty()) return;
    for (auto it = anim_->tweens_.begin(); it != anim_->tweens_.end();) {
        it->second.erase(
            std::remove_if(it->second.begin(), it->second.end(),
                           [&](const Tween& t) {
                               return t.set_id && touched_sets.count(*t.set_id);
                           }),
            it->second.end());
        if (it->second.empty())
            it = anim_->tweens_.erase(it);
        else
            ++it;
    }
}

void Compositor::set_props(const std::string& id,
                           const std::map<std::string, std::string>& props) {
    ensure_path(id); // lyprop on a missing id materializes the node
    Layer* l = find_mut(id);
    // [lyprop] does NOT author the node for text drawing.
    // set_props runs for anchoring/hiding/centering rewrites everywhere
    // (uihelp tip centering, csvbtn clear rows, mw window show/hide), so a
    // binding-materialized node must not become drawable for unboxed message
    // text just because such a rewrite touched it — snll config uihelp tips
    // (500.z.help, printed with no [font] row in data) used to draw via this
    // hole after their hover lyprop (moved them from the story slot to the
    // default corner; user rule: with no text box the tip must not draw at
    // all, FPM's boxed uihelp keeps drawing). Only [lyc]/[lyc2] creation
    // (Compositor::create) marks the node as script-created.
    l->set_props(props);
    // 簿记: lyprop 写入口(含拖拽/图标等经本入口的写;
    // 缺失 id 的物化一并计入)。统一收口(收敛表行 set_props:逐键幂等
    // 解析含 color 清 file / w/h 角色升级,全部 kind 相关 typed 写经此)。
    note_content_change(id, props_aspect(props));
}

Layer* Compositor::resolve_host_bind_carrier(const std::string& id, bool allow_auto) {
    Layer* l = find_mut(id);
    if (l) return l;
    // A missing carrier is auto-materialized only in the system overlay zone
    // — the "500." id family (repo overlay convention, e.g. snll's title
    // petal layer 500.z.mv whose [video] row precedes the later lyprop that
    // creates it) — or when the caller passes allow_auto.
    // In-story layer videos (story [bg movie]/[video] rows whose carriers are
    // owned by the ADV scene system) must keep the strict bind-onto-existing-
    // layer rule: auto-materializing them bound snll's recall-montage noise
    // strip (ノイズa.ogv — a 7-frame loop of 88-100%-white frames drawn
    // fullscreen over the recap) and the montage turned into a near-white
    // flicker.
    // The exception widens for LOOPING effect-strip videos
    // whose decoded frame-0 content is a sparse overlay (dark canvas,
    // keyed-alpha coverage <= 1/3 — the runtime computes this with the
    // same oa::media::layer_video_key_alpha map the upload host uses):
    // btjy's story-start weather loops (snow03.ogv etc.) never create
    // their carrier leaf (the framework only lyprops the ancestor chain),
    // so without this allowance the whole layer-video family is silently
    // invisible in-story. Dense strips (snll's near-white noise) and
    // one-shot wipes/cut-ins still refuse here, keeping the
    // regression surface untouched.
    if (id.rfind("500.", 0) != 0 && !allow_auto) return nullptr;
    ensure_path(id);
    return find_mut(id);
}

bool Compositor::set_layer_file(const std::string& id, const std::string& file,
                                bool allow_auto) {
    // 资源文件绑定(Asset 域)—— 取代旧的
    // "set_layer_file 兼作宿主供帧绑定"的双义入口。file/path 换成资源,
    // 任何宿主供帧绑定与 solid/mask 状态让位。
    Layer* l = resolve_host_bind_carrier(id, allow_auto);
    if (!l) return false;
    l->set_resource_file(file);
    l->path.clear();
    l->has_color = false;
    l->mask.clear();
    // 簿记: 绑定成功(含物化)记内容写;失败 no-op 不记。
    // 统一收口(收敛表行 set_layer_file:file/path 绑定清 color/mask)
    note_content_change(id, DirtyAspect::Content);
    return true;
}

bool Compositor::bind_video_layer(const std::string& id, bool allow_auto) {
    // [video] 播放绑定(取代 `set_layer_file(id, "__video_layer__:"+id)`)。
    // 该层此后画自己视频通道的宿主上传帧(通道 id == 层 id;纹理键由角色
    // VideoContent::frame_key 单点给出),file/path/color/mask 全部让位 ——
    // 旧拼写绑定把 file 换成保留名,像素语义完全相同(层不再画旧图),
    // 解绑后同样回到空态。
    Layer* l = resolve_host_bind_carrier(id, allow_auto);
    if (!l) return false;
    l->file.clear();
    l->path.clear();
    l->has_color = false;
    l->mask.clear();
    l->content = LayerContent::VideoFrame;
    // 簿记: 绑定成功(含物化)记内容写;失败 no-op 不记。
    note_content_change(id, DirtyAspect::Content);
    return true;
}

bool Compositor::unbind_video_layer(const std::string& id) {
    Layer* l = find_mut(id);
    if (!l) return false;
    // 匹配语义:视频起停/EOF 收尾只在层仍绑着视频帧时解绑 —— 场景期间
    // 重新绑定过的层(静态图/emote)不被清掉(旧 file 字符串比对的内容态版本)。
    if (l->content != LayerContent::VideoFrame) return false;
    l->content = LayerContent::Unbound;
    l->file.clear();
    // 簿记: 实际解绑才记。统一收口
    note_content_change(id, DirtyAspect::Content);
    return true;
}

bool Compositor::bind_emote_layer(const std::string& id, bool allow_auto) {
    // emote 画布绑定(取代 `set_layer_file(id, emote_texture_name(id))`):
    // 该层内容 = emote 画布(纹理键由角色 EmoteContent::canvas_key 单点给出),
    // 盒由调用方 set_props 先行设置。
    Layer* l = resolve_host_bind_carrier(id, allow_auto);
    if (!l) return false;
    l->file.clear();
    l->path.clear();
    l->has_color = false;
    l->mask.clear();
    l->content = LayerContent::EmoteCanvas;
    note_content_change(id, DirtyAspect::Content);
    return true;
}

std::vector<const Layer*> Compositor::draw_order() const {
    std::vector<const Layer*> out;
    out.reserve(nodes_.size());
    // 场景区显式树前序收集 O(n)。等价性:兄弟表(roots_/children)按
    // compare_ids 升序、父先于子 ⇒ 前序 == compare_ids sorted-roots DFS ==
    // 旧全局
    // stable_sort 序(compare_ids 是全序:任意两节点在"最小不同段"处分叉于
    // 某父的不同孩子或祖先-后代,字典序与 DFS 序一致)。
    for (const SceneNode* r : roots_) append_preorder(r, &out);
    // 消息槽区整段跟在场景区之后(创建序)。旧实现靠 openartemis-<hex>
    // 命名 + compare_ids 的 message-last 拼写分支垫底;槽顺序现由结构承担。
    for (const SceneNode* s : message_slots_) out.push_back(&s->layer);
    // Overlay-video slot: a structural topmost layer (never part of the
    // script tree / message slots); it draws last while bound to a texture.
    // 绑定判定 = 角色 textured_content(原
    // !file.empty() 逐字;overlay 槽幂等特例保持 —— 结构行位置不变)。
    const auto ov = nodes_.find(kOverlayNodeId);
    const bool overlay_live =
        ov != nodes_.end() && ov->second->layer.visible &&
        oa::render::content_role_of(ov->second->layer).textured_content(ov->second->layer);
    if (overlay_live) out.push_back(&ov->second->layer);
#ifndef NDEBUG
    // 调试自检:场景区前序必须逐元素等于场景层全量 compare_ids 排序(旧
    // flat 序;槽区不参与 id 排序)。
    std::vector<const Layer*> sorted;
    for (const auto& [id, node] : nodes_)
        if (!node->message_slot && id != kOverlayNodeId) sorted.push_back(&node->layer);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Layer* x, const Layer* y) {
                         return compare_ids(x->id, y->id) < 0;
                     });
    const size_t scene_n =
        out.size() - message_slots_.size() - (overlay_live ? 1u : 0u);
    assert(std::equal(out.begin(), out.begin() + ptrdiff_t(scene_n),
                      sorted.begin(), sorted.end()));
#endif
    return out;
}

void Compositor::set_overlay_file() {
    SceneNode* n = find_node(kOverlayNodeId);
    if (n) {
        // clear_overlay_file() 只清 file(节点留驻);再次播放时必须幂等
        // 恢复 file/visible —— 否则后续全屏视频(如 常轨脱离ReReCall 路线
        // OP)画的是空文件节点,整段黑屏(首个视频后的所有全屏视频)。
        const bool changed = n->layer.file != kOverlayNodeId || !n->layer.visible;
        n->layer.file = kOverlayNodeId;
        n->layer.visible = true;
        // 簿记: 只记实际转场(播放中每帧幂等重复不记);
        // 统一收口(收敛表行 set_overlay_file:幂等不变量)
        if (changed) note_content_change(kOverlayNodeId, DirtyAspect::Content);
        return;
    }
    auto overlay_node_ = std::make_unique<SceneNode>();
    overlay_node_->layer.id = kOverlayNodeId;
    overlay_node_->layer.file = kOverlayNodeId;
    overlay_node_->layer.has_color = false;
    overlay_node_->layer.visible = true;
    SceneNode* raw = overlay_node_.get();
    nodes_.emplace(raw->layer.id, std::move(overlay_node_));
    // 簿记: 槽节点首次物化 = 内容写;统一收口同上
    note_content_change(kOverlayNodeId, DirtyAspect::Content);
}

const oa::render::SceneNode* Compositor::overlay_node() const {
    const auto it = nodes_.find(kOverlayNodeId);
    if (it == nodes_.end() || !it->second->layer.visible) return nullptr;
    return it->second.get();
}

void Compositor::clear_overlay_file() {
    SceneNode* n = find_node(kOverlayNodeId);
    if (!n) return;
    const bool was_bound = !n->layer.file.empty();
    n->layer.file.clear();
    // 簿记: 只记实际解绑(空态重复调用不记);统一
    // 收口(收敛表行 clear_overlay_file:幂等不变量)
    if (was_bound) note_content_change(kOverlayNodeId, DirtyAspect::Content);
}

// ---------------------------------------------------------------------------
// message-layer registry
// ---------------------------------------------------------------------------

std::string Compositor::ensure_message_scene_node(const std::string& message_id,
                                                  bool layered) {
    // 去字符串编码 — layered identity 消息直接走宿主树节点(场景区,
    // ensure_path 按 id 物化);layered=0 独立消息走引擎消息槽(槽区,创建序,
    // 槽 id 不随消息 id 变化)。返回 scene_id 供调用方绑定/摆位。
    if (layered) {
        ensure_path(message_id);
        return message_id;
    }
    // 复用:消息→槽映射(message_slot_of_)独立于绑定表——绑定随宿主子树
    // 删除被清,而槽是引擎资源,生命周期到 clear_scene/槽被
    // remove 为止(宿主树删光后继续 print 不得反复建新槽,与旧 hex 确定
    // 命名复用行为一致)。
    const auto it = message_slot_of_.find(message_id);
    if (it != message_slot_of_.end()) {
        const SceneNode* b = find_node(it->second);
        if (b && b->message_slot) return it->second;
    }
    SceneNode* slot = make_message_slot();
    message_slot_of_[message_id] = slot->layer.id;
    return slot->layer.id;
}

SceneNode* Compositor::make_message_slot() {
    // 槽节点不进 roots_(场景区):只入 nodes_ 索引 + message_slots_ 槽表。
    auto node = std::make_unique<SceneNode>();
    node->layer.id = "@msg" + std::to_string(next_message_slot_serial_++);
    node->message_slot = true;
    SceneNode* raw = node.get();
    nodes_.emplace(raw->layer.id, std::move(node));
    message_slots_.push_back(raw);
    return raw;
}

bool Compositor::is_message_slot(const std::string& id) const {
    const SceneNode* n = find_node(id);
    return n && n->message_slot;
}

std::string Compositor::bound_scene_id(const std::string& message_id) const {
    const auto it = message_layer_bindings_.find(message_id);
    return it == message_layer_bindings_.end() ? message_id : it->second;
}

void Compositor::set_message_layer_binding(const std::string& message_id,
                                           const std::string& scene_id) {
    message_layer_bindings_[message_id] = scene_id;
    deleted_message_layers_.erase(message_id);
}

void Compositor::revive_message_layer(const std::string& message_id) {
    deleted_message_layers_.erase(message_id);
}

void Compositor::set_default_message_layer(
    const std::optional<std::string>& message_id) {
    default_message_layer_ = message_id;
}

std::optional<std::string> Compositor::resolve_message_target(
    const std::string& raw) const {
    if (raw.empty() || raw[0] != '~') return raw;
    const std::string rest = raw.substr(1);
    if (rest.empty()) {
        // 裸 "~"：默认消息层（未设置 → 忽略本次操作）
        if (!default_message_layer_) return std::nullopt;
        const auto it = message_layer_bindings_.find(*default_message_layer_);
        return it == message_layer_bindings_.end() ? *default_message_layer_
                                                   : it->second;
    }
    const auto it = message_layer_bindings_.find(rest);
    return it == message_layer_bindings_.end() ? rest : it->second;
}

bool Compositor::is_message_layer_visible(const std::string& message_id) const {
    // 宿主树/删除/绑定的收敛
    // 显式规则（compositor_test 断言锚点）。消息文本生命周期 = 宿主
    // 窗口树生命周期（窗口树模型：节点删除即消息失效）：
    //   1. 删除簿记标记（随 lydel/[alldelete] 父子树清理置位）⇒ 不可见；
    //   2. 绑定目标场景节点存在但不可有效可见 ⇒ 不可见（文本绘制槽位自身
    //      或其祖先被隐藏）；
    //   3. 绑定目标是引擎消息槽节点（scene_id != message_id，layered=0 独立
    //      消息，为显式槽区节点——结构判定，无前缀字符串），或绑定目标
    //      缺失（layered identity 节点尚未建 / 独立 overlay 未被引用）⇒
    //      host_tree_anchor_visible：沿消息 id 祖先链取最近现存节点的有效
    //      可见性（FPM 的 [lyprop id=game.mwid visible=0]（mw.lua msg_hide）
    //      隐藏整棵消息窗树、[alldelete]（image.lua lydel 1/400/500/600）
    //      删光整棵窗口树——两种情况下覆盖文本都必须随之消失，真机：切屏/
    //      退出/回标题黑页上的剧情文字残留）；
    //   4. 绑定目标即消息 id（layered identity）且节点存在 ⇒ 自身有效可见性
    //      （规则 2 已排除不可见情形）。
    if (deleted_message_layers_.count(message_id)) return false;
    const std::string scene_id = bound_scene_id(message_id);
    const Layer* target = find(scene_id);
    if (target && !is_effectively_visible(scene_id)) return false;
    if (target && scene_id == message_id) return true;
    // overlay 绑定（目标 ≠ 消息 id）或目标缺失：宿主树锚点活性决定。
    return host_tree_anchor_visible(message_id);
}

bool Compositor::host_tree_anchor_visible(const std::string& message_id) const {
    // 消息 id 路径上的最近现存祖先决定显隐：其有效可见性已折叠它上面的整条
    // 祖先链（is_effectively_visible 沿现存祖先逐段检查）。整条链被删光 =
    // 宿主窗口树已死（[alldelete] 等）⇒ 多点消息 id 不可见；单段消息 id
    // （引擎自管 overlay，无宿主树概念）不受树约束。
    std::string pref = message_id;
    for (;;) {
        const size_t dot = pref.rfind('.');
        if (dot == std::string::npos) break;
        pref = pref.substr(0, dot);
        if (find(pref)) return is_effectively_visible(pref);
    }
    return message_id.find('.') == std::string::npos;
}

bool Compositor::is_message_layer_drawable(const std::string& message_id,
                                           bool text_positioned) const {
    // 文本绘制门槛 = is_message_layer_visible（宿主树/删除/自身显隐语义）
    // + 绘制盒门槛：渲染器只在文本层有脚本指定落点时才注入字形——
    //   a) 文本层配置过 [font] 排版键（left/top/width/height）→ 自带绘制盒；
    //   b) 绑定节点被脚本显式创建/改写（script_created,lyc/lyc2/lyprop）。
    // 文本层既无绘制盒、绑定节点又只是 chgmsg 绑定自动物化（ensure_path）
    // 的孤儿打印 → 不绘制：场景里没有任何东西给它一个位置,绘制只会把它
    // 丢在继承来的外来几何上（NekoMiko config.lua 残留的 ui_message 数值
    // 打印 500.p0X —— 该游戏布局没有这些读出槽,真机 Artemis 无可依附文本
    // 盒即不显示;此处把"无可依附盒"落地为引擎规则,不改游戏数据）。
    if (!is_message_layer_visible(message_id)) return false;
    const std::string scene_id = bound_scene_id(message_id);
    const Layer* target = find(scene_id);
    if (!target) return false; // 无绘制目标节点
    if (text_positioned) return true;
    return target->script_created;
}

bool Compositor::is_effectively_visible(const std::string& id) const {
    const SceneNode* n = find_node(id);
    if (!n) return false;
    // root props (!) hidden => whole tree invisible
    if (!root_props_.visible) return false;
    // 自身与全部祖先(显式树中 = parent 链全体;旧"现存祖先"遍历的
    // 等价面——树不变量保证祖先必在树中)。
    for (const SceneNode* p = n; p; p = p->parent)
        if (!p->layer.visible) return false;
    return true;
}

double Compositor::chain_opacity(const std::string& id) const {
    double op = root_props_.alpha; // root (!) opacity multiplies the tree
    const SceneNode* n = find_node(id);
    if (!n) return op;
    // 祖先沿 parent 收集(深→浅),再自浅→深乘,与旧 ancestor_ids 顺序一致
    // (浮点乘序逐位不变)。
    std::vector<const SceneNode*> anc;
    for (const SceneNode* p = n->parent; p; p = p->parent) anc.push_back(p);
    for (auto it = anc.rbegin(); it != anc.rend(); ++it) op *= (*it)->layer.alpha;
    op *= n->layer.alpha;
    return op;
}

void Compositor::set_root_props(const std::map<std::string, std::string>& props) {
    root_props_.set_props(props); // no "!" node is created
    // 簿记: 根 props holder 写 = 伪 id ""(非节点;
    // 现状 legacy 的 LayerSetProps("!") 一并计数)。
    // 收敛表行 set_root_props = 特例:根 holder 非节点、
    // 无内容角色,经统一收口仅为簿记位不变(无重派生可做)。
    note_content_change("", props_aspect(props));
}

bool Compositor::world_transform(const std::string& id, Affine2* out) const {
    const SceneNode* self = find_node(id);
    if (!self) return false;
    // root props transform is the parent of every root layer
    Affine2 w = root_props_.local_transform();
    // 祖先沿 parent 收集(深→浅),再自浅→深连乘,与旧 ancestor_ids
    // 顺序一致(矩阵乘序逐位不变)。
    std::vector<const SceneNode*> anc;
    for (const SceneNode* p = self->parent; p; p = p->parent) anc.push_back(p);
    for (auto it = anc.rbegin(); it != anc.rend(); ++it)
        w = w * (*it)->layer.local_transform();
    w = w * self->layer.local_transform();
    *out = w;
    return true;
}

bool Compositor::world_rect(const std::string& id, double w, double h, double* ox, double* oy,
                            double* ow, double* oh) const {
    Affine2 t;
    if (!world_transform(id, &t)) return false;
    // axis-aligned bounding box of the four transformed corners
    // (transform_rect)
    const double xs[2] = {0.0, w};
    const double ys[2] = {0.0, h};
    double min_x = 1e300, min_y = 1e300, max_x = -1e300, max_y = -1e300;
    for (double y : ys) {
        for (double x : xs) {
            double px = 0, py = 0;
            t.transform_point(x, y, &px, &py);
            if (px < min_x) min_x = px;
            if (px > max_x) max_x = px;
            if (py < min_y) min_y = py;
            if (py > max_y) max_y = py;
        }
    }
    *ox = min_x;
    *oy = min_y;
    *ow = max_x - min_x;
    *oh = max_y - min_y;
    return true;
}

std::vector<std::string> Compositor::hit_test_all(double mx, double my,
                                                  const QuadSizeFn& size,
                                                  void* userdata,
                                                  const AlphaSamplerFn& alpha) const {
    std::vector<std::string> hits;
    if (!root_props_.visible) return hits;
    const auto order = draw_order();
    // back-to-front scan == compare_ids sorted-roots reverse DFS (children
    // before parents, topmost subtree first)
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const Layer* l = *it;
        if (!is_effectively_visible(l->id)) continue;
        // hit quad size precedence: width/height > clip w/h > texture.
        // Pure group nodes without a resolvable size
        // never hit.
        double w = 0, h = 0;
        if (l->width > 0 && l->height > 0) {
            w = l->width;
            h = l->height;
        } else if (l->has_clip) {
            w = l->clip_w;
            h = l->clip_h;
        } else if (size) {
            const auto sz = size(userdata , *l);
            if (!sz) continue;
            w = sz->first;
            h = sz->second;
        } else {
            continue;
        }
        if (w <= 0 || h <= 0) continue;
        Affine2 world;
        if (!world_transform(l->id, &world)) continue;
        // local point via the inverse world transform, tested against the
        // local AABB: a rotated quad hits exactly inside
        // the rotated quad, not its world AABB.
        if (!world.invertible()) continue;
        const Affine2 inv = world.inverse();
        double lx = 0, ly = 0;
        inv.transform_point(mx, my, &lx, &ly);
        if (lx >= 0.0 && lx < w && ly >= 0.0 && ly < h &&
            !is_pointer_transparent(*l, lx, ly, alpha, userdata)) {
            hits.push_back(l->id);
        }
    }
    return hits;
}

std::string Compositor::hit_test(double mx, double my, const QuadSizeFn& size,
    void* userdata, const AlphaSamplerFn& alpha) const {
    const auto hits = hit_test_all(mx, my, size, userdata, alpha);
    return hits.empty() ? std::string() : hits.front();
}

// ---------------------------------------------------------------------------
// event registries + draggable/dragarea + clickablethreshold
// ---------------------------------------------------------------------------

namespace {

/// complete_event_filter_params: extra + the
/// non-empty string fields + "1" flags of the boolean fields.
std::map<std::string, std::string> complete_filter_params(
    const std::map<std::string, std::string>& extra,
    const std::map<std::string, std::string>& fields,
    const std::vector<std::string>& flags) {
    std::map<std::string, std::string> params = extra;
    for (const auto& [k, v] : fields)
        if (!v.empty()) params[k] = v;
    for (const std::string& f : flags) params[f] = "1";
    return params;
}

bool parse_flag_i32(const std::string& v) {
    int x = 0;
    return parse_i32(v, &x) && x != 0;
}

/// Build one LayerEventHandler row from parsed registration fields.
LayerEventHandler make_layer_handler_row(
    const std::map<std::string, std::string>& extra, const std::string& id,
    const std::string& event_type, const std::string& mode,
    const std::string& file, const std::string& label, bool call,
    const std::string& handler, bool penetration) {
    LayerEventHandler row;
    row.enabled = true;
    row.penetration = penetration;
    row.file = file;
    row.label = label;
    row.call = call;
    row.handler = handler;
    row.params = extra;
    row.filter_params = complete_filter_params(
        extra,
        {{"id", id},
         {"type", event_type},
         {"mode", mode},
         {"file", file},
         {"label", label},
         {"handler", handler}},
        {"call", "penetration"});
    if (!penetration) row.filter_params.erase("penetration");
    if (!call) row.filter_params.erase("call");
    return row;
}

const char* kFpmGlueKeys[][2] = {{"click", "click"},       {"over", "rollover"},
                                 {"out", "rollout"},       {"dragin", "dragin"},
                                 {"drag", "drag"},         {"dragout", "dragout"}};

/// FPM `lyevent` mode ops (disable/enable) are GROUP
/// operations. The framework stops a choice screen's rows after an option
/// click through btnstat (system/adv/button.lua:707) on the select group
/// root — getMWID("select") = "1.80.120" — while the actual rows are
/// registered on descendant layers ("1.80.120.N.0.0.0"). With an exact-id
/// apply the disable never reached any row: every over/out stayed live inside
/// the select-exit chain, and a rollout in the click -> select_clicknext
/// window ran the game's select_out, cleared scr.select.id and the queued
/// clicknext indexed v[nil] (select.lua:484/482 crash family — the
/// still-pointer maintenance channel is closed elsewhere; this closes the
/// game's own group-disable channel). Walk the target layer plus descendants
/// and flip the rows of the given event type.
void set_row_mode_recursive(SceneNode* n, const std::string& event_type,
                            bool enable) {
    if (!n) return;
    if (!event_type.empty()) {
        const auto it = n->layer.event_handlers.find(event_type);
        if (it != n->layer.event_handlers.end()) it->second.enabled = enable;
    } else {
        for (auto& [type, row] : n->layer.event_handlers) {
            (void)type;
            row.enabled = enable;
        }
    }
    for (SceneNode* c : n->children) set_row_mode_recursive(c, event_type, enable);
}

} // namespace

void Compositor::apply_lyevent(const std::string& id,
                               const std::map<std::string, std::string>& raw) {
    auto get = [&raw](const char* k) -> std::string {
        const auto it = raw.find(k);
        return it == raw.end() ? std::string() : it->second;
    };
    const std::string event_type = get("type");
    const std::string mode = get("mode");
    const std::string file = get("file");
    const std::string label = get("label");
    const bool call = parse_flag_i32(get("call"));
    const std::string handler = get("handler");
    const bool penetration = parse_flag_i32(get("penetration"));

    // extras: everything except the known fields.
    static const char* kKnown[] = {"id",  "type", "mode", "file", "label",
                                   "call", "handler", "penetration"};
    std::map<std::string, std::string> extra;
    for (const auto& [k, v] : raw) {
        bool known = false;
        for (const char* kk : kKnown) known = known || k == kk;
        if (!known) extra[k] = v;
    }

    ensure_path(id);
    Layer* layer = find_mut(id);
    if (!layer) return;
    // 簿记: [lyevent] 写入口(行增删改/递归 mode;行只
    // 影响命中/拖拽行为面;ensure_path 物化也计入)。非内容态写 —— 不在
    // 内容写收敛表内,直走 mark_dirty。
    mark_dirty(id, DirtyAspect::Handlers);

    if (event_type.empty() && mode != "reset") {
        // FPM glue: a type-less lyevent row (button.lua:125-128) is expanded
        // like the Artemis Lua global `lyevent` (func.lua:776-786): one typed
        // row per present click/over/out(/dragin/drag/dragout) function key.
        bool expanded = false;
        for (const auto& pair : kFpmGlueKeys) {
            const std::string key(pair[0]);
            const auto it = raw.find(key);
            if (it == raw.end()) continue;
            expanded = true;
            std::map<std::string, std::string> e = extra;
            e["function"] = it->second;
            const LayerEventHandler row = make_layer_handler_row(
                e, id, pair[1], mode.empty() ? "init" : mode, file, label, call,
                handler.empty() ? "calllua" : handler, penetration);
            layer->event_handlers[pair[1]] = row;
        }
        if (expanded) return;
    }

    // mode table.
    auto& map = layer->event_handlers;
    if (mode == "reset") {
        map.erase(event_type);
        return;
    }
    if (mode == "disable" || mode == "enable") {
        // group semantics — btnstat disables the select
        // group root while rows live on descendant layers; an exact-id apply
        // left every row live inside the select-exit chain (rollout ->
        // select_out -> scr.select.id=nil -> clicknext v[nil] crash family).
        if (const auto nit = nodes_.find(id); nit != nodes_.end())
            set_row_mode_recursive(nit->second.get(), event_type,
                                   mode == "enable");
        // "enable" without an existing row registers from this event's params
        // (legacy single-layer behavior; rows on the exact layer only).
        if (mode == "enable" && !map.count(event_type))
            map[event_type] =
                make_layer_handler_row(extra, id, event_type, mode, file, label,
                                       call, handler, penetration);
        return;
    }
    // "" / "init" / anything else: insert/replace
    map[event_type] =
        make_layer_handler_row(extra, id, event_type, mode, file, label, call,
                               handler, penetration);
}

void Compositor::apply_legacy_layer_event(const std::string& id,
                                          const std::string& event_type, bool reset,
                                          const std::map<std::string, std::string>& raw) {
    // legacy seton*/delon* = lyevent mode init/reset;
    // its known fields are id/file/label/call/handler/penetration.
    std::map<std::string, std::string> merged = raw;
    merged["type"] = event_type;
    merged["mode"] = reset ? "reset" : "init";
    apply_lyevent(id, merged);
}

void Compositor::set_input_handler(const std::string& event_name,
                                   const std::map<std::string, std::string>& raw) {
    auto get = [&raw](const char* k) -> std::string {
        const auto it = raw.find(k);
        return it == raw.end() ? std::string() : it->second;
    };
    const std::string file = get("file");
    const std::string label = get("label");
    const bool call = parse_flag_i32(get("call"));
    const std::string handler = get("handler");
    // key is part of the extras; all other params are extras.
    std::map<std::string, std::string> extra;
    for (const auto& [k, v] : raw) {
        if (k != "file" && k != "label" && k != "call" && k != "handler") extra[k] = v;
    }
    const std::string key = extra.count("key") ? extra.at("key") : std::string();
    InputHandler row;
    row.file = file;
    row.label = label;
    row.call = call;
    row.handler = handler;
    row.params = extra;
    row.filter_params = complete_filter_params(extra, {{"file", file}, {"label", label},
                                                       {"handler", handler}},
                                               {"call"});
    if (!call) row.filter_params.erase("call");
    input_handlers_[{event_name, key}] = std::move(row);
}

void Compositor::del_input_handler(const std::string& event_name, const std::string& key) {
    if (key.empty()) {
        for (auto it = input_handlers_.begin(); it != input_handlers_.end();) {
            if (it->first.first == event_name)
                it = input_handlers_.erase(it);
            else
                ++it;
        }
    } else {
        input_handlers_.erase({event_name, key});
    }
}

const InputHandler* Compositor::get_input_handler(const std::string& event_name,
                                                  const std::string& key) const {
    const auto it = input_handlers_.find({event_name, key});
    return it == input_handlers_.end() ? nullptr : &it->second;
}

bool Compositor::has_any_enabled_handler(const std::string& id) const {
    const Layer* l = find(id);
    if (!l) return false;
    for (const auto& [type, h] : l->event_handlers)
        if (h.enabled) return true;
    return false;
}

const LayerEventHandler* Compositor::find_event_handler(const std::string& id,
                                                        const std::string& event_type) const {
    const Layer* l = find(id);
    if (!l) return nullptr;
    const auto it = l->event_handlers.find(event_type);
    return it == l->event_handlers.end() ? nullptr : &it->second;
}

bool Compositor::is_layer_draggable(const std::string& id) const {
    const Layer* l = find(id);
    if (!l) return false;
    const auto it = l->props.find("draggable");
    if (it == l->props.end()) return false;
    const std::string_view v = trim_view(it->second);
    return v != "" && v != "0" && v != "off" && v != "false";
}

std::optional<std::pair<double, double>> Compositor::layer_offset(const std::string& id) const {
    const Layer* l = find(id);
    if (!l) return std::nullopt;
    return std::make_pair(l->left, l->top);
}

bool Compositor::drag_layer_to(const std::string& id, double origin_left,
                               double origin_top, double dx, double dy,
                               double* out_left, double* out_top) {
    Layer* l = find_mut(id);
    if (!l) return false;
    // dragarea = 4 f64 bounds; the third/
    // fourth value act as the max bound (the clamp below).
    double min_x = -1.0e300, min_y = -1.0e300, max_x = 1.0e300, max_y = 1.0e300;
    const auto area_it = l->props.find("dragarea");
    if (area_it != l->props.end()) {
        std::array<double, 4> b{};
        std::string_view v = area_it->second;
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            const size_t comma = v.find(',');
            const std::string_view part = trim_view(v.substr(0, comma));
            if (!parse_f64(part, &b[(size_t)i])) {
                ok = false;
                break;
            }
            if (i < 3) {
                if (comma == std::string_view::npos) {
                    ok = false;
                    break;
                }
                v.remove_prefix(comma + 1);
            }
        }
        if (ok) {
            min_x = b[0];
            min_y = b[1];
            max_x = b[2];
            max_y = b[3];
        }
    }
    const double left = std::clamp(origin_left + dx, min_x, max_x);
    const double top = std::clamp(origin_top + dy, min_y, max_y);
    auto format = [](double v) -> std::string {
        const double i = std::floor(v);
        if (v == i) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%lld", (long long)i);
            return buf;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", v);
        return buf;
    };
    std::map<std::string, std::string> p;
    p["left"] = format(left);
    p["top"] = format(top);
    set_props(id, p);
    if (out_left) *out_left = left;
    if (out_top) *out_top = top;
    return true;
}

bool Compositor::is_pointer_transparent(const Layer& l, double local_x, double local_y,
                                        const AlphaSamplerFn& alpha, void* userdata) const {
    // clickablethreshold absent / unparsable -> always clickable.
    const auto th_it = l.props.find("clickablethreshold");
    if (th_it == l.props.end()) return false;
    int threshold = 0;
    if (!parse_i32(th_it->second, &threshold)) return false;
    // sample coords: local pixel (stage scale 1) + clip offset.
    int tx = (int)local_x; // scale = 1 (App window == stage)
    int ty = (int)local_y;
    if (l.has_clip) {
        tx += (int)l.clip_x;
        ty += (int)l.clip_y;
    }
    int hit_alpha = 255; // fallback when sampling is unavailable
    if (alpha) {
        const auto pa = alpha(userdata, l, tx, ty);
        hit_alpha = pa ? (int)*pa : (int)(l.alpha * 255.0 + 0.5);
    } else {
        hit_alpha = (int)(l.alpha * 255.0 + 0.5);
    }
    return hit_alpha < threshold; // "低于"才透明
}

// ---------------------------------------------------------------------------
// minimal animation (FPM transition path)
// ---------------------------------------------------------------------------

namespace {
const char* kTweenKnownKeys[] = {
    "id",    "param",  "from", "to",    "ease",  "time",  "delay", "loop",
    "yoyo",  "loopdelay", "sync", "delete", "file", "label", "handler",
};
bool is_tween_known(const std::string& k) {
    for (const char* c : kTweenKnownKeys)
        if (k == c) return true;
    return false;
}
} // namespace

void Compositor::apply_one_tween(const std::map<std::string, std::string>& p,
                                 const std::optional<uint64_t>& delay_override,
                                 std::optional<uint64_t> set_id, uint64_t clock_ms) {
    auto g = [&p](const char* k) -> const std::string* {
        const auto it = p.find(k);
        return it == p.end() ? nullptr : &it->second;
    };
    // to must parse, otherwise the request is ignored
    double to = 0;
    const std::string* to_s = g("to");
    if (!to_s || !parse_f64(*to_s, &to)) return;
    const std::string id = g("id") ? *g("id") : std::string();
    const std::string param = g("param") ? *g("param") : std::string();

    ensure_path(id); // scene.ensure(request.id)
    Layer* l = find_mut(id);
    if (!l) return;

    double from = 0;
    bool from_ok = false;
    const std::string* from_s = g("from");
    if (from_s) from_ok = parse_f64(*from_s, &from);
    if (!from_ok) from = current_param_value(*l, param); // from defaults to current

    uint64_t delay = 0;
    if (const std::string* d = g("delay")) (void)parse_u64(*d, &delay);
    uint64_t time = 0;
    if (const std::string* t = g("time")) (void)parse_u64(*t, &time);
    uint64_t loop_delay = 0;
    if (const std::string* d = g("loopdelay")) (void)parse_u64(*d, &loop_delay);

    int loop_i = 0, yoyo_i = 0;
    if (const std::string* v = g("loop")) (void)parse_i32(*v, &loop_i);
    if (const std::string* v = g("yoyo")) (void)parse_i32(*v, &yoyo_i);

    // loop/yoyo resolution: -1 infinite; N = N cycles;
    // yoyo count wins over loop count when both enabled.
    const bool infinite = loop_i == -1 || yoyo_i == -1;
    const bool yoyo_enabled = yoyo_i == -1 || yoyo_i > 0;
    std::optional<uint32_t> cycles;
    if (yoyo_enabled) {
        if (yoyo_i > 0) cycles = uint32_t(yoyo_i);
    } else if (loop_i > 0) {
        cycles = uint32_t(loop_i);
    }

    int delete_i = 0, sync_i = 0;
    if (const std::string* v = g("delete")) (void)parse_i32(*v, &delete_i);
    if (const std::string* v = g("sync")) (void)parse_i32(*v, &sync_i);
    (void)sync_i; // sync wait is handled elsewhere; the tween itself is identical

    Tween tw;
    tw.param = param;
    tw.from = from;
    tw.to = to;
    tw.easing = parse_easing(g("ease") ? *g("ease") : std::string_view());
    tw.start_ms = clock_ms + delay_override.value_or(delay);
    tw.duration_ms = time; // absent time = 0 -> snap
    tw.infinite_loop = infinite;
    tw.loop_count = cycles;
    tw.yoyo = yoyo_enabled;
    tw.yoyo_reverse = false;
    tw.loop_delay_ms = loop_delay;
    tw.delete_on_finish = delete_i != 0;
    tw.handler = g("handler") ? *g("handler") : std::string();
    tw.handler_file = g("file") ? *g("file") : std::string();
    tw.handler_label = g("label") ? *g("label") : std::string();
    for (const auto& [k, v] : p)
        if (!is_tween_known(k)) tw.extra[k] = v;
    tw.set_id = set_id;

    std::vector<Tween>& bucket = anim_->tweens_[id];
    if (!set_id) {
        // outside [tweenset]: later same-param tween replaces the earlier one
        bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                    [&](const Tween& t) { return t.param == param; }),
                     bucket.end());
    }
    bucket.push_back(std::move(tw));
    // 簿记: 补间注册 = 目标层属性写(实际生效在活动期,
    // 由 animated_now 帧项覆盖;此处只记事件级写,param 归方面)。属性动画轨
    // 注册不改内容态(写发生在活动帧,走 advance_tweens 的 None 收口) ——
    // 不在内容写收敛表内,直走 mark_dirty。
    mark_dirty(id, props_aspect({{param, std::string()}}));
}

void Compositor::apply_lytween(const std::map<std::string, std::string>& params,
                               uint64_t clock_ms) {
    if (anim_->collecting_set_) { // collect until [/tweenset]
        anim_->set_pending_.push_back(params);
        return;
    }
    apply_one_tween(params, std::nullopt, std::nullopt, clock_ms);
}

void Compositor::tweenset_start() {
    anim_->collecting_set_ = true;
    anim_->set_pending_.clear();
}

void Compositor::tweenset_end(uint64_t clock_ms) {
    if (!anim_->collecting_set_) return;
    anim_->collecting_set_ = false;
    if (anim_->set_pending_.empty()) return;
    const uint64_t set_id = anim_->next_tween_set_id_++;
    // sequential start: each entry's start = accumulated (delay + time) of
    // the previous entries
    uint64_t offset = 0;
    for (const auto& pending : anim_->set_pending_) {
        uint64_t delay = 0;
        if (const auto it = pending.find("delay"); it != pending.end())
            (void)parse_u64(it->second, &delay);
        uint64_t time = 0;
        if (const auto it = pending.find("time"); it != pending.end())
            (void)parse_u64(it->second, &time);
        const uint64_t start_delay = offset + delay;
        apply_one_tween(pending, start_delay, set_id, clock_ms);
        offset = start_delay + time;
    }
    anim_->set_pending_.clear();
}

std::vector<TweenDone> Compositor::apply_lytweendel(const std::string& id) {
    std::vector<TweenDone> cancelled;
    auto it = anim_->tweens_.find(id);
    if (it == anim_->tweens_.end()) return cancelled;
    Layer* l = find_mut(id);
    std::set<uint64_t> sets;
    for (const Tween& t : it->second) {
        // settle to the final value first
        if (l) {
            l->set_props({{t.param, format_tween_param(t.param, t.to)}});
            // 簿记: 实际 settle 写(按 param 方面;无
            // tween 的 lytweendel no-op 不记 —— legacy 保守计数);
            // 统一收口(收敛表行 apply_lytweendel settle)
            note_content_change(id, props_aspect({{t.param, std::string()}}));
        }
        if (t.set_id) sets.insert(*t.set_id);
        // a removed tween with a pending completion is a script-timer
        // round that ends HERE — deliver the completion once so the data's
        // round-end bookkeeping runs (the FPM sample-preview cycle in
        // ui/config.lua rearms only from its completion handler; a silent
        // cancel strands it). Only the directly-deleted id's bucket
        // is delivered; tweenset cascade removals keep their plain abort
        // semantics (no completion churn for aborted sequences).
        if (!t.handler.empty() || !t.handler_file.empty() ||
            !t.handler_label.empty()) {
            TweenDone d;
            d.id = id;
            d.handler = t.handler;
            d.handler_file = t.handler_file;
            d.handler_label = t.handler_label;
            d.extra = t.extra;
            cancelled.push_back(std::move(d));
        }
    }
    anim_->tweens_.erase(it);
    if (!sets.empty()) {
        for (auto jt = anim_->tweens_.begin(); jt != anim_->tweens_.end();) {
            jt->second.erase(
                std::remove_if(jt->second.begin(), jt->second.end(),
                               [&](const Tween& t) {
                                   return t.set_id && sets.count(*t.set_id);
                               }),
                jt->second.end());
            if (jt->second.empty())
                jt = anim_->tweens_.erase(jt);
            else
                ++jt;
        }
    }
    return cancelled;
}

std::vector<TweenDone> Compositor::advance_tweens(uint64_t now_ms) {
    std::vector<TweenDone> done;
    std::vector<std::string> ids;
    ids.reserve(anim_->tweens_.size());
    for (const auto& [id, unused] : anim_->tweens_) ids.push_back(id);

    std::vector<std::string> delete_ids;
    for (const std::string& id : ids) {
        auto it = anim_->tweens_.find(id);
        if (it == anim_->tweens_.end()) continue;
        Layer* l = find_mut(id);
        if (!l) { // layer disappeared under us (lydel etc.)
            anim_->tweens_.erase(it);
            continue;
        }
        std::vector<Tween> keep;
        keep.reserve(it->second.size());
        bool wants_delete = false;
        for (const Tween& t : it->second) {
            if (t.is_finished(now_ms)) {
                // settle final value (gc_finished_tweens)
                l->set_props({{t.param, format_tween_param(t.param, t.to)}});
                // 活动帧写收口(None = 簿记不记 —— 自然完成帧由
                // animated_now/prev 兜底;与 lytweendel 的事件级 settle 记法
                // 区分,见收敛表行 advance_tweens)
                note_content_change(id, DirtyAspect::None);
                wants_delete = wants_delete || t.delete_on_finish;
                if (!t.handler.empty() || !t.handler_file.empty() ||
                    !t.handler_label.empty()) {
                    TweenDone d;
                    d.id = id;
                    d.handler = t.handler;
                    d.handler_file = t.handler_file;
                    d.handler_label = t.handler_label;
                    d.extra = t.extra;
                    done.push_back(std::move(d));
                }
            } else {
                keep.push_back(t);
            }
        }
        // apply current values of the still-running tweens in order (resolved
        // props); future [tweenset] members stay latent so an
        // earlier segment keeps driving the property.
        for (const Tween& t : keep) {
            if (t.set_id && now_ms < t.start_ms) continue;
            l->set_props({{t.param, format_tween_param(t.param, t.get_value(now_ms))}});
            // 活动帧值写收口(None = 簿记不记 —— animated_now
            // 帧项覆盖;收敛表行 advance_tweens 的逐帧值写落点)
            note_content_change(id, DirtyAspect::None);
        }
        if (keep.empty()) {
            anim_->tweens_.erase(it);
        } else {
            it->second = std::move(keep);
        }
        if (wants_delete) delete_ids.push_back(id);
    }
    for (const std::string& id : delete_ids) remove(id);

    // [anime] frames: advance playbacks to the same clock (
    // Compositor::advance runs gc_finished_tweens then update_anime_frames).
    // A finished finite playback keeps
    // the last frame on the layer (already applied) and drops the state.
    if (!anim_->anime_states_.empty()) {
        std::vector<std::string> finished;
        for (auto& [id, st] : anim_->anime_states_) {
            if (st.frames.empty() || st.total_duration_ms == 0) continue;
            const uint64_t elapsed = now_ms >= st.start_ms ? now_ms - st.start_ms : 0;
            uint64_t t = 0;
            if (st.loop_count < 0) {
                // -1 (and other negatives) cycle forever.
                t = elapsed % st.total_duration_ms;
            } else {
                // loop=0 plays once, loop=N plays N rounds; total = rounds x
                // single round.
                const uint64_t rounds = st.loop_count == 0 ? 1 : uint64_t(st.loop_count);
                const uint64_t max_elapsed = st.total_duration_ms * rounds;
                if (elapsed >= max_elapsed) {
                    finished.push_back(id);
                    t = st.total_duration_ms - 1; // clamp to the last frame
                } else {
                    t = elapsed % st.total_duration_ms;
                }
            }
            const AnimeFrame* frame = nullptr;
            for (auto it = st.frames.rbegin(); it != st.frames.rend(); ++it) {
                if (it->time_ms <= t) {
                    frame = &*it;
                    break;
                }
            }
            if (!frame) frame = &st.frames.front();
            if (Layer* l = find_mut(id)) {
                l->set_resource_file(frame->file); // 帧写同时解除宿主供帧绑定
                l->mask = frame->mask; // a mask-less frame clears the mask
                l->set_props(frame->props);
                // 活动帧写收口(None = 簿记不记;anime 帧直写
                // file/mask/props 的落点 —— 收敛表行 advance_tweens 的
                // 原坐标;角色随帧写即时投影,值语义无缓存)
                note_content_change(id, DirtyAspect::None);
            }
        }
        for (const std::string& id : finished) anim_->anime_states_.erase(id);
    }
    return done;
}

bool Compositor::layer_tweens_finished(const std::string& id) const {
    // sync_tween_finished (runtime/): the layer is gone or
    // keeps no active tween after this tick's garbage collection.
    const auto it = anim_->tweens_.find(id);
    if (it == anim_->tweens_.end()) return true; // layer gone or no bucket
    return it->second.empty();
}

// ---------------------------------------------------------------------------
// [anime] frame animation
// ---------------------------------------------------------------------------

void Compositor::apply_anime(const std::map<std::string, std::string>& params,
                             uint64_t clock_ms) {
    auto g = [&params](const char* k) -> const std::string* {
        const auto it = params.find(k);
        return it == params.end() ? nullptr : &it->second;
    };
    const std::string id = g("id") ? *g("id") : std::string();
    const std::string mode = g("mode") ? *g("mode") : std::string();

    // props whitelist for the [anime] parameter surface.
    static const char* kAnimePropKeys[] = {
        "left",      "top",       "alpha",    "anchorx", "anchory",
        "xscale",    "yscale",    "rotate",   "reversex", "reversey",
        "clip",      "layermode", "negative", "grayscale", "colormultiply",
        "visible",
    };
    std::map<std::string, std::string> props;
    for (const char* k : kAnimePropKeys)
        if (const auto it = params.find(k); it != params.end()) props[k] = it->second;

    uint64_t time = 0;
    if (const std::string* v = g("time")) (void)parse_u64(*v, &time);
    int32_t loop = -1; // init default -1; parse failure -> -1
    if (const std::string* v = g("loop")) {
        int32_t parsed = 0;
        if (parse_i32(*v, &parsed)) loop = parsed;
    }
    const std::string file = g("file") ? *g("file") : std::string();
    const std::string mask = g("mask") ? *g("mask") : std::string();

    if (mode == "init") {
        ensure_path(id); // scene.ensure(request.id)
        if (Layer* l = find_mut(id)) {
            l->set_resource_file(file); // 帧写同时解除宿主供帧绑定
            l->mask = mask;
            l->set_props(props);
        }
        AnimeFrame frame;
        frame.time_ms = time;
        frame.file = file;
        frame.mask = mask;
        frame.props = std::move(props);
        AnimeState st;
        st.frames.push_back(std::move(frame));
        st.loop_count = loop;
        st.start_ms = 0;
        st.total_duration_ms = 0;
        anim_->anime_states_[id] = std::move(st);
        // 簿记: [anime] init = 内容/属性写(文件绑定+状态);
        // 统一收口(收敛表行 apply_anime init:首帧 file/mask/props 直写)
        note_content_change(id, DirtyAspect::Content);
        return;
    }
    if (mode == "add") {
        const auto it = anim_->anime_states_.find(id);
        if (it == anim_->anime_states_.end()) return; // no state -> ignored
        AnimeFrame frame;
        frame.time_ms = time;
        frame.file = file;
        frame.mask = mask;
        frame.props = std::move(props);
        it->second.frames.push_back(std::move(frame));
        // 簿记: 状态追加(事件级写;播放期帧写不记)。
        // 统一收口(状态追加本身不改内容态,簿记位不变)
        note_content_change(id, DirtyAspect::Content);
        return;
    }
    if (mode == "end") {
        const auto it = anim_->anime_states_.find(id);
        if (it == anim_->anime_states_.end()) return; // no state -> ignored
        std::stable_sort(it->second.frames.begin(), it->second.frames.end(),
                         [](const AnimeFrame& a, const AnimeFrame& b) {
                             return a.time_ms < b.time_ms;
                         });
        it->second.total_duration_ms = time;
        it->second.start_ms = clock_ms;
        // apply the first frame right away
        if (Layer* l = find_mut(id)) {
            if (!it->second.frames.empty()) {
                const AnimeFrame& f = it->second.frames.front();
                l->set_resource_file(f.file); // 帧写同时解除宿主供帧绑定
                l->mask = f.mask;
                l->set_props(f.props);
            }
        }
        // 簿记: 播放启动(首帧已应用)。
        // 统一收口(收敛表行 apply_anime end:首帧应用 = 内容写)
        note_content_change(id, DirtyAspect::Content);
        return;
    }
    // unknown modes are ignored
}

} // namespace oa::render

// ===========================================================================
// §8 anim/transition 语义核。本段集中 anim/tween/[anime] 的缓动与时间线数学、
// [trans] 语义机（Transition）：
//   - tween/[anime] 的持有者与逐帧消费者都是本文件的 Compositor
//     （AnimStore + apply_lytween/advance_tweens/apply_anime）；
//   - [trans] 语义机（Transition）的唯一改写者是 runtime，但它的声明面与
//     tween 同在 render_internal.h，故一并落此，保持"每域一个实现文件"。
// 落点在同 TU 的另一条硬约束（实测）：本文件是 tests/* 里链接 libopenartemis
// 时**必被拉取**的成员，且不含字体/后端依赖；若把这段语义核并进
// renderer.cpp.o，会因该成员
// 携带 RenderEngine→FontSystem→FT_* 未定义引用而连带拉入 font.cpp.o，使不链
// Freetype::Freetype 的测试目标链接失败（静态库按需拉取闭包）。
// 本段只依赖本文件头部统一提供的 include/namespace 包装，函数体无其他前置。
// ===========================================================================
namespace oa::render {


namespace {
double clamp01(double t) { return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t); }

// Standard easing formulas (canonical Penner equations, public domain;
// constants identical to the framework's easing table).
double ease_in_out_back(double t) {
    const double c1 = 1.70158;
    const double c2 = c1 * 1.525;
    if (t < 0.5) {
        const double t2 = 2.0 * t;
        return (t2 * t2 * ((c2 + 1.0) * t2 - c2)) / 2.0;
    }
    const double t2 = 2.0 * t - 2.0;
    return (t2 * t2 * ((c2 + 1.0) * t2 + c2)) / 2.0 + 1.0;
}
double ease_out_elastic(double t) {
    if (t <= 0.0 || t >= 1.0) return t;
    const double c4 = 2.0 * 3.14159265358979323846 / 3.0;
    return std::pow(2.0, -10.0 * t) * std::sin((t * 10.0 - 0.75) * c4) + 1.0;
}
double ease_out_bounce(double t) {
    const double n1 = 7.5625;
    const double d1 = 2.75;
    if (t < 1.0 / d1) return n1 * t * t;
    if (t < 2.0 / d1) {
        const double t1 = t - 1.5 / d1;
        return n1 * t1 * t1 + 0.75;
    }
    if (t < 2.5 / d1) {
        const double t1 = t - 2.25 / d1;
        return n1 * t1 * t1 + 0.9375;
    }
    const double t1 = t - 2.625 / d1;
    return n1 * t1 * t1 + 0.984375;
}
} // namespace

Easing parse_easing(std::string_view name) {
    std::string n(name);
    for (char& c : n) c = char(std::tolower((unsigned char)c));
    const std::string_view s = n;
    // legacy short aliases (compat with old scripts)
    if (s == "in" || s == "easein") return Easing::EaseInQuad;
    if (s == "out" || s == "easeout") return Easing::EaseOutQuad;
    if (s == "inout" || s == "easeinout") return Easing::EaseInOutQuad;
    struct NameMap {
        std::string_view name;
        Easing value;
    };
    static const NameMap kMap[] = {
        {"easein_quad", Easing::EaseInQuad},
        {"easeinquad", Easing::EaseInQuad},
        {"easeout_quad", Easing::EaseOutQuad},
        {"easeoutquad", Easing::EaseOutQuad},
        {"easeinout_quad", Easing::EaseInOutQuad},
        {"easeinoutquad", Easing::EaseInOutQuad},
        {"easein_cubic", Easing::EaseInCubic},
        {"easeincubic", Easing::EaseInCubic},
        {"easeout_cubic", Easing::EaseOutCubic},
        {"easeoutcubic", Easing::EaseOutCubic},
        {"easeinout_cubic", Easing::EaseInOutCubic},
        {"easeinoutcubic", Easing::EaseInOutCubic},
        {"easein_quart", Easing::EaseInQuart},
        {"easeinquart", Easing::EaseInQuart},
        {"easeout_quart", Easing::EaseOutQuart},
        {"easeoutquart", Easing::EaseOutQuart},
        {"easeinout_quart", Easing::EaseInOutQuart},
        {"easeinoutquart", Easing::EaseInOutQuart},
        {"easein_quint", Easing::EaseInQuint},
        {"easeinquint", Easing::EaseInQuint},
        {"easeout_quint", Easing::EaseOutQuint},
        {"easeoutquint", Easing::EaseOutQuint},
        {"easeinout_quint", Easing::EaseInOutQuint},
        {"easeinoutquint", Easing::EaseInOutQuint},
        {"easein_expo", Easing::EaseInExpo},
        {"easeinexpo", Easing::EaseInExpo},
        {"easeout_expo", Easing::EaseOutExpo},
        {"easeoutexpo", Easing::EaseOutExpo},
        {"easeinout_expo", Easing::EaseInOutExpo},
        {"easeinoutexpo", Easing::EaseInOutExpo},
        {"easein_circ", Easing::EaseInCirc},
        {"easeincirc", Easing::EaseInCirc},
        {"easeout_circ", Easing::EaseOutCirc},
        {"easeoutcirc", Easing::EaseOutCirc},
        {"easeinout_circ", Easing::EaseInOutCirc},
        {"easeinoutcirc", Easing::EaseInOutCirc},
        {"easein_sine", Easing::EaseInSine},
        {"easeinsine", Easing::EaseInSine},
        {"easeout_sine", Easing::EaseOutSine},
        {"easeoutsine", Easing::EaseOutSine},
        {"easeinout_sine", Easing::EaseInOutSine},
        {"easeinoutsine", Easing::EaseInOutSine},
        {"easein_back", Easing::EaseInBack},
        {"easeinback", Easing::EaseInBack},
        {"easeout_back", Easing::EaseOutBack},
        {"easeoutback", Easing::EaseOutBack},
        {"easeinout_back", Easing::EaseInOutBack},
        {"easeinoutback", Easing::EaseInOutBack},
        {"easein_elastic", Easing::EaseInElastic},
        {"easeinelastic", Easing::EaseInElastic},
        {"easeout_elastic", Easing::EaseOutElastic},
        {"easeoutelastic", Easing::EaseOutElastic},
        {"easeinout_elastic", Easing::EaseInOutElastic},
        {"easeinoutelastic", Easing::EaseInOutElastic},
        {"easein_bounce", Easing::EaseInBounce},
        {"easeinbounce", Easing::EaseInBounce},
        {"easeout_bounce", Easing::EaseOutBounce},
        {"easeoutbounce", Easing::EaseOutBounce},
        {"easeinout_bounce", Easing::EaseInOutBounce},
        {"easeinoutbounce", Easing::EaseInOutBounce},
    };
    for (const NameMap& m : kMap)
        if (s == m.name) return m.value;
    return Easing::Linear; // unknown names are Linear
}

double ease_value(Easing e, double t) {
    const double x = clamp01(t);
    switch (e) {
        case Easing::Linear:
            return x;
        case Easing::EaseInQuad:
            return x * x;
        case Easing::EaseOutQuad:
            return x * (2.0 - x);
        case Easing::EaseInOutQuad:
            return x < 0.5 ? 2.0 * x * x : -1.0 + (4.0 - 2.0 * x) * x;
        case Easing::EaseInCubic:
            return x * x * x;
        case Easing::EaseOutCubic: {
            const double t1 = x - 1.0;
            return t1 * t1 * t1 + 1.0;
        }
        case Easing::EaseInOutCubic: {
            if (x < 0.5) return 4.0 * x * x * x;
            const double t1 = -2.0 * x + 2.0;
            return 1.0 - t1 * t1 * t1 / 2.0;
        }
        case Easing::EaseInQuart:
            return x * x * x * x;
        case Easing::EaseOutQuart: {
            const double t1 = x - 1.0;
            return 1.0 - t1 * t1 * t1 * t1;
        }
        case Easing::EaseInOutQuart: {
            if (x < 0.5) return 8.0 * x * x * x * x;
            const double t1 = -2.0 * x + 2.0;
            return 1.0 - t1 * t1 * t1 * t1 / 2.0;
        }
        case Easing::EaseInQuint:
            return x * x * x * x * x;
        case Easing::EaseOutQuint: {
            const double t1 = x - 1.0;
            return 1.0 + t1 * t1 * t1 * t1 * t1;
        }
        case Easing::EaseInOutQuint: {
            if (x < 0.5) return 16.0 * x * x * x * x * x;
            const double t1 = -2.0 * x + 2.0;
            return 1.0 - t1 * t1 * t1 * t1 * t1 / 2.0;
        }
        case Easing::EaseInExpo:
            return x <= 0.0 ? 0.0 : std::pow(2.0, 10.0 * x - 10.0);
        case Easing::EaseOutExpo:
            return x >= 1.0 ? 1.0 : 1.0 - std::pow(2.0, -10.0 * x);
        case Easing::EaseInOutExpo: {
            if (x <= 0.0) return 0.0;
            if (x >= 1.0) return 1.0;
            if (x < 0.5) return std::pow(2.0, 20.0 * x - 10.0) / 2.0;
            return (2.0 - std::pow(2.0, -20.0 * x + 10.0)) / 2.0;
        }
        case Easing::EaseInCirc:
            return 1.0 - std::sqrt(1.0 - x * x);
        case Easing::EaseOutCirc:
            return std::sqrt(1.0 - (x - 1.0) * (x - 1.0));
        case Easing::EaseInOutCirc: {
            if (x < 0.5) {
                const double a = 1.0 - (2.0 * x) * (2.0 * x);
                return (1.0 - std::sqrt(a)) / 2.0;
            }
            const double a = 1.0 - (-2.0 * x + 2.0) * (-2.0 * x + 2.0);
            return (std::sqrt(a) + 1.0) / 2.0;
        }
        case Easing::EaseInSine:
            return 1.0 - std::cos(x * 3.14159265358979323846 / 2.0);
        case Easing::EaseOutSine:
            return std::sin(x * 3.14159265358979323846 / 2.0);
        case Easing::EaseInOutSine:
            return -(std::cos(3.14159265358979323846 * x) - 1.0) / 2.0;
        case Easing::EaseInBack: {
            const double c1 = 1.70158;
            const double c3 = c1 + 1.0;
            return c3 * x * x * x - c1 * x * x;
        }
        case Easing::EaseOutBack: {
            const double c1 = 1.70158;
            const double c3 = c1 + 1.0;
            return 1.0 + c3 * std::pow(x - 1.0, 3.0) + c1 * std::pow(x - 1.0, 2.0);
        }
        case Easing::EaseInOutBack:
            return ease_in_out_back(x);
        case Easing::EaseInElastic: {
            if (x <= 0.0 || x >= 1.0) return x;
            const double c4 = 2.0 * 3.14159265358979323846 / 3.0;
            return -std::pow(2.0, 10.0 * x - 10.0) *
                   std::sin((x * 10.0 - 10.75) * c4);
        }
        case Easing::EaseOutElastic:
            return ease_out_elastic(x);
        case Easing::EaseInOutElastic: {
            if (x <= 0.0 || x >= 1.0) return x;
            const double c5 = 2.0 * 3.14159265358979323846 / 4.5;
            if (x < 0.5)
                return -(std::pow(2.0, 20.0 * x - 10.0) *
                         std::sin((20.0 * x - 11.125) * c5)) /
                       2.0;
            return std::pow(2.0, -20.0 * x + 10.0) *
                       std::sin((20.0 * x - 11.125) * c5) /
                       2.0 +
                   1.0;
        }
        case Easing::EaseInBounce:
            return 1.0 - ease_out_bounce(1.0 - x);
        case Easing::EaseOutBounce:
            return ease_out_bounce(x);
        case Easing::EaseInOutBounce:
            if (x < 0.5)
                return (1.0 - ease_out_bounce(1.0 - 2.0 * x)) / 2.0;
            return (1.0 + ease_out_bounce(2.0 * x - 1.0)) / 2.0;
    }
    return x;
}

bool Tween::is_looping() const {
    return infinite_loop || loop_count.has_value();
}

uint64_t Tween::effective_time(uint64_t now) const {
    const uint64_t total_cycle = duration_ms + loop_delay_ms;
    if (total_cycle == 0 || !is_looping()) {
        const uint64_t elapsed = now >= start_ms ? now - start_ms : 0;
        return elapsed < duration_ms ? elapsed : duration_ms;
    }
    const uint64_t elapsed = now >= start_ms ? now - start_ms : 0;
    const uint64_t cycle_index = elapsed / total_cycle;
    if (loop_count.has_value() && cycle_index >= uint64_t(*loop_count)) {
        return duration_ms; // past all finite cycles -> terminal value
    }
    const uint64_t intra = elapsed % total_cycle;
    return intra < duration_ms ? intra : duration_ms;
}

double Tween::get_value(uint64_t now) const {
    if (duration_ms == 0) return to;
    if (now < start_ms) return from; // delay phase
    const bool reverse = yoyo_reverse;
    const double local_from = reverse ? to : from;
    const double local_to = reverse ? from : to;
    const uint64_t t = effective_time(now);
    if (t >= duration_ms) return local_to;
    const double progress = ease_value(easing, double(t) / double(duration_ms));
    return local_from + (local_to - local_from) * progress;
}

bool Tween::is_finished(uint64_t now) const {
    if (infinite_loop) return false;
    if (duration_ms == 0) return true;
    if (loop_count.has_value()) {
        // N cycles = N*duration + (N-1)*loop_delay
        const uint64_t max = uint64_t(*loop_count);
        const uint64_t total =
            start_ms + max * duration_ms + (max > 0 ? max - 1 : 0) * loop_delay_ms;
        return now >= total;
    }
    return now >= start_ms + duration_ms;
}

bool Tween::is_yoyo_reverse(uint64_t now) const {
    if (!yoyo || !is_looping()) return false;
    const uint64_t total_cycle = duration_ms + loop_delay_ms;
    if (total_cycle == 0) return false;
    const uint64_t elapsed = now >= start_ms ? now - start_ms : 0;
    const uint64_t cycle_index = elapsed / total_cycle;
    return cycle_index % 2 == 1;
}

std::string format_tween_param(std::string_view param, double value) {
    static const char* kIntKeys[] = {
        "alpha",     "visible",  "reversex", "reversey", "grayscale",
        "negative",  "delete",   "stack",    "vertical", "hung",
        "anchorcenter", "overflow",
    };
    for (const char* k : kIntKeys) {
        if (param == k) return std::to_string((int64_t)std::llround(value));
    }
    // shortest numeric string like Rust f32::to_string for the rest
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), value);
    if (res.ec == std::errc()) return std::string(buf, res.ptr);
    return std::to_string(value);
}



void Transition::start(int trans_type, std::optional<uint64_t> time,
                       std::string rule, std::optional<int> vague, int input,
                       uint64_t clock_ms) {
    if (trans_type == 0) { // instant switch: clear
        clear();
        return;
    }
    State s;
    s.type = trans_type;
    s.start_ms = clock_ms;
    s.duration_ms = time.value_or(1000); // default 1000ms
    s.captured = false;
    s.needs_capture = true;
    s.input = input;
    s.rule = std::move(rule);
    s.vague = vague.value_or(32);
    state_ = s;
}

void Transition::clear() { state_.reset(); }

void Transition::mark_captured(uint64_t clock_ms) {
    if (!state_ || !state_->needs_capture) return;
    state_->needs_capture = false;
    state_->captured = true;
    state_->start_ms = clock_ms; // duration counts from the capture moment
}

void Transition::clear_finished(uint64_t clock_ms) {
    if (!state_) return;
    if (!state_->needs_capture &&
        clock_ms - state_->start_ms >= state_->duration_ms) {
        state_.reset();
    }
}

bool Transition::skip_by_input(bool in_skip_mode) {
    if (!state_) return false;
    const int policy = state_->input;
    const bool allowed = policy == 0 ? false : (policy == 2 ? in_skip_mode : true);
    if (allowed) state_.reset();
    return allowed;
}

double Transition::progress(uint64_t clock_ms) const {
    if (!state_ || state_->needs_capture) return 0.0;
    const uint64_t elapsed = clock_ms - state_->start_ms;
    const double p = double(elapsed) / double(state_->duration_ms);
    return p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
}

} // namespace oa::render
