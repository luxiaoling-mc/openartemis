#include "core/render/renderer.h"
#include "core/render/render_internal.h"
#include "core/runtime/runtime_game.h" // compatibility adapter ctor
#include "core/render/backend_gles.h"
#include "core/render/backend_sdl.h"
#include "core/emote/emote_file.h"
#include "core/emote/emote_player.h"
#include "core/media/video.h" // rgba_luma / layer_video_key_alpha (keyed upload)
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include <SDL3/SDL_mouse.h> // warp_mouse_to_stage
#include <SDL3/SDL_video.h> // create_renderer 诊断打印的窗口尺寸查询

// 本文件不直接触碰 SDL_Render；所有绘制
// 原语经 RenderBackend 接口（sdl 实现 backend_sdl.cpp = 引擎原本调用面的
// 同构搬移；gles 实现 backend_gles.cpp = 原生 GLES 线）。除类型层替换与
// create_renderer 的后端名参数外，逻辑/顺序/参数与引擎原本逐行等价。

namespace oa::render {

RenderEngine::RenderEngine()
    : transition_owned_(std::make_unique<oa::render::Transition>()),
      transition_(transition_owned_.get()) {}

RenderEngine::RenderEngine(const oa::fs::IFileSystem* fs, oa::render::RenderHost* host,
                           bool bind_hooks)
    : RenderEngine()
{
    configure(fs, host, &text_, &scene_, bind_hooks);
}

RenderEngine::RenderEngine(const oa::fs::IFileSystem* fs, oa::runtime::GameRuntime* rt,
                           bool bind_hooks)
    : RenderEngine()
{
    configure(fs, rt, &rt->text(), &rt->scene(), bind_hooks);
    transition_ = &rt->transition_state();
}

void RenderEngine::configure(const oa::fs::IFileSystem* fs, oa::render::RenderHost* host,
                             oa::render::TextEngine* text,
                             oa::render::Compositor* scene, bool bind_hooks)
{
    fs_ = fs;
    host_ = host;
    text_ptr_ = text;
    scene_ptr_ = scene;
    fontSystem = std::make_unique<oa::render::FontSystem>(fs, host, text_ptr_);
    if (bind_hooks) bind_runtime_hooks();
}

void RenderEngine::bind_runtime_hooks()
{
    if (!host_) return;
    host_->bind_text_metrics([this] { return text_metrics(); });
}

std::tuple<double, double, double> RenderEngine::text_metrics()
{
    const auto& t = text();
    const std::string aid = t.active_layer_id();
    const oa::render::MessageLayer* ml = t.layer(aid);
    if (!ml || !fontSystem) return {0.0, 0.0, 0.0};
    const auto ext = fontSystem->measure_text_extents(*ml);
    return {ext.width, ext.height, ext.line_width};
}

RenderEngine::~RenderEngine()
{
    // 成员析构序 = 声明逆序：fontSystem 先于 backend_ 销毁（字形纹理销毁时
    // 后端对象仍存活，见 renderer.h 成员注释）。
}

bool RenderEngine::create_renderer(void* native_window,
                                   const std::string& backend_kind)
{
    SDL_Window* ctx = static_cast<SDL_Window*>(native_window);
    window_ = ctx;
    // Logical presentation（注释沿 create_renderer 的后端呈现约定）：引擎
    if (host_) {
        const int w = host_->stage_width();
        const int h = host_->stage_height();
        if (w > 0 && h > 0) {
            stage_w_ = w;
            stage_h_ = h;
        }
    }
    // 后端接入点（唯一）：--renderer / OA_RENDERER 选线；缺省 = sdl（与
    // HEAD 行为逐位一致）。gles 不可用（无 GLES 上下文等）→ create false +
    // last_error，宿主报错退出——不静默降级。
    const std::string kind = backend_kind.empty() ? "sdl" : backend_kind;
    if (kind == "gles") {
        backend_ = std::make_unique<GlesRenderBackend>();
    } else if (kind == "sdl") {
        backend_ = std::make_unique<SdlRenderBackend>();
    } else {
        std::fprintf(stderr,
                     "openartemis: unknown render backend '%s' (sdl|gles)\n",
                     kind.c_str());
        return false;
    }
    BackendInfo info;
    if (!backend_->create(ctx, stage_w_, stage_h_, &info)) {
        // 失败时渲染器保持 null（renderer_ok()=false）；等价复位。
        backend_.reset();
        return false;
    }
    fontSystem->set_backend(backend_.get());
    // The fixed stage offscreen target. All scene drawing
    stage_rt = backend_->create_texture(stage_w_, stage_h_,
                                        TextureAccess::Target);
    if (stage_rt) {
        backend_->set_texture_blend(stage_rt, BlendMode::Blend);
        stage_frame_valid_ = false;
        std::printf("[app] stage target %dx%d (offscreen)\n", stage_w_, stage_h_);
    } else {
        std::fprintf(stderr, "[app] no stage render target (%s); legacy "
            "direct-to-window present\n", backend_->last_error());
    }
    {
        int ww = 0, wh = 0, wpw = 0, wph = 0;
        SDL_GetWindowSize(ctx, &ww, &wh);
        SDL_GetWindowSizeInPixels(ctx, &wpw, &wph);
        std::printf("[app] window=%dx%d pixels=%dx%d render_out=%dx%d "
            "logical=%dx%d mode=%d\n",
            ww, wh, wpw, wph, info.output_w, info.output_h, info.logical_w,
            info.logical_h, info.logical_mode);
    }
    vsync_active_ = backend_->set_vsync(true);
    return true;
}

void RenderEngine::release_stencil_scratch()
{
    if (!backend_) return;
    for (auto& t : stencil_scratch_) {
        if (t) backend_->destroy_texture(t);
        t = nullptr;
    }
    stencil_scratch_w_ = stencil_scratch_h_ = 0;
}

void RenderEngine::release_all()
{
    if (!backend_) return;
    // 源创建的宿主纹理可能仍被层持有：先把后端句柄统一释放，避免后端
    // 销毁后 HostTexture 析构访问悬空 backend。
    for (auto& w : host_textures_)
        if (auto t = w.lock()) t->release();
    host_textures_.clear();
    release_stencil_scratch();
    if (stage_rt) {
        backend_->destroy_texture(stage_rt);
        stage_rt = nullptr;
    }
    if (trans_capture_tex) {
        backend_->destroy_texture(trans_capture_tex);
        trans_capture_tex = nullptr;
    }
    for (auto& [k, t] : textures)
        if (t) backend_->destroy_texture(t);
    // canvas textures are also listed in `textures` (destroyed above);
    // the atlas textures are owned by emote_atlases_ alone.
    for (auto& [k, t] : emote_atlases_)
        if (t) backend_->destroy_texture(t);
    // 1x1 solid textures + intermediate-group bakes were torn down implicitly
    // at process exit before the abstraction; destroy them explicitly here so
    // every backend texture dies before the renderer does (同销毁集，仅提前).
    for (auto& [k, t] : solid_tex_cache)
        if (t) backend_->destroy_texture(t);
    solid_tex_cache.clear();
    emote_atlases_.clear();
    textures.clear();
    trans_capture_tex = nullptr;
    trans_rule_tex = nullptr; // rule texture lived in `textures` (destroyed above)
    trans_rule_checked_ = false;
    backend_->shutdown();
    window_ = nullptr;
    dirty_ = true;
    rendered_any_ = false;
    frame_presented_ = false;
    vsync_active_ = false;
    prev_present_ms_ = 0;
    present_ema_ms_ = -1.0;
    present_samples_ = 0;
}

bool RenderEngine::capture_transition_source()
{
    // GPU-only [trans] capture on the stage-target path.
    if (trans_capture_tex) {
        backend_->destroy_texture(trans_capture_tex);
        trans_capture_tex = nullptr;
    }
    // per-transition rule-dissolve state reset (type-2 resolve happens once
    // in progress_transition; the rule texture itself stays cached).
    trans_rule_tex = nullptr;
    trans_rule_checked_ = false;
    // 运动图形限制: 画面里有"运动图形"(有效可见的带内容层上有运行中的
    // tween/[anime]) 时, 本 [trans] 不生成 overlay — 整帧 overlay 会把运动
    // 中的层冻出残影 (标题立绘/logo 入场段的"回退重影"), 运动期间淡化按
    // 瞬时切换处理. 语义机照常计时: 脚本仍停在 [trans] 等待上 (段落节奏
    // 不变), 只是没有视觉淡化. type=0 不经此路径.
    {
        bool motion = false;
        const oa::render::Compositor& sc = scene();
        for (const oa::render::Layer* l : sc.draw_order()) {
            if (motion) break;
            if (!sc.is_effectively_visible(l->id)) continue;
            // 空容器不算内容(原 file 空 &&
            // !has_color 跳过逐字;角色 content_present 取反)。
            if (!oa::render::content_present(*l)) continue;
            if (!sc.layer_tweens_finished(l->id) || sc.layer_anime_active(l->id))
                motion = true;
        }
        if (motion)
            return true;
    }
    if (backend_ && stage_rt && stage_frame_valid_) {
        TextureRef tex = backend_->create_texture(stage_w_, stage_h_,
                                                  TextureAccess::Target);
        if (!tex) return false;
        backend_->set_texture_blend(tex, BlendMode::Blend);
        backend_->set_target(tex);
        backend_->clear_clip();
        const FRect full{0.0f, 0.0f, float(stage_w_), float(stage_h_)};
        backend_->draw_texture(stage_rt, nullptr, &full);
        backend_->set_target(stage_rt);
        trans_capture_tex = tex;
        trans_capture_w = stage_w_;
        trans_capture_h = stage_h_;
        return true;
    }
    // legacy no-RT fallback: CPU snapshot of the current target
    oa::media::Image img;
    if (!snapshot_renderer(img) || img.w <= 0 || img.h <= 0) return false;
    trans_capture_tex = make_texture(img);
    trans_capture_w = img.w;
    trans_capture_h = img.h;
    return trans_capture_tex != nullptr;
}

bool RenderEngine::snapshot_renderer(oa::media::Image& out)
{
    if (!backend_) return false;
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    if (!backend_->read_target(&w, &h, &px)) return false;
    out.w = w;
    out.h = h;
    out.rgba = std::move(px);
    return true;
}

TextureRef RenderEngine::make_texture(const oa::media::Image& img)
{
    TextureRef tex = backend_->create_texture(img.w, img.h,
                                              TextureAccess::Static);
    if (!tex) return nullptr;
    backend_->update_texture(tex, img.rgba.data(), img.w * 4);
    backend_->set_texture_blend(tex, BlendMode::Blend);
    return tex;
}

bool RenderEngine::get_renderer_coordinates(float winx, float winy, float* rx, float* ry)
{
    return backend_ && backend_->window_to_render(winx, winy, rx, ry);
}

bool RenderEngine::stage_to_window_coordinates(float sx, float sy, float* wx, float* wy)
{
    return backend_ && backend_->render_to_window(sx, sy, wx, wy);
}

bool RenderEngine::warp_mouse_to_stage(float sx, float sy)
{
    if (!window_) return false;
    float wx = 0.0f, wy = 0.0f;
    if (!stage_to_window_coordinates(sx, sy, &wx, &wy)) return false;
    SDL_WarpMouseInWindow(window_, wx, wy);
    return true;
}

std::optional<std::vector<uint8_t>> RenderEngine::overlay_read(const std::string& rel)
{
    return host_ ? host_->read_save_file(rel) : std::nullopt;
}

// ---------------------------------------------------------------------------
// 渲染流水线：层树 ── build_render_plan（本文件末段）──► 帧计划
//           帧计划 ── rasterize_plan（本文件）──► backend 调用
// 本文件不再遍历层树、不读层 props、不决定绘制顺序：它只按序消费计划。
// ---------------------------------------------------------------------------

void RenderEngine::build_plan(const oa::render::Compositor& sc,
                              double global_fade) {
    // 本帧脏报告的唯一消费者：取走（有界保护不参与语义），计划是否重算由
    // 层容器版本号决定。
    const uint64_t scene_rev = sc.revision();
    const uint64_t text_rev = text_ptr_ ? text_ptr_->revision() : 0;
    // OA_PLAN_REUSE=0：关掉计划复用（每帧重建），用于定位"复用门漏信号"类问题。
    static const bool reuse_enabled = [] {
        const char* v = std::getenv("OA_PLAN_REUSE");
        return !(v && *v == '0');
    }();
    if (reuse_enabled && plan_valid_ && plan_scene_ == &sc &&
        scene_rev == plan_scene_rev_ &&
        text_rev == plan_text_rev_ && global_fade == plan_fade_ &&
        stage_w_ == plan_stage_w_ && stage_h_ == plan_stage_h_) {
        return;
    }
    oa::render::PlanInput in;
    in.scene = &sc;
    in.text = text_ptr_;
    in.sizes = this; // 解码保证 + 内容自然尺寸
    in.stage_w = stage_w_;
    in.stage_h = stage_h_;
    in.global_fade = global_fade;
    oa::render::build_render_plan(in, &plan_);
    plan_valid_ = true;
    plan_scene_ = &sc;
    plan_scene_rev_ = scene_rev;
    plan_text_rev_ = text_rev;
    plan_fade_ = global_fade;
    plan_stage_w_ = stage_w_;
    plan_stage_h_ = stage_h_;

}

oa::render::HostTextureRef RenderEngine::create_host_texture(int w, int h) {
    if (!backend_ || w <= 0 || h <= 0) return nullptr;
    // 登记（弱引用）：后端销毁前统一 release；顺带清掉已死的条目。
    for (auto it = host_textures_.begin(); it != host_textures_.end();) {
        if (it->expired()) it = host_textures_.erase(it);
        else ++it;
    }
    auto tex = std::make_shared<oa::render::HostTexture>(backend_.get(), w, h);
    host_textures_.push_back(tex);
    return tex;
}


bool RenderEngine::compose_canvas(const std::string& layer_id,
                                  const oa::render::HostTextureRef& target,
                                  const oa::emote::EmoteFile& file,
                                  const std::vector<oa::emote::EmoteDrawPart>& parts,
                                  int w, int h) {
    if (!emote_render_parts(layer_id, target, file, parts, w, h)) return false;
    dirty_ = true; // 目标纹理变了：本帧重绘
    return true;
}

void RenderEngine::sweep_host_frames() {
    // 宿主纹理随层持有（shared_ptr），这里只回收 emote 图集缓存。
    for (auto it = emote_atlases_.begin(); it != emote_atlases_.end();) {
        if (!scene().find(it->first.layer)) {
            if (it->second) backend_->destroy_texture(it->second);
            it = emote_atlases_.erase(it);
        } else {
            ++it;
        }
    }
}



size_t RenderEngine::draw_scene(const oa::render::Compositor& sc,
                                double global_fade) {
    if (!backend_) return 0;
    build_plan(sc, global_fade);
    return rasterize_plan(plan_);
}

namespace {

/// 栅格化作用域（栈式）。这是后端状态机的一部分，不属于帧计划。
struct RasterScope {
    TextureRef saved_target = nullptr; // 进离屏前的 target
    TextureRef group_tex = nullptr;    // 本次合成的离屏目标
    bool had_clip = false;
    IRect old_clip{};
    bool fallback = false; // 无离屏目标：内联 + 组 alpha 回退
    double alpha_scale = 1.0;
    double scale_before = 1.0;
    const oa::render::PlanItem* begin = nullptr;
};

} // namespace

size_t RenderEngine::rasterize_plan(const oa::render::RenderPlan& plan) {
    if (!backend_) return 0;
    // 组 = 计划里一段连续绘制项。碰到就开离屏、画完立刻合成回父目标：
    // 每帧/每次绘制都是一次完整的组渲染，没有跨帧缓存，也就不存在
    // "mask 晚一帧才生效"的刷新时机问题。
    size_t drawn = 0;
    std::vector<RasterScope> stack;
    double alpha_scale = 1.0; // 无离屏回退时的累积组 alpha

    for (const oa::render::PlanItem& it : plan.items()) {
        switch (it.op) {
            case oa::render::PlanOp::GroupBegin: {
                RasterScope s;
                s.begin = &it;
                s.had_clip = backend_->clip_enabled();
                if (s.had_clip) s.old_clip = backend_->clip_rect();
                s.group_tex = backend_->create_texture(stage_w_, stage_h_,
                                                       TextureAccess::Target);
                if (!s.group_tex) {
                    // 无离屏能力：子树内联绘制，组 alpha 乘进去（旧回退同像素）。
                    s.fallback = true;
                    s.scale_before = alpha_scale;
                    s.alpha_scale = it.composite.alpha;
                    alpha_scale *= s.alpha_scale;
                    stack.push_back(s);
                    break;
                }
                s.saved_target = begin_offscreen_pass(s.group_tex, it.composite);
                stack.push_back(s);
                break;
            }
            case oa::render::PlanOp::GroupEnd: {
                RasterScope s = stack.back();
                stack.pop_back();
                if (s.fallback) {
                    alpha_scale = s.scale_before;
                    break;
                }
                TextureRef baked = bake_group(*s.begin, s.group_tex,
                                              s.saved_target, s.had_clip,
                                              s.old_clip);
                if (baked) {
                    composite_group_bake(baked, s.begin->composite, s.had_clip,
                                         s.old_clip);
                    backend_->destroy_texture(baked); // 一次性的合成结果
                }
                break;
            }
            case oa::render::PlanOp::Content:
                drawn += rasterize_content(it, alpha_scale);
                break;
            case oa::render::PlanOp::Glyphs:
                drawn += rasterize_glyphs(it, alpha_scale);
                break;
        }
    }
    return drawn;
}

size_t RenderEngine::rasterize_content(const oa::render::PlanItem& it,
                                       double scale) {
    const double op = it.opacity * scale;
    const oa::render::Affine2& t = it.world;
    if (it.solid) {
        // lyc solid-color quad (file-less) sized by width/height
        if (t.is_plain_translation()) {
            backend_->set_draw_blend(BlendMode::Blend);
            backend_->set_draw_color(it.solid_rgba[0], it.solid_rgba[1],
                                     it.solid_rgba[2],
                                     alpha_byte(it.solid_rgba[3] / 255.0 * op));
            const FRect dst{float(t.e), float(t.f), float(it.w), float(it.h)};
            backend_->fill_rect(dst);
        } else {
            TextureRef stex = solid_texture(it.solid_rgba);
            if (!stex) return 0;
            backend_->set_texture_alpha_mod(stex,
                                            alpha_byte(it.solid_rgba[3] / 255.0 * op));
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            t.transform_point(0, 0, &x0, &y0);
            t.transform_point(it.w, 0, &x1, &y1);
            t.transform_point(0, it.h, &x2, &y2);
            const FPoint o{float(x0), float(y0)}, r{float(x1), float(y1)},
                dn{float(x2), float(y2)};
            backend_->draw_texture_affine(stex, nullptr, o, r, dn);
        }
        return 1;
    }
    TextureRef tex = nullptr;
    if (it.host) {
        const oa::render::Layer* l = scene().find(it.id);
        if (!l || !l->texture) return 0;
        l->texture->flush(); // 渲染线程：把源 update 的帧上传
        tex = l->texture->native();
    } else {
        tex = it.mask.empty() ? texture_for_key(it.texture)
                              : texture_for_masked_key(it.texture, it.mask);
    }
    if (!tex) return 0;
    backend_->set_texture_alpha_mod(tex, alpha_byte(op));
    // lyprop colormultiply (research: thyt 消息窗调色): 纹理可能复用 ⇒ 每次显式复位。
    if (it.has_color_multiply) {
        backend_->set_texture_color_mod(tex, it.color_multiply[0],
                                        it.color_multiply[1],
                                        it.color_multiply[2]);
    } else {
        backend_->set_texture_color_mod(tex, 255, 255, 255);
    }
    const FRect* src = it.has_src ? &it.src : nullptr;
    if (t.is_plain_translation()) {
        // axis-aligned unit-scale: exact legacy dst-rect path keeps the
        // pixel baselines stable
        const FRect dst{float(t.e), float(t.f), float(it.w), float(it.h)};
        backend_->draw_texture(tex, src, &dst);
    } else {
        // general affine (rotate / non-uniform or negative scale)
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        t.transform_point(0, 0, &x0, &y0);
        t.transform_point(it.w, 0, &x1, &y1);
        t.transform_point(0, it.h, &x2, &y2);
        const FPoint o{float(x0), float(y0)}, r{float(x1), float(y1)},
            dn{float(x2), float(y2)};
        backend_->draw_texture_affine(tex, src, o, r, dn);
    }
    return 1;
}

size_t RenderEngine::rasterize_glyphs(const oa::render::PlanItem& it,
                                      double scale) {
    if (!backend_ || !fontSystem || it.messages.empty()) return 0;
    size_t drawn = 0;
    for (const std::string& id : it.messages) {
        const oa::render::MessageLayer* ml = text().layer(id);
        if (!ml) continue;
        drawn += fontSystem->draw_text_layer(backend_.get(), *ml, it.world,
                                             it.opacity * scale);
    }
    return drawn;
}

TextureRef RenderEngine::bake_group(const oa::render::PlanItem& begin,
                                    TextureRef group_tex, TextureRef saved_target,
                                    bool had_clip, const IRect& old_clip) {
    // 掩码解不出来 ⇒ 这一组这一帧不画（少一层 mask 会把整组按生图糊在
    // 画面上）。下一帧还会重来，资源修好就正常。
    if (begin.composite.has_mask &&
        resolve_image(begin.composite.mask) == nullptr) {
        end_offscreen_pass(saved_target, had_clip, old_clip);
        backend_->destroy_texture(group_tex);
        if (++mask_miss_run_ < 240) request_repaint();
        return nullptr;
    }
    mask_miss_run_ = 0;
    int gw = 0, gh = 0;
    std::vector<uint8_t> px;
    if (!backend_->read_target(&gw, &gh, &px) || gw != stage_w_ ||
        gh != stage_h_) {
        // failure / size mismatch → all-zero stage buffer（后端抽象前同语义）
        px.assign(size_t(stage_w_) * stage_h_ * 4, 0);
    }
    // CPU composite → straight RGBA (bake content only; alpha at draw time)
    std::vector<uint8_t> out_px(size_t(stage_w_) * stage_h_ * 4, 0);
    double gox = 0, goy = 0;
    begin.world.transform_point(0, 0, &gox, &goy);
    group_composite_cpu(px, stage_w_, stage_h_, begin.composite, &out_px,
                        int(std::floor(gox)), int(std::floor(goy)));
    end_offscreen_pass(saved_target, had_clip, old_clip);
    TextureRef baked = upload_baked_texture(out_px, stage_w_, stage_h_);
    backend_->destroy_texture(group_tex);
    return baked; // 调用方画完即毁
}

void RenderEngine::restore_render_clip(bool had_clip, const IRect& old_clip) {
    if (had_clip) {
        backend_->set_clip(old_clip);
    } else {
        backend_->clear_clip();
    }
}

void RenderEngine::composite_group_bake(TextureRef baked,
                                        const oa::render::PlanComposite& p,
                                        bool had_clip,
                                        const IRect& old_clip) {
    // The bake is cached until a scene mutation / active animation
    // invalidated it: message-window / mask groups are typically static while
    // displayed, so only one CPU composite pass is needed per content change.
    backend_->set_texture_alpha_mod(baked, alpha_byte(p.alpha));
    backend_->set_texture_blend(baked, p.blend);
    if (p.clip.has && p.clip.rect.w > 0 && p.clip.rect.h > 0) {
        backend_->set_clip(p.clip.rect);
    }
    const FRect full{0, 0, float(stage_w_), float(stage_h_)};
    backend_->draw_texture(baked, nullptr, &full);
    restore_render_clip(had_clip, old_clip);
}

TextureRef RenderEngine::begin_offscreen_pass(
    TextureRef target, const oa::render::PlanComposite& p) {
    backend_->set_texture_blend(target, BlendMode::Blend);
    TextureRef saved_target = backend_->current_target();
    backend_->set_target(target);
    backend_->set_draw_blend(BlendMode::None);
    backend_->set_draw_color(0, 0, 0, 0);
    backend_->clear();
    backend_->set_draw_blend(BlendMode::Blend);
    if (p.clip.has && p.clip.rect.w > 0 && p.clip.rect.h > 0) {
        backend_->set_clip(p.clip.rect);
    } else {
        backend_->clear_clip();
    }
    return saved_target;
}

void RenderEngine::end_offscreen_pass(TextureRef saved_target, bool had_clip,
                                      const IRect& old_clip) {
    backend_->set_target(saved_target);
    restore_render_clip(had_clip, old_clip);
}

void RenderEngine::group_composite_cpu(const std::vector<uint8_t>& px,
                                       int stage_w, int stage_h,
                                       const oa::render::PlanComposite& p,
                                       std::vector<uint8_t>* out_px, int origin_x,
                                       int origin_y) {
    // CPU composite: un-premultiply, filters/mask, straight RGBA.
    // Empty bake pixels stay alpha 0 (never opaque black).
    const oa::media::Image* mask = p.has_mask ? resolve_image(p.mask) : nullptr;
    // 掩码的取值面按图内容判定（官方 mask 有三种常见形态）：
    //   1) 带 alpha 的图          → 用 alpha 当覆盖率；
    //   2) 有内容的灰度/彩色图    → 用亮度（黑底白带，边缘柔化）；
    //   3) 全黑占位图（无 alpha） → 只把 mask 的矩形当裁剪窗（1 = 保留）。
    bool mask_alpha = false;   // 有真 alpha
    bool mask_luma = false;    // 有亮度内容
    if (mask) {
        mask_alpha = false;
        mask_luma = false;
        for (size_t i = 0; i < mask->rgba.size(); i += 4) {
            if (mask->rgba[i + 3] != 255) { mask_alpha = true; break; }
            if (mask->rgba[i] || mask->rgba[i + 1] || mask->rgba[i + 2]) {
                mask_luma = true;
                break;
            }
        }
        if (mask_alpha) mask_luma = false;
    }
    // 落位：mask 画在组的中间渲染缓冲上（组画布原点），保持自然尺寸。
    // 矩形之外一律裁掉 —— mask 即裁剪窗。
    const int mask_x0 = origin_x;
    const int mask_y0 = origin_y;
    const int mask_w = mask ? mask->w : 0;
    const int mask_h = mask ? mask->h : 0;
    for (int y = 0; y < stage_h; ++y) {
        for (int x = 0; x < stage_w; ++x) {
            const size_t i = (size_t(y) * stage_w + size_t(x)) * 4;
            const double A = px[i + 3] / 255.0;
            double r = 0, g = 0, b = 0;
            if (A > 0.0) {
                r = px[i] / (A * 255.0);
                g = px[i + 1] / (A * 255.0);
                b = px[i + 2] / (A * 255.0);
            }
            r *= p.color_multiply[0];
            g *= p.color_multiply[1];
            b *= p.color_multiply[2];
            if (p.grayscale) {
                const double luma = 0.299 * r + 0.587 * g + 0.114 * b;
                r = g = b = luma;
            }
            if (p.negative) {
                r = 1.0 - r;
                g = 1.0 - g;
                b = 1.0 - b;
            }
            double maskA = 1.0;
            if (mask) {
                const int sx = x - mask_x0;
                const int sy = y - mask_y0;
                if (sx < 0 || sy < 0 || sx >= mask_w || sy >= mask_h) {
                    maskA = 0.0; // mask 矩形之外 = 裁掉
                } else if (!mask_alpha && !mask_luma) {
                    maskA = 1.0; // 全黑占位：只用矩形
                } else {
                    const int mx = sx * mask->w / mask_w;
                    const int my = sy * mask->h / mask_h;
                    const size_t mi = (size_t(my) * size_t(mask->w) +
                                       size_t(mx)) * 4;
                    maskA = mask_alpha
                                ? mask->rgba[mi + 3] / 255.0
                                : (0.299 * mask->rgba[mi + 0] +
                                   0.587 * mask->rgba[mi + 1] +
                                   0.114 * mask->rgba[mi + 2]) / 255.0;
                }
            }
            const double out_a = A * maskA;
            (*out_px)[i] = uint8_t(std::clamp(r * 255.0, 0.0, 255.0) + 0.5);
            (*out_px)[i + 1] = uint8_t(std::clamp(g * 255.0, 0.0, 255.0) + 0.5);
            (*out_px)[i + 2] = uint8_t(std::clamp(b * 255.0, 0.0, 255.0) + 0.5);
            (*out_px)[i + 3] =
                uint8_t(std::clamp(out_a * 255.0, 0.0, 255.0) + 0.5);
        }
    }
}

TextureRef RenderEngine::upload_baked_texture(const std::vector<uint8_t>& px,
                                              int w, int h) {
    TextureRef baked = backend_->create_texture(w, h, TextureAccess::Static);
    if (baked) backend_->update_texture(baked, px.data(), w * 4);
    return baked;
}

const oa::media::Image* RenderEngine::resolve_image(const std::string& name)
{
    if (name.empty()) return nullptr;
    const auto it = decoded.find(name);
    if (it != decoded.end()) {
        decoded_use_[name] = ++cache_stamp_; // LRU refresh
        return &it->second;
    }
    std::string resolved = host_ ? host_->resolve_magic_path(name) : name;
    std::optional<std::vector<uint8_t>> bytes;
    // Try raw / .png / .jpg, then the same names under the _data/ root.
    std::vector<std::string> candidates{resolved, resolved + ".png", resolved + ".jpg"};
    if (!resolved.empty() && resolved[0] != ':' && resolved.rfind("_data/", 0) != 0) {
        candidates.push_back("_data/" + resolved);
        candidates.push_back("_data/" + resolved + ".png");
        candidates.push_back("_data/" + resolved + ".jpg");
    }
    for (const std::string& cand : candidates) {
        if (auto b = overlay_read(cand)) {
            bytes = b;
            break;
        }
        if (auto b = fs_->read(cand)) {
            bytes = b;
            break;
        }
    }
    // .lnq indirection: one-line text pointing at the real image.
    if (!bytes) {
        std::optional<std::vector<uint8_t>> lnq;
        std::vector<std::string> lnq_candidates{resolved + ".lnq"};
        if (!resolved.empty() && resolved[0] != ':' && resolved.rfind("_data/", 0) != 0)
            lnq_candidates.push_back("_data/" + resolved + ".lnq");
        for (const std::string& cand : lnq_candidates) {
            if (auto b = overlay_read(cand)) {
                lnq = b;
                break;
            }
            if (auto b = fs_->read(cand)) {
                lnq = b;
                break;
            }
        }
        if (lnq) {
            std::string target(reinterpret_cast<const char*>(lnq->data()), lnq->size());
            if (const size_t nl = target.find_first_of("\r\n"); nl != std::string::npos)
                target.resize(nl);
            while (!target.empty() && (target.back() == ' ' || target.back() == '\t'))
                target.pop_back();
            // The pointer is written in the game's own path vocabulary: the
            // leading `os_name_path/` is the project's data root (the archive
            // root itself for this family, published by first.iet).
            const std::string os_prefix = "os_name_path/";
            if (target.rfind(os_prefix, 0) == 0) {
                std::string data_root = "_data/";
                if (auto v = host_ ? host_->host_variable("os_name_path")
                                   : std::nullopt) {
                    const std::string& s = *v;
                    if (!s.empty()) data_root = s.back() == '/' ? s : s + "/";
                }
                target = data_root + target.substr(os_prefix.size());
            }
            if (!target.empty()) {
                if (auto b = overlay_read(target)) bytes = b;
                else if (auto c = fs_->read(target)) bytes = c;
            }
        }
    }
    if (!bytes) return nullptr;
    oa::media::Image img;
    if (!oa::media::decode_image(*bytes, img)) return nullptr;
    decoded_note(name, std::move(img));
    std::printf("[app] texture: %s (%dx%d)\n", resolved.c_str(), decoded[name].w,
        decoded[name].h);
    return &decoded[name];
}

/// Insert decoded pixels with cache accounting, then trim the cache to its
/// byte cap. Eviction destroys the GPU textures mirroring the evicted
/// pixels (the plain asset texture and every masked composite built from
/// it) — they re-create on demand if the scene shows them again.
void RenderEngine::decoded_note(const std::string& name, oa::media::Image&& img) {
    decoded_bytes_ += img.rgba.size();
    decoded[name] = std::move(img);
    decoded_use_[name] = ++cache_stamp_;
    decoded_evict_if_needed();
}

void RenderEngine::decoded_evict_if_needed() {
    static const size_t cap_mb = size_t(1024);
    if (cap_mb == 0 || decoded_bytes_ <= cap_mb * 1024ull * 1024ull) return;
    // least-recently used first (oldest stamp)
    std::vector<std::pair<uint64_t, std::string>> order;
    order.reserve(decoded_use_.size());
    for (const auto& [name, stamp] : decoded_use_)
        order.emplace_back(stamp, name);
    std::sort(order.begin(), order.end());
    for (const auto& [stamp, name] : order) {
        if (decoded_bytes_ <= cap_mb * 1024ull * 1024ull) break;
        const auto di = decoded.find(name);
        if (di == decoded.end()) continue; // composite key already gone
        decoded_bytes_ -= di->second.rgba.size();
        decoded.erase(di);
        // destroy the GPU mirrors: the plain asset texture + masked composites
        const oa::render::TextureKey base = oa::render::asset_key(name);
        const std::string masked_prefix = name + '';
        for (auto it = textures.lower_bound(base); it != textures.end();) {
            if (it->first.domain != oa::render::TextureKey::Domain::Asset ||
                (it->first.name != name &&
                 it->first.name.compare(0, masked_prefix.size(), masked_prefix) != 0))
                break;
            if (it->second) backend_->destroy_texture(it->second);
            it = textures.erase(it);
        }
        decoded_use_.erase(name);
    }
}

bool RenderEngine::emote_render_parts(const std::string& layer_id,
                                      const oa::render::HostTextureRef& target,
                                      const oa::emote::EmoteFile& file,
                                      const std::vector<oa::emote::EmoteDrawPart>& parts,
                                      int w, int h) {
    if (!backend_ || !target || w <= 0 || h <= 0) return false;
    const uint64_t file_uid = file.uid();
    // Drop this layer's atlas textures that belong to another PSB (the game
    // swapped the character file behind the same layer id).
    for (auto it = emote_atlases_.begin(); it != emote_atlases_.end();) {
        if (it->first.layer == layer_id && it->first.file != file_uid) {
            if (it->second) backend_->destroy_texture(it->second);
            it = emote_atlases_.erase(it);
        } else {
            ++it;
        }
    }
    TextureRef canvas = target->ensure_target(w, h);
    if (!canvas) return false;
    // Clear the canvas, then draw parts in order; stencil groups use two
    // offscreen targets (mask × content alpha). Details: docs/RENDERING.md.
    TextureRef prev_target = backend_->current_target();
    backend_->set_target(canvas);
    backend_->set_draw_blend(BlendMode::None);
    backend_->set_draw_color(0, 0, 0, 0);
    backend_->clear();
    backend_->set_draw_blend(BlendMode::Blend);
    backend_->clear_clip();
    // Reused vertex staging buffer.
    std::vector<Vertex> stage;

    struct StencilBucket {
        std::vector<const oa::emote::EmoteDrawPart*> mask;
        std::vector<const oa::emote::EmoteDrawPart*> content;
    };
    std::map<int, StencilBucket> stencil;
    std::vector<const oa::emote::EmoteDrawPart*> plain;
    // Group content in draw order; composite at the first content part.
    std::map<int, std::vector<const oa::emote::EmoteDrawPart*>> group_content;
    for (const auto& part : parts) {
        if (part.mask_role == 1) {
            // Masks collect up front (they may follow the first content part).
            stencil[part.mask_group].mask.push_back(&part);
            if (part.stencil_group >= 0)
                group_content[part.stencil_group].push_back(&part);
        } else if (part.stencil_group >= 0) {
            group_content[part.stencil_group].push_back(&part);
        }
    }
    (void)plain;

    // one part draw, translated by (ox,oy) scratch-space offset
    auto draw_part = [&](const oa::emote::EmoteDrawPart& part,
                         float ox, float oy) {
        if (part.source < 0 || part.source >= int(file.sources.size())) {
            return;
        }
        const oa::emote::EmoteSource& src = *file.sources[size_t(part.source)];
        if (part.icon < 0 || part.icon >= int(src.icons.size())) {
            return;
        }
        if (part.verts.size() < 3) {
            return;
        }
        const RenderEngine::EmoteAtlasKey akey{layer_id, part.source, file_uid};
        TextureRef atlas = nullptr;
        const auto ai = emote_atlases_.find(akey);
        if (ai != emote_atlases_.end()) atlas = ai->second;
        if (!atlas) {
            std::string err;
            if (!file.ensure_atlas(const_cast<oa::emote::EmoteSource*>(&src), &err)) {
                return;
            }
            if (src.rgba.empty() || src.textureWidth <= 0 || src.textureHeight <= 0)
                return;
            atlas = backend_->create_texture(src.textureWidth, src.textureHeight,
                                             TextureAccess::Static);
            if (!atlas) return;
            backend_->update_texture(atlas, src.rgba.data(),
                                     src.textureWidth * 4);
            backend_->set_texture_blend(atlas, BlendMode::Blend);
            emote_atlases_[akey] = atlas;
        }
        const oa::emote::EmoteIcon& ic = src.icons[size_t(part.icon)];
        stage.clear();
        stage.reserve(part.verts.size());
        const float a = float(std::clamp(part.alpha, 0.0, 1.0));
        for (const auto& v : part.verts) {
            // the atlas mapping comes from the same single
            // helper the CPU raster uses — (left + u*width)/texW with no trim
            // compensation term (the format has none).
            double tu = 0, tv = 0;
            oa::emote::emote_icon_uv_to_atlas(ic, src.textureWidth,
                                                     src.textureHeight, v.u, v.v, &tu, &tv);
            Vertex sv;
            sv.pos = {float(v.x) + ox, float(v.y) + oy};
            sv.color = {1.0f, 1.0f, 1.0f, a};
            sv.uv = {float(tu), float(tv)};
            stage.push_back(sv);
        }
        backend_->draw_geometry(atlas, stage.data(), int(stage.size()),
                                nullptr, 0);
    };

    // Composite one stencil group: mask shapes -> scratch0, content ->
    // scratch1, multiply the mask into the content's alpha, blend the result
    // back onto the canvas at the group's bbox.
    auto composite_group = [&](int gid) {
        StencilBucket& bucket = stencil[gid];
        const auto ci = group_content.find(gid);
        if (ci == group_content.end() || ci->second.empty()) return;
        const auto& content = ci->second;
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        bool any_v = false;
        auto grow = [&](const std::vector<const oa::emote::EmoteDrawPart*>& list) {
            for (const auto* part : list)
                for (const auto& v : part->verts) {
                    any_v = true;
                    x0 = std::min(x0, v.x);
                    y0 = std::min(y0, v.y);
                    x1 = std::max(x1, v.x);
                    y1 = std::max(y1, v.y);
                }
        };
        grow(bucket.mask);
        grow(content);
        if (!any_v) return;
        int bx0 = std::max(0, int(std::floor(x0)));
        int by0 = std::max(0, int(std::floor(y0)));
        int bx1 = std::min(w - 1, int(std::ceil(x1)));
        int by1 = std::min(h - 1, int(std::ceil(y1)));
        if (bx0 > bx1 || by0 > by1) return;
        const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
        if (stencil_scratch_w_ != bw || stencil_scratch_h_ != bh) {
            release_stencil_scratch();
            for (auto& t : stencil_scratch_) {
                t = backend_->create_texture(bw, bh, TextureAccess::Target);
                if (!t) break;
                // Target textures do NOT default to alpha blending: an opaque
                // copy back onto the canvas would REPLACE every pixel inside
                // the bbox.
                backend_->set_texture_blend(t, BlendMode::Blend);
            }
            if (!stencil_scratch_[0] || !stencil_scratch_[1]) {
                release_stencil_scratch();
                return;
            }
            stencil_scratch_w_ = bw;
            stencil_scratch_h_ = bh;
        }
        backend_->set_target(stencil_scratch_[0]);
        backend_->set_draw_blend(BlendMode::None);
        backend_->set_draw_color(0, 0, 0, 0);
        backend_->clear();
        backend_->set_draw_blend(BlendMode::Blend);
        backend_->clear_clip();
        for (const auto* part : bucket.mask) draw_part(*part, -float(bx0), -float(by0));
        backend_->set_target(stencil_scratch_[1]);
        backend_->set_draw_blend(BlendMode::None);
        backend_->set_draw_color(0, 0, 0, 0);
        backend_->clear();
        backend_->set_draw_blend(BlendMode::Blend);
        backend_->clear_clip();
        for (const auto* part : content) draw_part(*part, -float(bx0), -float(by0));
        // Mask step: RGBA *= coverage.
        backend_->set_texture_blend(stencil_scratch_[0], BlendMode::AlphaMultiply);
        backend_->draw_texture(stencil_scratch_[0], nullptr, nullptr);
        backend_->set_target(canvas);
        backend_->set_draw_blend(BlendMode::Blend);
        const FRect dst{float(bx0), float(by0), float(bw), float(bh)};
        // Content target is premultiplied → use BlendPremult.
        backend_->set_texture_blend(stencil_scratch_[1], BlendMode::BlendPremult);
        backend_->draw_texture(stencil_scratch_[1], nullptr, &dst);
        backend_->set_texture_blend(stencil_scratch_[1], BlendMode::Blend);
        backend_->set_texture_blend(stencil_scratch_[0], BlendMode::Blend);
    };

    // Ordered walk; stencil groups composite at their first content part.
    std::map<int, char> group_done;
    for (const auto& p : parts) {
        // Masks are also real artwork; never skip drawing them.
        if (p.stencil_group < 0) {
            draw_part(p, 0.0f, 0.0f);
            continue;
        }
        const int g = p.stencil_group;
        if (group_done[g]) continue; // already composited at its first part
        group_done[g] = 1;
        composite_group(g);
    }
    backend_->set_target(prev_target);
    return true;
}

// Host-frame pump: emote first, then video layer/overlay frames.



TextureRef RenderEngine::texture_for_asset(const std::string& name)
{
    return texture_for_key(oa::render::asset_key(name));
}

TextureRef RenderEngine::texture_for_key(const oa::render::TextureKey& key)
{
    if (key.empty()) return nullptr;
    const auto it = textures.find(key);
    if (it != textures.end()) return it->second;
    // 只有资源域会解码；宿主内容的纹理由层直接持有。
    if (!key.is_asset()) return nullptr;
    const oa::media::Image* img = resolve_image(key.name);
    if (!img) return nullptr;
    TextureRef tex = make_texture(*img);
    if (!tex) return nullptr;
    textures[key] = tex;
    return tex;
}

RenderEngine::CacheStats RenderEngine::cache_stats() const
{
    CacheStats out;
    out.host_textures = textures.size();
    out.decoded_images = decoded.size();
    out.emote_canvases = 0;
    out.emote_atlases = emote_atlases_.size();
    return out;
}

std::optional<std::pair<double, double>> RenderEngine::decoded_size(const oa::render::TextureKey& key) const
{
    // ContentSizeSource: decoded lookup, Asset domain only.
    if (key.empty() || !key.is_asset()) return std::nullopt;
    const auto di = decoded.find(key.name);
    if (di == decoded.end()) return std::nullopt;
    return std::make_pair(double(di->second.w), double(di->second.h));
}


TextureRef RenderEngine::texture_for_masked_key(
    const oa::render::TextureKey& base, const std::string& mask)
{
    if (base.empty()) return nullptr;
    // Mask composition is Asset-only.
    if (!base.is_asset() || mask.empty()) return texture_for_key(base);
    const std::string key = base.name + std::string("\x1f") + mask;
    const oa::render::TextureKey masked = oa::render::asset_key(key);
    const auto it = textures.find(masked);
    if (it != textures.end()) return it->second;
    const oa::media::Image* fimg = resolve_image(base.name);
    const oa::media::Image* mimg = resolve_image(mask);
    if (!fimg || !mimg || fimg->w != mimg->w || fimg->h != mimg->h) {
        return texture_for_key(base); // mask ignored on failure/mismatch
    }
    oa::media::Image out;
    out.w = fimg->w;
    out.h = fimg->h;
    out.rgba = fimg->rgba;
    for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
        out.rgba[i + 3] =
            (uint8_t)(((uint16_t)out.rgba[i + 3] * (uint16_t)mimg->rgba[i]) / 255u);
    }
    TextureRef tex = make_texture(out);
    if (!tex) return nullptr;
    decoded_note(key, std::move(out)); // cache pixels (hit sampling etc.)
    textures[masked] = tex;
    return tex;
}

bool RenderEngine::ensure_decoded(const oa::render::TextureKey& key) {
    // 计划构建期的"生产者前置"：只解像素，不建 GPU 纹理（上传归栅格化）。
    if (key.empty() || !key.is_asset()) return false;
    if (decoded.find(key.name) != decoded.end()) return true;
    return resolve_image(key.name) != nullptr;
}

uint8_t RenderEngine::alpha_byte(double a) {
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return uint8_t(a * 255.0 + 0.5);
}

std::optional<std::pair<double, double>> RenderEngine::hit_size(void* userdata, const oa::render::Layer& l) {
    RenderEngine* ptr = static_cast<RenderEngine*>(userdata);
    // Hit size: content key → decoded size.
    if (!oa::render::textured_content(l)) return std::nullopt;
    if (l.host_bound) {
        if (l.tex_w > 0 && l.tex_h > 0)
            return std::make_pair(double(l.tex_w), double(l.tex_h));
        return std::nullopt;
    }
    const oa::render::TextureKey key = oa::render::content_texture_key(l);
    if (auto size = ptr->decoded_size(key)) return size;
    // Headless has no decoded pixels; probe the file header instead so
    // natural size for their hit quad.
    if (!ptr->backend_ && !key.empty() && key.is_asset()) {
        return ptr->probe_asset_size(key.name);
    }
    return std::nullopt;
}

namespace {

/// PNG/JPEG natural size from the first bytes (no pixel decode).
bool image_header_size(const std::vector<uint8_t>& b, int* w, int* h) {
    static const unsigned char kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (b.size() >= 24 && std::memcmp(b.data(), kPng, 8) == 0) {
        *w = (int)((uint32_t(b[16]) << 24) | (uint32_t(b[17]) << 16) |
                   (uint32_t(b[18]) << 8) | uint32_t(b[19]));
        *h = (int)((uint32_t(b[20]) << 24) | (uint32_t(b[21]) << 16) |
                   (uint32_t(b[22]) << 8) | uint32_t(b[23]));
        return *w > 0 && *h > 0;
    }
    if (b.size() >= 4 && b[0] == 0xFF && b[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < b.size()) {
            if (b[i] != 0xFF) {
                ++i;
                continue;
            }
            const uint8_t marker = b[i + 1];
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
                i += 2;
                continue;
            }
            const size_t len = (size_t(b[i + 2]) << 8) | b[i + 3];
            const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
                             marker != 0xC8 && marker != 0xCC;
            if (sof && i + 9 < b.size()) {
                *h = (int)((uint32_t(b[i + 5]) << 8) | b[i + 6]);
                *w = (int)((uint32_t(b[i + 7]) << 8) | b[i + 8]);
                return *w > 0 && *h > 0;
            }
            if (len < 2) break;
            i += 2 + len;
        }
    }
    return false;
}

} // namespace

std::optional<std::pair<double, double>> RenderEngine::probe_asset_size(const std::string& name) {
    const auto cached = probed_sizes_.find(name);
    if (cached != probed_sizes_.end()) {
        return cached->second.first > 0 ? std::optional<std::pair<double, double>>(cached->second)
                                        : std::nullopt;
    }
    std::string resolved = host_ ? host_->resolve_magic_path(name) : name;
    std::optional<std::vector<uint8_t>> bytes;
    for (const std::string& cand : {resolved, resolved + ".png", resolved + ".jpg"}) {
        if (auto b = overlay_read(cand)) {
            bytes = b;
            break;
        }
        if (auto b = fs_->read(cand)) {
            bytes = b;
            break;
        }
    }
    if (!bytes && resolved.size() >= 4 &&
        resolved.compare(resolved.size() - 4, 4, ".lnq") == 0) {
        // .lnq is a text pointer at the real image; the caller passes the
        // extension-less name, so probe that variant too.
        if (auto b = fs_->read(resolved)) bytes = b;
    }
    int w = 0, h = 0;
    if (!bytes || !image_header_size(*bytes, &w, &h)) {
        probed_sizes_.emplace(name, std::make_pair(0.0, 0.0));
        return std::nullopt;
    }
    const std::pair<double, double> size{double(w), double(h)};
    probed_sizes_.emplace(name, size);
    return size;
}

std::optional<uint8_t> RenderEngine::alpha_sampler(void* userdata, const oa::render::Layer& l, int tx, int ty)
{
    RenderEngine* ptr = static_cast<RenderEngine*>(userdata);
    // 采样只走解码缓存,宿主供帧回退层 alpha。
    if (!oa::render::textured_content(l)) return std::nullopt;
    if (l.host_bound) {
        // 宿主纹理在 GPU 侧：回退层 alpha（调用方按不透明处理）。
        (void)tx;
        (void)ty;
        return std::nullopt;
    }
    const oa::render::TextureKey key = oa::render::content_texture_key(l);
    if (key.empty() || !key.is_asset()) return std::nullopt; // 上传纹理无解码像素
    const auto di = ptr->decoded.find(key.name);
    if (di == ptr->decoded.end()) return std::nullopt;
    const oa::media::Image& img = di->second;
    if (tx < 0 || ty < 0 || tx >= img.w || ty >= img.h) return std::nullopt;
    return img.rgba[(size_t(ty) * img.w + size_t(tx)) * 4 + 3];
}

void RenderEngine::process_event(const oa::runtime::Event& e)
{
    using K = oa::runtime::Event::Kind;
    // Scene/text events are consumed by the runtime; only trans/flip arrive.
    if (e.kind == K::LayerEventCmd) {
        // LayerEventCmd is a recognized no-op here.
    }
    else if (e.kind == K::Trans) {
        // [trans]: start the transition (type 0 clears).
        ++trans_events;
        if (trans_events <= 8) {
            std::string dbg;
            for (const auto& [k, v] : e.params) dbg += k + "=" + v + " ";
            std::printf("[app] trans event #%d f=%llu params: %s\n",
                        (int)trans_events, (unsigned long long)host_->now_ms(),
                        dbg.c_str());
        }
        host_->transition_begin(e.params);
    }
    else if (e.kind == K::Flip) {
        // [flip] requests a repaint; it must not cancel a live transition.
    }
}

TextureRef RenderEngine::solid_texture(const uint8_t rgba[4])
{
    const uint32_t key = (uint32_t(rgba[0]) << 24) | (uint32_t(rgba[1]) << 16) |
                         (uint32_t(rgba[2]) << 8) | rgba[3];
    auto it = solid_tex_cache.find(key);
    if (it != solid_tex_cache.end()) return it->second;
    uint8_t px[4] = {rgba[0], rgba[1], rgba[2], 255};
    TextureRef tex = backend_->create_texture(1, 1, TextureAccess::Static);
    if (!tex) return nullptr;
    backend_->update_texture(tex, px, 4);
    backend_->set_texture_blend(tex, BlendMode::Blend);
    solid_tex_cache[key] = tex;
    return tex;
}

void RenderEngine::render_begin()
{
    if (!backend_) return;
    if (stage_rt) {
        backend_->set_target(stage_rt);
        backend_->clear_clip();
    }
    backend_->set_draw_color(0, 0, 0, 255);
    backend_->clear();
}

void RenderEngine::progress_transition()
{
    // Old frame over new scene: crossfade, or type-2 rule dissolve when
    // supported; otherwise crossfade fallback.
    if (trans_capture_tex) {
        if (transition().is_in_progress(host_->now_ms())) {
            const double p = transition().progress(host_->now_ms());
            const double op = 1.0 - p;
            if (op > 0.0) {
                bool drew = false;
                if (transition().type() == 2) {
                    if (!trans_rule_checked_) {
                        // Resolve the rule once per transition.
                        trans_rule_checked_ = true;
                        const std::string& rule = transition().rule();
                        trans_rule_tex =
                            rule.empty() ? nullptr : texture_for_asset(rule);
                    }
                    if (trans_rule_tex) {
                        // neutral mods: the capture may still carry the
                        // crossfade alpha from an earlier frame of another
                        // transition path.
                        backend_->set_texture_alpha_mod(trans_capture_tex, 255);
                        backend_->set_texture_color_mod(trans_capture_tex, 255,
                                                        255, 255);
                        const int vague =
                            std::clamp(transition().vague(), 0, 255);
                        float band = float(vague) / 255.0f;
                        if (band < 1.0f / 255.0f) band = 1.0f / 255.0f;
                        drew = backend_->draw_rule_transition(
                            trans_capture_tex, trans_rule_tex, float(p), band);
                    }
                }
                if (!drew) {
                    backend_->set_texture_alpha_mod(trans_capture_tex,
                                                    alpha_byte(op));
                    const FRect full{ 0, 0, float(trans_capture_w),
                                            float(trans_capture_h) };
                    backend_->draw_texture(trans_capture_tex, nullptr, &full);
                }
            }
            else {
                // transition ended this frame: drop the capture
                backend_->destroy_texture(trans_capture_tex);
                trans_capture_tex = nullptr;
                trans_rule_tex = nullptr;
                trans_rule_checked_ = false;
            }
        }
        else if (!transition().active()) {
            // no transition anymore (finished / flipped / replaced): the
            // capture texture is only kept while the overlay can draw it.
            backend_->destroy_texture(trans_capture_tex);
            trans_capture_tex = nullptr;
            trans_rule_tex = nullptr;
            trans_rule_checked_ = false;
        }
    }
}

void RenderEngine::render_end()
{
    if (!backend_) return;
    if (stage_rt) {
        backend_->set_target(nullptr);
        backend_->clear_clip();
        backend_->set_draw_color(0, 0, 0, 255);
        backend_->clear();
        const FRect full{ 0.0f, 0.0f, float(stage_w_), float(stage_h_) };
        backend_->draw_texture(stage_rt, nullptr, &full);
        stage_frame_valid_ = true;
    }
    backend_->present();
    if (stage_rt) backend_->set_target(stage_rt);
}

void RenderEngine::begin_all_delete(uint64_t time_ms, uint64_t now_ms)
{
    all_delete_start_ms_ = now_ms;
    all_delete_duration_ms_ = time_ms;
    all_delete_active_ = true;
    dirty_ = true;
}

double RenderEngine::all_delete_fade(uint64_t now_ms) const
{
    if (!all_delete_active_ || all_delete_duration_ms_ == 0) return 1.0;
    const uint64_t elapsed =
        now_ms >= all_delete_start_ms_ ? now_ms - all_delete_start_ms_ : 0;
    const double p = elapsed >= all_delete_duration_ms_
                         ? 1.0
                         : double(elapsed) / double(all_delete_duration_ms_);
    return 1.0 - p;
}

bool RenderEngine::all_delete_expired(uint64_t now_ms) const
{
    return all_delete_active_ && all_delete_duration_ms_ != 0 &&
           now_ms >= all_delete_start_ms_ + all_delete_duration_ms_;
}

void RenderEngine::stop_all_delete()
{
    all_delete_active_ = false;
    all_delete_start_ms_ = 0;
    all_delete_duration_ms_ = 0;
    dirty_ = true;
}

void RenderEngine::note(Note what)
{
    switch (what) {
        case Note::Layer: ++stats_.layers; break;
        case Note::Tween: ++stats_.tweens; break;
        case Note::TweenComplete: ++stats_.tween_done; break;
        case Note::Text: ++stats_.text; break;
        case Note::MessageSwitch: ++stats_.message_switch; break;
    }
    dirty_ = true;
}

bool RenderEngine::render_frame(uint64_t now_ms, double global_fade)
{
    if (!backend_) {
        frame_presented_ = false;
        return false;
    }

    const bool text_changed = text().revision() != last_text_rev_;
    last_text_rev_ = text().revision();
    const bool animated = scene().has_tweens() || scene().has_anime();
    const bool trans = transition_->is_in_progress(now_ms) ||
                       transition_->active() || trans_capture_tex != nullptr;
    const bool fade_active = global_fade < 1.0;
    // 任何层写（含源更新纹理后的 set_host_texture）都递增场景版本 ⇒ 重绘。
    const bool scene_changed = dirty_ || scene().revision() != plan_scene_rev_;

    const bool need = scene_changed || !rendered_any_ ||
                      animated || trans || text_changed ||
                      fade_active;
    dirty_ = false;
    frame_presented_ = false;
    if (!need) return false;

    render_begin();
    draw_scene(scene(), global_fade);
    progress_transition();
    if (host_) host_->post_frame_capture();
    render_end();

    if (vsync_active_) {
        const uint64_t real_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (prev_present_ms_ && real_ms > prev_present_ms_) {
            const double d = double(real_ms - prev_present_ms_);
            present_ema_ms_ =
                present_ema_ms_ < 0 ? d : present_ema_ms_ * 0.9 + d * 0.1;
            if (++present_samples_ > 60 && present_ema_ms_ < 3.5) {
                vsync_active_ = false;
                std::printf("[render] vsync is not pacing presentation (%.1f ms "
                            "between frames); using the timer delay instead\n",
                            present_ema_ms_);
            }
        }
        prev_present_ms_ = real_ms ? real_ms : 1;
    }

    rendered_any_ = true;
    frame_presented_ = true;
    return true;
}

std::optional<std::vector<uint8_t>> RenderEngine::capture_frame(uint32_t* w,
                                                               uint32_t* h)
{
    oa::media::Image img;
    if (!snapshot_renderer(img) || img.w <= 0 || img.h <= 0) return std::nullopt;
    if (w) *w = uint32_t(img.w);
    if (h) *h = uint32_t(img.h);
    return std::move(img.rgba);
}

namespace {

const std::string* prop_of(const Layer& l, const char* k) {
    const auto it = l.props.find(k);
    return it == l.props.end() ? nullptr : &it->second;
}

bool prop_truthy(const std::string* v) {
    return v && (*v == "1" || *v == "true" || *v == "on" || *v == "yes");
}

int intermediate_mode_of(const Layer& l) {
    return intermediate_render_mode(l);
}

// 组语义门（旧 renderer 私有实现的逐条搬移；本 TU 内纯函数）。

bool plan_group_needs_offscreen(const Layer& l) {
    if (intermediate_mode_of(l) == 0) return false;
    if (intermediate_mode_of(l) == 2) return true;
    if (l.alpha < 1.0 - 1e-9) return true;
    if (prop_of(l, "colormultiply")) return true;
    if (prop_truthy(prop_of(l, "grayscale"))) return true;
    if (prop_truthy(prop_of(l, "negative"))) return true;
    if (prop_of(l, "intermediate_render_mask")) return true;
    // File-less container clip crops the subtree (texture clip stays source).
    if (l.file.empty() && l.has_clip) return true;
    const std::string* lm = prop_of(l, "layermode");
    if (lm && *lm != "alpha" && !lm->empty()) return true;
    return false;
}

bool plan_group_path_required(const Layer& l) {
    return intermediate_mode_of(l) != 0 && plan_group_needs_offscreen(l);
}

std::optional<std::array<double, 3>> parse_color_multiply(const std::string& v) {
    auto hexd = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string s = v;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2);
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    // Artemis 语义: "0" / "0x0" / 空 = 关闭染色(白乘,不乘)。
    if (s.empty() || s == "0") return std::nullopt;
    if ((s.size() == 6 || s.size() == 8) &&
        std::all_of(s.begin(), s.end(),
                    [&](char c) { return hexd(c) >= 0; })) {
        const size_t off = s.size() == 8 ? 2 : 0; // AARRGGBB -> skip AA
        std::array<double, 3> out{};
        for (int i = 0; i < 3; ++i) {
            out[(size_t)i] = double(hexd(s[off + size_t(i) * 2]) * 16 +
                                    hexd(s[off + size_t(i) * 2 + 1])) /
                             255.0;
        }
        return out;
    }
    std::array<double, 3> out{};
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t comma = s.find(',', pos);
        const std::string part =
            s.substr(pos, comma == std::string::npos ? std::string::npos
                                                    : comma - pos);
        try {
            const double vv = std::stod(part);
            out[(size_t)i] = std::clamp(vv, 0.0, 255.0) / 255.0;
        } catch (...) {
            return std::nullopt;
        }
        if (comma == std::string::npos) return out;
        pos = comma + 1;
    }
    return std::optional(out);
}

} // namespace

// ---------------------------------------------------------------------------
// RenderPlan
// ---------------------------------------------------------------------------

void RenderPlan::clear() {
    items_.clear(); // 容量保留：每帧重建无堆抖动
    content_count = 0;
    glyph_count = 0;
    group_count = 0;
}

PlanItem& RenderPlan::push(PlanOp op, std::string id) {
    PlanItem& it = items_.emplace_back();
    it.op = op;
    it.id = std::move(id);
    it.z = uint32_t(items_.size() - 1);
    switch (op) {
        case PlanOp::Content:
            ++content_count;
            break;
        case PlanOp::Glyphs:
            ++glyph_count;
            break;
        case PlanOp::GroupBegin:
        case PlanOp::GroupEnd:
            ++group_count;
            break;
    }
    return it;
}

// ---------------------------------------------------------------------------
// 计划构建：层树 → 绘制项流
// ---------------------------------------------------------------------------
namespace {

struct Builder {
    const PlanInput& in;
    RenderPlan* out;
    /// 槽位 → 绑定到它的消息层 id（每帧快照，纯读）。
    std::map<std::string, std::vector<std::string>> messages;
    /// 组背板锚框缓存（与旧 anchor_frame_cache_ 同语义，仅本次构建有效）。
    std::map<std::string, std::optional<IRect>> anchor_cache;

    std::optional<std::array<double, 4>> layer_world_rect(const Layer& l) {
        if (!in.sizes) return std::nullopt;
        const ContentQuad q = content_quad(l, *in.sizes);
        if (!q.has) return std::nullopt;
        double x = 0, y = 0, w = 0, h = 0;
        if (!in.scene->world_rect(l.id, q.w, q.h, &x, &y, &w, &h))
            return std::nullopt;
        return std::array<double, 4>{x, y, w, h};
    }

    /// 与组同原点的最后一张纹理背板的并集（裁剪窗对齐用）。
    std::optional<IRect> anchor_frame(const Layer& l) {
        const auto cit = anchor_cache.find(l.id);
        if (cit != anchor_cache.end()) return cit->second;
        std::optional<IRect> frame;
        do {
            Affine2 t;
            if (!in.scene->world_transform(l.id, &t)) break;
            double ox = 0, oy = 0;
            t.transform_point(0, 0, &ox, &oy);
            const std::string sub = l.id + ".";
            for (const Layer* q : in.scene->draw_order()) {
                if (q == &l) break; // anchor quads draw BEFORE the group
                if (!textured_content(*q)) continue;
                // 不可见的背板不能收紧裁剪窗：存档恢复会把消息窗口的
                // 平板层（world 1280x120）一起建出来，父层仍隐藏；若参与
                // 求交，标题组的全屏 clip 会被切成顶部 120px（标题下方全黑）。
                if (!in.scene->is_effectively_visible(q->id)) continue;
                if (q->id.size() > l.id.size() &&
                    q->id.compare(0, sub.size(), sub) == 0)
                    continue; // safety: no subtree quad qualifies
                const auto qr = layer_world_rect(*q);
                if (!qr) continue;
                const double qx = (*qr)[0], qy = (*qr)[1];
                if (std::fabs(qx - ox) > 0.5 || std::fabs(qy - oy) > 0.5)
                    continue; // must share the group's origin
                const IRect r{int(std::floor(qx)), int(std::floor(qy)),
                              int(std::ceil(qx + (*qr)[2])) - int(std::floor(qx)),
                              int(std::ceil(qy + (*qr)[3])) - int(std::floor(qy))};
                if (!frame) {
                    frame = r;
                    continue;
                }
                const IRect a = *frame, b = r;
                const int lx = std::max(a.x, b.x);
                const int ly = std::max(a.y, b.y);
                const int rx = std::min(a.x + a.w, b.x + b.w);
                const int ry = std::min(a.y + a.h, b.y + b.h);
                frame = (rx > lx && ry > ly)
                            ? std::optional<IRect>(IRect{lx, ly, rx - lx, ry - ly})
                            : std::nullopt;
                if (!frame) break;
            }
        } while (false);
        anchor_cache[l.id] = frame;
        return frame;
    }

    /// 一组离屏合成的求值参数（旧 resolve_group_plan 的逐条搬移）。
    PlanComposite group_plan(const Layer& layer, std::optional<IRect> inherited) {
        PlanComposite p;
        if (inherited) {
            p.clip.has = true;
            p.clip.rect = *inherited;
        }
        if (const std::string* cv = prop_of(layer, "clip")) {
            std::array<double, 4> c{0, 0, 0, 0};
            const int used = std::sscanf(cv->c_str(), "%lf,%lf,%lf,%lf", &c[0],
                                         &c[1], &c[2], &c[3]);
            if (used == 4) {
                if (const auto r = world_aabb_local(layer, c[0], c[1], c[2], c[3])) {
                    std::optional<IRect> crop = r;
                    // Anchor confinement applies only to file-less containers.
                    if (!textured_content(layer)) {
                        if (const auto f = anchor_frame(layer)) {
                            const IRect a = *crop, b = *f;
                            const int lx = std::max(a.x, b.x);
                            const int ly = std::max(a.y, b.y);
                            const int rx = std::min(a.x + a.w, b.x + b.w);
                            const int ry = std::min(a.y + a.h, b.y + b.h);
                            if (rx > lx && ry > ly)
                                crop = IRect{lx, ly, rx - lx, ry - ly};
                        }
                    }
                    if (p.clip.has) {
                        const IRect a = p.clip.rect, b = *crop;
                        const int lx = std::max(a.x, b.x);
                        const int ly = std::max(a.y, b.y);
                        const int rx = std::min(a.x + a.w, b.x + b.w);
                        const int ry = std::min(a.y + a.h, b.y + b.h);
                        if (rx <= lx || ry <= ly) {
                            p.clip.rect = IRect{0, 0, 0, 0};
                        } else {
                            p.clip.rect = IRect{lx, ly, rx - lx, ry - ly};
                        }
                    } else {
                        p.clip.has = true;
                        p.clip.rect = *crop;
                    }
                }
            }
        }
        // draw-time params: group alpha stays OUT of the bake (alpha-mod at
        // composite time — exact: straight rgb * (a*alpha)).
        p.alpha = layer.alpha;
        std::array<double, 3> cm = {1.0, 1.0, 1.0};
        if (const std::string* cmv = prop_of(layer, "colormultiply")) {
            if (const auto parsed = parse_color_multiply(*cmv)) cm = *parsed;
        }
        p.color_multiply[0] = cm[0];
        p.color_multiply[1] = cm[1];
        p.color_multiply[2] = cm[2];
        p.grayscale = prop_truthy(prop_of(layer, "grayscale"));
        p.negative = prop_truthy(prop_of(layer, "negative"));
        if (const std::string* mv = prop_of(layer, "intermediate_render_mask")) {
            p.has_mask = !mv->empty();
            p.mask = *mv;
        }
        // composite blend onto the parent (layermode uniform)
        if (const std::string* lm = prop_of(layer, "layermode")) {
            if (*lm == "add" || *lm == "additive")
                p.blend = BlendMode::Add;
            else if (*lm == "screen")
                p.blend = BlendMode::Blend; // approx.
            else if (*lm == "multiply" || *lm == "mul")
                p.blend = BlendMode::Mod;
        }
        return p;
    }

    /// 世界矩形（clip 局部矩形 → 层世界 AABB）。
    std::optional<IRect> world_aabb_local(const Layer& l, double x, double y,
                                          double w, double h) {
        Affine2 t;
        if (!in.scene->world_transform(l.id, &t)) return std::nullopt;
        const double cx[4] = {x, x + w, x, x + w};
        const double cy[4] = {y, y, y + h, y + h};
        double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
        for (int i = 0; i < 4; ++i) {
            double px = 0, py = 0;
            t.transform_point(cx[i], cy[i], &px, &py);
            minx = std::min(minx, px);
            maxx = std::max(maxx, px);
            miny = std::min(miny, py);
            maxy = std::max(maxy, py);
        }
        const double x0 = std::floor(minx), y0 = std::floor(miny);
        const double x1 = std::ceil(maxx), y1 = std::ceil(maxy);
        if (x1 <= x0 || y1 <= y0) return std::nullopt;
        return IRect{int(x0), int(y0), int(x1 - x0), int(y1 - y0)};
    }

    /// 一层自身的内容项（纯色或纹理 quad）。无内容则不产生项。
    void emit_content(const Layer& l, const Affine2& world, double op) {
        if (l.file.empty() && l.has_color && l.width > 0 && l.height > 0) {
            PlanItem& it = out->push(PlanOp::Content, l.id);
            it.solid = true;
            std::memcpy(it.solid_rgba, l.solid_rgba, sizeof(it.solid_rgba));
            it.w = l.width;
            it.h = l.height;
            it.world = world;
            it.opacity = op;
            return;
        }
        if (l.host_bound) {
            // 宿主内容（视频帧/emote 画布）：尺寸取层上登记的帧尺寸，
            // 纹理挂在层上，栅格化时 flush 一次。
            const ContentQuad q = content_quad(l, *in.sizes);
            if (!q.has) return;
            PlanItem& it = out->push(PlanOp::Content, l.id);
            it.host = true;
            it.w = q.w;
            it.h = q.h;
            it.world = world;
            it.opacity = op;
            return;
        }
        if (!textured_content(l)) return;
        const TextureKey key = content_texture_key(l);
        if (key.empty() || !in.sizes) return;
        // 与旧绘制路径同序：先保证像素（解码）可用，再问 quad 尺寸 ——
        // 否则首帧资产尚无 decoded 尺寸，quad 会判成“无内容”。
        if (key.is_asset()) in.sizes->ensure_decoded(key);
        const ContentQuad q = content_quad(l, *in.sizes);
        if (!q.has) return;
        PlanItem& it = out->push(PlanOp::Content, l.id);
        it.solid = q.solid;
        it.texture = key;
        it.w = q.w;
        it.h = q.h;
        if (q.has_src) {
            it.has_src = true;
            it.src = FRect{float(q.src_x), float(q.src_y), float(q.src_w),
                           float(q.src_h)};
        }
        it.world = world;
        it.opacity = op;
        if (!l.mask.empty() && key.is_asset())
            it.mask = l.mask;
        if (const std::string* cmv = prop_of(l, "colormultiply")) {
            if (const auto cm = parse_color_multiply(*cmv)) {
                it.has_color_multiply = true;
                for (int i = 0; i < 3; ++i) {
                    it.color_multiply[i] = uint8_t(std::clamp(
                        (*cm)[(size_t)i] * 255.0 + 0.5, 0.0, 255.0));
                }
            }
        }
    }

    /// 该槽位绑定的字形（消息层）。
    void emit_glyphs(const Layer& l, const Affine2& world, double op) {
        const auto it = messages.find(l.id);
        if (it == messages.end() || it->second.empty()) return;
        PlanItem& g = out->push(PlanOp::Glyphs, l.id);
        g.messages = it->second;
        g.world = world;
        g.opacity = op;
    }

    void walk(const SceneNode& node, const Affine2& parent_world, double parent_op,
              std::optional<IRect> scissor);
    void walk_plain(const SceneNode& node, const Affine2& world, int intermediate,
                    double parent_op, std::optional<IRect> scissor);
    void walk_group(const SceneNode& node, const Affine2& world, double parent_op,
                    std::optional<IRect> scissor);
};

void Builder::walk_plain(const SceneNode& node, const Affine2& world,
                         int intermediate, double parent_op,
                         std::optional<IRect> scissor) {
    const Layer& layer = node.layer;
    // intermediate 组的 alpha 不进子树（组 alpha 由合成一步应用）；
    // 非组节点 = 自身 alpha 下传。
    const double own_op = parent_op * (intermediate != 0 ? 1.0 : layer.alpha);
    emit_content(layer, world, own_op);
    emit_glyphs(layer, world, own_op);
    for (const SceneNode* c : node.children)
        walk(*c, world, own_op, scissor);
}

void Builder::walk_group(const SceneNode& node, const Affine2& world,
                         double parent_op, std::optional<IRect> scissor) {
    const Layer& layer = node.layer;
    const PlanComposite p = group_plan(layer, scissor);
    // Identity composite → inline draw (pixel-identical, skips full-stage bake).
    const bool identity_composite =
        !p.has_mask && !p.grayscale && !p.negative &&
        p.alpha >= 1.0 - 1e-9 && p.blend == BlendMode::Blend &&
        std::abs(p.color_multiply[0] - 1.0) < 1e-9 &&
        std::abs(p.color_multiply[1] - 1.0) < 1e-9 &&
        std::abs(p.color_multiply[2] - 1.0) < 1e-9 &&
        (!p.clip.has || (p.clip.rect.x <= 0 && p.clip.rect.y <= 0 &&
                         p.clip.rect.w >= in.stage_w && p.clip.rect.h >= in.stage_h));
    if (identity_composite) {
        walk_plain(node, world, 1, parent_op, scissor);
        return;
    }
    PlanItem& begin = out->push(PlanOp::GroupBegin, layer.id);
    begin.composite = p;
    begin.world = world;
    begin.opacity = parent_op;
    // group's own content and subtree, group alpha excluded
    emit_content(layer, world, parent_op);
    emit_glyphs(layer, world, parent_op);
    const std::optional<IRect> child_scissor =
        p.clip.has ? std::optional<IRect>(p.clip.rect) : std::nullopt;
    for (const SceneNode* c : node.children)
        walk(*c, world, parent_op, child_scissor);
    out->push(PlanOp::GroupEnd, layer.id);
}

void Builder::walk(const SceneNode& node, const Affine2& parent_world,
                   double parent_op, std::optional<IRect> scissor) {
    const Layer& layer = node.layer;
    if (!in.scene->is_effectively_visible(layer.id)) return;
    const Affine2 world = parent_world * layer.local_transform();
    if (plan_group_path_required(layer)) {
        walk_group(node, world, parent_op, scissor);
        return;
    }
    walk_plain(node, world, intermediate_mode_of(layer), parent_op, scissor);
}

} // namespace

void build_render_plan(const PlanInput& in, RenderPlan* out) {
    if (!out) return;
    out->clear();
    ++out->generation;
    if (!in.scene) return;

    Builder b{in, out};
    // 槽位 → 消息层 id（纯快照；绘制=可见 + 有绘制盒/显式节点）。
    if (in.text) {
        for (const std::string& mid : in.text->visible_content_layers()) {
            const MessageLayer* ml = in.text->layer(mid);
            if (!in.scene->is_message_layer_drawable(mid, ml && ml->positioned))
                continue;
            b.messages[in.scene->bound_scene_id(mid)].push_back(mid);
        }
    }
    const Affine2 root_w = in.scene->root_props().local_transform();
    const double root_op = in.scene->root_props().alpha * in.global_fade;
    for (const SceneNode* r : in.scene->roots())
        b.walk(*r, root_w, root_op, std::nullopt);
    // Message slots draw after the scene region.
    for (const SceneNode* s : in.scene->message_slots())
        b.walk(*s, root_w, root_op, std::nullopt);
    // Overlay-video slot: structurally topmost; stage space (root props ignored).
    if (const SceneNode* ov = in.scene->overlay_node())
        b.walk(*ov, Affine2{}, 1.0, std::nullopt);
}

}
