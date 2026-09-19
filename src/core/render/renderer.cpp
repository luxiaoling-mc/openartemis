#include "core/render/renderer.h"
#include "core/render/backend_gles.h"
#include "core/render/backend_sdl.h"
#include "core/emote/emote_file.h"
#include "core/emote/emote_player.h"
#include "core/media/video.h" // rgba_luma / layer_video_key_alpha (keyed upload)
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL_video.h> // create_renderer 诊断打印的窗口尺寸查询（仅此）

// 本文件不直接触碰 SDL_Render；所有绘制
// 原语经 RenderBackend 接口（sdl 实现 backend_sdl.cpp = 引擎原本调用面的
// 同构搬移；gles 实现 backend_gles.cpp = 原生 GLES 线）。除类型层替换与
// create_renderer 的后端名参数外，逻辑/顺序/参数与引擎原本逐行等价。

namespace oa::render {

RenderEngine::RenderEngine(oa::fs::IFileSystem* fs, oa::runtime::GameRuntime* rt) : fs_(fs), rt_(rt)
{
    fontSystem = std::make_unique<oa::render::FontSystem>(fs, rt);
    // 文本度量钩子：Lua get_fontsize → e:var system=get_message_layer_width/height
    // → interpreter hooks_.message_layer_metrics → FontSystem 排版测量。
    // 必须在 fontSystem 就绪后、boot 之前接入（boot 触发 font_init → get_fontdata
    // → get_fontsize，此时钩子必须可用）。
    auto* fs_ptr = fontSystem.get();
    rt_->interpreter().hooks().message_layer_metrics =
        [fs_ptr, rt = rt_]() -> std::tuple<double, double, double> {
        const auto& text = rt->text();
        const std::string aid = text.active_layer_id();
        const oa::render::MessageLayer* ml = text.layer(aid);
        if (!ml) return {0.0, 0.0, 0.0};
        const auto ext = fs_ptr->measure_text_extents(*ml);
        // M10c probe: which layer a sync metric var actually measured.
        if (std::getenv("OA_DEBUG_VAR")) {
            std::fprintf(stderr, "[var] metric active='%s' units=%zu -> w=%.1f "
                                 "h=%.1f lw=%.1f\n",
                         aid.c_str(), ml->page.size(), ext.width, ext.height,
                         ext.line_width);
        }
        return {ext.width, ext.height, ext.line_width};
    };
}

RenderEngine::~RenderEngine()
{
    // 成员析构序 = 声明逆序：fontSystem 先于 backend_ 销毁（字形纹理销毁时
    // 后端对象仍存活，见 renderer.h 成员注释）。
}

bool RenderEngine::create_renderer(SDL_Window* ctx,
                                   const std::string& backend_kind)
{
    // Logical presentation（注释沿 create_renderer 的后端呈现约定）：引擎
    // 场景 = 项目 stage 尺寸（system.ini [WINDOWS] WIDTH/HEIGHT — 1280x720 for
    // FPM, 1920x1080 for NekoMiko etc.）。窗口以该尺寸创建，但 *pixel* 输出在
    // HiDPI/合成器缩放下可能不同；后端把 stage 等比呈现进真实输出
    // （letterbox；sdl = SDL3 logical presentation，gles = 后端内镜像数学），
    // 绘制坐标恒为引擎坐标；窗口坐标鼠标事件经 window→render 坐标反算
    // （1:1 显示下为恒等映射 — HiDPI/scale 次根因）。
    if (rt_ && rt_->project_.config.stage_width > 0) {
        stage_w_ = rt_->project_.config.stage_width;
        stage_h_ = rt_->project_.config.stage_height;
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
    // (geometry / textures / video layers / emote canvases / group bakes)
    // renders here; render_end presents it into the window and pixel reads
    // sample it, so both stay fixed at stage size regardless of window size.
    // Backends that cannot make render targets keep the legacy
    // direct-to-window path (stage_rt == nullptr).
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
    for (auto& [k, t] : group_tex_cache)
        if (t) backend_->destroy_texture(t);
    group_tex_cache.clear();
    emote_canvas_.clear();
    emote_atlases_.clear();
    textures.clear();
    trans_capture_tex = nullptr;
    trans_rule_tex = nullptr; // rule texture lived in `textures` (destroyed above)
    trans_rule_checked_ = false;
    backend_->shutdown();
}

bool RenderEngine::capture_transition_source()
{
    // GPU-only [trans] capture on the stage-target path.
    // At event-apply time the stage target still holds the previous frame
    // (the last render_end left it there and nothing cleared it since) —
    // that frame IS the "old scene" the fade-out shows, so the capture is a
    // render-target swap + texture blit (copy), zero readback. The CPU
    // snapshot path below survives only for hosts whose backend cannot make
    // render targets (never on the shipped sdl line).
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
        const oa::render::Compositor& sc = rt_->scene();
        for (const oa::render::Layer* l : sc.draw_order()) {
            if (motion) break;
            if (!sc.is_effectively_visible(l->id)) continue;
            // 空容器不算内容(原 file 空 &&
            // !has_color 跳过逐字;角色 content_present 取反)。
            if (!oa::render::content_role_of(*l).content_present(*l)) continue;
            if (!sc.layer_tweens_finished(l->id) || sc.layer_anime_active(l->id))
                motion = true;
        }
        if (motion) {
            if (std::getenv("OA_TRANSDBG"))
                std::printf("[trans] motion: overlay skipped (type=%d)\n",
                            rt_->transition().type());
            return true; // captured-but-empty: 时钟走满, 无 overlay 可画
        }
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

bool RenderEngine::read_host_frame(const oa::render::TextureKey& key, oa::media::Image& out)
{
    if (!backend_ || key.empty()) return false;
    TextureRef tex = texture_for_key(key); // host-frame domains are cache-only
    if (!tex) return false;
    TextureRef prev_target = backend_->current_target();
    backend_->set_target(tex);
    const bool ok = snapshot_renderer(out);
    backend_->set_target(prev_target);
    return ok;
}

bool RenderEngine::read_window_surface(oa::media::Image& out)
{
    // Diagnostics (presentation probes only): the presented window surface.
    // On the stage-target path the scene target is restored afterwards —
    // snapshots in the main loop keep hitting the stage target, never this
    // read.
    if (!backend_) return false;
    TextureRef prev_target = backend_->current_target();
    if (stage_rt && prev_target == stage_rt) backend_->set_target(nullptr);
    const bool ok = snapshot_renderer(out);
    if (stage_rt && prev_target == stage_rt) backend_->set_target(stage_rt);
    return ok;
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

const char* RenderEngine::renderer_error() const
{
    return backend_ ? backend_->last_error() : "";
}

double RenderEngine::frame_luma()
{
    if (!backend_) return -1.0;
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    if (!backend_->read_target(&w, &h, &px)) return -1.0;
    if (w <= 0 || h <= 0) return -1.0;
    double sum = 0;
    size_t cnt = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = px.data() + size_t(y) * size_t(w) * 4;
        for (int x = 0; x < w; ++x) {
            sum += (uint32_t(row[x * 4]) + row[x * 4 + 1] + row[x * 4 + 2]) / 3;
            ++cnt;
        }
    }
    return cnt ? sum / double(cnt) : 0.0;
}

bool RenderEngine::get_renderer_coordinates(float winx, float winy, float* rx, float* ry)
{
    return backend_ && backend_->window_to_render(winx, winy, rx, ry);
}

bool RenderEngine::stage_to_window_coordinates(float sx, float sy, float* wx, float* wy)
{
    return backend_ && backend_->render_to_window(sx, sy, wx, wy);
}

std::optional<std::vector<uint8_t>> RenderEngine::overlay_read(const std::string& rel)
{
    // Only names inside the logical savepath may come from the save store;
    // everything else falls through to the asset filesystem (resolve_image).
    const std::string& sp = rt_->savepath();
    if (sp.empty() || rel.empty()) return std::nullopt;
    if (!(rel == sp || (sp.size() < rel.size() && rel.rfind(sp + "/", 0) == 0)))
        return std::nullopt;
    const auto& store = rt_->save_store();
    if (!store) return std::nullopt;
    return store->read(rel);
}

size_t RenderEngine::draw_node_text(const oa::render::Layer& node,
                                    const oa::render::Affine2& world, double opacity)
{
    // draw the glyphs of every message layer bound to this scene
    // node at the node's own slot in the draw order — the single text paint
    // point. Glyphs are scene content: later layers cover them, node
    // hide/delete removes them, group composites/masks/clips apply like for
    // images. The old always-last scene-wide pass (and with it the "missing
    // node" absolute fallback) is gone: every drawable message owns a node
    // (materialized by the runtime). The per-node drawable list is
    // resolved once per frame in draw_scene (frame_node_messages_), so this
    // draw point stays a pure slot lookup — membership and visibility are
    // unchanged because scene/text state cannot change during a draw.
    if (!backend_) return 0;
    size_t drawn = 0;
    const auto it = frame_node_messages_.find(node.id);
    if (it == frame_node_messages_.end()) return 0;
    for (const std::string& id : it->second) {
        const oa::render::MessageLayer* ml = rt_->text().layer(id);
        if (!ml) continue;
        drawn += fontSystem->draw_text_layer(backend_.get(), *ml, world, opacity);
    }
    frame_glyphs_ += drawn;
    return drawn;
}

// ---------------------------------------------------------------------------
// draw-node split + intermediate-group local compositing
// (declared in the renderer.h private section). 结构: draw_node 收敛为调度器,
// 需要真离屏合成的中间组走组路径
// (draw_group_node: resolve_group_plan → 烘焙缓存命中则直接复合, 未命中则
// 离屏 target 会话 + 子树绘制 + readback + group_composite_cpu + 上传入缓存),
// 其余节点走普通路径 (draw_plain_node)。组烘焙是节点局部策略: 中间组 =
// 离屏 LayerBitmap 会话, group_tex_cache = 组合 Bitmap 缓存, 复合按 layermode
// 把烘焙结果叠回宿主层 —— 与主循环完全解耦 (烘焙只含 straight-RGBA 内容;
// alpha/blend/clip 在复合时应用, alpha 补间永不重烘焙).
// ---------------------------------------------------------------------------

RenderEngine::GroupPlan RenderEngine::resolve_group_plan(
    const oa::render::Compositor& sc, const oa::render::Layer& layer,
    std::optional<IRect> inherited) {
    // scissor = inherited ∩ this group's own clip rect (its clip is the
    // offscreen target size, subtree_clip_bounds).
    GroupPlan p;
    p.clip = inherited;
    if (const std::string* cv = layer_prop(layer, "clip")) {
        std::array<double, 4> c{0, 0, 0, 0};
        int used = std::sscanf(cv->c_str(), "%lf,%lf,%lf,%lf", &c[0], &c[1],
                               &c[2], &c[3]);
        if (used == 4) {
            if (const auto r =
                    world_aabb_local(sc, layer, c[0], c[1], c[2], c[3])) {
                // A content-crop
                // group (file-less container; clip = subtree crop window) is
                // confined to its ANCHOR FRAME — the backdrop quad(s) drawn
                // before it that start at the same world origin. btjy backlog
                // face containers sit exactly on their row panel (both start
                // at (row x, row y)), so the shader_crop full-frame window
                // (0,0,1600,900) crops to the row band (1144x228): the
                // 310%-zoomed bust no longer spills below the row over the
                // following rows / page bottom. Crop groups without a
                // coincident backdrop (favoface-style windows offset into
                // their art) keep the unchanged crop.
                std::optional<IRect> crop = r;
                // "无自纹理内容的容器"判定 =
                // 角色 textured_content 取反(原 layer.file.empty() 逐字;
                // anchor-frame 收纳只对 file 空裁剪组)。
                if (!oa::render::content_role_of(layer).textured_content(layer)) {
                    if (const auto f = anchor_frame(sc, layer)) {
                        const IRect a = *crop, b = *f;
                        const int lx = std::max(a.x, b.x);
                        const int ly = std::max(a.y, b.y);
                        const int rx = std::min(a.x + a.w, b.x + b.w);
                        const int ry = std::min(a.y + a.h, b.y + b.h);
                        if (rx > lx && ry > ly)
                            crop = IRect{lx, ly, rx - lx, ry - ly};
                    }
                }
                if (p.clip) {
                    const IRect a = *p.clip, b = *crop;
                    const int lx = std::max(a.x, b.x);
                    const int ly = std::max(a.y, b.y);
                    const int rx = std::min(a.x + a.w, b.x + b.w);
                    const int ry = std::min(a.y + a.h, b.y + b.h);
                    if (rx <= lx || ry <= ly) {
                        p.clip = IRect{0, 0, 0, 0};
                    } else {
                        p.clip = IRect{lx, ly, rx - lx, ry - ly};
                    }
                } else {
                    p.clip = crop;
                }
            }
        }
    }
    // draw-time params: group alpha stays OUT of the bake (alpha-mod at
    // composite time — exact: straight rgb * (a*alpha)).
    p.group_alpha = layer.alpha;
    std::array<double, 3> cm = {1.0, 1.0, 1.0};
    if (const std::string* cmv = layer_prop(layer, "colormultiply")) {
        if (const auto parsed = color_multiply_rgb(*cmv)) cm = *parsed;
    }
    p.color_multiply = cm;
    p.grayscale = prop_true(layer_prop(layer, "grayscale"));
    p.negative = prop_true(layer_prop(layer, "negative"));
    if (const std::string* mv = layer_prop(layer, "intermediate_render_mask")) {
        p.mask = resolve_image(*mv);
    }
    // composite blend onto the parent (layermode uniform)
    if (const std::string* lm = layer_prop(layer, "layermode")) {
        if (*lm == "add" || *lm == "additive")
            p.blend = BlendMode::Add;
        else if (*lm == "screen")
            p.blend = BlendMode::Blend; // approx.
        else if (*lm == "multiply" || *lm == "mul")
            p.blend = BlendMode::Mod;
    }
    return p;
}

void RenderEngine::restore_render_clip(bool had_clip, const IRect& old_clip) {
    if (had_clip) {
        backend_->set_clip(old_clip);
    } else {
        backend_->clear_clip();
    }
}

void RenderEngine::composite_group_bake(TextureRef baked, const GroupPlan& p,
                                        bool had_clip,
                                        const IRect& old_clip) {
    // The bake is cached until a scene mutation / active animation
    // invalidated it: message-window / mask groups are typically static while
    // displayed, so only one CPU composite pass is needed per content change.
    backend_->set_texture_alpha_mod(baked, alpha_byte(p.group_alpha));
    backend_->set_texture_blend(baked, p.blend);
    if (p.clip && p.clip->w > 0 && p.clip->h > 0) {
        backend_->set_clip(*p.clip);
    }
    const FRect full{0, 0, float(stage_w_), float(stage_h_)};
    backend_->draw_texture(baked, nullptr, &full);
    restore_render_clip(had_clip, old_clip);
}

TextureRef RenderEngine::begin_offscreen_pass(TextureRef target,
                                              const GroupPlan& p) {
    backend_->set_texture_blend(target, BlendMode::Blend);
    TextureRef saved_target = backend_->current_target();
    backend_->set_target(target);
    backend_->set_draw_blend(BlendMode::None);
    backend_->set_draw_color(0, 0, 0, 0);
    backend_->clear();
    backend_->set_draw_blend(BlendMode::Blend);
    if (p.clip && p.clip->w > 0 && p.clip->h > 0) {
        backend_->set_clip(*p.clip);
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
                                       const GroupPlan& p,
                                       std::vector<uint8_t>* out_px) {
    // CPU composite:
    // un-premultiply the group target, apply the filters and (stretched)
    // mask, and store straight rgb with the baked alpha = content coverage
    // A * maskA (group alpha applied at draw time).
    //
    // intermediate_render=2 ("opaque" mode) must NOT force alpha 1 on empty
    // bake pixels: the offscreen target is cleared to (0,0,0,0), so a group
    // whose subtree does not cover its whole (often stage-wide, unclipped)
    // rect would composite rgb=0,alpha=1 — an opaque black quad over every
    // pixel outside the drawn content. The mw-face / backlog-row mask
    // containers (maskface/maskblog groups with small oval art) triggered
    // exactly that: the whole scene behind the avatar / backlog went black.
    // Content coverage keeps the group transparent where its
    // subtree painted nothing; full-stage opaque roots (the 1.0 scene group)
    // are unaffected visually because they draw first over the stage target
    // that render_beigin already cleared to opaque black.
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
            if (p.mask) {
                const int mx = int((double(x) / stage_w) * p.mask->w);
                const int my = int((double(y) / stage_h) * p.mask->h);
                const size_t mi =
                    (size_t(std::clamp(my, 0, p.mask->h - 1)) *
                         size_t(p.mask->w) +
                     size_t(std::clamp(mx, 0, p.mask->w - 1))) *
                        4 +
                    3;
                maskA = p.mask->rgba[mi] / 255.0;
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

void RenderEngine::draw_plain_node(const oa::render::Compositor& sc,
                                   const oa::render::SceneNode& node,
                                   const oa::render::Affine2& world,
                                   int intermediate, double parent_op,
                                   std::optional<IRect> scissor,
                                   size_t* drawn_out) {
    const oa::render::Layer& layer = node.layer;
    // Plain path: own content and bound glyphs at this node's slot, then
    // the child subtree in pre-order (an intermediate_render group that needs
    // no offscreen pass draws inline at its group alpha — "over" is
    // associative, so this is pixel-identical to the offscreen composite).
    const double own_op = parent_op * (intermediate != 0 ? 1.0 : layer.alpha);
    *drawn_out += draw_one(layer, world, own_op);
    // glyphs of the node's bound message layer(s) paint here, inside
    // the node's draw slot (covered by any later layer / blended into the
    // group composite like the node's own image content).
    *drawn_out += draw_node_text(layer, world, own_op);
    for (const oa::render::SceneNode* c : node.children)
        draw_node(sc, *c, world, own_op, scissor, drawn_out);
}

void RenderEngine::draw_group_node(const oa::render::Compositor& sc,
                                   const oa::render::SceneNode& node,
                                   const oa::render::Affine2& world,
                                   double parent_op,
                                   std::optional<IRect> scissor,
                                   size_t* drawn_out) {
    const oa::render::Layer& layer = node.layer;
    // ---- offscreen group composite (node-local strategy) ----------
    GroupPlan p = resolve_group_plan(sc, layer, scissor);
    const bool had_clip = backend_->clip_enabled();
    IRect old_clip{};
    if (had_clip) old_clip = backend_->clip_rect();
    const auto cmit = group_tex_cache.find(layer.id);
    if (cmit != group_tex_cache.end() && cmit->second) {
        // The bake already contains the subtree glyphs.
        composite_group_bake(cmit->second, p, had_clip, old_clip);
        return;
    }
    TextureRef group_tex = backend_->create_texture(stage_w_, stage_h_,
                                                    TextureAccess::Target);
    if (!group_tex) {
        // no offscreen possible: naive fallback (children with the group
        // alpha multiplied; only reachable when the backend cannot make
        // render targets).
        const double fallback_op = parent_op * layer.alpha;
        *drawn_out += draw_one(layer, world, fallback_op);
        for (const oa::render::SceneNode* c : node.children)
            draw_node(sc, *c, world, fallback_op, scissor, drawn_out);
        return;
    }
    // group's own content and subtree, group alpha excluded
    TextureRef saved_target = begin_offscreen_pass(group_tex, p);
    *drawn_out += draw_one(layer, world, parent_op);
    // the group's own glyphs bake into the offscreen target (later
    // layers and the composite pass then blend them like any other group
    // content).
    *drawn_out += draw_node_text(layer, world, parent_op);
    for (const oa::render::SceneNode* c : node.children)
        draw_node(sc, *c, world, parent_op, p.clip, drawn_out);
    // read the group target back for the CPU composite
    int gw = 0, gh = 0;
    std::vector<uint8_t> px;
    if (!backend_->read_target(&gw, &gh, &px) || gw != stage_w_ ||
        gh != stage_h_) {
        // failure / size mismatch → all-zero stage buffer（后端抽象前的
        // read_target_pixels 同语义）。
        px.assign(size_t(stage_w_) * stage_h_ * 4, 0);
    }
    // CPU composite → straight RGBA (bake content only; alpha at draw time)
    std::vector<uint8_t> out_px(size_t(stage_w_) * stage_h_ * 4, 0);
    group_composite_cpu(px, stage_w_, stage_h_, p, &out_px);
    end_offscreen_pass(saved_target, had_clip, old_clip);
    // upload the baked texture into the cache and blend once
    TextureRef baked = upload_baked_texture(out_px, stage_w_, stage_h_);
    if (baked) {
        group_tex_cache[layer.id] = baked; // owns it until invalidation
        composite_group_bake(baked, p, had_clip, old_clip);
    }
    backend_->destroy_texture(group_tex);
}

void RenderEngine::draw_node(const oa::render::Compositor& sc,
                             const oa::render::SceneNode& node,
                             const oa::render::Affine2& parent_world,
                             double parent_op, std::optional<IRect> scissor,
                             size_t* drawn_out) {
    const oa::render::Layer& layer = node.layer;
    // dispatcher: 前序槽位语义不变 — 节点先自内容后子树; 需要真离屏
    // 合成的中间组走组路径, 其余走普通路径. 沿 Compositor 显式树
    // 递归(node.children 已按 compare_ids 序), 免每帧前缀重建.
    if (!sc.is_effectively_visible(layer.id)) return;
    const oa::render::Affine2 w = parent_world * layer.local_transform();
    if (group_path_required(layer)) {
        draw_group_node(sc, node, w, parent_op, scissor, drawn_out);
        return;
    }
    draw_plain_node(sc, node, w, intermediate_mode(layer), parent_op,
                    scissor, drawn_out);
}

size_t RenderEngine::draw_scene(const oa::render::Compositor& sc, double global_fade) {
    // ONE recursive traversal over the Compositor's explicit
    // SceneNode tree (roots → node.children), passed by const& down the
    // recursion — no draw_order snapshot and no per-frame prefix-based
    // SceneTree rebuild anymore (the prefix machinery is gone; sibling
    // tables are compare_ids-sorted, so this pre-order is the flat draw
    // order, scene creation keeps parents before children).
    if (!backend_) return 0;
    size_t drawn = 0;
    // resolve the node → drawable-message map ONCE per frame instead
    // of re-scanning every visible content layer at every node. Equivalence:
    // drawability (bound node + is_message_layer_visible) is a pure function
    // of scene/text state, and nothing mutates either during a draw — scene
    // and text changes land in GameRuntime::tick before the render block,
    // and this traversal already holds raw node pointers across recursion
    // (a mid-draw mutation would be UB), so every draw-time read sees exactly
    // this snapshot's state. Per-node draw order is preserved because the
    // outer visible_content_layers iteration order is kept per node. The
    // scene queries stay on rt_->scene, the object draw_node_text always
    // consulted (the app's only call site passes rt->scene itself).
    frame_node_messages_.clear();
    for (const std::string& mid : rt_->text().visible_content_layers()) {
        // is_message_layer_drawable = 有效可见 + 绘制盒/脚本节点门槛（孤儿
        // 打印:无 [font] 排版盒、节点仅由消息绑定自动物化 ⇒ 不注入字形）。
        const oa::render::MessageLayer* ml = rt_->text().layer(mid);
        if (!rt_->scene().is_message_layer_drawable(mid, ml && ml->positioned))
            continue;
        frame_node_messages_[rt_->scene().bound_scene_id(mid)].push_back(mid);
    }
    const oa::render::Affine2 root_w = sc.root_props().local_transform();
    const double root_op = sc.root_props().alpha * global_fade;
    for (const oa::render::SceneNode* r : sc.roots()) {
        draw_node(sc, *r, root_w, root_op, std::nullopt, &drawn);
    }
    // 消息槽区在场景区之后绘制(旧 openartemis-<hex> 命名 +
    // compare_ids message-last 排序垫底的显式化;槽节点画绑定消息的字形,
    // 其上的宿主叠层/过渡逻辑不变)。
    for (const oa::render::SceneNode* s : sc.message_slots()) {
        draw_node(sc, *s, root_w, root_op, std::nullopt, &drawn);
    }
    // Overlay-video slot: structurally topmost (outside the script tree /
    // message region); drawn in stage space, ignoring root-props transform.
    if (const oa::render::SceneNode* ov = sc.overlay_node()) {
        draw_node(sc, *ov, oa::render::Affine2{}, 1.0f, std::nullopt, &drawn);
    }
    return drawn;
}

const oa::media::Image* RenderEngine::resolve_image(const std::string& name)
{
    if (name.empty()) return nullptr;
    const auto it = decoded.find(name);
    if (it != decoded.end()) {
        decoded_use_[name] = ++cache_stamp_; // LRU refresh
        return &it->second;
    }
    std::string resolved = rt_->interpreter().resolve_magic_path(name);
    std::optional<std::vector<uint8_t>> bytes;
    // Extension-less layer files probe the real archive formats:
    // Android/iOS thyt-class data ships story bgs (image/bg) as .jpg while
    // ev/fg/cg/rule/title stay .png; exact filenames (raw `resolved`) always
    // win first, then .png, then .jpg (png-first keeps legacy archives —
    // fpm/NekoMiko, sole-.png bgs — byte-identical behavior).
    for (const std::string& cand : { resolved, resolved + ".png", resolved + ".jpg" }) {
        if (auto b = overlay_read(cand)) {
            bytes = b;
            break;
        }
        if (auto b = fs_->read(cand)) {
            bytes = b;
            break;
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
    static const size_t cap_mb = [] {
        if (const char* v = std::getenv("OA_DECODED_CACHE_MB")) {
            const long n = std::atol(v);
            if (n >= 0) return size_t(n);
        }
        return size_t(1024);
    }();
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

bool RenderEngine::emote_render_parts(const oa::render::TextureKey& key,
                                      const oa::emote::EmoteFile& file,
                                      const std::vector<oa::emote::EmoteDrawPart>& parts,
                                      int w, int h) {
    if (!backend_ || w <= 0 || h <= 0 || key.empty()) return false;
    // PSB identity in the atlas key (research/132). OA_EMOTE_ATLAS_KEY=legacy
    // keeps the pre-fix layer-only key so the old behaviour stays reachable as
    // an A/B arm. Latched once, like the other emote switches.
    static const bool atlas_key_legacy = [] {
        const char* v = std::getenv("OA_EMOTE_ATLAS_KEY");
        return v && std::strcmp(v, "legacy") == 0;
    }();
    const uint64_t file_uid = atlas_key_legacy ? 0 : file.uid();
    // Drop this layer's textures that belong to another PSB (the game swapped
    // the character file behind the same layer id): they are unreachable now
    // and would otherwise leak one atlas set per costume change.
    if (!atlas_key_legacy) {
        for (auto it = emote_atlases_.begin(); it != emote_atlases_.end();) {
            if (it->first.layer == key && it->first.file != file_uid) {
                if (std::getenv("OA_EMOTE_DEBUG"))
                    std::fprintf(stderr,
                                 "[emote] stale atlas dropped layer='%s' src=%d "
                                 "file_uid=%llu -> %llu\n",
                                 key.name.c_str(), it->first.source,
                                 (unsigned long long)it->first.file,
                                 (unsigned long long)file_uid);
                if (it->second) backend_->destroy_texture(it->second);
                it = emote_atlases_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // canvas (offscreen target) registered under the layer's TextureKey
    TextureRef canvas = nullptr;
    const auto ct = emote_canvas_.find(key);
    if (ct != emote_canvas_.end()) canvas = ct->second;
    if (!canvas) {
        canvas = backend_->create_texture(w, h, TextureAccess::Target);
        if (!canvas) return false;
        backend_->set_texture_blend(canvas, BlendMode::Blend);
        // take over the key from any CPU-uploaded texture
        const auto old = textures.find(key);
        if (old != textures.end() && old->second != canvas) {
            if (emote_canvas_.find(key) == emote_canvas_.end())
                backend_->destroy_texture(old->second);
            textures.erase(old);
        }
        emote_canvas_[key] = canvas;
        textures[key] = canvas;
    }
    // draw into the canvas: clear transparent, then parts in draw order.
    // Stencil-composite parts (type-12 eye masks) split off: per group the
    // mask shapes render into one scratch target, the content into another,
    // the mask multiplies the content's alpha, and the result lands on the
    // canvas — SDL geometry has no stencil, so the E-mote stencil composite
    // is a two-offscreen alpha multiply.
    TextureRef prev_target = backend_->current_target();
    backend_->set_target(canvas);
    backend_->set_draw_blend(BlendMode::None);
    backend_->set_draw_color(0, 0, 0, 0);
    backend_->clear();
    backend_->set_draw_blend(BlendMode::Blend);
    backend_->clear_clip();
    // the staging buffer is reused across the part loop (and grows once): a
    // per-part vector allocated a fresh 2400-vertex buffer per warped part
    // per pose (~30 poses/s per layer).
    std::vector<Vertex> stage;

    struct StencilBucket {
        std::vector<const oa::emote::EmoteDrawPart*> mask;
        std::vector<const oa::emote::EmoteDrawPart*> content;
    };
    std::map<int, StencilBucket> stencil;
    std::vector<const oa::emote::EmoteDrawPart*> plain;
    for (const auto& part : parts) {
        if (part.mask_role == 1)
            stencil[part.mask_group].mask.push_back(&part);
        else if (part.stencil_group >= 0)
            stencil[part.stencil_group].content.push_back(&part);
        else
            plain.push_back(&part);
    }

    // one part draw, translated by (ox,oy) scratch-space offset
    auto draw_part = [&](const oa::emote::EmoteDrawPart& part,
                         float ox, float oy) {
        if (part.source < 0 || part.source >= int(file.sources.size())) return;
        const oa::emote::EmoteSource& src = *file.sources[size_t(part.source)];
        if (part.icon < 0 || part.icon >= int(src.icons.size())) return;
        if (part.verts.size() < 3) return;
        const RenderEngine::EmoteAtlasKey akey{key, part.source, file_uid};
        TextureRef atlas = nullptr;
        const auto ai = emote_atlases_.find(akey);
        if (ai != emote_atlases_.end()) atlas = ai->second;
        if (!atlas) {
            std::string err;
            if (!file.ensure_atlas(const_cast<oa::emote::EmoteSource*>(&src), &err))
                return;
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

    for (const oa::emote::EmoteDrawPart* part : plain)
        draw_part(*part, 0.0f, 0.0f);

    // stencil groups: two-offscreen alpha multiply
    for (auto& [gid, bucket] : stencil) {
        if (bucket.content.empty()) continue;
        // group bbox over mask + content vertices (canvas space)
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        bool any_v = false;
        for (const auto* plist : {&bucket.mask, &bucket.content})
            for (const auto* part : *plist)
                for (const auto& v : part->verts) {
                    any_v = true;
                    x0 = std::min(x0, v.x);
                    y0 = std::min(y0, v.y);
                    x1 = std::max(x1, v.x);
                    y1 = std::max(y1, v.y);
                }
        if (!any_v) continue;
        int bx0 = std::max(0, int(std::floor(x0)));
        int by0 = std::max(0, int(std::floor(y0)));
        int bx1 = std::min(w - 1, int(std::ceil(x1)));
        int by1 = std::min(h - 1, int(std::ceil(y1)));
        if (bx0 > bx1 || by0 > by1) continue;
        const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
        if (stencil_scratch_w_ != bw || stencil_scratch_h_ != bh) {
            release_stencil_scratch();
            for (auto& t : stencil_scratch_) {
                t = backend_->create_texture(bw, bh, TextureAccess::Target);
                if (!t) break;
            }
            if (!stencil_scratch_[0] || !stencil_scratch_[1]) {
                release_stencil_scratch();
                continue;
            }
            stencil_scratch_w_ = bw;
            stencil_scratch_h_ = bh;
        }
        // pass 1: mask shapes into scratch 0 (straight alpha accumulation)
        backend_->set_target(stencil_scratch_[0]);
        backend_->set_draw_blend(BlendMode::None);
        backend_->set_draw_color(0, 0, 0, 0);
        backend_->clear();
        backend_->set_draw_blend(BlendMode::Blend);
        backend_->clear_clip();
        for (const auto* part : bucket.mask) draw_part(*part, -float(bx0), -float(by0));
        // pass 2: content into scratch 1
        backend_->set_target(stencil_scratch_[1]);
        backend_->set_draw_blend(BlendMode::None);
        backend_->set_draw_color(0, 0, 0, 0);
        backend_->clear();
        backend_->set_draw_blend(BlendMode::Blend);
        backend_->clear_clip();
        for (const auto* part : bucket.content)
            draw_part(*part, -float(bx0), -float(by0));
        // pass 3: mask multiplies the content's alpha (color untouched)
        backend_->set_texture_blend(stencil_scratch_[0], BlendMode::AlphaMultiply);
        backend_->draw_texture(stencil_scratch_[0], nullptr, nullptr);
        // pass 4: composited content onto the emote canvas at its bbox
        backend_->set_target(canvas);
        backend_->set_draw_blend(BlendMode::Blend);
        const FRect dst{float(bx0), float(by0), float(bw), float(bh)};
        backend_->draw_texture(stencil_scratch_[1], nullptr, &dst);
        backend_->set_texture_blend(stencil_scratch_[0], BlendMode::Blend);
    }
    backend_->set_target(prev_target);
    return true;
}

// ---------------------------------------------------------------------------
// Host-frame pump (see renderer.h). The former app-side emote/video pump
// pair: one pass over the runtime's media state that streams every fresh
// frame into the textures the scene draw resolves. Order preserved from the
// app loop (emote first, then video) so the redraw gate sees the same
// per-frame flags.
// ---------------------------------------------------------------------------

/// upload_host_frame with the luma-key transform fused into the texture
/// fill: alpha = layer_video_key_alpha(luma) per pixel, RGB verbatim — the
/// same bytes key_overlay_frame produced, without the intermediate
/// full-frame copy (one 8.3 MB pass saved per keyed 1080p frame).
bool RenderEngine::upload_host_frame_keyed(const oa::render::TextureKey& key,
                                           int w, int h, const uint8_t* rgba) {
    if (!backend_ || key.empty() || w <= 0 || h <= 0 || !rgba) return false;
    // mode switch back to CPU uploads: drop any GPU canvas / stale texture
    // under this key first (same lifecycle as upload_host_frame).
    const auto cc = emote_canvas_.find(key);
    if (cc != emote_canvas_.end()) {
        textures.erase(key);
        backend_->destroy_texture(cc->second);
        emote_canvas_.erase(cc);
    }
    const auto it = textures.find(key);
    if (it != textures.end()) {
        backend_->destroy_texture(it->second);
        textures.erase(it);
    }
    TextureRef tex = backend_->create_texture(w, h, TextureAccess::Streaming);
    if (!tex) return false;
    backend_->set_texture_blend(tex, BlendMode::Blend);
    textures[key] = tex;
    uint8_t* px = nullptr;
    int pitch = 0;
    if (!backend_->lock_texture(tex, &px, &pitch)) return false;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = px + size_t(y) * size_t(pitch);
        const uint8_t* src = rgba + size_t(y) * size_t(w) * 4;
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = src + size_t(x) * 4;
            uint8_t* d = row + size_t(x) * 4;
            d[0] = p[0];
            d[1] = p[1];
            d[2] = p[2];
            d[3] = oa::media::layer_video_key_alpha(oa::media::rgba_luma(p));
        }
    }
    backend_->unlock_texture(tex);
    return true;
}

bool RenderEngine::pump_host_frames(bool* emote_presented) {
    if (emote_presented) *emote_presented = false;
    if (!rt_) return false;
    bool any = false;

    // ---- emote layers ------------------------------------------------------
    // GPU compositing by default when a renderer exists (the host latched
    // emote_default_external_pose at startup); OA_EMOTE_GPU=0 restores the
    // CPU canvas upload. In GPU mode the players skip their own CPU raster
    // (external-pose mode) so pose updates stay cheap.
    const bool gpu = oa::emote::emote_default_external_pose() && renderer_ok();
    for (const auto& [id, st] : rt_->emote_layers()) {
        if (!st.player) continue;
        st.player->set_external_pose(gpu);
        if (st.revision == 0 || pump_emote_rev_[id] == st.revision) continue;
        // 读取域键 = 角色实例决定(emote 画布域,名 = 层 id;与场景侧
        // content_role_of(layer).texture_key 同源单点)。
        const oa::render::TextureKey tkey = oa::render::EmoteContent::canvas_key(id);
        bool presented = false;
        if (gpu) {
            std::vector<oa::emote::EmoteDrawPart> parts;
            std::string err;
            if (st.player->collect_pose_parts(&parts, &err) &&
                emote_render_parts(tkey, st.player->file(), parts, st.width,
                                   st.height)) {
                presented = true;
            }
        }
        if (!presented) {
            const auto& rgba = st.player->rgba();
            if (upload_host_frame(tkey, st.width, st.height, rgba.data()))
                presented = true;
        }
        if (presented) {
            pump_emote_rev_[id] = st.revision;
            // layer-model 簿记: 实际上传/GPU 合成 = HostFrame 内容写
            // （与 video 帧同款；redraw 门由返回值消费）。
            rt_->scene().mark_dirty(id, oa::render::DirtyAspect::HostFrame);
            if (emote_presented) *emote_presented = true;
            any = true;
        }
    }

    // ---- video layer channels + overlay ------------------------------------
    oa::media::VideoEngine& ve = rt_->video();
    const auto vsnap = ve.state();
    std::set<std::string> live_video;
    for (const auto& [id, ch] : vsnap.video_layers) live_video.insert(id);
    for (const auto& [id, ch] : vsnap.video_layers) {
        if (!ch.playing || !ch.decoded) continue;
        any = true;
        const uint64_t rev = ve.frame_revision(id);
        if (rev == 0 || pump_video_rev_[id] == rev) continue;
        int w = 0, h = 0;
        const uint8_t* rgba = nullptr;
        // A channel with an auto-detected `_m` mask partner (ch.mask_on)
        // already carries the engine-level composite alpha (luma-key of the
        // main picture x mask gray), so its frames upload verbatim;
        // unmasked channels get the fused luma-key fill.
        bool ok = ve.video_frame(id, &w, &h, &rgba, nullptr);
        if (ok) {
            const oa::render::TextureKey tkey = oa::render::VideoContent::frame_key(id);
            ok = ch.mask_on ? upload_host_frame(tkey, w, h, rgba)
                            : upload_host_frame_keyed(tkey, w, h, rgba);
        }
        if (ok) {
            pump_video_rev_[id] = rev;
            // layer-model 簿记: 实际上传 = HostFrame 内容写(见泵写面)。
            rt_->scene().mark_dirty(id, oa::render::DirtyAspect::HostFrame);
        }
    }
    // overlay video (host-drawn surface, uploaded verbatim).
    if (ve.is_overlay_playing()) {
        any = true;
        const uint64_t rev = ve.frame_revision("");
        if (rev != pump_overlay_rev_) {
            int w = 0, h = 0;
            const uint8_t* rgba = nullptr;
            if (ve.video_frame("", &w, &h, &rgba, nullptr) &&
                upload_host_frame(oa::render::OverlayContent::frame_key(), w, h,
                                  rgba)) {
                pump_overlay_rev_ = rev;
                // layer-model 簿记: overlay 上传 = HostFrame 写
                rt_->scene().mark_dirty(oa::render::kOverlayNodeId,
                                        oa::render::DirtyAspect::HostFrame);
            }
        }
    }

    // ---- evict host-frame surfaces whose owner is gone ---------------------
    // The runtime erases emote players / video channels when their layer is
    // deleted, but the GPU surfaces the pump created for them used to stay
    // in the caches forever: one 1920x1620 canvas + the atlas set per emote
    // layer, the last 8 MB frame per video channel — accumulating on every
    // scene exit. A stopped channel whose layer still exists keeps showing
    // its frozen last frame, so video textures evict only when the scene no
    // longer has the id either (the overlay keeps its last frame by design).
    std::set<std::string> live_emote;
    for (const auto& [id, st] : rt_->emote_layers())
        if (st.player) live_emote.insert(id);
    for (auto it = emote_atlases_.begin(); it != emote_atlases_.end();) {
        if (!live_emote.count(it->first.layer.name)) {
            if (it->second) backend_->destroy_texture(it->second);
            it = emote_atlases_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = emote_canvas_.begin(); it != emote_canvas_.end();) {
        if (!live_emote.count(it->first.name)) {
            textures.erase(it->first); // same surface registered for the scene
            if (it->second) backend_->destroy_texture(it->second);
            it = emote_canvas_.erase(it);
        } else {
            ++it;
        }
    }
    // Any remaining host-frame texture whose owner is gone is stale too —
    // notably the EmoteCanvas-domain streaming texture the CPU-upload
    // fallback creates (it is not registered in emote_canvas_, so the pass
    // above never sees it).
    for (auto it = textures.begin(); it != textures.end();) {
        bool stale = false;
        if (it->first.domain == oa::render::TextureKey::Domain::VideoFrame)
            stale = !live_video.count(it->first.name) &&
                    rt_->scene().find(it->first.name) == nullptr;
        else if (it->first.domain == oa::render::TextureKey::Domain::EmoteCanvas)
            stale = !live_emote.count(it->first.name);
        if (stale) {
            if (it->second) backend_->destroy_texture(it->second);
            it = textures.erase(it);
        } else {
            ++it;
        }
    }
    // the pump bookkeeping itself: drop entries of ids that no longer exist
    for (auto it = pump_emote_rev_.begin(); it != pump_emote_rev_.end();) {
        if (!live_emote.count(it->first)) it = pump_emote_rev_.erase(it);
        else ++it;
    }
    for (auto it = pump_video_rev_.begin(); it != pump_video_rev_.end();) {
        if (!live_video.count(it->first)) it = pump_video_rev_.erase(it);
        else ++it;
    }
    return any;
}

uint64_t RenderEngine::host_video_rev(const std::string& channel) const {
    if (channel.empty()) return pump_overlay_rev_;
    const auto it = pump_video_rev_.find(channel);
    return it == pump_video_rev_.end() ? 0 : it->second;
}

TextureRef RenderEngine::texture_for_asset(const std::string& name)
{
    return texture_for_key(oa::render::asset_key(name));
}

TextureRef RenderEngine::texture_for_key(const oa::render::TextureKey& key)
{
    if (key.empty()) return nullptr;
    const auto it = textures.find(key);
    if (it != textures.end()) return it->second;
    // 只有资源文件域有解码回退;宿主供帧域(video/emote/overlay)由上传面
    // 填充缓存,缺失即"还没上传"(旧 textures-only 语义的域版本)。
    if (!key.is_asset()) return nullptr;
    const oa::media::Image* img = resolve_image(key.name);
    if (!img) return nullptr;
    TextureRef tex = make_texture(*img);
    if (!tex) return nullptr;
    textures[key] = tex;
    return tex;
}

bool RenderEngine::upload_host_frame(const oa::render::TextureKey& key, int w, int h,
                                     const uint8_t* rgba)
{
    // mode switch back to CPU uploads: drop any GPU canvas registered under
    // this key first (the scene texture is re-created as a streaming one).
    const auto cc = emote_canvas_.find(key);
    if (cc != emote_canvas_.end()) {
        textures.erase(key);
        backend_->destroy_texture(cc->second);
        emote_canvas_.erase(cc);
    }
    if (!backend_ || key.empty() || w <= 0 || h <= 0 || !rgba) {
        if (std::getenv("OA_VIDEO_DEBUG"))
            std::fprintf(stderr, "[video] upload rejected backend=%p name='%s' %dx%d rgba=%p\n",
                         (void*)backend_.get(), key.name.c_str(), w, h, (void*)rgba);
        return false;
    }
    // every frame upload RECREATES the streaming texture
    // (destroy + fresh) instead of updating the existing object in place.
    // In-place refreshes of a reused texture (lock write, also update) were
    // observed NOT reaching the presented frame in the app's reference
    // display environment while the identical update pattern stays live in a
    // standalone SDL harness — the video layer kept showing its first
    // upload's pixels. Draw refetches textures by key per frame, so the
    // object churn is invisible to callers; per-frame video/emote textures
    // refresh at their channel rate (30/s at most).
    const auto it = textures.find(key);
    if (it != textures.end()) {
        backend_->destroy_texture(it->second);
        textures.erase(it);
    }
    TextureRef tex = backend_->create_texture(w, h, TextureAccess::Streaming);
    if (!tex) {
        if (std::getenv("OA_VIDEO_DEBUG"))
            std::fprintf(stderr, "[video] CreateTexture failed: %s\n",
                         backend_->last_error());
        return false;
    }
    backend_->set_texture_blend(tex, BlendMode::Blend);
    textures[key] = tex;
    // Per-frame pixel fill: lock + memcpy into the fresh texture (the
    // canonical streaming fill; update was seen failing under the software
    // renderer, so the fresh texture is filled via lock/unlock).
    uint8_t* px = nullptr;
    int pitch = 0;
    if (!backend_->lock_texture(tex, &px, &pitch)) {
        if (std::getenv("OA_VIDEO_DEBUG"))
            std::fprintf(stderr, "[video] LockTexture failed: %s\n",
                         backend_->last_error());
        return false;
    }
    for (int y = 0; y < h; ++y) {
        std::memcpy(px + size_t(y) * size_t(pitch),
                    rgba + size_t(y) * size_t(w) * 4, size_t(w) * 4);
    }
    backend_->unlock_texture(tex);
    return true;
}

bool RenderEngine::texture_uploaded(const oa::render::TextureKey& key) const
{
    if (key.empty()) return false;
    return textures.find(key) != textures.end();
}

std::optional<std::pair<double, double>> RenderEngine::decoded_size(const oa::render::TextureKey& key) const
{
    // ContentSizeSource 的解码面 —— 现状 quad_for_layer / hit_size /
    // alpha_sampler 的 decoded 查找逐条搬移。按域门控 —— decoded 只服务
    // Asset 域(宿主供帧不是可解码资产,旧注释"保留命名空间永不在此"的
    // 结构化版本)。
    if (key.empty() || !key.is_asset()) return std::nullopt;
    const auto di = decoded.find(key.name);
    if (di == decoded.end()) return std::nullopt;
    return std::make_pair(double(di->second.w), double(di->second.h));
}

std::optional<std::pair<double, double>> RenderEngine::uploaded_size(const oa::render::TextureKey& key) const
{
    // ContentSizeSource 的上传面 —— 现状 quad_for_layer 的 textures 查找
    // + backend texture_size 逐条
    // 搬移。归一化:引擎创建纹理尺寸恒可得(create_texture 即知
    // w/h),"上传纹理存在" == "自然尺寸可得",故单查询即可表达原
    // presence+size 两步;texture_size 失败 = 无此纹理。Asset 域不走上传
    // 面(资源纹理由 decoded 面回答)。
    if (key.empty() || key.is_asset()) return std::nullopt;
    const auto it = textures.find(key);
    if (it == textures.end() || !it->second) return std::nullopt;
    float tw = 0, th = 0;
    if (!backend_ || !backend_->texture_size(it->second, &tw, &th) || tw <= 0 || th <= 0)
        return std::nullopt;
    return std::make_pair(double(tw), double(th));
}

oa::render::TextureKey RenderEngine::layer_texture_key(const oa::render::Layer& l) {
    // 资源读取域键由角色实例决定(原
    // layer_file_key 的 path+file 字符串公式升级为 TextureKey)。
    return oa::render::content_role_of(l).texture_key(l);
}

TextureRef RenderEngine::texture_for_masked(const oa::render::Layer& l)
{
    const oa::render::TextureKey base = layer_texture_key(l); // 角色读取域键
    if (base.empty()) return nullptr;
    // mask 合成面只对 Asset 域可达(mask 组合只对可解码资产面可及;
    // 宿主供帧绑定清 mask)—— 域门控与旧"上传纹理无解码像素"同判。
    if (!base.is_asset() || l.mask.empty()) return texture_for_key(base);
    const std::string key = base.name + std::string("\x1f") + l.mask;
    const oa::render::TextureKey masked = oa::render::asset_key(key);
    const auto it = textures.find(masked);
    if (it != textures.end()) return it->second;
    const oa::media::Image* fimg = resolve_image(base.name);
    const oa::media::Image* mimg = resolve_image(l.mask);
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

LocalQuad RenderEngine::quad_for_layer(const oa::render::Layer& l)
{
    // 内容 quad 经角色 content_quad 派生 ——
    // clip 前置 + 角色片段(纹理/纯色/无)的逐字
    // 搬移(content_role.h;content_role_test 矩阵对照参考实现逐字段)。
    // ContentQuad(场景侧,double)在此转为 LocalQuad(render 侧,FRect float):
    // 转换即现状 float(l.clip_*) 的逐字段同形。
    const oa::render::ContentQuad q = oa::render::content_role_of(l).content_quad(l, *this);
    LocalQuad out;
    if (!q.has) return out;
    out.has = true;
    out.w = q.w;
    out.h = q.h;
    out.solid = q.solid;
    if (q.has_src) {
        out.has_src = true;
        out.src = {float(q.src_x), float(q.src_y), float(q.src_w), float(q.src_h)};
    }
    return out;
}

uint8_t RenderEngine::alpha_byte(double a) {
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return uint8_t(a * 255.0 + 0.5);
}

std::optional<std::array<double, 4>> RenderEngine::layer_world_rect(const oa::render::Layer& l) {
    const LocalQuad q = quad_for_layer(l);
    if (!q.has) return std::nullopt;
    double x = 0, y = 0, w = 0, h = 0;
    if (!rt_->scene().world_rect(l.id, q.w, q.h, &x, &y, &w, &h)) return std::nullopt;
    return std::array<double, 4>{x, y, w, h};
}

std::optional<std::pair<double, double>> RenderEngine::hit_size(void* userdata, const oa::render::Layer& l) {
    RenderEngine* ptr = static_cast<RenderEngine*>(userdata);
    // 命中尺寸面经角色分发 —— 读取域键 =
    // 角色 texture_key(原公式收敛),解码面只读 decoded_size(原 hit_size
    // 逐条搬移;宿主供帧域在 decoded 恒 miss ⇒ 无自然尺寸命中
    // —— 现状语义:盒/clip 优先由 Compositor hit_test_all 骨架承担)。
    const oa::render::ContentRole& role = oa::render::content_role_of(l);
    if (!role.textured_content(l)) return std::nullopt;
    return ptr->decoded_size(role.texture_key(l));
}

std::optional<uint8_t> RenderEngine::alpha_sampler(void* userdata, const oa::render::Layer& l, int tx, int ty)
{
    RenderEngine* ptr = static_cast<RenderEngine*>(userdata);
    // 采样面门/键经角色(alpha_sampler
    // 原条件逐条搬移;采样只走解码缓存,宿主供帧回退层 alpha)。
    const oa::render::ContentRole& role = oa::render::content_role_of(l);
    if (!role.textured_content(l)) return std::nullopt;
    const oa::render::TextureKey key = role.texture_key(l);
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
    // Layer events (Create/Delete/SetProps) and the tween events never
    // reach the host: GameRuntime applies them to its scene at dispatch.
    // 文本事件（print/chgmsg/rt/rp/font/...）已在 runtime 派发点被
    // oa::render::TextEngine 消费（apply_text_event），不再到达宿主；宿主
    // 只读 rt->text 排版并绘制。诊断计数迁移到 runtime。
    if (e.kind == K::LayerEventCmd) {
        // [anime]/[video] (LayerEventCmd) stay recognized no-ops: the
        // frame animation state machine lives in the Compositor. Tween tags
        // and the [lyevent] registrations were already consumed by the
        // runtime scene.
    }
    else if (e.kind == K::Trans) {
        // [trans]: start the semantic transition in the runtime (type 0
        // clears, type != 0 captures the old frame & pauses the script).
        ++trans_events;
        if (trans_events <= 8) {
            std::string dbg;
            for (const auto& [k, v] : e.params) dbg += k + "=" + v + " ";
            std::printf("[app] trans event #%d f=%llu params: %s\n",
                        (int)trans_events, (unsigned long long)rt_->now_ms(),
                        dbg.c_str());
        }
        rt_->transition_begin(e.params);
    }
    else if (e.kind == K::Flip) {
        // [flip] = repaint request ("commit layer changes now"): it must NOT
        // cancel an in-flight transition. FPM flows repaint
        // constantly (each ui_message/estag step queues flips) and UI
        // entrances (uiopenanime -> uitrans -> [trans], 150..300ms crossfade)
        // were being truncated ~1 frame after they began — every flip killed
        // the capture overlay, so the UI screens appeared without their
        // entrance transition. The overlay now lives until the transition
        // ends (progress_transition drops it on completion; a type-0 [trans]
        // or a new [trans] replaces it).
    }
}

TextureRef RenderEngine::solid_texture(const oa::render::Layer& l)
{
    const uint32_t key = (uint32_t(l.solid_rgba[0]) << 24) |
        (uint32_t(l.solid_rgba[1]) << 16) |
        (uint32_t(l.solid_rgba[2]) << 8) | l.solid_rgba[3];
    auto it = solid_tex_cache.find(key);
    if (it != solid_tex_cache.end()) return it->second;
    uint8_t px[4] = { l.solid_rgba[0], l.solid_rgba[1], l.solid_rgba[2], 255 };
    TextureRef tex = backend_->create_texture(1, 1, TextureAccess::Static);
    if (!tex) return nullptr;
    backend_->update_texture(tex, px, 4);
    backend_->set_texture_blend(tex, BlendMode::Blend);
    solid_tex_cache[key] = tex;
    return tex;
}

const std::string* RenderEngine::layer_prop(const oa::render::Layer& l, const char* k)
{
    const auto it = l.props.find(k);
    return it == l.props.end() ? nullptr : &it->second;
}
int RenderEngine::intermediate_mode(const oa::render::Layer& l) {
    // 组语义开关 = 角色方法(原 stoi 语义逐字搬入
    // content_role.h intermediate_mode;与
    // layer_kind.h intermediate_render_nonzero 同源)。
    return oa::render::content_role_of(l).intermediate_mode(l);
}
bool RenderEngine::prop_true(const std::string* v) {
    return v && (*v == "1" || *v == "true" || *v == "on" || *v == "yes");
}

bool RenderEngine::group_needs_offscreen(const oa::render::Layer& l)
{
    if (intermediate_mode(l) == 0) return false;
    if (intermediate_mode(l) == 2) return true;
    if (l.alpha < 1.0 - 1e-9) return true;
    if (layer_prop(l, "colormultiply")) return true;
    if (prop_true(layer_prop(l, "grayscale"))) return true;
    if (prop_true(layer_prop(l, "negative"))) return true;
    if (layer_prop(l, "intermediate_render_mask")) return true;
    // File-less container clip = the group's CONTENT crop window (Artemis
    // container semantics, favoface-family rows): an
    // intermediate group with an explicit clip composites offscreen and only
    // the clip rect (world rect of the local clip x,y,w,h) is kept. Without
    // this rule such a group draws inline, its clip never applies to the
    // subtree, and oversized content overflows the window — btjy backlog
    // face containers (intermediate_render=1 + clip from the shader_crop
    // pipeline) painted their 310%-zoomed busts across the neighboring rows.
    // Texture layers keep clip = texture source sub-rect (quad_for_layer),
    // so only file-less containers take the crop path.
    // 该门 = 角色 clip_crop_window(file 空 &&
    // has_clip;content_role.h 逐字搬移)。
    if (oa::render::content_role_of(l).clip_crop_window(l)) return true;
    const std::string* lm = layer_prop(l, "layermode");
    if (lm && *lm != "alpha" && !lm->empty()) return true;
    return false;
}
bool RenderEngine::group_path_required(const oa::render::Layer& l)
{
    return intermediate_mode(l) != 0 && group_needs_offscreen(l);
}

void RenderEngine::render_beigin()
{
    if (!backend_) return;
    if (stage_rt) {
        backend_->set_target(stage_rt);
        backend_->clear_clip();
    }
    backend_->set_draw_color(0, 0, 0, 255);
    backend_->clear();
    // per-frame glyph count (single paint point = node slot).
    frame_glyphs_ = 0;
}
void RenderEngine::render_clear_tex_cache()
{
    if (!backend_) return;
    for (auto& [k, t] : group_tex_cache)
        if (t) backend_->destroy_texture(t);
    group_tex_cache.clear();
    // anchor-frame bounds are scene-dependent — they must
    // re-resolve whenever group bakes are invalidated (scene/text/anime/
    // fade/video changed).
    anchor_frame_cache_.clear();
}
size_t RenderEngine::draw_one(const oa::render::Layer& l, const oa::render::Affine2& t,
    double op) {
    // 内容形态准入经角色面分发 ——
    // solid_content(file 空 && has_color && w/h>0)与原纯色分支同门(原
    // 条件逐字搬入 content_role.h),textured_content(file 非空)与
    // 原纹理分支同门;两门互斥,顺序调整不改变任何状态的判定与像素输出。
    const oa::render::ContentRole& role = oa::render::content_role_of(l);
    if (role.solid_content(l)) {
        // lyc solid-color quad (file-less) sized by width/height
        if (t.is_plain_translation()) {
            backend_->set_draw_blend(BlendMode::Blend);
            backend_->set_draw_color(l.solid_rgba[0], l.solid_rgba[1],
                l.solid_rgba[2],
                alpha_byte(l.solid_rgba[3] / 255.0 * op));
            const FRect dst{ float(t.e), float(t.f), float(l.width),
                            float(l.height) };
            backend_->fill_rect(dst);
        }
        else {
            TextureRef stex = solid_texture(l);
            if (!stex) return 0;
            backend_->set_texture_alpha_mod(stex,
                alpha_byte(l.solid_rgba[3] / 255.0 * op));
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            t.transform_point(0, 0, &x0, &y0);
            t.transform_point(l.width, 0, &x1, &y1);
            t.transform_point(0, l.height, &x2, &y2);
            const FPoint o{ float(x0), float(y0) }, r{ float(x1), float(y1) },
                dn{ float(x2), float(y2) };
            backend_->draw_texture_affine(stex, nullptr, o, r, dn);
        }
        return 1;
    }
    if (!role.textured_content(l)) return 0;
    // textured layer: file + optional lyc mask composition (texture_
    // for_masked), then the local quad (clip w/h or natural size)
    TextureRef tex = texture_for_masked(l);
    if (!tex) return 0;
    const LocalQuad q = quad_for_layer(l);
    if (!q.has) return 0;
    backend_->set_texture_alpha_mod(tex, alpha_byte(op));
    // lyprop colormultiply (research: thyt 消息窗调色): texture color
    // mod 乘色作用于普通层绘制;无 colormultiply 时复位(纹理可能复用)。
    if (const std::string* cmv = layer_prop(l, "colormultiply")) {
        if (const auto cm = color_multiply_rgb(*cmv)) {
            backend_->set_texture_color_mod(tex,
                uint8_t((*cm)[0] * 255.0 + 0.5),
                uint8_t((*cm)[1] * 255.0 + 0.5),
                uint8_t((*cm)[2] * 255.0 + 0.5));
        } else {
            backend_->set_texture_color_mod(tex, 255, 255, 255);
        }
    } else {
        backend_->set_texture_color_mod(tex, 255, 255, 255);
    }
    if (t.is_plain_translation()) {
        // axis-aligned unit-scale: exact legacy dst-rect path keeps the
        // pixel baselines stable
        const FRect dst{ float(t.e), float(t.f), float(q.w), float(q.h) };
        backend_->draw_texture(tex, q.has_src ? &q.src : nullptr, &dst);
    }
    else {
        // general affine (rotate / non-uniform or negative scale):
        // map the clip/local quad corners through the world transform
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        t.transform_point(0, 0, &x0, &y0);
        t.transform_point(q.w, 0, &x1, &y1);
        t.transform_point(0, q.h, &x2, &y2);
        const FPoint o{ float(x0), float(y0) }, r{ float(x1), float(y1) },
            dn{ float(x2), float(y2) };
        backend_->draw_texture_affine(tex, q.has_src ? &q.src : nullptr, o, r,
            dn);
    }
    return 1;
}
std::optional<std::array<double, 3>> RenderEngine::color_multiply_rgb(const std::string& v) {
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
        }
        catch (...) {
            return std::nullopt;
        }
        if (comma == std::string::npos) return out;
        pos = comma + 1;
    }
    return std::optional(out);
}

std::optional<IRect> RenderEngine::world_aabb_local(const oa::render::Compositor& sc, const oa::render::Layer& l, double x, double y, double w,
    double h) {
    oa::render::Affine2 t;
    if (!sc.world_transform(l.id, &t)) return std::nullopt;
    const double cx[4] = { x, x + w, x, x + w };
    const double cy[4] = { y, y, y + h, y + h };
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
    return IRect{ int(x0), int(y0), int(x1 - x0), int(y1 - y0) };
}

std::optional<IRect> RenderEngine::anchor_frame(
    const oa::render::Compositor& sc, const oa::render::Layer& l) {
    if (!anchor_frame_enabled_) return std::nullopt;
    const auto cit = anchor_frame_cache_.find(l.id);
    if (cit != anchor_frame_cache_.end()) return cit->second;
    std::optional<IRect> frame;
    do {
        oa::render::Affine2 t;
        if (!sc.world_transform(l.id, &t)) break;
        double ox = 0, oy = 0;
        t.transform_point(0, 0, &ox, &oy);
        const std::string sub = l.id + ".";
        for (const oa::render::Layer* q : sc.draw_order()) {
            if (q == &l) break;              // anchor quads draw BEFORE the group
            // backdrop 必须是有纹理内容的 quad
            // (原 q->file.empty() 跳过逐字;角色 textured_content 取反)。
            if (!oa::render::content_role_of(*q).textured_content(*q)) continue;
            if (q->id.size() > l.id.size() &&
                q->id.compare(0, sub.size(), sub) == 0)
                continue;                    // safety: no subtree quad qualifies
            const auto qr = layer_world_rect(*q);
            if (!qr) continue;
            const double qx = (*qr)[0], qy = (*qr)[1];
            if (std::fabs(qx - ox) > 0.5 || std::fabs(qy - oy) > 0.5)
                continue;                    // must share the group's origin
            const IRect r{ int(std::floor(qx)), int(std::floor(qy)),
                           int(std::ceil(qx + (*qr)[2])) - int(std::floor(qx)),
                           int(std::ceil(qy + (*qr)[3])) - int(std::floor(qy)) };
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
    anchor_frame_cache_[l.id] = frame;
    return frame;
}

void RenderEngine::progress_transition()
{
    // transition overlay: old frame over the new scene. Type-1 =
    // crossfade at opacity 1-progress. Type-2 rule dissolve (per-pixel GPU
    // path, draw_rule_transition) when the rule texture resolves AND the
    // backend supports per-pixel shaders; everything else (no rule /
    // unresolvable rule / sdl backend) falls back to the same crossfade —
    // the code-level rule-dissolve contract lives with
    // RenderBackend::draw_rule_transition.
    if (trans_capture_tex) {
        if (rt_->transition().is_in_progress(rt_->now_ms())) {
            const double p = rt_->transition().progress(rt_->now_ms());
            const double op = 1.0 - p;
            if (op > 0.0) {
                bool drew = false;
                if (rt_->transition().type() == 2) {
                    if (!trans_rule_checked_) {
                        // one resolve attempt per transition: a missing rule
                        // file keeps the crossfade fallback without probing
                        // the fs on every frame.
                        trans_rule_checked_ = true;
                        const std::string& rule = rt_->transition().rule();
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
                            std::clamp(rt_->transition().vague(), 0, 255);
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
        else if (!rt_->transition().active()) {
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

}
