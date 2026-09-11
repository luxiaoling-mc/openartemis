#pragma once

#include <vector>
#include <map>
#include <memory>
#include <string>

#include "core/render/backend.h"
#include "core/render/text.h"
#include "core/fs/fs.h"
#include "core/runtime/runtime.h"

extern "C" {
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_STROKER_H
#include FT_GLYPH_H
}

// ------------------------------------------------------------------
// 连续描边层（现象 J+）：描边环必须连续。
// 旧实现只画 4 个对角偏移副本（{-1,-1},{1,-1},{-1,1},{1,1}），偏移 >1px 时
// 笔画四周出缺口（"边框断裂"），且偏移副本无法越出源位图盒。本实现改用
// FreeType `FT_Stroker`：对字形轮廓向外描边 width px（LINECAP/JOIN_ROUND）
// 后栅格化为独立描边层（自带 bearing，不与填充位图盒共享）⇒ 环连续、
// 宽度均匀、不越出字形盒（层盒 = 填充盒外扩 width + AA 边）。
// 退路 = 全向圆盘膨胀（raster_edge_alpha 的 Dilate 分支）。
// ------------------------------------------------------------------
namespace oa::render {
/// 描边层编码方式（开关 OA_FONT_OUTLINE=stroke|dilate|legacy；legacy 为
/// 旧 4 对角副本，仅供 PRE 像素对照，走 draw_text_layer 内联分支）。
enum class EdgeEncoding { Stroke, Dilate };
/// 环境开关解析（未设/未知 → Stroke）。
EdgeEncoding edge_encoding_from_env();
/// 栅格化描边层：alpha 位图 + 位图 bearing（与填充位图同一笔端原点网格，
/// 故两者落点公式一致）。width_px < 1 或栅格失败 → false。
/// cover_fill=true 时把字形自身位图（α≥1 → 255）并入层内：描边色必须盖住
/// 字形自身的 AA 边/越出环内的边沿（否则环内沿与字形之间留 1px 亮缝，
/// 度量面表现为"缺口"）。绘制路径恒为 true；度量工具两种都取。
struct EdgeBitmap {
    std::vector<uint8_t> alpha; // w*h, 行优先
    int width = 0, height = 0;
    int left = 0; // = FT_BitmapGlyph.left（相对笔端）
    int top = 0;  // = FT_BitmapGlyph.top
};
bool raster_edge_alpha(FT_Library lib, FT_Stroker stroker, FT_Face face,
    int ppem, uint32_t cp, double width_px, EdgeEncoding enc, bool cover_fill,
    EdgeBitmap* out);
} // namespace oa::render

// ------------------------------------------------------------------
// message-layer text rendering: 字形由 FreeType 栅格化，
// 排版用 oa::render::layout_page（引擎文本层内容 + 本回调提供度量）。
// 消息层文本以 overlay 绘制在场景之上（独立消息层语义；layered 的图层
// 窗口本体由 Lua 绘制，文本同样叠在其上）。颜色支持 font color（RRGGBB）；
// 阴影/描边/逐字/backlog 记为近似。
//
// 绘制面不直接拿 SDL_Renderer/SDL_Texture，
// 字形纹理与绘制都经 RenderBackend 接口（draw_text_layer 收 RenderBackend*、
// CachedGlyph 存 TextureRef）；SDL 类型不进本头文件。
// ------------------------------------------------------------------
namespace oa::render {

struct FaceEntry {
    std::vector<uint8_t> bytes;
    FT_Face face = nullptr;
};
class FontSystem {
public:
    FontSystem(const oa::fs::IFileSystem* fs, const oa::runtime::GameRuntime* rt);
    ~FontSystem();

    /// 字形纹理销毁需要后端（字形缓存在本类析构时随引擎销毁）；后端由
    /// RenderEngine 在 create_renderer 后注入。
    void set_backend(oa::render::RenderBackend* backend) { backend_ = backend; }

    FT_Face load_font_face(const std::string& logical);
    FT_Face face_for(const oa::render::FontDesc& f);
    // 度量：字形 advance/包围盒/行高（缺字形/缺字体 → 字号占位）
    uint32_t utf8_next(const std::string& s, size_t& i);
    // 颜色解析（RRGGBB / #RRGGBB / Artemis 惯例 "0RRGGBB" 前导零容错）
    std::optional<oa::render::Rgba> parse_rgb(const std::string& raw);
    oa::render::Rgba text_color(const oa::render::FontDesc& f);
    // 样式键（描边 = 连续环：FT_Stroker 外扩 width px 的独立描边层，
    // 见文件头 raster_edge_alpha；shadow 偏移按数值键 px）
    void style_colors(const oa::render::FontDesc& f, oa::render::Rgba* shadow,
        oa::render::Rgba* outline);
    // 画一个消息层的一页：字形布局为文本节点本地坐标（ml.left/top + 排版偏移），
    // 整体经绑定场景节点世界变换 world 合成（节点世界变换的 text_for 注入语义；
    // 纯平移走快速矩形路径，仿射/缩放/旋转走 affine 角点映射），
    // opacity = 节点链不透明度（父层/根 alpha 乘入；近似 cmd.opacity *= opacity）。
    // 逐字显示只画 < reveal_index 的字形；支持 shadow/outline 样式近似。
    // 字形纹理在 glyph_slot 中按后端创建/缓存，经 backend 绘制。
    size_t draw_text_layer(oa::render::RenderBackend* backend,
        const oa::render::MessageLayer& ml,
        const oa::render::Affine2& world, double opacity);
    // 文本度量：对消息层当前页执行排版，返回 {最大行宽, 总高度, 末行宽度}。
    // 供 e:var system=get_message_layer_width/height/line_width。
    struct TextExtents { double width = 0; double height = 0; double line_width = 0; };
    TextExtents measure_text_extents(const oa::render::MessageLayer& ml) const;
    /// Diagnostics/tests: whether a default text face resolved
    /// (engine default name + project font/ fallback).
    bool default_face_loaded() const { return default_face != nullptr; }

    // ------------------------------------------------------------------
    // 字形度量（视觉修复；rasterize_glyph + text_line_metrics
    // 的 FreeType 等价面）。
    //
    // 字体库栅格化语义：PxScale::from(sz) 令"字体 typographic 高度
    // （hhea ascender−descender，fu）= sz px"，即 outline 比例 = sz/(asc−desc)；
    // 字形 ascent_px = sz·asc_fu/(asc−desc)、advance_px = fu·sz/(asc−desc)。
    // FreeType 以整数 ppem 栅格化：取最近整数 ppem(s)=round(sz·upem/H)
    // 复现同一比例（≤0.5% 栅格级差，属"同公式下光栅化差异"允许口径）。
    // 缺字段回退：upem/H 不可用 → ppem = round(sz)。
    // ------------------------------------------------------------------
    static int raster_ppem(FT_Face face, double size);
    /// 单字形完整度量（布局步进/墨迹框 + 绘制落点）。
    struct GlyphMeasure {
        double ppem = 0;     // FT 栅格像素尺寸
        double advance = 0;  // 笔端步进 px（h_advance 比例语义）
        double ink_w = 0;    // 墨迹宽 px（换行判定宽；空格等无墨迹字形为 0）
        double ink_h = 0;    // 墨迹高 px
        double ascent = 0;   // 基线以上 px（= sz·asc_fu/H；FT size ascender/64）
        double offset_x = 0; // 字形左 bearing（bitmap_left；offset_x=b.min.x）
        double offset_y = 0; // 行内上落点 = ascent − bitmap_top（offset_y =
                            // sf.ascent+px_bounds.min.y；y-down min.y ≡ −bitmap_top）
    };
    GlyphMeasure measure_glyph(const FontDesc& f, uint32_t cp);
    /// layout_page 度量回调面（CharMetrics: advance / 墨迹宽 / 墨迹高）。
    oa::render::CharMetrics metrics_for(const FontDesc& f, uint32_t cp);
    /// 栅格化字形槽（face,size,cp 缓存；缺字形 → 空槽）。位图 bearing 随槽缓存，
    /// 绘制落点以实际栅格为准（hinting 网格差 ≤1px 与度量 load 一致）。
    struct CachedGlyph {
        oa::render::TextureRef tex = nullptr;
        int bitmap_left = 0;
        int bitmap_top = 0;
    };
    CachedGlyph glyph_slot(oa::render::RenderBackend* backend, FT_Face face,
                           double size, uint32_t cp);
    /// 描边层槽（face,ppem,cp,width,encoding 缓存；缺字形 → 空槽）。
    /// 位图 bearing 随槽缓存 ⇒ 描边落点与填充落点用同一公式（同一原点网格）。
    CachedGlyph edge_slot(oa::render::RenderBackend* backend, FT_Face face,
                          double size, uint32_t cp, double width_px);

private:
    FT_Library ft_lib = nullptr;
    FT_Stroker stroker_ = nullptr; // 连续描边（FT_Stroker_New(ft_lib)）
    std::map<std::string, FaceEntry> font_faces; // 逻辑 face → 已加载字体
    std::map<std::string, CachedGlyph> glyph_cache; // key face\tppem\tcp
    std::map<std::string, CachedGlyph> edge_cache;  // key face\tppem\tcp\tw\tenc

    // 默认面（默认字体候选）
    const std::string kDefaultFace = std::string(oa::render::kDefaultFontFile);
    FT_Face default_face = nullptr;

    oa::render::RenderBackend* backend_ = nullptr; // 非拥有；字形纹理销毁用
    const oa::fs::IFileSystem* fs_;
    const oa::runtime::GameRuntime* rt_;
};
}
