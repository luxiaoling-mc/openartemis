#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/fs/fs.h"
#include "core/media/image.h"
#include "core/render/backend.h"
#include "core/render/font.h"
#include "core/render/layer.h"
#include "core/render/render_host.h"

namespace oa::emote {
struct EmoteDrawPart;
class EmoteFile;
}
namespace oa::render {
class Transition;
}
namespace oa::runtime {
class GameRuntime;
struct Event;
}

struct SDL_Window; // host-owned; no SDL types beyond this handle.

namespace oa::render {

class Compositor;
class TextEngine;

/// 绘制项种类。栅格化侧按此分派，不读任何层属性。
enum class PlanOp : uint8_t {
    Content,    // 一层自身的内容四边形（纹理 quad 或纯色 quad）
    Glyphs,     // 一层槽位上的字形（该层绑定的消息层）
    GroupBegin, // 离屏合成组：子树进离屏目标，合成参数在 composite
    GroupEnd,   // 离屏合成组结束：读回 → CPU 合成 → 上传 → 合成到父目标
};

/// 已求值的裁剪窗（组离屏目标的范围）。
struct PlanClip {
    bool has = false;
    IRect rect{};
};

/// 组合成参数（旧 resolve_group_plan 的求值结果）。
/// 栅格化侧只消费它，不再解析 intermediate_render/colormultiply/layermode…。
struct PlanComposite {
    double alpha = 1.0;                        // 组 alpha（合成时应用，不进烘焙）
    BlendMode blend = BlendMode::Blend;        // layermode 归一
    double color_multiply[3] = {1.0, 1.0, 1.0}; // 烘焙后逐通道乘色
    bool grayscale = false;
    bool negative = false;
    bool has_mask = false;
    std::string mask;  // intermediate_render_mask 资源名（栅格化时解码）
    PlanClip clip;     // 继承 clip ∩ 本组 clip；空 = 不裁
};

/// 一条绘制项。字段按 op 取用（Content / Glyphs / GroupBegin）。
struct PlanItem {
    PlanOp op = PlanOp::Content;
    std::string id;      // 层 id（诊断名；组合成不靠它做缓存键）
    uint32_t z = 0;      // 流内序号 == 绘制序（诊断/批处理分组用）
    Affine2 world;       // 已合成的世界变换（父链 × 本层 local_transform）
    double opacity = 1.0; // 已合成的链式不透明度（组 alpha 若走合成则不在内）

    // ---- Content ----
    bool host = false;   // 内容来自层上的宿主帧（frame/frame_w/h），无纹理键
    bool solid = false;                    // 纯色 quad（无纹理）
    uint8_t solid_rgba[4] = {0, 0, 0, 0};  // r,g,b,a（层 typed 值原样）
    TextureKey texture;                    // 内容纹理域键（角色决定）
    bool has_color_multiply = false;       // colormultiply 已解析进 color_multiply
    uint8_t color_multiply[3] = {255, 255, 255};
    std::string mask;                      // 纹理 mask 合成（仅 Asset 域）
    double w = 0, h = 0;                   // 内容四边形局部尺寸
    bool has_src = false;                  // clip 纹理源子矩形
    FRect src{};

    // ---- Glyphs ----
    std::vector<std::string> messages;     // 绑定到该槽位的消息层 id（有序）

    // ---- GroupBegin ----
    PlanComposite composite;
};

/// 一帧的绘制项流。容量跨帧复用（clear 保留 vector 容量）。
class RenderPlan {
public:
    void clear();
    /// 追加一项并返回引用（z 自动取当前序号）。
    PlanItem& push(PlanOp op, std::string id);

    const std::vector<PlanItem>& items() const { return items_; }

    size_t content_count = 0;
    size_t glyph_count = 0;
    size_t group_count = 0;
    /// 最近一次构建的代数（诊断：是否重建、重建了几次）。
    uint64_t generation = 0;

private:
    std::vector<PlanItem> items_;
};

/// 计划构建输入。sizes 可空（无解码面时只按已缓存尺寸出 quad）。
struct PlanInput {
    const Compositor* scene = nullptr;
    const TextEngine* text = nullptr;
    ContentSizeSource* sizes = nullptr;
    int stage_w = 1280;
    int stage_h = 720;
    double global_fade = 1.0;
};

/// 把层树求值成绘制项流（前序 DFS：场景区 → 消息槽区 → overlay 槽）。
/// 只读 scene/text，无后端依赖 —— headless 也可构建与断言。
void build_render_plan(const PlanInput& in, RenderPlan* out);


/// Owns the backend, the stage target and the scene/text/transition domains.
/// The host only boots it, feeds events and advances frames.
class RenderEngine : public oa::render::ContentSizeSource {
public:
    // ---- lifecycle / backend ----------------------------------------------
    RenderEngine(const oa::fs::IFileSystem* fs, oa::render::RenderHost* host,
                 bool bind_hooks = true);
    /// Compatibility adapter: shares the runtime-owned render domains.
    RenderEngine(const oa::fs::IFileSystem* fs, oa::runtime::GameRuntime* rt,
                 bool bind_hooks = true);
    ~RenderEngine();
    /// Create the backend on the host window ("sdl" default, or "gles").
    bool create_renderer(void* native_window,
                         const std::string& backend_kind = "sdl");
    void release_all();
    bool renderer_ok() const { return backend_ != nullptr; }

    // ---- render domains ----------------------------------------------------
    oa::render::Compositor& scene() { return *scene_ptr_; }
    const oa::render::Compositor& scene() const { return *scene_ptr_; }
    oa::render::TextEngine& text() { return *text_ptr_; }
    const oa::render::TextEngine& text() const { return *text_ptr_; }
    oa::render::Transition& transition() { return *transition_; }
    const oa::render::Transition& transition() const { return *transition_; }

    // ---- frame pump --------------------------------------------------------
    /// Sync host frames, draw and present (once per tick).
    bool render_frame(uint64_t now_ms, double global_fade);
    /// Window resize/expose: force one repaint even when static.
    /// 请求下一帧强制重绘（窗口暴露/尺寸变化等）。
    void request_repaint() { dirty_ = true; }
    bool vsync_active() const { return vsync_active_; }
    bool frame_presented() const { return frame_presented_; }
    /// Mark visual state changed (scene/text/pointer/transition tags).
    /// Tag one runtime event: bumps its counter and marks the frame dirty.
    enum class Note { Layer, Tween, TweenComplete, Text, MessageSwitch };
    void note(Note what);
    struct Stats {
        size_t layers = 0;
        size_t tweens = 0;
        size_t tween_done = 0;
        size_t text = 0;
        size_t message_switch = 0;
    };
    /// Event counters read by the headless end-of-run dump.
    const Stats& stats() const { return stats_; }
    /// Host frame capture for [takess]/[savess].
    std::optional<std::vector<uint8_t>> capture_frame(uint32_t* w, uint32_t* h);
    /// [trans] capture of the previous stage frame; false = capture-less.
    bool capture_transition_source();

    // ---- 宿主纹理 --------------------------------------------------------
    /// 创建一块与后端无关的可写纹理（源持有、update；层持有同一句柄）。
    oa::render::HostTextureRef create_host_texture(int w, int h);
    /// emote GPU 画布：把已求值部件画进目标纹理的离屏 target。
    bool compose_canvas(const std::string& layer_id,
                        const oa::render::HostTextureRef& target,
                        const oa::emote::EmoteFile& file,
                        const std::vector<oa::emote::EmoteDrawPart>& parts,
                        int w, int h);
    void process_event(const oa::runtime::Event& e);
    void bind_runtime_hooks();

    // ---- [alldelete] fade clock -------------------------------------------
    void begin_all_delete(uint64_t time_ms, uint64_t now_ms);
    double all_delete_fade(uint64_t now_ms) const;
    bool all_delete_active() const { return all_delete_active_; }
    bool all_delete_expired(uint64_t now_ms) const;
    void stop_all_delete();

    // ---- content, hit testing, pointer mapping -----------------------------
    /// Read a save-area file through the runtime's SaveStore.
    std::optional<std::vector<uint8_t>> overlay_read(const std::string& rel);
    /// Decode + cache one image (magic paths, ".png" fallback).
    const oa::media::Image* resolve_image(const std::string& name);
    /// Hit-size fallback for file layers without explicit geometry.
    static std::optional<std::pair<double, double>> hit_size(void* userdata, const oa::render::Layer& l);
    /// Pixel-alpha sampler for clickablethreshold hit testing.
    static std::optional<uint8_t> alpha_sampler(void* userdata, const oa::render::Layer& l, int tx, int ty);
    bool get_renderer_coordinates(float winx, float winy, float* rx, float* ry);
    /// Move the OS pointer onto a stage point ([mouse] warp requests).
    bool warp_mouse_to_stage(float sx, float sy);

    // ---- probe-only step path (perf probes; hosts use render_frame()) ------
    void render_begin();
    size_t draw_scene(const oa::render::Compositor& sc, double global_fade);
    /// 最近一次构建的帧计划（流水线中间件；诊断/探针只读）。
    const oa::render::RenderPlan& render_plan() const { return plan_; }
    /// 只构建帧计划，不落任何后端调用（headless 断言/计划检查用）。
    void build_plan(const oa::render::Compositor& sc, double global_fade);
    void progress_transition();
    void render_end();
    struct CacheStats {
        size_t host_textures = 0;
        size_t decoded_images = 0;
        size_t emote_canvases = 0;
        size_t emote_atlases = 0;
    };
    /// Memory probes: live cache entries.
    CacheStats cache_stats() const;

private:
    RenderEngine();
    void configure(const oa::fs::IFileSystem* fs, oa::render::RenderHost* host,
                   oa::render::TextEngine* text, oa::render::Compositor* scene,
                   bool bind_hooks);
    std::tuple<double, double, double> text_metrics();
    int stage_w_ = 1280;
    int stage_h_ = 720;

    // ---- texture services (pump/draw internals) -----------------------
    TextureRef make_texture(const oa::media::Image& img);
    TextureRef texture_for_asset(const std::string& name);
    TextureRef texture_for_key(const oa::render::TextureKey& key);
    /// 上传一帧到内容槽；`luma_key` = 视频亮度键（未遮罩通道）。
    bool emote_render_parts(const std::string& layer_id,
                            const oa::render::HostTextureRef& target,
                            const oa::emote::EmoteFile& file,
                            const std::vector<oa::emote::EmoteDrawPart>& parts,
                            int w, int h);
    /// 回收已经没有对应场景层的 emote 图集（计划重建时扫一遍）。
    void sweep_host_frames();
    /// 源创建的宿主纹理登记：后端销毁前统一 release（层可能仍持有句柄）。
    std::vector<std::weak_ptr<oa::render::HostTexture>> host_textures_;

    // ---- rasterizer: 只消费帧计划，不遍历层树/不读 props --------------
    /// 按序栅格化一条帧计划（流水线的唯一后端出口）。
    size_t rasterize_plan(const oa::render::RenderPlan& plan);
    /// 一条内容项（纯色/纹理 quad）。scale = 无离屏目标时的组 alpha 回退。
    size_t rasterize_content(const oa::render::PlanItem& it, double scale);
    /// 一条字形项（该槽位绑定的消息层）。
    size_t rasterize_glyphs(const oa::render::PlanItem& it, double scale);
    /// 一次离屏烘焙：读回 → CPU 合成 → 上传 → 缓存。
    TextureRef bake_group(const oa::render::PlanItem& begin, TextureRef group_tex,
                          TextureRef saved_target, bool had_clip,
                          const IRect& old_clip);
    uint8_t alpha_byte(double a);
    TextureRef texture_for_masked_key(const oa::render::TextureKey& base,
                                      const std::string& mask);
    TextureRef solid_texture(const uint8_t rgba[4]);
    std::map<uint32_t, TextureRef> solid_tex_cache;
    /// Capture the fixed stage target ([takess]/[savess] and [trans] fallback).
    bool snapshot_renderer(oa::media::Image& out);
    /// Stage → window coordinates (inverse of get_renderer_coordinates).
    bool stage_to_window_coordinates(float sx, float sy, float* wx, float* wy);

    // ContentSizeSource queries: Asset → decoded, host-frame → textures.
    std::optional<std::pair<double, double>> decoded_size(const oa::render::TextureKey& key) const override;
    /// 计划构建期的解码保证（生产者前置；只解像素，不建 GPU 纹理）。
    bool ensure_decoded(const oa::render::TextureKey& key) override;
    /// Header-only asset size (PNG/JPEG) for headless hit testing.
    std::optional<std::pair<double, double>> probe_asset_size(const std::string& name);
    std::map<std::string, std::pair<double, double>> probed_sizes_;
    /// Composite a bake with group alpha/blend/clip.
    void composite_group_bake(TextureRef baked,
                              const oa::render::PlanComposite& p, bool had_clip,
                              const IRect& old_clip);
    /// Begin the offscreen pass; returns the previous target.
    TextureRef begin_offscreen_pass(TextureRef target,
                                    const oa::render::PlanComposite& p);
    /// Leave the offscreen session: restore the previous target and clip.
    void end_offscreen_pass(TextureRef saved_target, bool had_clip,
                            const IRect& old_clip);
    void restore_render_clip(bool had_clip, const IRect& old_clip);
    /// CPU composite of the group target (filters + mask; straight RGBA).
    /// origin_x/origin_y = 组的画布原点（mask 画在中间渲染缓冲上的位置）。
    void group_composite_cpu(const std::vector<uint8_t>& px, int stage_w,
                             int stage_h,
                             const oa::render::PlanComposite& p,
                             std::vector<uint8_t>* out_px, int origin_x = 0,
                             int origin_y = 0);
    /// Upload straight-RGBA pixels as a static stage-sized texture.
    TextureRef upload_baked_texture(const std::vector<uint8_t>& px, int w,
                                    int h);

    /// Render backend (created in create_renderer, freed in release_all).
    std::unique_ptr<RenderBackend> backend_;
    SDL_Window* window_ = nullptr; // host window handle (create_renderer)
    oa::render::Compositor scene_;
    oa::render::TextEngine text_;
    oa::render::Compositor* scene_ptr_ = &scene_;
    oa::render::TextEngine* text_ptr_ = &text_;
    std::unique_ptr<oa::render::Transition> transition_owned_;
    oa::render::Transition* transition_ = nullptr;
    // Fixed stage target: scene draws here; render_end presents it.
    TextureRef stage_rt = nullptr;
    bool stage_frame_valid_ = false;
    // Decoded pixel cache (assets + mask composites), LRU-bounded.
    std::map<std::string, oa::media::Image> decoded;
    std::map<std::string, uint64_t> decoded_use_;
    uint64_t cache_stamp_ = 0;
    size_t decoded_bytes_ = 0;
    void decoded_note(const std::string& name, oa::media::Image&& img);
    void decoded_evict_if_needed();
    // Texture cache keyed by TextureKey domain (Asset vs host frames).
    std::map<oa::render::TextureKey, TextureRef> textures;
    // GPU emote canvases (offscreen targets, also registered in
    // `textures` under the same key while active) and their atlas textures.
    /// Pixel size each cached canvas was created with. The canvas is reused
    /// across poses, but a reload can change the render-buffer size (E-mote
    /// half-res policy differs per PSB / per load): reusing the old texture
    /// then draws the new pose into a target of the wrong size and the figure
    /// is cropped (seen as "only the feet" after a config round-trip).
    // Emote atlas cache keyed by (layer, source, PSB uid).
    struct EmoteAtlasKey {
        std::string layer;
        int source = 0;
        uint64_t file = 0; // EmoteFile::uid()
        bool operator<(const EmoteAtlasKey& o) const {
            if (layer < o.layer) return true;
            if (o.layer < layer) return false;
            if (source != o.source) return source < o.source;
            return file < o.file;
        }
    };
    std::map<EmoteAtlasKey, TextureRef> emote_atlases_;

    // Stencil scratch targets (sized to the group bbox).
    TextureRef stencil_scratch_[2] = {nullptr, nullptr};
    int stencil_scratch_w_ = 0, stencil_scratch_h_ = 0;
    void release_stencil_scratch();
    // Transition capture texture (type-1 crossfade / type-2 rule source).
    TextureRef trans_capture_tex = nullptr;
    int trans_capture_w = 0;
    int trans_capture_h = 0;
    // Type-2 rule texture state (resolution checked once per transition).
    TextureRef trans_rule_tex = nullptr;
    bool trans_rule_checked_ = false;
    size_t trans_events = 0;
    /// 连续"掩码没解出来"的帧数（成功烘焙即清零）：坏资源不会无限重绘。
    int mask_miss_run_ = 0;
    // 组离屏合成：每次绘制该组时一次做完（不缓存、不跨帧复用）。
    // 组 = 计划里的一段连续绘制项；打进离屏目标 → 合成本帧的 mask/clip/
    // 滤镜 → 立刻贴回父目标。职责只有"这一帧把这段画完"。
    // 当前帧的绘制项流（流水线中间件；容量跨帧复用）。
    oa::render::RenderPlan plan_;
    // 计划缓存键 = 层容器版本 + 文本版本 + 全局淡出 + stage 尺寸 + 场景身份。
    // 任何一项变化才重建；宿主供帧上传不在键里（见 Compositor::revision）。
    bool plan_valid_ = false;
    const oa::render::Compositor* plan_scene_ = nullptr;
    uint64_t plan_scene_rev_ = 0;
    uint64_t plan_text_rev_ = 0;
    double plan_fade_ = -1.0;
    int plan_stage_w_ = 0;
    int plan_stage_h_ = 0;

    // [alldelete] global fade clock.
    bool all_delete_active_ = false;
    uint64_t all_delete_start_ms_ = 0;
    uint64_t all_delete_duration_ms_ = 0;

    // Event counters (note() bumps one; stats() exposes them).
    Stats stats_;

    // Frame presentation state (host no longer owns this).
    bool dirty_ = true;
    bool rendered_any_ = false;
    bool frame_presented_ = false;
    bool vsync_active_ = false;
    uint64_t prev_present_ms_ = 0;
    double present_ema_ms_ = -1.0;
    int present_samples_ = 0;
    uint64_t last_text_rev_ = 0;

    oa::render::RenderHost* host_ = nullptr;
    const oa::fs::IFileSystem* fs_;
    // Declared after backend_ so glyph textures die before the backend.
    std::shared_ptr<oa::render::FontSystem> fontSystem;
};
}
