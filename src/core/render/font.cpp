#include "core/render/font.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

// 字体
namespace oa::render {

namespace {
// Case-insensitive extension check for candidate font files.
bool is_font_file(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string ext = name.substr(dot);
    if (ext.size() != 4) return false;
    char c1 = (char)tolower((unsigned char)ext[1]);
    char c2 = (char)tolower((unsigned char)ext[2]);
    char c3 = (char)tolower((unsigned char)ext[3]);
    return (c1 == 't' && c2 == 't' && c3 == 'f') ||
           (c1 == 'o' && c2 == 't' && c3 == 'f');
}
/// style 词门控（FPM 系 [font style="outline,shadow"]）。
bool style_word_on(const oa::render::FontDesc& f, const char* word) {
    const std::string style = f.get_or("style", "");
    return style.find(word) != std::string::npos;
}
/// 数值边缘键的**宽度/偏移**语义（现象 J+）。
/// 键值即像素量：`outline=N` → 描边宽度 N px（N≥1）、`shadow=N` → 偏移 N px；
/// 真值词（on/true/yes）→ 1；0/缺失/不可解析 → 不启用。
/// 曾把键值降级为"仅 1/on 才算开、宽度恒 1px hairline"的做法已撤销（实机判据：
/// 描边本该存在，"边框断裂"才是被误判为异常黑边的根本原因 ⇒ 键值恢复量纲）。
bool numeric_edge_amount(const oa::render::FontDesc& f, const char* key, double* amount) {
    const std::string* v = f.get(key);
    if (!v) return false;
    std::string s = *v;
    // trim
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n'))
        --e;
    s = s.substr(b, e - b);
    std::string low = s;
    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
    if (low == "on" || low == "true" || low == "yes") {
        *amount = 1.0;
        return true;
    }
    if (s.empty()) return false;
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(d) || d < 1.0) return false;
    *amount = d;
    return true;
}
} // namespace

EdgeEncoding edge_encoding_from_env() {
    // 进程内只解析一次：描边编码在绘制循环里逐字形取用，getenv + 字符串构造
    // 不应进热路径（开关按"启动时设定"语义）。
    static const EdgeEncoding cached = [] {
        const char* v = std::getenv("OA_FONT_OUTLINE");
        if (!v || !*v) return EdgeEncoding::Stroke;
        std::string s = v;
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        if (s == "dilate") return EdgeEncoding::Dilate;
        // legacy（4 对角副本）由 draw_text_layer 内联分支处理，编码面仍回落
        // Stroke（该分支不会调用本函数的结果）。
        return EdgeEncoding::Stroke;
    }();
    return cached;
}

/// OA_FONT_OUTLINE=legacy → 旧 4 对角副本路径（仅 PRE 像素对照用）。
bool legacy_edge_env() {
    static const bool cached = [] {
        const char* v = std::getenv("OA_FONT_OUTLINE");
        if (!v || !*v) return false;
        std::string s = v;
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s == "legacy";
    }();
    return cached;
}

bool raster_edge_alpha(FT_Library lib, FT_Stroker stroker, FT_Face face,
    int ppem, uint32_t cp, double width_px, EdgeEncoding enc, bool cover_fill,
    EdgeBitmap* out)
{
    (void)lib;
    if (!face || !out || width_px < 1.0 || ppem < 1) return false;
    out->alpha.clear();
    out->width = out->height = out->left = out->top = 0;
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)ppem) != 0) return false;
    bool have_ring = false;
    // ── 首选：FT_Stroker 对（hinted）字形轮廓向外描边 ──────────────
    // 与填充位图同用 FT_LOAD_DEFAULT 的轮廓 ⇒ 环内沿 = 填充外沿，无缝；
    // StrokeBorder(inside=false) 只保留外扩半边，厚度 = width px。
    if (enc == EdgeEncoding::Stroke && stroker) {
        if (FT_Load_Char(face, (FT_ULong)cp, FT_LOAD_NO_BITMAP | FT_LOAD_DEFAULT) == 0 &&
            face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_Glyph glyph = nullptr;
            if (FT_Get_Glyph(face->glyph, &glyph) == 0) {
                FT_Stroker_Set(stroker, (FT_Fixed)std::lround(width_px * 64.0),
                    FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
                if (FT_Glyph_StrokeBorder(&glyph, stroker, 0, 1) == 0) {
                    // 成功：glyph 已是外扩边框轮廓（原轮廓已随 destroy=1 释放）。
                    if (FT_Glyph_To_Bitmap(&glyph, FT_RENDER_MODE_NORMAL, nullptr, 1) == 0) {
                        const FT_BitmapGlyph bg = (const FT_BitmapGlyph)glyph;
                        const FT_Bitmap& bm = bg->bitmap;
                        if (bm.width > 0 && bm.rows > 0) {
                            out->width = (int)bm.width;
                            out->height = (int)bm.rows;
                            out->left = bg->left;
                            out->top = bg->top;
                            out->alpha.assign(size_t(bm.width) * bm.rows, 0);
                            const int pitch = bm.pitch;
                            for (unsigned y = 0; y < bm.rows; ++y) {
                                const uint8_t* src = bm.buffer +
                                    (pitch >= 0 ? (long)y * pitch : -(long)y * pitch);
                                for (unsigned x = 0; x < bm.width; ++x)
                                    out->alpha[size_t(y) * bm.width + x] = src[x];
                            }
                            have_ring = true;
                        }
                    }
                }
                if (!have_ring) FT_Done_Glyph(glyph);
            }
        }
        // 非轮廓字形（内嵌位图字体等）→ 落到膨胀退路
    }
    if (!have_ring) {
    // ── 退路：全向圆盘膨胀（源 = 已渲染 alpha 位图，盒外扩 r px 防裁切）──
    if (FT_Load_Char(face, (FT_ULong)cp, FT_LOAD_RENDER) != 0) return false;
    const FT_Bitmap& bm = face->glyph->bitmap;
    if (bm.width == 0 || bm.rows == 0) return false;
    const int r = (int)std::lround(width_px);
    const int pitch = bm.pitch;
    auto src_at = [&](int x, int y) -> int {
        if (x < 0 || y < 0 || x >= (int)bm.width || y >= (int)bm.rows) return 0;
        const uint8_t* row = bm.buffer + (pitch >= 0 ? (long)y * pitch : -(long)y * pitch);
        return row[x];
    };
    out->width = (int)bm.width + 2 * r;
    out->height = (int)bm.rows + 2 * r;
    out->left = face->glyph->bitmap_left - r;
    out->top = face->glyph->bitmap_top + r;
    out->alpha.assign(size_t(out->width) * out->height, 0);
    for (int y = 0; y < out->height; ++y) {
        for (int x = 0; x < out->width; ++x) {
            uint8_t best = 0;
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (dx * dx + dy * dy > r * r) continue; // 圆盘
                    const int a = src_at(x - r + dx, y - r + dy);
                    if (a > best) best = (uint8_t)a;
                }
            }
            out->alpha[size_t(y) * out->width + x] = best;
        }
    }
    } // !have_ring（膨胀退路）
    // ── 并入字形自身位图（cover_fill）：描边色须盖住字形自身的 AA 边，
    // 否则环内沿与字形之间留 1px 亮缝（FT smooth 渲染的 AA 边可越出 hinted
    // 轮廓 ≤1px，落在环的孔内）。只在"字形墨迹"处补 255，故不会把环外扩到
    // 字形轮廓之外（不改变描边的外沿宽度）。
    if (cover_fill && out->width > 0 && out->height > 0) {
        if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)ppem) == 0 &&
            FT_Load_Char(face, (FT_ULong)cp, FT_LOAD_RENDER) == 0 &&
            face->glyph->bitmap.width > 0 && face->glyph->bitmap.rows > 0) {
            const FT_Bitmap& fbm = face->glyph->bitmap;
            const int fl = face->glyph->bitmap_left, ft = face->glyph->bitmap_top;
            const int fpitch = fbm.pitch;
            const int cmin = std::min(out->left, fl);
            const int cmax = std::max(out->left + out->width, fl + (int)fbm.width);
            const int tmax = std::max(out->top, ft);
            const int tmin = std::min(out->top - out->height, ft - (int)fbm.rows);
            EdgeBitmap n;
            n.width = cmax - cmin;
            n.height = tmax - tmin;
            n.left = cmin;
            n.top = tmax;
            n.alpha.assign(size_t(n.width) * n.height, 0);
            for (int j = 0; j < out->height; ++j)
                for (int i = 0; i < out->width; ++i)
                    n.alpha[size_t(n.top - 1 - (out->top - 1 - j)) * n.width +
                            (out->left + i - n.left)] =
                        out->alpha[size_t(j) * out->width + i];
            for (unsigned j = 0; j < fbm.rows; ++j) {
                const uint8_t* row = fbm.buffer +
                    (fpitch >= 0 ? (long)j * fpitch : -(long)j * fpitch);
                for (unsigned i = 0; i < fbm.width; ++i) {
                    if (row[i] == 0) continue;
                    n.alpha[size_t(n.top - 1 - (ft - 1 - (int)j)) * n.width +
                            (fl + (int)i - n.left)] = 255;
                }
            }
            *out = std::move(n);
        }
    }
    return true;
}

FontSystem::FontSystem(const oa::fs::IFileSystem* fs, const oa::runtime::GameRuntime* rt)
    : fs_(fs), rt_(rt)
{
    if (FT_Init_FreeType(&ft_lib) != 0) {
        std::fprintf(stderr, "[app] freetype init failed\n");
        ft_lib = nullptr;
        return;
    }
    // 连续描边：FT_Stroker 复用同一 library；失败则描边走
    // 圆盘膨胀退路（raster_edge_alpha 内分支），不影响填充面。
    if (FT_Stroker_New(ft_lib, &stroker_) != 0) stroker_ = nullptr;
    // Default text face resolution: the engine default font
    // (font/sourcehansans-medium.otf) is an FPM asset. Projects ship their
    // own CJK font under font/ (NekoMiko: font/GenJyuuGothic-Bold.ttf) —
    // without the fallback below the face stays null and no glyph is ever
    // drawn (story text invisible in windows). Order: the engine default
    // name, then a listing of the project's font/ directory, then a small
    // explicit candidate list for backends that cannot list.
    std::string chosen;
    auto try_load = [&](const std::string& logical) -> bool {
        auto fb = fs->read(logical);
        if (!fb) return false;
        FaceEntry fe;
        fe.bytes = *fb;
        if (FT_New_Memory_Face(ft_lib, fe.bytes.data(), (FT_Long)fe.bytes.size(), 0,
                               &fe.face) != 0)
            return false;
        default_face = fe.face;
        font_faces[kDefaultFace] = std::move(fe);
        chosen = logical;
        return true;
    };
    if (!try_load(std::string(oa::render::kDefaultFontFile))) {
        bool found = false;
        if (auto files = fs->list("font")) {
            std::sort(files->begin(), files->end());
            for (const auto& name : *files) {
                if (!is_font_file(name)) continue;
                if (try_load("font/" + name)) { found = true; break; }
            }
        }
        if (!found) {
            for (const char* cand : {"font/GenJyuuGothic-Bold.ttf",
                                     "font/GenJyuuGothic-Monospace-Bold.ttf",
                                     "font/sourcehansans-medium.otf"}) {
                if (try_load(cand)) break;
            }
        }
    }
    if (default_face)
        std::printf("[app] freetype default font ready: %s\n", chosen.c_str());
}

FontSystem::~FontSystem()
{
    // 字形纹理经后端销毁（release_all 已 shutdown 渲染器时与后端抽象前直接
    // SDL_DestroyTexture 同语义——sdl 后端 destroy 不依赖渲染器存活）。
    for (auto& [k, cg] : glyph_cache)
        if (cg.tex && backend_) backend_->destroy_texture(cg.tex);
    for (auto& [k, cg] : edge_cache)
        if (cg.tex && backend_) backend_->destroy_texture(cg.tex);
    for (auto& [k, fe] : font_faces)
        if (fe.face) FT_Done_Face(fe.face);
    if (stroker_) FT_Stroker_Done(stroker_);
    if (ft_lib) FT_Done_FreeType(ft_lib);
}

FT_Face FontSystem::load_font_face(const std::string& logical)
{
    if (!ft_lib) return nullptr;
    if (logical.empty()) return nullptr;
    const auto fit = font_faces.find(logical);
    if (fit != font_faces.end()) return fit->second.face;
    // 候选：原路径 / .otf / .ttf（face 可带 magic 前缀）
    std::optional<std::vector<uint8_t>> bytes;
    std::string resolved_name = logical;
    
    for (const std::string& cand :
        { logical, logical + ".otf", logical + ".ttf" }) {
        std::string r = rt_->interpreter().resolve_magic_path(cand);
        if (auto b = fs_->read(r)) {
            bytes = b;
            resolved_name = r;
            break;
        }
    }

    if (!bytes) return nullptr;
    FaceEntry fe;
    fe.bytes = *bytes;
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(ft_lib, fe.bytes.data(), (FT_Long)fe.bytes.size(), 0,
        &face) != 0) {
        return nullptr;
    }
    fe.face = face;
    font_faces[logical] = std::move(fe);
    // Per-face-load debug line (OA_DEBUG_FONT=1); see text.cpp font_debug_enabled.
    const char* dbg = std::getenv("OA_DEBUG_FONT");
    if (dbg && *dbg == '1' && !dbg[1])
        std::printf("[app] text face: %s <- %s\n", logical.c_str(), resolved_name.c_str());
    return font_faces[logical].face;
}

FT_Face FontSystem::face_for(const oa::render::FontDesc& f)
{
    const std::string face = f.face();
    FT_Face r = face.empty() ? default_face : load_font_face(face);
    if (!r) r = default_face; // 缺字形回退：脚本字体缺失 → 默认字体
    return r;
}

uint32_t FontSystem::utf8_next(const std::string& s, size_t& i)
{
    if (i >= s.size()) return 0;
    const uint8_t c = (uint8_t)s[i];
    if (c < 0x80) {
        ++i;
        return c;
    }
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
    uint32_t cp = c & (extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07);
    for (int k = 1; k <= extra && i + k < s.size(); ++k)
        cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
    i += extra + 1;
    return cp;
}

int FontSystem::raster_ppem(FT_Face face, double size)
{
    if (face) {
        const long upem = (long)face->units_per_EM;
        const long hh = (long)face->ascender - (long)face->descender; // desc<0
        if (upem > 0 && hh > 0 && size > 0.0) {
            const double ppem = size * double(upem) / double(hh);
            if (ppem >= 1.0) return (int)std::lround(ppem);
        }
    }
    return (int)std::lround(size);
}

FontSystem::GlyphMeasure FontSystem::measure_glyph(const FontDesc& f, uint32_t cp)
{
    GlyphMeasure gm;
    const double size = f.size();
    FT_Face face = face_for(f);
    if (!face) {
        // 字体未就绪：字号方块占位（绘制面此时本就不出字形）。
        gm.ppem = raster_ppem(nullptr, size);
        gm.advance = size;
        gm.ink_w = size;
        gm.ink_h = size;
        gm.ascent = size;
        return gm;
    }
    const int ppem = raster_ppem(face, size);
    gm.ppem = double(ppem);
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)ppem) != 0 ||
        FT_Load_Char(face, (FT_ULong)cp, FT_LOAD_DEFAULT) != 0) {
        gm.advance = size;
        gm.ink_w = size;
        gm.ink_h = size;
        gm.ascent = face->size ? double(face->size->metrics.ascender) / 64.0 : size;
        return gm;
    }
    const double adv = double(face->glyph->advance.x) / 64.0;
    gm.advance = adv > 0.0 ? adv : 0.0;
    // 墨迹框宽/高（metrics.* = outline 包围盒比例语义；空格等无墨迹 → 0）。
    gm.ink_w = double(face->glyph->metrics.width) / 64.0;
    gm.ink_h = double(face->glyph->metrics.height) / 64.0;
    gm.ascent = face->size ? double(face->size->metrics.ascender) / 64.0 : size;
    // 绘制落点：x = 左 bearing（bitmap_left）；行内上偏移 = ascent − bitmap_top
    // （对应 offset_y = sf.ascent + px_bounds.min.y）。
    gm.offset_x = double(face->glyph->bitmap_left);
    gm.offset_y = gm.ascent - double(face->glyph->bitmap_top);
    return gm;
}

oa::render::CharMetrics FontSystem::metrics_for(const FontDesc& f, uint32_t cp)
{
    oa::render::CharMetrics m;
    const GlyphMeasure g = measure_glyph(f, cp);
    m.advance = g.advance; // 0 步进字形保持 0（同 advance_x）
    m.width = g.ink_w;     // 换行判定宽（0 = 无墨迹，不触发换行）
    m.height = g.ink_h;
    return m;
}

static CharMetrics metrics_fn(void* userdata, const oa::render::FontDesc& f, uint32_t cp)
{
    return static_cast<FontSystem*>(userdata)->metrics_for(f, cp);
}

FontSystem::CachedGlyph FontSystem::glyph_slot(oa::render::RenderBackend* backend,
                                               FT_Face face, double size,
                                               uint32_t cp)
{
    CachedGlyph none;
    if (!face || !backend) return none;
    char keybuf[96];
    std::snprintf(keybuf, sizeof(keybuf), "%p\t%d\t%u", (void*)face,
        raster_ppem(face, size), cp);
    const std::string key = keybuf;
    const auto it = glyph_cache.find(key);
    if (it != glyph_cache.end()) return it->second;
    CachedGlyph cg;
    const int ppem = raster_ppem(face, size);
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)ppem) == 0 &&
        FT_Load_Char(face, (FT_ULong)cp, FT_LOAD_RENDER) == 0) {
        FT_Bitmap& bm = face->glyph->bitmap;
        if (bm.width > 0 && bm.rows > 0) {
            std::vector<uint8_t> rgba;
            rgba.reserve(size_t(bm.width) * bm.rows * 4);
            for (unsigned y = 0; y < bm.rows; ++y) {
                for (unsigned x = 0; x < bm.width; ++x) {
                    const uint8_t a = bm.buffer[y * bm.pitch + x];
                    rgba.push_back(255);
                    rgba.push_back(255);
                    rgba.push_back(255);
                    rgba.push_back(a);
                }
            }
            cg.tex = backend->create_texture((int)bm.width, (int)bm.rows,
                                             oa::render::TextureAccess::Static);
            if (cg.tex) {
                backend->update_texture(cg.tex, rgba.data(), (int)bm.width * 4);
                backend->set_texture_blend(cg.tex, oa::render::BlendMode::Blend);
            }
            cg.bitmap_left = face->glyph->bitmap_left;
            cg.bitmap_top = face->glyph->bitmap_top;
        }
    }
    glyph_cache[key] = cg; // 缺字形/栅格失败 → 空槽占位（不再重试）
    return cg;
}

FontSystem::CachedGlyph FontSystem::edge_slot(oa::render::RenderBackend* backend,
                                              FT_Face face, double size,
                                              uint32_t cp, double width_px)
{
    CachedGlyph none;
    if (!face || !backend || width_px < 1.0) return none;
    const int ppem = raster_ppem(face, size);
    const EdgeEncoding enc = edge_encoding_from_env();
    char keybuf[128];
    std::snprintf(keybuf, sizeof(keybuf), "%p\t%d\t%u\t%.2f\t%d", (void*)face, ppem,
        cp, width_px, (int)enc);
    const std::string key = keybuf;
    const auto it = edge_cache.find(key);
    if (it != edge_cache.end()) return it->second;
    CachedGlyph cg;
    EdgeBitmap eb;
    // cover_fill=true：描边色盖住字形自身 AA 边（防环内亮缝），见头文件。
    if (raster_edge_alpha(ft_lib, stroker_, face, ppem, cp, width_px, enc, true, &eb)) {
        std::vector<uint8_t> rgba;
        rgba.reserve(eb.alpha.size() * 4);
        for (uint8_t a : eb.alpha) {
            rgba.push_back(255);
            rgba.push_back(255);
            rgba.push_back(255);
            rgba.push_back(a);
        }
        cg.tex = backend->create_texture(eb.width, eb.height,
                                         oa::render::TextureAccess::Static);
        if (cg.tex) {
            backend->update_texture(cg.tex, rgba.data(), eb.width * 4);
            backend->set_texture_blend(cg.tex, oa::render::BlendMode::Blend);
        }
        cg.bitmap_left = eb.left;
        cg.bitmap_top = eb.top;
    }
    edge_cache[key] = cg;
    return cg;
}

std::optional<oa::render::Rgba> FontSystem::parse_rgb(const std::string& raw)
{
    auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
        };
    std::string v = raw;
    if (v.size() > 1 && v[0] == '#') v = v.substr(1);
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
        v = v.substr(2);
    else if (v.size() > 1 && v[0] == 'x')
        v = v.substr(1);
    else if (v.size() == 7 && v[0] == '0')
        v = v.substr(1); // Artemis "0RRGGBB"
    if (v.size() == 6 && std::all_of(v.begin(), v.end(),
        [&](char ch) { return hex(ch) >= 0; })) {
        oa::render::Rgba c;
        c.r = (uint8_t)(hex(v[0]) * 16 + hex(v[1]));
        c.g = (uint8_t)(hex(v[2]) * 16 + hex(v[3]));
        c.b = (uint8_t)(hex(v[4]) * 16 + hex(v[5]));
        c.a = 255;
        return c;
    }
    // Lua value_to_string converts 0xffffff → "16777215" (decimal integer).
    if (!v.empty() && std::all_of(v.begin(), v.end(),
        [](char ch) { return ch >= '0' && ch <= '9'; })) {
        char* end = nullptr;
        const unsigned long n = std::strtoul(v.c_str(), &end, 10);
        if (end == v.c_str() + v.size() && n <= 0xFFFFFF) {
            oa::render::Rgba c;
            c.r = (uint8_t)((n >> 16) & 0xFF);
            c.g = (uint8_t)((n >> 8) & 0xFF);
            c.b = (uint8_t)(n & 0xFF);
            c.a = 255;
            return c;
        }
    }
    return std::nullopt;
}

oa::render::Rgba FontSystem::text_color(const oa::render::FontDesc& f)
{
    oa::render::Rgba c{ 0, 0, 0, 255 };
    const std::string col = f.color();
    if (const auto p = parse_rgb(col)) c = *p;
    return c;
}

void FontSystem::style_colors(const oa::render::FontDesc& f, oa::render::Rgba* shadow,
    oa::render::Rgba* outline)
{
    *shadow = oa::render::Rgba{ 0, 0, 0, 96 };
    *outline = oa::render::Rgba{ 0, 0, 0, 255 };
    if (const std::string* v = f.get("shadowcolor"))
        if (const auto p = parse_rgb(*v)) *shadow = *p;
    if (const std::string* v = f.get("outlinecolor"))
        if (const auto p = parse_rgb(*v)) *outline = *p;
}

size_t FontSystem::draw_text_layer(oa::render::RenderBackend* backend,
                                   const oa::render::MessageLayer& ml,
                                   const oa::render::Affine2& world,
                                   double opacity)
{
    if (!backend) return 0;
    if (!default_face && font_faces.empty()) return 0;
    const oa::render::LaidPage page = oa::render::layout_page(
        ml, metrics_fn, rt_->text().layout_config(), this);
    if (page.glyphs.empty()) return 0;
    size_t drawn = 0;
    // 字形布局坐标 = 文本节点本地坐标（节点世界变换的 text_for 注入语义：
    // glyph 落点 ly.left/top+排版，再整体乘节点世界变换）。
    // layout_page 恒产出 ≥1 的行高（text_line_metrics .max(1.0)）。
    const double line_h = page.line_height > 0 ? page.line_height : 40.0;
    // 通用行盒语义: FPM 字表 top 是"行盒
    // 顶",行内内容的纵向起点 = 行盒顶 + spacetop —— spacetop 是 FPM
    // text_line_metrics 行高公式里的内容上边距,可为负。此前引擎丢弃了该偏移,
    // 负 spacetop 的帮助字表整体偏低(dock mwhelp=-8 → 提示文字沉在 dock 亮条
    // 下半不可读)。这里按通用语义补回行内容顶,不再需要任何
    // id/字号判据;spacetop=0 的字表(正文/backlog 等)数值不变 → 逐像素不变
    // (像素窗口验证)。判据完全来自 FPM 布局字段本身,撤销了控件特异 hack。
    const double line_content_top = ml.top + ml.font.spacetop();
    const bool plain = world.is_plain_translation();
    auto map = [&](double lx, double ly, double* wx, double* wy) {
        if (plain) {
            *wx = lx + world.e;
            *wy = ly + world.f;
        } else {
            world.transform_point(lx, ly, wx, wy);
        }
    };
    // 画一个字形贴图：本地左上角 (lx,ly)、尺寸取贴图实际宽高；
    // alpha = 请求 alpha × 链不透明度（近似 cmd.opacity *= opacity）。
    auto draw_glyph_tex = [&](oa::render::TextureRef tex, double lx, double ly,
        const oa::render::Rgba& col, double alpha) {
            float wf = 0, hf = 0;
            backend->texture_size(tex, &wf, &hf);
            backend->set_texture_color_mod(tex, col.r, col.g, col.b);
            const int am = int(alpha * opacity + 0.5);
            backend->set_texture_alpha_mod(
                tex, (uint8_t)(am < 0 ? 0 : am > 255 ? 255 : am));
            if (plain) {
                const oa::render::FRect dst{ float(lx + world.e),
                    float(ly + world.f), float(wf), float(hf) };
                backend->draw_texture(tex, nullptr, &dst);
                return;
            }
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            map(lx, ly, &x0, &y0);
            map(lx + wf, ly, &x1, &y1);
            map(lx, ly + hf, &x2, &y2);
            const oa::render::FPoint origin{ float(x0), float(y0) };
            const oa::render::FPoint right{ float(x1), float(y1) };
            const oa::render::FPoint down{ float(x2), float(y2) };
            backend->draw_texture_affine(tex, nullptr, origin, right, down);
        };
    for (const oa::render::LaidGlyph& g : page.glyphs) {
        if (g.newline || !g.font) continue;
        // reveal：只画已揭示字符
        if (g.order >= ml.reveal_index) continue;
        const double gsize = g.font->size();
        // 栅格/落点按比例语义（ppem=round(size·upem/H)，见 measure_glyph）。
        const GlyphMeasure gm = measure_glyph(*g.font, g.cp);
        const CachedGlyph cg = glyph_slot(backend, face_for(*g.font), gsize, g.cp);
        if (!cg.tex) continue;
        // 本地落点：x = ml.left + 排版 x + 左 bearing；y = 行内容顶
        // (line_content_top = 行盒顶 ml.top + spacetop) + 行内偏移 + ascent
        // − 位图顶（offset_x=b.min.x、offset_y=sf.ascent+b.min.y，
        // y-down 下 min.y ≡ −bitmap_top）。
        const double lx = ml.left + g.x + double(cg.bitmap_left);
        const double ly = line_content_top + double(g.line) * line_h +
            gm.ascent - double(cg.bitmap_top);
        const oa::render::Rgba col = text_color(*g.font);
        oa::render::Rgba shadow{ 0, 0, 0, 0 };
        oa::render::Rgba outline{ 0, 0, 0, 0 };
        style_colors(*g.font, &shadow, &outline);
        // 描边/阴影开关语义（两代框架并存）：
        //   - FPM 系（gt/NekoMiko …）[font] 行带 style="outline,shadow" 词;
        //   - iMel 系（rr/N1/N2/btjy/slny/tg3 list_windows*.tbl lang.font.*：
        //     adv01/name/select/config01… 及 boot.lua get_fontdata 原样下发）
        //     不带 style 词,用数值键 outline=N/shadow=N(+ outlinecolor/
        //     shadowcolor)。旧实现只认 style 词 → iMel 系永不描边：正文白字
        //     无边框(实机“没有边框”),设置页预览文字白底白字(实机“预览文字
        //     区域空白”)。
        //   现象 J+：数值键恢复**量纲**语义
        //     （outline=N → 描边宽度 N px、shadow=N → 偏移 N px）。曾把
        //     键值降级为"开关 + 恒 1px hairline"，但实机判据是：描边本该存在，
        //     "边框断裂"（4 个对角偏移副本在 N>1 时四周留缺口）才是被误判为
        //     异常黑边的根本原因。**保留此前的保守项**：数值路径必须该行显式
        //     给出可解析 outlinecolor，否则不描边（不回退默认黑）。
        //     style 词路径语义不变（宽度取同一 outline 键/缺省 1px）。
        const bool style_outline = style_word_on(*g.font, "outline");
        const bool style_shadow = style_word_on(*g.font, "shadow");
        double ow_key = 0, sd_key = 0;
        const bool num_outline_key = numeric_edge_amount(*g.font, "outline", &ow_key);
        bool num_outline = num_outline_key;
        if (num_outline) {
            const std::string* oc = g.font->get("outlinecolor");
            if (!oc || !parse_rgb(*oc)) num_outline = false;
        }
        const bool num_shadow = numeric_edge_amount(*g.font, "shadow", &sd_key);
        const bool has_shadow = style_shadow || num_shadow;
        const bool has_outline = style_outline || num_outline;
        // 描边宽度 px：数值键值（≥1）；缺失/不可解析 → 1px（style 词路径）。
        const double os = ow_key >= 1.0 ? ow_key : 1.0;
        // 阴影/描边/正文 pass：偏移在本地坐标加后再经世界映射（为世界坐标
        // 偏移 +sd；平移路径下两者严格等价，仿射路径下相差旋转/缩放的 ~1px 级，
        // 记录为近似）。
        auto emit_pass = [&](double offx, double offy, const oa::render::Rgba& pc, double pa) {
            draw_glyph_tex(cg.tex, lx + offx, ly + offy, pc, pa);
        };
        if (has_shadow) {
            // 数值键 = 偏移 px；纯 style 词路径保留既有 2px。
            const double sd = num_shadow ? sd_key : 2.0;
            emit_pass(sd, sd, shadow, 96);
        }
        if (has_outline) {
            // 连续描边：单层独立描边位图（FT_Stroker 外扩
            // os px，自带 bearing），绘在填充层之下。旧"4 对角副本"仅在
            // OA_FONT_OUTLINE=legacy（PRE 像素对照）时走。
            if (legacy_edge_env()) {
                const double off[][2] = { {-1, -1}, {1, -1}, {-1, 1}, {1, 1} };
                for (const auto& o : off)
                    emit_pass(o[0] * os, o[1] * os, outline, 255);
            } else {
                const CachedGlyph eg =
                    edge_slot(backend, face_for(*g.font), gsize, g.cp, os);
                if (eg.tex) {
                    // 与填充同一原点网格（bearing 相对笔端）⇒ 落点公式一致。
                    const double ex = ml.left + g.x + double(eg.bitmap_left);
                    const double ey = line_content_top + double(g.line) * line_h +
                        gm.ascent - double(eg.bitmap_top);
                    draw_glyph_tex(eg.tex, ex, ey, outline, 255);
                }
            }
        }
        emit_pass(0, 0, col, 255);
        ++drawn;
    }
    // ── 注音（ruby）：整段居中于基础区上方（ruby_top =
    // ly.top + metrics.ruby_top + line·line_height，；ruby_top =
    // −(ruby_height + spacemiddle)）；字形逐字按 ruby_positions
    // ：x_i = base_x0 + ((base_x1−base_x0)−total)/2 +
    // Σ(a_j+rubykerning)。ruby 字形与用主字体栅格化（rubyface 参数
    // 仅存不读；FPM rubyface≡主字体，差异面为 0）。
    double ruby_top_offset = 0;
    {
        const double ruby_height = std::max(0.0, ml.font.rubysize());
        ruby_top_offset = -(ruby_height + ml.font.spacemiddle());
    }
    double ruby_kerning = 0;
    if (const std::string* v = ml.font.get("rubykerning")) {
        double d = 0;
        char* end = nullptr;
        d = std::strtod(v->c_str(), &end);
        if (end != v->c_str() + v->size() || !std::isfinite(d)) d = 0.0;
        ruby_kerning = d;
    }
    const oa::render::Rgba col = text_color(ml.font);
    for (const oa::render::LaidRuby& rb : page.rubies) {
        if (rb.text.empty()) continue;
        double rsize = rb.size > 0 ? rb.size : 20.0;
        FT_Face rface = default_face ? default_face : load_font_face(kDefaultFace);
        if (!rface) continue;
        std::vector<uint32_t> cps;
        for (size_t i2 = 0; i2 < rb.text.size();) cps.push_back(utf8_next(rb.text, i2));
        if (cps.empty()) continue;
        // 注音度量（同一主面 + rubysize 比例语义）
        FontDesc rfd;
        rfd.raw["size"] = std::to_string(rsize);
        std::vector<GlyphMeasure> measures;
        measures.reserve(cps.size());
        double total = 0;
        for (const uint32_t rcp : cps) {
            measures.push_back(measure_glyph(rfd, rcp));
            total += measures.back().advance;
        }
        total += ruby_kerning * (double)(cps.size() - 1);
        double x_cursor = ml.left + rb.x + (rb.width - total) * 0.5;
        const double ruby_line_top =
            line_content_top + ruby_top_offset + double(rb.line) * line_h;
        for (size_t k = 0; k < cps.size(); ++k) {
            const uint32_t rcp = cps[k];
            const GlyphMeasure& gm = measures[k];
            const CachedGlyph cg = glyph_slot(backend, rface, rsize, rcp);
            if (!cg.tex) {
                x_cursor += gm.advance + ruby_kerning;
                continue;
            }
            const double lx = x_cursor + double(cg.bitmap_left);
            const double ly = ruby_line_top + (gm.ascent - double(cg.bitmap_top));
            draw_glyph_tex(cg.tex, lx, ly, col, 255);
            x_cursor += gm.advance + ruby_kerning;
            ++drawn;
        }
    }
    return drawn;
}

FontSystem::TextExtents FontSystem::measure_text_extents(const oa::render::MessageLayer& ml) const
{
    TextExtents ext;
    if (!const_cast<FontSystem*>(this)->face_for(ml.font)) return ext;
    const oa::render::LaidPage page = oa::render::layout_page(
        ml, metrics_fn, rt_->text().layout_config(),
        const_cast<FontSystem*>(this));
    if (page.glyphs.empty()) return ext;
    uint32_t max_line = 0;
    for (const auto& g : page.glyphs) {
        if (g.newline) continue;
        if (g.line > max_line) max_line = g.line;
    }
    double max_w = 0;
    double last_w = 0;
    for (const auto& g : page.glyphs) {
        if (g.newline) continue;
        // 右端按笔端（x+advance）计（active_layer_text_metrics，
        // 用 advance_x；换行符 advance=0 不影响）。
        const double right = g.x + g.advance;
        if (right > max_w) max_w = right;
        if (g.line == max_line) {
            const double r2 = g.x + g.advance;
            if (r2 > last_w) last_w = r2;
        }
    }
    ext.width = max_w;
    ext.height = (max_line + 1) * page.line_height;
    ext.line_width = last_w;
    return ext;
}

}
