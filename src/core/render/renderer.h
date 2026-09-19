#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <algorithm>

#include "core/render/backend.h"
#include "core/media/image.h"
#include "core/render/layer.h"
#include "core/render/content_role.h"
namespace oa::emote {
struct EmoteDrawPart;
class EmoteFile;
}
#include "core/fs/fs.h"
#include "core/runtime/runtime.h"
#include "core/render/font.h"

struct SDL_Window; // opaque SDL3 window (host-owned; renderer.h keeps no
                   // SDL_render dependency)

namespace oa::render {

struct LocalQuad {
    bool has = false;
    double w = 0, h = 0;
    bool solid = false; // color quad, no texture
    bool has_src = false;
    FRect src{}; // clip source rect when has_src
};

class RenderEngine : public oa::render::ContentSizeSource {
public:
	RenderEngine(oa::fs::IFileSystem* fs, oa::runtime::GameRuntime* rt);
    /// Engine stage size (from the project's [WINDOWS] WIDTH/HEIGHT; default
    /// 1280x720). Updated by the host after open_project, before window
    /// creation — window size, logical presentation and offscreen group
    /// baking all follow the stage.
    int stage_w_ = 1280;
    int stage_h_ = 720;
    ~RenderEngine();

    /// Create the render backend on the host window. backend_kind selects the
    /// line: "sdl" (default — the historical SDL3 SDL_Render path) or "gles"
    /// (native GLES backend). The engine
    /// draws through the RenderBackend interface — the implementations live
    /// in backend_sdl.{h,cpp} / backend_gles.{h,cpp}.
    bool create_renderer(SDL_Window* ctx,
                         const std::string& backend_kind = "sdl");
    void release_all();
    /// Backend error surface (backend->last_error()) — host
    /// canaries like OA_RENDER_DIAG no longer read SDL_GetError directly,
    /// the gles backend keeps its own error text.
    const char* renderer_error() const;
    /// [trans] capture with ZERO CPU readback on the
    /// stage-target path. The "old scene" a transition fades out of is still
    /// the content of the stage offscreen target at event-apply time (the
    /// previous frame rendered into it and nothing cleared it since), so the
    /// capture is a pure GPU copy: render-target swap + texture blit into the
    /// overlay texture. The old CPU snapshot (read_target → make_texture)
    /// survives only as the no-render-target fallback. Returns true when an
    /// overlay texture now holds the old frame (progress_ transition draws
    /// it); false keeps the caller's capture-complete fallback (transition
    /// plays without the old-scene overlay).
    bool capture_transition_source();
    /// Diagnostics (presentation probes only): read the presented WINDOW
    /// surface (the letterbox logical viewport content — its pixel size
    /// follows the window aspect). Readback of the stage target itself stays
    /// on snapshot_renderer; this window read is off the hot path and never
    /// used by transitions.
    bool read_window_surface(oa::media::Image& out);
    TextureRef make_texture(const oa::media::Image& img);

    // Pixel-read canary: read the current render target (the stage offscreen
    // target — always stage-sized and window-size
    // independent) and return its luma.
    double frame_luma();
    bool get_renderer_coordinates(float winx, float winy, float* rx, float* ry);
    /// Stage (logical render) coordinates -> window coordinates: inverse of
    /// get_renderer_coordinates, including the letterbox logical
    /// presentation. Used to warp the OS pointer onto a stage point (the
    /// [mouse] config tag; SDL_WarpMouseInWindow works in window coords).
    bool stage_to_window_coordinates(float sx, float sy, float* wx, float* wy);

    /// Read the renderer's current output into an Image. The whole scene
    /// renders into the stage offscreen target, so every
    /// snapshot lands on that target — fixed stage_w x stage_h pixels, no
    /// letterbox bars, independent of the window size (previously this read
    /// the window backbuffer, whose pixel size followed the window). [takess]
    /// frame capture and the evidence probes still consume this; the [trans]
    /// capture no longer does (capture_transition_source is GPU-only).
    bool snapshot_renderer(oa::media::Image& out);

    /// Diagnostic: read back one host-frame canvas — the
    /// GPU emote compositing target that `emote_render_parts` fills — so a
    /// same-run GPU-vs-CPU parity check can compare it with the CPU raster of
    /// the very same pose. False when the key is not an uploaded host texture
    /// or there is no backend.
    bool read_host_frame(const oa::render::TextureKey& key, oa::media::Image& out);

    // Decode + cache one resolved image (magic paths, ".png" fallback). The
    // cache key is the *logical* file name used by the scene; the lyc
    // file+mask composition below reuses the decoded pixels.
    // Save-area files (slot thumbnails written by [savess] under the
    // save root) resolve as layer images. Read them through the runtime's
    // SaveStore (the same store savess/syssave write into) when the name
    // lives under the engine savepath. This used to re-read only the
    // OA_SAVE_ROOT directory, so plain runs (default ./save store, commit
    // 0855089) wrote thumbnails the UI could never read back — save/load
    // slots showed no image. Routing through the store keeps every host
    // (env override / default root / in-memory test stores) on one seam.
    std::optional<std::vector<uint8_t>> overlay_read(const std::string& rel);
    const oa::media::Image* resolve_image(const std::string& name);
    /// 资源(解码资产)纹理:Asset 域缓存命中 → 解码 → 上传。宿主供帧
    /// (VideoFrame/EmoteCanvas/OverlayFrame)不走本入口(无解码回退)。
    TextureRef texture_for_asset(const std::string& name);
    /// 任意域键的纹理(缓存命中即返回;Asset 域可解码,宿主供帧域仅查缓存)。
    TextureRef texture_for_key(const oa::render::TextureKey& key);
    /// 角色读取域键(角色实例决定怎么读一层的资源;
    /// 旧的 path+file 拼写公式升级为 TextureKey)。
    oa::render::TextureKey layer_texture_key(const oa::render::Layer& l);

    /// Host upload of one decoded RGBA32 frame under a TextureKey — the
    /// layer video frame (VideoFrame domain), the emote canvas (EmoteCanvas)
    /// and fullscreen frames (OverlayFrame; drawn by the host directly).
    /// Creates a cached streaming texture on first use and updates its pixels
    /// afterwards (re-creating on size change); texture_for_key finds the same
    /// cache. Callers compare VideoEngine/emote revisions and upload only on
    /// change.
    bool upload_host_frame(const oa::render::TextureKey& key, int w, int h,
                           const uint8_t* rgba);
    /// Diagnostics: whether `key` is an uploaded host texture
    /// (emote/video frames); used by window-level regression assertions.
    bool texture_uploaded(const oa::render::TextureKey& key) const;
    bool renderer_ok() const { return backend_ != nullptr; }
    /// Whether the stage offscreen target holds a real presented frame
    /// (first render_end happened) — the [trans] GPU capture may copy it.
    bool stage_frame_ready() const { return stage_rt != nullptr && stage_frame_valid_; }

    // ------------------------------------------------------------------
    // GPU emote compositing — the pose (emote_collect_
    // parts triangle geometry) is filled into an offscreen canvas texture
    // (registered under the layer's TextureKey so the scene draws it like
    // the CPU-uploaded frame) with geometry; CPU only subdivides the mesh.
    // Falls back cleanly when there is no backend.
    // ------------------------------------------------------------------
    bool emote_render_parts(const oa::render::TextureKey& key,
                            const oa::emote::EmoteFile& file,
                            const std::vector<oa::emote::EmoteDrawPart>& parts,
                            int w, int h);

    // ------------------------------------------------------------------
    // Host-frame pump — the media→texture streaming half of the compositor
    // (the former app-side *_pump_frames, moved beside the textures it
    // feeds so the domain keys, revision bookkeeping and upload paths live
    // in one place). Streams every decoded frame that changed since the
    // last call, BEFORE the host's draw pass:
    //   * emote layers — GPU geometry compositing for external-pose players
    //     (the OA_EMOTE_GPU default), or the CPU canvas upload;
    //   * video layer channels — verbatim for mask-composited channels,
    //     luma-keyed otherwise (the keyed transform runs straight into the
    //     locked streaming texture: no intermediate full-frame copy);
    //   * the overlay video (verbatim).
    // Returns true while any channel is presenting (the host redraw gate);
    // `emote_presented` reports a fresh emote pose composited/uploaded this
    // call. No-op-safe without a backend (uploads report failure).
    // ------------------------------------------------------------------
    bool pump_host_frames(bool* emote_presented = nullptr);
    /// Last frame revision the pump uploaded for `channel` ("" = the overlay
    /// video; a layer id otherwise). Diagnostics/tests.
    uint64_t host_video_rev(const std::string& channel) const;
    // Cache counters (memory probes/tests): live GPU textures by cache,
    // decoded asset pixels and the emote GPU surfaces.
    size_t host_texture_count() const { return textures.size(); }
    size_t decoded_count() const { return decoded.size(); }
    size_t emote_canvas_count() const { return emote_canvas_.size(); }
    size_t emote_atlas_count() const { return emote_atlases_.size(); }

    // lyc [anime] mask composition: out.rgb = file.rgb and
    // out.a = file.a * mask灰度 (the R channel of a grey image) per pixel,
    // composited at the texture-provider level. The mask must decode and match
    // the file size; otherwise the mask is ignored and the plain file draws.
    // Cached under the masked_texture_name key.
    TextureRef texture_for_masked(const oa::render::Layer& l);

    // Local content quad of a layer in *its own coordinate space*, mirroring
    // the frame build: clip=[x,y,w,h] is a texture
    // SOURCE sub-rect and also sets the drawn size w x h; plain image layers
    // draw at their natural texture size; width/height size only lyc
    // solid-color (file-less) layers. The world position/size comes from the
    // scene's ancestor transform chain (world_transform).
    LocalQuad quad_for_layer(const oa::render::Layer& l);
    uint8_t alpha_byte(double a);
    /// World AABB of a layer's drawn content (ancestor chain included). Used
    /// by the diagnostics + the title-button probe.
    std::optional<std::array<double, 4>> layer_world_rect(const oa::render::Layer& l);
    /// Hit-size fallback handed to the scene hit test: only file layers
    /// without width/height/clip need their decoded texture size (the scene
    /// itself implements width/height > clip > texture precedence).
    static std::optional<std::pair<double, double>> hit_size(void* userdata, const oa::render::Layer& l);
    /// clickablethreshold texture pixel-alpha sampler for the runtime hit
    /// test: resolve the layer file through the decoded cache and return the
    /// alpha at texture pixel (tx,ty) (clip offset already applied by the
    /// scene); out-of-range / unresolved -> nullopt (layer-alpha fallback).
    static std::optional<uint8_t> alpha_sampler(void* userdata, const oa::render::Layer& l, int tx, int ty);

    void process_event(const oa::runtime::Event& e);
    bool is_transition_active() { return trans_capture_tex != nullptr; }
    size_t transition_events_size() { return trans_events; }

    // 1x1 solid texture used to draw solid quads under a general affine
    // transform (affine texture draw needs a texture).
    std::map<uint32_t, TextureRef> solid_tex_cache;
    TextureRef solid_texture(const oa::render::Layer& l);
    // ------------------------------------------------------------------
    // draw helpers (semantics): one layer's own content (solid
    // quad or file+mask textured quad) at a world affine and opacity.
    // Texture quads size by clip w/h or natural texture size; width/height
    // only sizes solid (file-less) color quads.
    // ------------------------------------------------------------------
    const std::string* layer_prop(const oa::render::Layer& l, const char* k);
    int intermediate_mode(const oa::render::Layer& l);
    bool prop_true(const std::string* v);
    // An intermediate_render group whose result differs from drawing its
    // subtree directly needs the true offscreen pass: own alpha < 255,
    // mode (1|2), color filters, a blend mode, an
    // intermediate_render_mask, or a container (file-less) clip — the last
    // is the group's content crop window (clip-bearing
    // file-less intermediate containers crop their subtree; texture layers
    // keep clip = source sub-rect). alpha==255 groups without any filter
    // are pixel-identical drawn directly — "over" is associative and group
    // composition exists exactly for the single alpha application.
    bool group_needs_offscreen(const oa::render::Layer& l);
    // Does this node take the offscreen group path (draw_node
    // dispatcher predicate; intermediate_render=0 or offscreen-unneeded
    // groups draw inline on the plain path).
    bool group_path_required(const oa::render::Layer& l);

    // ---- render (static-frame skip: identical frames keep the last
    // presented image — last_submitted_frame logic) --
    // The whole scene draws into the fixed stage
    // offscreen target (`stage_rt`); render_end presents it. Clear the
    // target opaque black (the same color the backbuffer clear used before
    // the offscreen architecture, so stage pixels are unchanged).
    void render_beigin();
    void render_clear_tex_cache();
    size_t draw_one(const oa::render::Layer& l, const oa::render::Affine2& t,
        double op);
    std::optional<std::array<double, 3>> color_multiply_rgb(const std::string& v);
    // World AABB of a local content rect (used for the group clip
    // scissor; transform_rect).
    std::optional<IRect> world_aabb_local(const oa::render::Compositor& sc, const oa::render::Layer& l, double x, double y, double w,
        double h);
    // The
    // "frame" of a content-crop group (file-less intermediate container
    // whose explicit clip is the subtree crop window) = the backdrop
    // quad(s) drawn before it that share its world origin (Artemis
    // list-row UIs anchor the row panel and the row's face container at
    // the same origin). Returns the world-rect intersection of those
    // quads (nullopt when there is no coincident backdrop, e.g.
    // favoface-style windows offset into their art). Resolved once per
    // crop-group id and cached; the cache dies with the group-bake cache
    // (render_clear_tex_cache) so scrolls/rebuilds re-resolve.
    std::optional<IRect> anchor_frame(const oa::render::Compositor& sc,
                                      const oa::render::Layer& l);
    std::map<std::string, std::optional<IRect>> anchor_frame_cache_;
    /// Test/diagnostic switch: disable the anchor-frame confinement
    /// so one process can capture the same settled frame with
    /// and without the fix (same-run pixel A/B — OA_BT99_AB journey hook).
    bool anchor_frame_enabled_ = true;
    void set_anchor_frame_enabled(bool on) { anchor_frame_enabled_ = on; }
    bool anchor_frame_enabled() const { return anchor_frame_enabled_; }
    // draw_node is now a thin dispatcher implemented in renderer.cpp
    // (offscreen-group path vs plain path; helpers + GroupPlan in the private
    // section below) — see the class doc comment there for the mapping.


    void progress_transition();
    // Glyphs painted this frame at their node slots (single text
    // paint point — the old scene-wide trailing pass is gone; every drawable
    // message owns a scene node, materialized lazily by the runtime).
    size_t last_frame_glyphs() const { return frame_glyphs_; }
    /// Inline glyph pass: draw the message-layer text bound to `node`
    /// right at the node's slot in the scene draw order (glyphs become scene
    /// content — any later layer, e.g. a black wipe, covers them; node
    /// hide/delete removes its text). Called by the recursive draw_node
    /// after the node's own content.
    size_t draw_node_text(const oa::render::Layer& node, const oa::render::Affine2& world,
                          double opacity);
    /// ONE recursive scene traversal — the dotted-id tree pre-order IS
    /// the draw order,
    /// so every node's image content and bound glyphs draw exactly at its
    /// slot and later nodes cover them; intermediate_render groups composite
    /// offscreen only when they actually need it. `global_fade`
    /// ([alldelete]) multiplies the whole chain. Returns the drawn count.
    size_t draw_scene(const oa::render::Compositor& sc, double global_fade);
    /// present = blit the stage offscreen target into the
    /// window. The SDL logical letterbox presentation (installed in
    /// create_renderer) scales the full-stage rect into the real window —
    /// aspect-preserving, centered, maximally fitted — so window resizes need
    /// no host code and pointer events keep converting automatically
    /// (window→render coordinates). The backbuffer is cleared opaque black
    /// first so the letterbox bars are black at any window aspect. The
    /// render target stays on the stage texture afterwards, so every pixel
    /// read between frames (snapshot_renderer / frame_luma / the [trans]
    /// GPU capture) hits the fixed stage frame.
    void render_end();

private:
    // ------------------------------------------------------------------
    // RenderEngine 作为角色内容 quad/命中/
    // 采样面的 ContentSizeSource(角色 content_quad 的纹理自然尺寸查询;
    // decoded = 解码资产缓存,uploaded = 宿主上传纹理缓存 + backend 尺寸)。
    // 语义与 quad_for_layer/hit_size/alpha_sampler 的现状查询逐条一致
    // (引擎创建纹理尺寸恒可得)。按
    // TextureKey 域分派(Asset → decoded;宿主供帧 → textures)。
    // ------------------------------------------------------------------
    std::optional<std::pair<double, double>> decoded_size(const oa::render::TextureKey& key) const override;
    std::optional<std::pair<double, double>> uploaded_size(const oa::render::TextureKey& key) const override;
    // ------------------------------------------------------------------
    // draw-node split + intermediate-group local compositing
    // (implemented in renderer.cpp). draw_node is a thin dispatcher:
    // groups that need real offscreen compositing take the group path
    // (offscreen target session + bake cache + draw-time composite),
    // everything else the plain path. 组路径: 中间组 = 离屏 LayerBitmap 会话,
    // group_tex_cache = 组合 Bitmap 缓存, 复合按 layermode 把烘焙结果叠回
    // 宿主层 —— 全为节点局部策略, 不溢出主循环.
    // ------------------------------------------------------------------
    /// One node's group-composite plan, re-parsed at draw time. The bake
    /// stores straight-RGBA content only; alpha / blend / clip live here and
    /// are applied when the (possibly cached) bake is composited, so group
    /// alpha tweens and clip/blend changes never re-bake.
    struct GroupPlan {
        double group_alpha = 1.0;                  // layer alpha (alpha-mod)
        BlendMode blend = BlendMode::Blend;        // layermode normalized
        std::array<double, 3> color_multiply{1.0, 1.0, 1.0};
        bool grayscale = false;
        bool negative = false;
        const oa::media::Image* mask = nullptr; // stretched mask image
        // clip = inherited scissor ∩ group-clip world rect; {0,0,0,0} when
        // disjoint; nullopt when no clip applies (backend state untouched).
        std::optional<IRect> clip;
    };
    /// Dispatcher: pre-order slot semantics — one node's own content first,
    /// then its subtree; offscreen-needed groups take the group path.
    /// Recurses over the Compositor's explicit SceneNode tree
    /// (children = compare_ids order), no per-frame tree rebuild.
    void draw_node(const oa::render::Compositor& sc,
                   const oa::render::SceneNode& node,
                   const oa::render::Affine2& parent_world, double parent_op,
                   std::optional<IRect> scissor, size_t* drawn_out);
    /// Plain path: own image content + bound glyphs + child recursion
    /// (an intermediate_render group that needs no offscreen pass draws
    /// inline at its group alpha — "over" is associative, pixel-identical).
    void draw_plain_node(const oa::render::Compositor& sc,
                         const oa::render::SceneNode& node,
                         const oa::render::Affine2& world, int intermediate,
                         double parent_op, std::optional<IRect> scissor,
                         size_t* drawn_out);
    /// Offscreen group path: plan → cached composite or full re-bake
    /// (target session + subtree draw + readback + CPU composite + upload).
    void draw_group_node(const oa::render::Compositor& sc,
                         const oa::render::SceneNode& node,
                         const oa::render::Affine2& world, double parent_op,
                         std::optional<IRect> scissor, size_t* drawn_out);
    /// Parse one group's draw-time parameters (clip world rect ∩ inherited
    /// scissor, alpha, colormultiply/grayscale/negative, mask image,
    /// layermode blend). Pure parse + image resolve; no backend state
    /// changes.
    GroupPlan resolve_group_plan(const oa::render::Compositor& sc,
                                 const oa::render::Layer& layer,
                                 std::optional<IRect> inherited);
    /// Draw-time composite of a (possibly cached) bake onto the parent:
    /// alpha-mod(group alpha) + layermode blend + group-clip scissor, then
    /// restore the caller's clip.
    void composite_group_bake(TextureRef baked, const GroupPlan& p,
                              bool had_clip, const IRect& old_clip);
    /// Enter the offscreen target session: set target, clear transparent,
    /// apply the group clip; returns the previous target to restore in
    /// end_offscreen_pass.
    TextureRef begin_offscreen_pass(TextureRef target, const GroupPlan& p);
    /// Leave the offscreen session: restore the previous target and clip.
    void end_offscreen_pass(TextureRef saved_target, bool had_clip,
                            const IRect& old_clip);
    void restore_render_clip(bool had_clip, const IRect& old_clip);
    /// CPU composite (GROUP_COMPOSITE_FRAGMENT_BODY):
    /// un-premultiply the group target, apply the filters and (stretched)
    /// mask, store straight rgb with baked alpha = A * maskA (content
    /// coverage — never forced opaque)
    /// (the group alpha is applied at draw time).
    void group_composite_cpu(const std::vector<uint8_t>& px, int stage_w,
                             int stage_h, const GroupPlan& p,
                             std::vector<uint8_t>* out_px);
    /// Upload straight-RGBA pixels as a static stage-sized texture.
    TextureRef upload_baked_texture(const std::vector<uint8_t>& px, int w,
                                    int h);

    /// The render backend (SDL3 SDL_Render line). Owned
    /// here; created in create_renderer, torn down in release_all.
    std::unique_ptr<RenderBackend> backend_;
    // The fixed stage offscreen render target — the whole
    // scene draws here; render_end blits it into the window (letterbox
    // logical presentation scales it) and every pixel read (snapshots, the
    // [trans] GPU capture) samples it. stage_frame_valid_ turns true after
    // the first present, i.e. the target holds a real frame that the [trans]
    // capture may copy.
    TextureRef stage_rt = nullptr;
    bool stage_frame_valid_ = false;
	// The scene lives inside GameRuntime; the host
	// only READS it for rendering and hit dispatch.
	std::map<std::string, oa::media::Image> decoded;
	// decoded pixel cache bounds (long sessions: every unique CG/bg/sprite
	// pixel buffer used to stay for the process lifetime — unbounded RAM
	// growth on asset-heavy games). Stamp per use; evict least-recently
	// used entries past a byte cap (OA_DECODED_CACHE_MB, default 1024, 0 =
	// unbounded), destroying the GPU textures that mirror the evicted
	// pixels (the plain asset texture + its masked composites).
	std::map<std::string, uint64_t> decoded_use_;
	uint64_t cache_stamp_ = 0;
	size_t decoded_bytes_ = 0;
	void decoded_note(const std::string& name, oa::media::Image&& img);
	void decoded_evict_if_needed();
	// 纹理缓存按 TextureKey 域分桶 —— 资源文件(Asset)与
	// 宿主供帧(VideoFrame/EmoteCanvas/OverlayFrame)结构性分开,撞名不可能,
	// 不再需要保留命名空间拼写。
	std::map<oa::render::TextureKey, TextureRef> textures;
    // GPU emote canvases (offscreen targets, also registered in
    // `textures` under the same key while active) and their atlas textures.
    std::map<oa::render::TextureKey, TextureRef> emote_canvas_;
    // Emote atlas textures, keyed by ({layer canvas key}, source index, PSB
    // identity). The PSB identity (EmoteFile::uid) is REQUIRED: a game reuses
    // one layer id while swapping the character's file ([fg] file="mit_0" ->
    // "mit_1", costume changes run_0 -> run_1), and with the layer key alone
    // the GPU sampled the PREVIOUS file's atlas under the NEW file's icon
    // rectangles — the on-screen portrait drew another file's pixels at this
    // file's part positions (research/132). Stale textures of a layer are
    // destroyed on its next draw. OA_EMOTE_ATLAS_KEY=legacy restores the
    // layer-only key (A/B arm for the pre-fix behaviour).
    struct EmoteAtlasKey {
        oa::render::TextureKey layer;
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

    // Host-frame pump bookkeeping: last uploaded revision per emote layer /
    // video channel (the app-side layer_*_rev maps, moved with the pump).
    std::map<std::string, uint64_t> pump_emote_rev_;
    std::map<std::string, uint64_t> pump_video_rev_;
    uint64_t pump_overlay_rev_ = 0;
    /// upload_host_frame with the host-side luma-key fused into the texture
    /// fill (alpha = layer_video_key_alpha(luma) per pixel, RGB verbatim):
    /// the keyed frame lands in the locked texture without the intermediate
    /// full-frame copy.
    bool upload_host_frame_keyed(const oa::render::TextureKey& key, int w, int h,
                                 const uint8_t* rgba);
    // Transitions: capture texture for the overlay — type-1 crossfade
    // draws it on top of the new scene at opacity 1-progress; type-2 rule
    // dissolve (GLES line) re-uses it as the dissolve source (per-pixel
    // coverage from the rule texture, draw_rule_transition).
    TextureRef trans_capture_tex = nullptr;
    int trans_capture_w = 0;
    int trans_capture_h = 0;
    // Type-2 rule dissolve state: the rule texture is cached in `textures`
    // via texture_for_asset (decoded pixels in `decoded`); resolution is
    // attempted once per transition (missing rule files must not re-probe
    // the fs every frame). reset in capture_transition_source.
    TextureRef trans_rule_tex = nullptr;
    bool trans_rule_checked_ = false;
    size_t trans_events = 0;
    // cached intermediate-group composite textures (baked WITHOUT the
    // group alpha — applied via alpha-mod at draw time so alpha tweens are
    // cheap); invalidated whenever any scene mutation / animation ran.
    std::map<std::string, TextureRef> group_tex_cache;
    // glyphs painted at node slots this frame (diagnostics).
    size_t frame_glyphs_ = 0;
    // per-frame snapshot — scene node id → drawable message ids bound
    // to it, resolved ONCE per frame at the top of draw_scene (drawability is
    // a pure function of scene/text state, which cannot change mid-draw, so
    // this equals the former per-node re-scans of visible_content_layers).
    std::map<std::string, std::vector<std::string>> frame_node_messages_;

    oa::fs::IFileSystem* fs_;
    oa::runtime::GameRuntime* rt_;
    // FontSystem 声明在 backend_ 之后：析构序 = fontSystem 先于 backend_，
    // 字形纹理销毁时后端对象仍存活（release_all 已 shutdown 渲染器时与
    // 后端抽象前的 SDL_DestroyTexture-after-DestroyRenderer 语义一致）。
    std::shared_ptr<oa::render::FontSystem> fontSystem;
};
}
