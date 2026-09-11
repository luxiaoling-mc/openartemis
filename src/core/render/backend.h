#pragma once

// RenderEngine 渲染后端接口。
//
// 本头文件是 RenderEngine（引擎场景绘制/合成逻辑）与"把像素真正画到
// target/window"的后端之间的唯一接缝，**不含任何 SDL_render 类型**：
//   - SDL_Texture / SDL_Renderer / SDL_Surface / SDL_BlendMode / SDL_Vertex
//     …全部留在 sdl 实现（backend_sdl.cpp）内部；
//   - 后端原语集合 = RenderEngine 真实触碰的 SDL 调用面的最小投影
//     （纹理创建/上传/缓存、quad/几何绘制、target 会话、clip、读回、
//     坐标换算、present/clear），形状贴近引擎的调用序列，保证
//     SdlRenderBackend 与引擎既有 SDL 调用是"同构搬移"
//     （顺序/参数逐行等价 → 逐像素等价由构造保证，窗口旅程交叉验证实证）。
//   - GLES 后端在同一接口上加第二个实现：原语全部映射得动
//     （每原语都是独立调用，无 SDL 状态机泄漏）。
//
// 说明：SDL_Window 只是宿主窗口句柄（SDL_video 的不透明类型，前向声明即
// 可——两个后端都建立在 SDL3 窗口之上）；窗口创建本身仍在 app 宿主，
// 不属渲染线抽象范围。

#include <cstdint>
#include <memory>
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
enum class BlendMode { None, Blend, Add, Mod };

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
    /// 整幅）画到当前 target 上，逐像素覆盖率
    ///   keep = smoothstep(t, t+band, rule.r)
    ///   t    = progress*(1+band) - band
    ///   band = max(vague,1)/255          （vague 0-255 尺度、缺省 32）
    /// 输出 = capture 的 rgb、alpha = cap.a*keep（标准 blend 叠到场景上）；
    /// rule 纹理按整幅舞台 UV 拉伸采样（灰度取 R 通道）。端点连续：
    /// progress=0 → keep 全 1（只见旧帧），progress=1 → keep 全 0（只见新
    /// 场景）；rule 灰度低的像素先揭示。仅逐像素 shader 后端可实现（GLES）；
    /// 无能力后端默认返回 false，引擎保持 type-1 交叉淡化回退。
    virtual bool draw_rule_transition(TextureRef capture, TextureRef rule,
                                      float progress, float band) {
        (void)capture;
        (void)rule;
        (void)progress;
        (void)band;
        return false;
    }
    virtual void present() = 0;

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

} // namespace oa::render
