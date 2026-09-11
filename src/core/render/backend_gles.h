#pragma once

// GlesRenderBackend：原生 OpenGL ES 后端的接口实现。
// 同一 RenderBackend 接口（backend.h）上的第二个实现，
// **不使用 SDL_Render** —— GLES 上下文（SDL_GL ES profile）由本后端在
// create() 内创建，之后全部原语用直接手写 GLES 调用实现：
//   纹理 = GL 纹理对象；离屏 target = FBO；quad/几何 = 顶点缓冲 +
//   GLSL ES 300 着色器（不做固定管线、不经 SDL）；blend/clip/读回 = GL 状态。
//
// 可观测语义基准 = SdlRenderBackend（对照基准，语义/行为零漂移）。为做到
// 逐像素一致，本实现镜像了本机 sdl 线实际生效的 SDL3 "opengl" 渲染驱动
// （GL2.1 compat + GLSL1.20）的渲染数学：直色 RGBA8（无 premultiply）、
// copy/fill 全走三角形几何、顶点色 = 纹理 color/alpha mod（fill = draw
// color）、片段色 = texture*vcolor、SDL3.4 FULL blend 模式位域分解成
// glBlendFuncSeparate、target 渲染 row0=内容顶（读回不翻行）、窗口渲染
// y 翻转到 letterbox dst 区（SDL_RenderReadPixels 语义同构）。
//
// 版本要求：GLES >= 3.0（shader #version 300 es）；创建失败 → create()
// false + last_error() 可读原因，宿主报错退出（不许静默降级到 sdl、
// 不许画错）。
//
// GL 头/库依赖为零：GLES3 入口点全部经 SDL_GL_GetProcAddress 加载
// （无 <GLES3/gl3.h> 依赖 → windows/android/linux 构建面一致）。

#include "core/render/backend.h"

#include <cstdint>
#include <string>
#include <vector>

namespace oa::render {

/// 后端持有的纹理对象。
struct GlesTexture : Texture {
    unsigned int tex = 0; // GL 纹理名
    unsigned int fbo = 0; // access==Target 时的 FBO 名（否则 0）
    int w = 0;
    int h = 0;
    TextureAccess access = TextureAccess::Static;
    std::vector<uint8_t> staging; // Streaming：CPU 填装缓冲（pitch=w*4）
    // 每纹理绘制状态（引擎"每次绘制点前先设"的依赖；与 SDL 纹理状态一致）
    BlendMode blend = BlendMode::Blend;
    uint8_t alpha_mod = 255;
    uint8_t color_mod[3] = {255, 255, 255};
};

class GlesRenderBackend : public RenderBackend {
public:
    ~GlesRenderBackend() override;

    bool create(SDL_Window* window, int stage_w, int stage_h,
                BackendInfo* info) override;
    void shutdown() override;
    const char* last_error() override;

    TextureRef current_target() override;
    void set_target(TextureRef t) override;
    bool clip_enabled() override;
    IRect clip_rect() override;
    void set_clip(const IRect& r) override;
    void clear_clip() override;

    void set_draw_blend(BlendMode m) override;
    void set_draw_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) override;
    void clear() override;
    void fill_rect(const FRect& dst) override;
    void draw_texture(TextureRef t, const FRect* src, const FRect* dst) override;
    void draw_texture_affine(TextureRef t, const FRect* src, const FPoint& o,
                             const FPoint& r, const FPoint& d) override;
    void draw_geometry(TextureRef t, const Vertex* verts, int nverts,
                       const int* indices, int nindices) override;
    bool draw_rule_transition(TextureRef capture, TextureRef rule,
                              float progress, float band) override;
    void present() override;

    TextureRef create_texture(int w, int h, TextureAccess access) override;
    void destroy_texture(TextureRef t) override;
    void update_texture(TextureRef t, const uint8_t* rgba, int pitch) override;
    bool lock_texture(TextureRef t, uint8_t** pixels, int* pitch) override;
    void unlock_texture(TextureRef t) override;
    bool texture_size(TextureRef t, float* w, float* h) override;
    void set_texture_blend(TextureRef t, BlendMode m) override;
    void set_texture_alpha_mod(TextureRef t, uint8_t a) override;
    void set_texture_color_mod(TextureRef t, uint8_t r, uint8_t g,
                               uint8_t b) override;

    bool read_target(int* w, int* h, std::vector<uint8_t>* rgba) override;

    bool window_to_render(float wx, float wy, float* rx, float* ry) override;
    bool render_to_window(float rx, float ry, float* wx, float* wy) override;

private:
    void fail(const char* fmt, ...); // 记录 last_error 文本（+ SDL_SetError）
    void apply_draw_state(GlesTexture* tex, BlendMode blend);
    void emit_quad(const float x0, const float y0, const float x1,
                   const float y1, const float u0, const float v0,
                   const float u1, const float v1, const float col[4]);
    void ensure_window_ready(); // 刷新输出尺寸/letterbox（目标=窗口时）
    bool ensure_program();
    bool ensure_rule_program(); // type-2 rule 溶解专用 program（懒建）

    // ---- GL 对象 ----
    SDL_Window* window_ = nullptr;
    void* gl_context_ = nullptr;
    unsigned int program_ = 0;
    // rule 溶解 program（同主 VS，专属 FS；双纹理 + progress/vague uniforms）
    unsigned int rule_program_ = 0;
    int rule_loc_scale_ = -1, rule_loc_off_ = -1;
    int rule_loc_progress_ = -1, rule_loc_band_ = -1;
    int rule_loc_utex_ = -1, rule_loc_urule_ = -1;
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0; // 动态顶点缓冲（8 float/顶点）
    int loc_scale_ = -1, loc_off_ = -1, loc_usetex_ = -1;
    // apply_draw_state 最后写入的像素→NDC 系数（rule program 复用同一数学）
    float last_scale_[2] = {1.0f, 1.0f};
    float last_off_[2] = {0.0f, 0.0f};

    // ---- 状态机 ----
    GlesTexture* cur_target_ = nullptr; // nullptr = 窗口 backbuffer
    bool clip_enabled_ = false;
    IRect clip_{};
    BlendMode draw_blend_ = BlendMode::Blend;
    uint8_t draw_color_[4] = {0, 0, 0, 255};
    std::string err_; // last_error 文本
    bool created_ = false;

    // ---- 逻辑呈现（窗口目标；SDL3 UpdateLogicalPresentation 数学镜像）----
    int logical_w_ = 0, logical_h_ = 0;   // stage（引擎逻辑尺寸）
    int out_w_ = 0, out_h_ = 0;           // 窗口像素（drawable）尺寸
    float dst_x_ = 0, dst_y_ = 0;         // letterbox 内容区（输出像素，顶部原点）
    float dst_w_ = 0, dst_h_ = 0;
    float cur_scale_x_ = 1.0f, cur_scale_y_ = 1.0f; // 逻辑→像素
    float dpi_x_ = 1.0f, dpi_y_ = 1.0f;             // 窗口坐标→像素

    GlesTexture* gles_tex(TextureRef t) const {
        return t ? static_cast<GlesTexture*>(t) : nullptr;
    }
};

} // namespace oa::render
