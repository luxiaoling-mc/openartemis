#include "core/render/backend_sdl.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// SDL3 SDL_Render 后端实现：引擎全部 SDL_Render 调用的同构搬移。
// 每条方法与引擎原本的调用点一一对应（顺序/参数逐行等价）；
// SDL 类型只存在于本文件/本类内部。

namespace oa::render {

namespace {

SDL_BlendMode to_sdl_blend(BlendMode m) {
    switch (m) {
        case BlendMode::None: return SDL_BLENDMODE_NONE;
        case BlendMode::Add: return SDL_BLENDMODE_ADD;
        case BlendMode::Mod: return SDL_BLENDMODE_MOD;
        case BlendMode::AlphaMultiply:
            // dstRGB kept, dstA *= srcA: the stencil mask multiplies the
            // content's alpha channel (color ops pass destination through)
            return SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE,
                SDL_BLENDOPERATION_ADD,           // color: dst kept
                SDL_BLENDFACTOR_DST_ALPHA, SDL_BLENDFACTOR_ZERO,
                SDL_BLENDOPERATION_ADD);          // alpha: dstA *= srcA
        case BlendMode::Blend: break; // fallthrough to default
    }
    return SDL_BLENDMODE_BLEND;
}

SDL_TextureAccess to_sdl_access(TextureAccess a) {
    switch (a) {
        case TextureAccess::Streaming: return SDL_TEXTUREACCESS_STREAMING;
        case TextureAccess::Target: return SDL_TEXTUREACCESS_TARGET;
        case TextureAccess::Static: break;
    }
    return SDL_TEXTUREACCESS_STATIC;
}

/// Shared ReadPixels→RGBA32 copy（当前渲染 target → 紧排行 RGBA 向量）。
/// 行 pitch 总是处理；语义 = snapshot 面与 frame_luma/read_target_pixels
/// 各自内联的同一段代码。
bool read_current_target(SDL_Renderer* renderer, int* out_w, int* out_h,
                         std::vector<uint8_t>* out) {
    if (!renderer) return false;
    SDL_Surface* snap = SDL_RenderReadPixels(renderer, nullptr);
    if (!snap) return false;
    const bool needs_convert = snap->format != SDL_PIXELFORMAT_RGBA32;
    SDL_Surface* rgba =
        needs_convert ? SDL_ConvertSurface(snap, SDL_PIXELFORMAT_RGBA32) : snap;
    if (needs_convert) SDL_DestroySurface(snap);
    if (!rgba) return false;
    *out_w = rgba->w;
    *out_h = rgba->h;
    out->resize(size_t(rgba->w) * size_t(rgba->h) * 4);
    for (int y = 0; y < rgba->h; ++y) {
        const uint8_t* src = (const uint8_t*)rgba->pixels + size_t(y) * rgba->pitch;
        uint8_t* dst = out->data() + size_t(y) * size_t(rgba->w) * 4;
        std::copy(src, src + size_t(rgba->w) * 4, dst);
    }
    SDL_DestroySurface(rgba);
    return true;
}

} // namespace

SdlRenderBackend::~SdlRenderBackend()
{
    shutdown();
}

SDL_Texture* SdlRenderBackend::sdl_tex(TextureRef t) const
{
    return t ? static_cast<SdlTexture*>(t)->tex : nullptr;
}

SdlTexture* SdlRenderBackend::wrap(SDL_Texture* raw) const
{
    if (!raw) return nullptr;
    const auto it = registry_.find(raw);
    return it == registry_.end() ? nullptr : it->second;
}

bool SdlRenderBackend::create(SDL_Window* window, int stage_w, int stage_h,
                              BackendInfo* info)
{
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    renderer_ = SDL_CreateRenderer(window, nullptr);
    if (!renderer_) return false;
    // Logical presentation（注释见 renderer.cpp create_renderer）：
    // 引擎场景 = 项目 stage 尺寸；SDL3 把 stage 呈现到真实窗口
    // （letterbox 等比居中放大），绘制坐标恒为引擎坐标，窗口坐标鼠标事件
    // 经 SDL_RenderCoordinatesFromWindow 反算（HiDPI/scale 下命中不变）。
    if (!SDL_SetRenderLogicalPresentation(
            renderer_, stage_w, stage_h, SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        std::fprintf(stderr, "SDL_SetRenderLogicalPresentation failed: %s\n",
                     SDL_GetError());
    }
    if (info) {
        SDL_GetCurrentRenderOutputSize(renderer_, &info->output_w,
                                       &info->output_h);
        SDL_RendererLogicalPresentation lm = SDL_LOGICAL_PRESENTATION_DISABLED;
        SDL_GetRenderLogicalPresentation(renderer_, &info->logical_w,
                                         &info->logical_h, &lm);
        info->logical_mode = (int)lm;
    }
    return true;
}

void SdlRenderBackend::shutdown()
{
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
        registry_.clear();
    }
}

const char* SdlRenderBackend::last_error()
{
    return SDL_GetError();
}

TextureRef SdlRenderBackend::current_target()
{
    return wrap(SDL_GetRenderTarget(renderer_));
}

void SdlRenderBackend::set_target(TextureRef t)
{
    SDL_SetRenderTarget(renderer_, sdl_tex(t));
}

bool SdlRenderBackend::clip_enabled()
{
    return SDL_RenderClipEnabled(renderer_);
}

IRect SdlRenderBackend::clip_rect()
{
    SDL_Rect r{};
    SDL_GetRenderClipRect(renderer_, &r);
    return IRect{r.x, r.y, r.w, r.h};
}

void SdlRenderBackend::set_clip(const IRect& r)
{
    const SDL_Rect sdl_r{r.x, r.y, r.w, r.h};
    SDL_SetRenderClipRect(renderer_, &sdl_r);
}

void SdlRenderBackend::clear_clip()
{
    SDL_SetRenderClipRect(renderer_, nullptr);
}

void SdlRenderBackend::set_draw_blend(BlendMode m)
{
    SDL_SetRenderDrawBlendMode(renderer_, to_sdl_blend(m));
}

void SdlRenderBackend::set_draw_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    SDL_SetRenderDrawColor(renderer_, r, g, b, a);
}

void SdlRenderBackend::clear()
{
    SDL_RenderClear(renderer_);
}

void SdlRenderBackend::fill_rect(const FRect& dst)
{
    const SDL_FRect r{dst.x, dst.y, dst.w, dst.h};
    SDL_RenderFillRect(renderer_, &r);
}

void SdlRenderBackend::draw_texture(TextureRef t, const FRect* src,
                                    const FRect* dst)
{
    const SDL_FRect s_sdl = src ? SDL_FRect{src->x, src->y, src->w, src->h}
                                : SDL_FRect{0, 0, 0, 0};
    const SDL_FRect d_sdl = dst ? SDL_FRect{dst->x, dst->y, dst->w, dst->h}
                                : SDL_FRect{0, 0, 0, 0};
    SDL_RenderTexture(renderer_, sdl_tex(t), src ? &s_sdl : nullptr,
                      dst ? &d_sdl : nullptr);
}

void SdlRenderBackend::draw_texture_affine(TextureRef t, const FRect* src,
                                           const FPoint& o, const FPoint& r,
                                           const FPoint& d)
{
    const SDL_FRect s_sdl = src ? SDL_FRect{src->x, src->y, src->w, src->h}
                                : SDL_FRect{0, 0, 0, 0};
    const SDL_FPoint sdl_o{o.x, o.y}, sdl_r{r.x, r.y}, sdl_d{d.x, d.y};
    SDL_RenderTextureAffine(renderer_, sdl_tex(t), src ? &s_sdl : nullptr,
                            &sdl_o, &sdl_r, &sdl_d);
}

void SdlRenderBackend::draw_geometry(TextureRef t, const Vertex* verts,
                                     int nverts, const int* indices,
                                     int nindices)
{
    if (nverts <= 0 || !verts) return;
    std::vector<SDL_Vertex> sdl_verts;
    sdl_verts.reserve(size_t(nverts));
    for (int i = 0; i < nverts; ++i) {
        SDL_Vertex v;
        v.position = {verts[i].pos.x, verts[i].pos.y};
        v.color = {verts[i].color.r, verts[i].color.g, verts[i].color.b,
                   verts[i].color.a};
        v.tex_coord = {verts[i].uv.x, verts[i].uv.y};
        sdl_verts.push_back(v);
    }
    SDL_RenderGeometry(renderer_, sdl_tex(t), sdl_verts.data(), nverts,
                       indices, nindices);
}

void SdlRenderBackend::present()
{
    SDL_RenderPresent(renderer_);
}

TextureRef SdlRenderBackend::create_texture(int w, int h, TextureAccess access)
{
    SDL_Texture* tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                         to_sdl_access(access), w, h);
    if (!tex) return nullptr;
    SdlTexture* wrap_tex = new SdlTexture(tex);
    registry_[tex] = wrap_tex;
    return wrap_tex;
}

void SdlRenderBackend::destroy_texture(TextureRef t)
{
    if (!t) return;
    SdlTexture* wt = static_cast<SdlTexture*>(t);
    if (wt->tex) {
        registry_.erase(wt->tex);
        SDL_DestroyTexture(wt->tex);
    }
    delete wt;
}

void SdlRenderBackend::update_texture(TextureRef t, const uint8_t* rgba,
                                      int pitch)
{
    SDL_UpdateTexture(sdl_tex(t), nullptr, rgba, pitch);
}

bool SdlRenderBackend::lock_texture(TextureRef t, uint8_t** pixels, int* pitch)
{
    void* px = nullptr;
    if (!SDL_LockTexture(sdl_tex(t), nullptr, &px, pitch)) return false;
    *pixels = static_cast<uint8_t*>(px);
    return true;
}

void SdlRenderBackend::unlock_texture(TextureRef t)
{
    SDL_UnlockTexture(sdl_tex(t));
}

bool SdlRenderBackend::texture_size(TextureRef t, float* w, float* h)
{
    return SDL_GetTextureSize(sdl_tex(t), w, h);
}

void SdlRenderBackend::set_texture_blend(TextureRef t, BlendMode m)
{
    SDL_SetTextureBlendMode(sdl_tex(t), to_sdl_blend(m));
}

void SdlRenderBackend::set_texture_alpha_mod(TextureRef t, uint8_t a)
{
    SDL_SetTextureAlphaMod(sdl_tex(t), a);
}

void SdlRenderBackend::set_texture_color_mod(TextureRef t, uint8_t r, uint8_t g,
                                             uint8_t b)
{
    SDL_SetTextureColorMod(sdl_tex(t), r, g, b);
}

bool SdlRenderBackend::read_target(int* w, int* h, std::vector<uint8_t>* rgba)
{
    return read_current_target(renderer_, w, h, rgba);
}

bool SdlRenderBackend::window_to_render(float wx, float wy, float* rx,
                                        float* ry)
{
    return SDL_RenderCoordinatesFromWindow(renderer_, wx, wy, rx, ry);
}

bool SdlRenderBackend::render_to_window(float rx, float ry, float* wx,
                                        float* wy)
{
    return SDL_RenderCoordinatesToWindow(renderer_, rx, ry, wx, wy);
}

} // namespace oa::render
