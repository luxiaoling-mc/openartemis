#pragma once

#include <vector>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "core/render/backend.h"
#include "core/render/layer.h" // Affine2
#include "core/render/text.h"
#include "core/fs/fs.h"

namespace oa::render {
class RenderHost;
}

extern "C" {
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_STROKER_H
#include FT_GLYPH_H
}

// Continuous stroke layer (FT_Stroker); details: docs/RENDERING.md.
namespace oa::render {
/// Stroke encoding (OA_FONT_OUTLINE=stroke|dilate|legacy).
enum class EdgeEncoding { Stroke, Dilate };
/// Parse OA_FONT_OUTLINE; unset/unknown → Stroke.
EdgeEncoding edge_encoding_from_env();
/// Rasterise one stroke layer (alpha bitmap + bearing).
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

// Message-layer text rendering through RenderBackend (no SDL types here).
namespace oa::render {

struct FaceEntry {
    std::vector<uint8_t> bytes;
    FT_Face face = nullptr;
};
class FontSystem {
public:
    FontSystem(const oa::fs::IFileSystem* fs, oa::render::RenderHost* host,
               const oa::render::TextEngine* text);
    ~FontSystem();

    /// Backend injection for glyph texture lifetime.
    void set_backend(oa::render::RenderBackend* backend) { backend_ = backend; }

    // Draw one page in node-local layout under the bound node's world transform.
    size_t draw_text_layer(oa::render::RenderBackend* backend,
        const oa::render::MessageLayer& ml,
        const oa::render::Affine2& world, double opacity);
    // Layout the current page: {max line width, height, last line width}.
    struct TextExtents { double width = 0; double height = 0; double line_width = 0; };
    TextExtents measure_text_extents(const oa::render::MessageLayer& ml) const;
    /// Diagnostics: whether the default text face resolved.
    bool default_face_loaded() const { return default_face != nullptr; }

    /// One glyph: advance, ink box, ascent, bearing.
    struct GlyphMeasure {
        double ppem = 0;     // FT 栅格像素尺寸
        double advance = 0;  // 笔端步进 px（h_advance 比例语义）
        double ink_w = 0;    // 墨迹宽 px（换行判定宽；空格等无墨迹字形为 0）
        double ink_h = 0;    // 墨迹高 px
        double ascent = 0;   // 基线以上 px（= sz·asc_fu/H；FT size ascender/64）
        double offset_x = 0; // 字形左 bearing（bitmap_left；offset_x=b.min.x）
        double offset_y = 0; // ascent − bitmap_top
    };
    GlyphMeasure measure_glyph(const FontDesc& f, uint32_t cp);
    /// layout_page metric callback.
    oa::render::CharMetrics metrics_for(const FontDesc& f, uint32_t cp);

private:
    /// Layout cache: unchanged inputs cost one hash (key covers all inputs).
    struct PageCacheEntry {
        uint64_t key = 0;
        oa::render::LaidPage page;
    };
    mutable std::unordered_map<std::string, PageCacheEntry> page_cache_;
    /// FNV-1a over every layout input.
    uint64_t layout_key(const oa::render::MessageLayer& ml) const;
    /// Cached laid-out page; re-lays only when an input changed.
    const oa::render::LaidPage& cached_page(const oa::render::MessageLayer& ml,
                                            oa::render::RenderBackend* backend) const;
    FT_Face load_font_face(const std::string& logical);
    FT_Face face_for(const oa::render::FontDesc& f);
    // Metrics: advance / ink box / line height (fallback to font size).
    uint32_t utf8_next(const std::string& s, size_t& i);
    // Colour parse (RRGGBB / #RRGGBB / leading-zero Artemis form).
    std::optional<oa::render::Rgba> parse_rgb(const std::string& raw);
    oa::render::Rgba text_color(const oa::render::FontDesc& f);
    // Style colours; stroke uses raster_edge_alpha.
    void style_colors(const oa::render::FontDesc& f, oa::render::Rgba* shadow,
        oa::render::Rgba* outline);
    // Glyph metrics / ppem mapping; details: docs/RENDERING.md.
    static int raster_ppem(FT_Face face, double size);
    /// Rasterised glyph slot (face,size,cp cache); missing glyph → empty.
    struct CachedGlyph {
        oa::render::TextureRef tex = nullptr;
        int bitmap_left = 0;
        int bitmap_top = 0;
    };
    CachedGlyph glyph_slot(oa::render::RenderBackend* backend, FT_Face face,
                           double size, uint32_t cp);
    /// Stroke slot; same origin grid as the fill slot.
    CachedGlyph edge_slot(oa::render::RenderBackend* backend, FT_Face face,
                          double size, uint32_t cp, double width_px);

private:
    FT_Library ft_lib = nullptr;
    FT_Stroker stroker_ = nullptr; // 连续描边（FT_Stroker_New(ft_lib)）
    std::map<std::string, FaceEntry> font_faces; // 逻辑 face → 已加载字体
    std::map<std::string, CachedGlyph> glyph_cache; // key face\tppem\tcp
    std::map<std::string, CachedGlyph> edge_cache;  // key face\tppem\tcp\tw\tenc

    // Default face.
    const std::string kDefaultFace = std::string(oa::render::kDefaultFontFile);
    FT_Face default_face = nullptr;

    oa::render::RenderBackend* backend_ = nullptr; // non-owning
    const oa::fs::IFileSystem* fs_;
    oa::render::RenderHost* host_ = nullptr;
    const oa::render::TextEngine* text_ = nullptr;
};
}
