// fontedge — 描边环断裂度量（现象 J+，research/126）。
//
// 对同一字形的两种描边模型给数字：
//   PRE  = legacy（4 个对角偏移副本，{-1,-1},{1,-1},{-1,1},{1,1} × N，
//          research/120 及以前的 draw_text_layer 行为）
//   POST = 连续描边（oa::render::raster_edge_alpha，FT_Stroker 外扩 N px；
//          工程真实绘制路径调用的同一函数）
//
// 度量口径（沿字形边界采样，全部在"笔端原点"网格上做，两种模型同网格）：
//   gap_dirs    沿边界的外向 4-邻方向中，环未覆盖（alpha<128）的方向数
//   gap_clusters 上述未覆盖像素的 8 连通簇数（= 断裂缺口数）
//   thick_min/max/sd  每个外向方向上环的连续覆盖长度（px）统计；
//               未覆盖方向计 0（故 min=0 即存在缺口）
//   ideal/ring/missing_px  POST 环像素数 / 本模型环像素数 / 差集；
//               missing 再分为 adjacent（8 邻接字形，属断裂）与 outer（更外，
//               属"偏移副本无法越出源位图盒"的裁切面）
//   box(+L/R/T/B)  环包围盒相对字形包围盒的外扩 px（POST 应 ≥ N ⇒ 无裁切）
//
// 用法: fontedge <font.ttf> [--widths 1,2,3] [--chars 邊響戯] [--ppem 40]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/render/font.h"

extern "C" {
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_STROKER_H
}

namespace {
constexpr int kThresh = 128;

struct Layer {
    std::vector<uint8_t> a;
    int w = 0, h = 0, left = 0, top = 0;
};

// 画布：列 c（x 右正）、行号 r（y 上正；r = top-1 为位图首行）。
struct Canvas {
    int cmin = 0, rmax = 0, w = 0, h = 0;
    std::vector<int> px;
    bool in(int c, int r) const {
        return c >= cmin && c < cmin + w && r > rmax - 1 - h && r <= rmax - 1;
    }
    int idx(int c, int r) const { return (rmax - 1 - r) * w + (c - cmin); }
    int get(int c, int r) const { return in(c, r) ? px[idx(c, r)] : 0; }
    void merge(const Layer& l) {
        if (l.w <= 0 || l.h <= 0) return;
        const int ncmin = w ? std::min(cmin, l.left) : l.left;
        const int ncmax = w ? std::max(cmin + w, l.left + l.w) : l.left + l.w;
        const int nrmax = w ? std::max(rmax, l.top) : l.top;
        const int nrmin = w ? std::min(rmax - h, l.top - l.h) : l.top - l.h;
        Canvas n;
        n.cmin = ncmin;
        n.rmax = nrmax;
        n.w = ncmax - ncmin;
        n.h = nrmax - nrmin;
        n.px.assign(size_t(n.w) * n.h, 0);
        for (int c = ncmin; c < ncmax; ++c)
            for (int r = nrmin; r < nrmax; ++r)
                n.px[n.idx(c, r)] = get(c, r);
        for (int j = 0; j < l.h; ++j)
            for (int i = 0; i < l.w; ++i) {
                const int c = l.left + i;
                const int r = l.top - 1 - j;
                n.px[n.idx(c, r)] = l.a[size_t(j) * l.w + i];
            }
        *this = std::move(n);
    }
    // 4 侧外向包围盒：{left, right, top, bottom}（含 alpha≥kThresh 的像素）
    void bbox(int* l, int* rr, int* t, int* b) const {
        *l = 1 << 30; *rr = -(1 << 30); *t = -(1 << 30); *b = 1 << 30;
        for (int c = cmin; c < cmin + w; ++c)
            for (int r = rmax - h; r < rmax; ++r)
                if (px[idx(c, r)] >= kThresh) {
                    if (c < *l) *l = c;
                    if (c > *rr) *rr = c;
                    if (r > *t) *t = r;
                    if (r < *b) *b = r;
                }
    }
};

bool load_fill(FT_Face face, int ppem, uint32_t cp, Layer* out)
{
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)ppem) != 0) return false;
    if (FT_Load_Char(face, (FT_ULong)cp, FT_LOAD_RENDER) != 0) return false;
    const FT_Bitmap& bm = face->glyph->bitmap;
    if (!bm.width || !bm.rows) return false;
    out->w = (int)bm.width;
    out->h = (int)bm.rows;
    out->left = face->glyph->bitmap_left;
    out->top = face->glyph->bitmap_top;
    out->a.assign(size_t(out->w) * out->h, 0);
    const int pitch = bm.pitch;
    for (unsigned y = 0; y < bm.rows; ++y) {
        const uint8_t* row = bm.buffer + (pitch >= 0 ? (long)y * pitch : -(long)y * pitch);
        for (unsigned x = 0; x < bm.width; ++x)
            out->a[size_t(y) * bm.width + x] = row[x];
    }
    return true;
}

// PRE：4 个对角偏移副本（每条 = 整张填充位图平移 ±N），取 max。
void legacy_ring(const Layer& fill, int n, Layer* out)
{
    out->w = fill.w + 2 * n;
    out->h = fill.h + 2 * n;
    out->left = fill.left - n;
    out->top = fill.top + n;
    out->a.assign(size_t(out->w) * out->h, 0);
    const int off[4][2] = { {1, 1}, {1, -1}, {-1, 1}, {-1, -1} }; // 目标 = 源 + off*N
    for (int j = 0; j < out->h; ++j)
        for (int i = 0; i < out->w; ++i) {
            int best = 0;
            for (const auto& o : off) {
                const int si = i - n - o[0] * n;
                const int sj = j - n - o[1] * n;
                if (si < 0 || sj < 0 || si >= fill.w || sj >= fill.h) continue;
                const int v = fill.a[size_t(sj) * fill.w + si];
                if (v > best) best = v;
            }
            out->a[size_t(j) * out->w + i] = (uint8_t)best;
        }
}

struct Stats {
    int hole_dirs = 0;    // 外向方向中，环未覆盖且该处字形 α=0（背景直接透出）
    int seam_dirs = 0;    // 环未覆盖但该处字形 α>0（字形自身 AA 边：亮缝）
    int hole_clusters = 0; // 上述"背景透出"像素的 8 连通簇数（= 可见断裂数）
    int thick_min = -1, thick_max = 0;
    double thick_mean = 0, thick_sd = 0;
    long ring_px = 0, ideal_px = 0, missing_px = 0, missing_adj = 0, missing_outer = 0;
    long miss_hole = 0; // missing 中字形 α=0 的像素（真洞：本模型环缺 → 背景/字色透出）
    int box[4] = { 0, 0, 0, 0 }; // +L,+R,+T,+B 外扩
};

// boundary = 字形核心（fill>=kThresh）；ring = 被评估模型的描边层；ideal = POST 绘制层。
Stats measure(const Canvas& fillc, const Canvas& ring, const Canvas& ideal)
{
    Stats st;
    auto core = [&](int c, int r) { return fillc.get(c, r) >= kThresh; };
    auto ink = [&](int c, int r) { return fillc.get(c, r) > 0; };
    auto on = [&](const Canvas& cv, int c, int r) { return cv.get(c, r) >= kThresh; };
    const int dirs[4][2] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
    std::vector<std::pair<int, int>> holes;
    std::vector<int> thicks;
    for (int c = fillc.cmin; c < fillc.cmin + fillc.w; ++c)
        for (int r = fillc.rmax - fillc.h; r < fillc.rmax; ++r) {
            if (!core(c, r)) continue;
            for (const auto& d : dirs) {
                const int qc = c + d[0], qr = r + d[1];
                if (core(qc, qr)) continue; // 内向
                if (!on(ring, qc, qr)) {
                    if (ink(qc, qr)) {
                        ++st.seam_dirs;
                    } else {
                        ++st.hole_dirs;
                        holes.push_back({ qc, qr });
                    }
                    thicks.push_back(0);
                    continue;
                }
                int len = 0, x = qc, y = qr;
                while (on(ring, x, y)) { ++len; x += d[0]; y += d[1]; }
                thicks.push_back(len);
            }
        }
    // 缺口 8 连通簇
    std::vector<char> seen(holes.size(), 0);
    for (size_t i = 0; i < holes.size(); ++i) {
        if (seen[i]) continue;
        ++st.hole_clusters;
        std::vector<size_t> stack{ i };
        seen[i] = 1;
        while (!stack.empty()) {
            const size_t k = stack.back();
            stack.pop_back();
            for (size_t j = 0; j < holes.size(); ++j) {
                if (seen[j]) continue;
                if (std::abs(holes[j].first - holes[k].first) <= 1 &&
                    std::abs(holes[j].second - holes[k].second) <= 1) {
                    seen[j] = 1;
                    stack.push_back(j);
                }
            }
        }
    }
    if (!thicks.empty()) {
        st.thick_min = thicks[0];
        double sum = 0;
        for (int t : thicks) {
            if (t < st.thick_min) st.thick_min = t;
            if (t > st.thick_max) st.thick_max = t;
            sum += t;
        }
        st.thick_mean = sum / (double)thicks.size();
        double var = 0;
        for (int t : thicks) var += (t - st.thick_mean) * (t - st.thick_mean);
        st.thick_sd = std::sqrt(var / (double)thicks.size());
    }
    // 像素差集：ideal \ ring，按"8 邻接字形核心"分断裂/裁切
    for (int c = ideal.cmin; c < ideal.cmin + ideal.w; ++c)
        for (int r = ideal.rmax - ideal.h; r < ideal.rmax; ++r) {
            if (!on(ideal, c, r)) continue;
            ++st.ideal_px;
            if (on(ring, c, r)) continue;
            ++st.missing_px;
            if (!ink(c, r)) ++st.miss_hole;
            bool adj = false;
            for (int dc = -1; dc <= 1 && !adj; ++dc)
                for (int dr = -1; dr <= 1; ++dr)
                    if (core(c + dc, r + dr)) { adj = true; break; }
            if (adj) ++st.missing_adj; else ++st.missing_outer;
        }
    for (int c = ring.cmin; c < ring.cmin + ring.w; ++c)
        for (int r = ring.rmax - ring.h; r < ring.rmax; ++r)
            if (on(ring, c, r)) ++st.ring_px;
    int fl, fr, ft, fb, rl, rr2, rt, rb;
    fillc.bbox(&fl, &fr, &ft, &fb);
    ring.bbox(&rl, &rr2, &rt, &rb);
    st.box[0] = fl - rl;      // 左外扩
    st.box[1] = rr2 - fr;     // 右
    st.box[2] = rt - ft;      // 上
    st.box[3] = fb - rb;      // 下
    return st;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: fontedge <font.ttf> [--widths 1,2,3] "
                             "[--chars 邊響戯] [--ppem 40]\n");
        return 2;
    }
    std::string path = argv[1];
    std::string chars = "邊響戯";
    std::vector<int> widths{ 1, 2, 3 };
    int ppem = 40;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--widths" && i + 1 < argc) {
            widths.clear();
            std::string s = argv[++i];
            size_t p = 0;
            while (p < s.size()) {
                size_t q = s.find(',', p);
                if (q == std::string::npos) q = s.size();
                widths.push_back(std::atoi(s.substr(p, q - p).c_str()));
                p = q + 1;
            }
        } else if (a == "--chars" && i + 1 < argc) {
            chars = argv[++i];
        } else if (a == "--ppem" && i + 1 < argc) {
            ppem = std::atoi(argv[++i]);
        }
    }
    FT_Library lib = nullptr;
    FT_Stroker stroker = nullptr;
    FT_Face face = nullptr;
    if (FT_Init_FreeType(&lib) != 0 || FT_Stroker_New(lib, &stroker) != 0 ||
        FT_New_Face(lib, path.c_str(), 0, &face) != 0) {
        std::fprintf(stderr, "fontedge: cannot load %s\n", path.c_str());
        return 1;
    }
    std::printf("# font=%s ppem=%d\n", path.c_str(), ppem);
    std::printf("%-4s %-8s %5s %6s %7s %6s %9s %9s %8s %8s %8s %8s %8s %8s %6s %6s\n",
        "char", "model", "N(px)", "holeD", "holeClu", "seamD", "thk_min", "thk_max",
        "thk_sd", "ring_px", "miss_px", "missHole", "missAdj", "missOut", "boxL", "boxT");
    // 逐字符的 UTF-8 分解
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < chars.size();) {
        const uint8_t c = (uint8_t)chars[i];
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
        uint32_t cp = c < 0x80 ? c : (c & (extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07));
        for (int k = 1; k <= extra && i + k < chars.size(); ++k)
            cp = (cp << 6) | ((uint8_t)chars[i + k] & 0x3F);
        cps.push_back(cp);
        i += extra + 1;
    }
    for (uint32_t cp : cps) {
        char ubuf[8] = { 0 };
        if (cp < 0x80) { ubuf[0] = (char)cp; }
        else if (cp < 0x800) { ubuf[0] = (char)(0xC0 | (cp >> 6)); ubuf[1] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            ubuf[0] = (char)(0xE0 | (cp >> 12));
            ubuf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            ubuf[2] = (char)(0x80 | (cp & 0x3F));
        }
        Layer fill;
        if (!load_fill(face, ppem, cp, &fill)) {
            std::printf("%-4s (no glyph / no bitmap)\n", ubuf);
            continue;
        }
        Canvas fc;
        fc.merge(fill);
        for (int n : widths) {
            Layer leg;
            legacy_ring(fill, n, &leg);
            Canvas lc;
            lc.merge(leg);
            auto edge = [&](oa::render::EdgeEncoding enc, bool cover) -> Layer {
                oa::render::EdgeBitmap eb;
                Layer l;
                if (oa::render::raster_edge_alpha(lib, stroker, face, ppem, cp, (double)n,
                        enc, cover, &eb)) {
                    l.a = eb.alpha; l.w = eb.width; l.h = eb.height;
                    l.left = eb.left; l.top = eb.top;
                }
                return l;
            };
            const Layer stl = edge(oa::render::EdgeEncoding::Stroke, true);   // 绘制层（POST）
            const Layer raw = edge(oa::render::EdgeEncoding::Stroke, false);  // 纯描边环
            const Layer dl = edge(oa::render::EdgeEncoding::Dilate, true);    // 退路（绘制层）
            Canvas sc, rc, dc;
            sc.merge(stl);
            rc.merge(raw);
            dc.merge(dl);
            const Stats sl = measure(fc, lc, sc);
            const Stats sr = measure(fc, rc, sc);
            const Stats sp = measure(fc, sc, sc);
            const Stats sd = measure(fc, dc, sc);
            auto row = [&](const char* model, const Stats& s) {
                std::printf("%-4s %-8s %5d %6d %7d %6d %9d %9d %8.2f %8ld %8ld %8ld %8ld %8ld %6d %6d\n",
                    ubuf, model, n, s.hole_dirs, s.hole_clusters, s.seam_dirs,
                    s.thick_min, s.thick_max, s.thick_sd, s.ring_px, s.missing_px,
                    s.miss_hole, s.missing_adj, s.missing_outer, s.box[0], s.box[2]);
            };
            row("legacy", sl);   // PRE：4 对角副本
            row("rawring", sr);  // 纯 FT_Stroker 环（未并字形位图）
            row("drawn", sp);    // POST：绘制层（环 ∪ 字形位图）
            row("dilate", sd);   // 退路：圆盘膨胀（绘制层）
        }
    }
    FT_Done_Face(face);
    FT_Stroker_Done(stroker);
    FT_Done_FreeType(lib);
    return 0;
}
