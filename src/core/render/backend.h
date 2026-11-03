#pragma once

// RenderEngine 渲染后端接口。 本头文件是 RenderEngine（引擎场景绘制/合成逻辑）与"把像素真正画到 target/window"的后端之间的唯一接缝，**不含任何 SDL_render 类型**： - SDL_Texture / SDL_Render...

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// opaque SDL3 window handle：tag + typedef 前向声明（SDL3 头文件里是同一
// typedef 指向同一类型——C++ 允许 typedef 重定义到相同类型，故各 TU 无论
// 是否已含 SDL 头都成立）。窗口创建仍属宿主，本接缝只需句柄。
typedef struct SDL_Window SDL_Window;

namespace oa::render {

// ---- 后端无关几何/颜色类型（sdl 实现内部与 SDL_* 一一映射） ----
struct FPoint {
    float x = 0.0f;
    float y = 0.0f;
};
struct FRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};
struct IRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};
/// 8bit 直色（SDL_Color 布局等价：r,g,b,a）。
struct Rgba {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};
/// 浮点色（SDL_FColor 布局等价），几何顶点用。
struct RgbaF {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

/// 引擎实际使用的混合语义子集（layermode: add/additive、screen≈blend、
/// multiply→mod；其余一律 blend；NONE 仅离屏清屏用）。
enum class BlendMode { None, Blend, Add, Mod,
                       // RGBA *= srcA (stencil mask step). A target filled by
                       // src-over blending holds PREMULTIPLIED rgb, so the
                       // mask has to scale rgb as well as alpha; scaling only
                       // alpha leaves the rgb at full strength and the whole
                       // sprite comes back too bright.
                       AlphaMultiply,
                       // src + dst*(1-srcA): "over" for a PREMULTIPLIED source.
                       BlendPremult };

/// 纹理创建方式（与引擎三种用法一一对应：静态贴图/流式逐帧刷新/离屏
/// target）。
enum class TextureAccess { Static, Streaming, Target };

/// 几何绘制顶点（SDL_Vertex 字段集等价；字段顺序不同，映射在 sdl 实现）。
struct Vertex {
    FPoint pos;
    RgbaF color;
    FPoint uv;
};

/// 纹理对象：不透明基类。各后端派生自己的持有类型
/// （sdl 实现 = SdlTexture{ SDL_Texture* }）。句柄生命周期：创建者
/// （RenderEngine / FontSystem）负责显式 destroy。
struct Texture {};
using TextureRef = Texture*;

/// create() 成功后的渲染器诊断信息（供宿主 create_renderer 的
/// 既有 "[app] window=… render_out=…" 打印使用）。
struct BackendInfo {
    int output_w = 0;  // SDL_GetCurrentRenderOutputSize
    int output_h = 0;
    int logical_w = 0; // SDL_GetRenderLogicalPresentation
    int logical_h = 0;
    int logical_mode = 0; // SDL_RendererLogicalPresentation 枚举值（只回显）
};

/// 渲染后端接口。方法 = RenderEngine/render.cpp 真实调用面的最小投影，
/// 语义 = SDL_Render 状态机原语（"当前 target/当前 clip/当前 draw
/// 色与混合"都是后端内部状态，与 SDL 一致）；sdl 实现逐条转发。
class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    /// 在窗口上创建渲染器并安装 letterbox 逻辑呈现（stage 尺寸）。
    /// 失败返回 false（last_error() 有详情）。成功填充 info。
    virtual bool create(SDL_Window* window, int stage_w, int stage_h,
                        BackendInfo* info) = 0;
    /// 销毁渲染器（宿主要求的顺序点：先毁纹理、后毁渲染器）。
    virtual void shutdown() = 0;
    /// 最近一次失败的可打印原因（SDL_GetError 语义；无失败时可为空串）。
    virtual const char* last_error() = 0;

    // ---- 当前 target 状态 ----
    virtual TextureRef current_target() = 0; // nullptr = 窗口 backbuffer
    virtual void set_target(TextureRef t) = 0;
    virtual bool clip_enabled() = 0;
    virtual IRect clip_rect() = 0; // clip_enabled() 时有效
    virtual void set_clip(const IRect& r) = 0;
    virtual void clear_clip() = 0;

    // ---- 绘制状态与图元（作用于当前 target） ----
    virtual void set_draw_blend(BlendMode m) = 0;
    virtual void set_draw_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) = 0;
    virtual void clear() = 0;
    virtual void fill_rect(const FRect& dst) = 0;
    virtual void draw_texture(TextureRef t, const FRect* src, const FRect* dst) = 0;
    /// 通用仿射：原点/右点/下点三角映射（SDL_RenderTextureAffine 语义，
    /// 旋转/非均匀/负比例路径用）。
    virtual void draw_texture_affine(TextureRef t, const FRect* src,
                                     const FPoint& o, const FPoint& r,
                                     const FPoint& d) = 0;
    /// 三角网格绘制（emote GPU 合成；indices 可为 null/0）。
    virtual void draw_geometry(TextureRef t, const Vertex* verts, int nverts,
                               const int* indices, int nindices) = 0;
    /// [trans type=2] rule 灰度溶解（逐像素 GPU 路径）：把 `capture`（旧帧，
    virtual bool draw_rule_transition(TextureRef capture, TextureRef rule,
                                      float progress, float band) {
        (void)capture;
        (void)rule;
        (void)progress;
        (void)band;
        return false;
    }
    virtual void present() = 0;
    /// Enable/disable vertical sync for the presentation path. Returns true
    /// when the request took effect; backends that cannot (or must not) change
    /// it keep the default no-op and answer false so the host keeps its own
    /// pacing delay. Presentation pacing is the host's call — see the app's
    /// pacing tail.
    virtual bool set_vsync(bool /*on*/) { return false; }

    // ---- 纹理对象 ----
    /// RGBA32 纹理（引擎所有像素都是直 RGBA）。
    virtual TextureRef create_texture(int w, int h, TextureAccess access) = 0;
    virtual void destroy_texture(TextureRef t) = 0;
    /// 整幅像素更新（pitch = 每行字节）。
    virtual void update_texture(TextureRef t, const uint8_t* rgba, int pitch) = 0;
    /// 流式纹理像素填装（Streaming 访问方式；lock 后逐行写，再 unlock）。
    virtual bool lock_texture(TextureRef t, uint8_t** pixels, int* pitch) = 0;
    virtual void unlock_texture(TextureRef t) = 0;
    /// 纹理自然尺寸（像素；失败/无效返回 false）。
    virtual bool texture_size(TextureRef t, float* w, float* h) = 0;
    virtual void set_texture_blend(TextureRef t, BlendMode m) = 0;
    virtual void set_texture_alpha_mod(TextureRef t, uint8_t a) = 0;
    virtual void set_texture_color_mod(TextureRef t, uint8_t r, uint8_t g,
                                       uint8_t b) = 0;

    // ---- 当前 target 像素读回（RGBA32、紧排行；失败返回 false） ----
    // 与既有 SDL_RenderReadPixels + ConvertSurface(RGBA32) 逐行拷贝路径
    // 同构（sdl 实现内实现逐字节一致）。
    virtual bool read_target(int* w, int* h, std::vector<uint8_t>* rgba) = 0;

    // ---- 窗口 ↔ 渲染（逻辑 stage）坐标 ----
    virtual bool window_to_render(float wx, float wy, float* rx, float* ry) = 0;
    virtual bool render_to_window(float rx, float ry, float* wx, float* wy) = 0;
};

/// 与后端无关的可写纹理：**源（视频/emote）持有并 update() 写整帧，
/// 层持有同一个句柄，渲染线程在绘制前 flush() 一次**。update 任意线程可调
/// （只暂存），flush 才触碰后端 —— 后端纹理/绘制永远只在渲染线程发生。
class HostTexture {
public:
    HostTexture(RenderBackend* backend, int w, int h)
        : backend_(backend), w_(w), h_(h) {}
    ~HostTexture() { release(); }
    /// 后端销毁前由渲染器统一调用：解绑后端句柄（对象仍可被层持有）。
    void release() {
        if (tex_ && backend_) backend_->destroy_texture(tex_);
        tex_ = nullptr;
        backend_ = nullptr;
    }
    HostTexture(const HostTexture&) = delete;
    HostTexture& operator=(const HostTexture&) = delete;

    int width() const { return w_.load(std::memory_order_relaxed); }
    int height() const { return h_.load(std::memory_order_relaxed); }

    /// 写整帧 RGBA（任意线程；只暂存，不碰后端）。
    bool update(int w, int h, const uint8_t* rgba) {
        if (w <= 0 || h <= 0 || !rgba) return false;
        std::lock_guard<std::mutex> lk(m_);
        pw_ = w;
        ph_ = h;
        pending_.assign(rgba, rgba + size_t(w) * size_t(h) * 4);
        has_pending_ = true;
        w_.store(w, std::memory_order_relaxed);
        h_.store(h, std::memory_order_relaxed);
        return true;
    }

    /// 渲染线程：把暂存帧上传（尺寸变化则重建纹理）。
    bool flush() {
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!has_pending_) return false;
            px.swap(pending_);
            w = pw_;
            h = ph_;
            has_pending_ = false;
        }
        if (!backend_) return false;
        if (!tex_ || tw_ != w || th_ != h) {
            if (tex_) backend_->destroy_texture(tex_);
            tex_ = backend_->create_texture(w, h, TextureAccess::Streaming);
            if (!tex_) return false;
            backend_->set_texture_blend(tex_, BlendMode::Blend);
            tw_ = w;
            th_ = h;
            w_.store(w, std::memory_order_relaxed);
            h_.store(h, std::memory_order_relaxed);
        }
        uint8_t* dst = nullptr;
        int pitch = 0;
        if (!backend_->lock_texture(tex_, &dst, &pitch)) return false;
        for (int y = 0; y < h; ++y)
            std::memcpy(dst + size_t(y) * size_t(pitch),
                        px.data() + size_t(y) * size_t(w) * 4, size_t(w) * 4);
        backend_->unlock_texture(tex_);
        return true;
    }

    /// 离屏合成目标（emote GPU 画布）：渲染线程调用，写入后 native() 可绘。
    TextureRef ensure_target(int w, int h) {
        if (!backend_ || w <= 0 || h <= 0) return nullptr;
        if (tex_ && tw_ == w && th_ == h && target_) return tex_;
        if (tex_) backend_->destroy_texture(tex_);
        tex_ = backend_->create_texture(w, h, TextureAccess::Target);
        if (!tex_) return nullptr;
        backend_->set_texture_blend(tex_, BlendMode::Blend);
        tw_ = w;
        th_ = h;
        w_.store(w, std::memory_order_relaxed);
        h_.store(h, std::memory_order_relaxed);
        target_ = true;
        return tex_;
    }

    TextureRef native() const { return tex_; }

private:
    RenderBackend* backend_ = nullptr;
    TextureRef tex_ = nullptr;
    int tw_ = 0, th_ = 0;
    bool target_ = false;
    std::atomic<int> w_{0}, h_{0};
    std::mutex m_;
    std::vector<uint8_t> pending_;
    int pw_ = 0, ph_ = 0;
    bool has_pending_ = false;
};
using HostTextureRef = std::shared_ptr<HostTexture>;

} // namespace oa::render
