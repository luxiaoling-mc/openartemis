#pragma once

// SdlRenderBackend：SDL3 SDL_Render 后端的接口实现。
// 引擎的全部 SDL_Render 调用按原语逐条迁入这里，逻辑/顺序/参数逐行
// 等价——本文件是"引擎渲染的 sdl 后端同构搬移"，只做搬移不加第二后端；
// SDL 类型全部留在本实现内部。

#include "core/render/backend.h"

#include <unordered_map>

// SDL3 渲染对象 opaque 句柄的 tag+typedef 前向声明（同 backend.h 的
// SDL_Window 做法；SDL_render.h 里的同名 typedef 指向同一类型，重定义合法）。
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;

namespace oa::render {

/// 后端持有的纹理对象（每个 SDL_Texture 包一层）。
struct SdlTexture : Texture {
    explicit SdlTexture(SDL_Texture* t) : tex(t) {}
    SDL_Texture* tex = nullptr;
};

class SdlRenderBackend : public RenderBackend {
public:
    ~SdlRenderBackend() override;

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
    SDL_Renderer* renderer_ = nullptr;
    /// SDL_Texture* → 包装对象（current_target 需要把 SDL 原始指针还原成
    /// 引擎持有的 TextureRef 同一身份，比较/恢复 target 才成立）。
    std::unordered_map<SDL_Texture*, SdlTexture*> registry_;
    SDL_Texture* sdl_tex(TextureRef t) const;
    SdlTexture* wrap(SDL_Texture* raw) const;
};

} // namespace oa::render
