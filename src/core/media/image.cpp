#include "core/media/image.h"

#include <csetjmp>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <png.h>
#include <jpeglib.h>
}

// 图片
namespace oa::media {

namespace {

/// libjpeg error sink: default error_exit calls exit(); route it through a
/// longjmp so corrupt/truncated JPEGs fail decode_image() instead of killing
/// the process (same contract as decode_png returning false).
struct JpegErrorSink {
    jpeg_error_mgr pub;
    char message[JMSG_LENGTH_MAX] = {};
};

void jpeg_error_exit(j_common_ptr cinfo) {
    JpegErrorSink* sink = reinterpret_cast<JpegErrorSink*>(cinfo->err);
    sink->pub.format_message(cinfo, sink->message);
    throw std::runtime_error(sink->message);
}

void jpeg_output_message(j_common_ptr cinfo) {
    // Warnings (e.g. "Corrupt JPEG data: N extraneous bytes") are logged to
    // the sink only; no per-warning stderr spam on ordinary recompressed bgs.
    JpegErrorSink* sink = reinterpret_cast<JpegErrorSink*>(cinfo->err);
    (cinfo->err->format_message)(cinfo, sink->message);
}

bool decode_jpeg(const std::vector<uint8_t>& bytes, Image& out) {
    JpegErrorSink sink;
    jpeg_decompress_struct cinfo{};
    cinfo.err = jpeg_std_error(&sink.pub);
    sink.pub.error_exit = jpeg_error_exit;
    sink.pub.output_message = jpeg_output_message;
    try {
        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, bytes.data(), static_cast<unsigned long>(bytes.size()));
        if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
            std::fprintf(stderr, "jpeg decode failed: invalid header\n");
            jpeg_destroy_decompress(&cinfo);
            return false;
        }
        cinfo.out_color_space = JCS_RGB; // libjpeg expands gray/CMYK sources
        jpeg_start_decompress(&cinfo);
        const JDIMENSION w = cinfo.output_width;
        const JDIMENSION h = cinfo.output_height;
        const int comp = cinfo.output_components; // JCS_RGB -> 3
        if (w == 0 || h == 0 || w > 16384 || h > 16384 || comp <= 0 || comp > 4) {
            jpeg_abort_decompress(&cinfo);
            jpeg_destroy_decompress(&cinfo);
            std::fprintf(stderr, "jpeg decode failed: bad geometry %ux%u c=%d\n",
                         static_cast<unsigned>(w), static_cast<unsigned>(h), comp);
            return false;
        }
        out.w = static_cast<int>(w);
        out.h = static_cast<int>(h);
        out.rgba.assign(size_t(out.w) * size_t(out.h) * 4, 0);
        std::vector<uint8_t> row(size_t(w) * size_t(comp));
        while (cinfo.output_scanline < h) {
            JSAMPROW rows[1] = {row.data()};
            jpeg_read_scanlines(&cinfo, rows, 1);
            uint8_t* dst = out.rgba.data() + size_t(cinfo.output_scanline - 1) * size_t(w) * 4;
            if (comp == 3) {
                for (JDIMENSION x = 0; x < w; ++x) {
                    dst[x * 4] = row[x * 3];
                    dst[x * 4 + 1] = row[x * 3 + 1];
                    dst[x * 4 + 2] = row[x * 3 + 2];
                    dst[x * 4 + 3] = 255;
                }
            } else { // grayscale (or 4-component) safety fallback
                for (JDIMENSION x = 0; x < w; ++x) {
                    const uint8_t v = row[x * comp];
                    dst[x * 4] = v;
                    dst[x * 4 + 1] = v;
                    dst[x * 4 + 2] = v;
                    dst[x * 4 + 3] = 255;
                }
            }
        }
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "jpeg decode failed: %s\n", e.what());
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    return true;
}

} // namespace

bool decode_image(const std::vector<uint8_t>& bytes, Image& out) {
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xD8) {
        return decode_jpeg(bytes, out);
    }
    return decode_png(bytes, out);
}

bool decode_png(const std::vector<uint8_t>& bytes, Image& out) {
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, bytes.data(), bytes.size())) {
        std::fprintf(stderr, "png decode failed: %s\n", png.message);
        return false;
    }
    png.format = PNG_FORMAT_RGBA;
    out.w = (int)png.width;
    out.h = (int)png.height;
    out.rgba.resize(PNG_IMAGE_SIZE(png));
    if (!png_image_finish_read(&png, nullptr, out.rgba.data(), 0, nullptr)) {
        png_image_free(&png);
        return false;
    }
    png_image_free(&png);
    return true;
}

std::map<std::string, std::string> png_text_chunks(const std::vector<uint8_t>& bytes) {
    std::map<std::string, std::string> out;
    constexpr uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), kSig, 8) != 0) return out;
    size_t i = 8;
    while (i + 8 <= bytes.size()) {
        const uint32_t len = (uint32_t(bytes[i]) << 24) | (uint32_t(bytes[i + 1]) << 16) |
                             (uint32_t(bytes[i + 2]) << 8) | uint32_t(bytes[i + 3]);
        const uint8_t typ[4] = {bytes[i + 4], bytes[i + 5], bytes[i + 6], bytes[i + 7]};
        const size_t data_start = i + 8;
        const size_t data_end = data_start + len;
        if (data_end > bytes.size()) break;
        if (typ[0] == 't' && typ[1] == 'E' && typ[2] == 'X' && typ[3] == 't') {
            const uint8_t* data = bytes.data() + data_start;
            const size_t n = len;
            size_t nul = 0;
            while (nul < n && data[nul] != 0) ++nul;
            if (nul < n) {
                std::string key;
                key.reserve(nul);
                for (size_t k = 0; k < nul; ++k) key.push_back(char(data[k]));
                std::string text;
                text.reserve(n - nul - 1);
                for (size_t k = nul + 1; k < n; ++k) text.push_back(char(data[k]));
                out[key] = std::move(text);
            }
        }
        if (typ[0] == 'I' && typ[1] == 'E' && typ[2] == 'N' && typ[3] == 'D') break;
        i = data_end + 4;
    }
    return out;
}

double band_luma(const Image& img, int y0, int y1) {
    uint64_t sum = 0;
    size_t cnt = 0;
    for (int y = y0; y < y1 && y < img.h; ++y) {
        const uint8_t* row = img.rgba.data() + size_t(y) * img.w * 4;
        for (int x = 0; x < img.w; ++x) {
            sum += (uint32_t(row[x * 4]) + row[x * 4 + 1] + row[x * 4 + 2]) / 3;
            ++cnt;
        }
    }
    return cnt ? double(sum) / double(cnt) : 0.0;
}

void put_u32_be(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void put_chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data,
    uint32_t len) {
    put_u32_be(out, len);
    const size_t start = out.size();
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(type[i]));
    out.insert(out.end(), data, data + len);
    // CRC32 over type+data.
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = out.data() + start;
    const size_t total = out.size() - start;
    for (size_t i = 0; i < total; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    put_u32_be(out, crc ^ 0xFFFFFFFFu);
}

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

std::vector<uint8_t> encode_png(uint32_t width, uint32_t height,
    const std::vector<uint8_t>& rgba) {
    if (width == 0 || height == 0 || rgba.size() != size_t(width) * height * 4) {
        return {};
    }
    std::vector<uint8_t> out;
    const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    out.insert(out.end(), sig, sig + 8);

    // IHDR
    uint8_t ihdr[13];
    ihdr[0] = uint8_t(width >> 24);
    ihdr[1] = uint8_t(width >> 16);
    ihdr[2] = uint8_t(width >> 8);
    ihdr[3] = uint8_t(width);
    ihdr[4] = uint8_t(height >> 24);
    ihdr[5] = uint8_t(height >> 16);
    ihdr[6] = uint8_t(height >> 8);
    ihdr[7] = uint8_t(height);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // color type RGBA
    ihdr[10] = 0; // compression
    ihdr[11] = 0; // filter
    ihdr[12] = 0; // interlace
    put_chunk(out, "IHDR", ihdr, sizeof(ihdr));

    // Filtered scanlines: filter byte 0 (None) + row RGBA.
    std::vector<uint8_t> raw;
    raw.reserve(size_t(width) * 4 * height + height);
    const uint32_t row_bytes = width * 4;
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + size_t(y) * row_bytes,
            rgba.begin() + size_t(y) * row_bytes + row_bytes);
    }
    // zlib stream: header 0x78 0x01, stored deflate blocks, adler32.
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    const size_t kMaxBlock = 65535;
    while (pos < raw.size()) {
        const size_t left = raw.size() - pos;
        const size_t n = left < kMaxBlock ? left : kMaxBlock;
        const bool final = pos + n == raw.size();
        z.push_back(final ? 0x01u : 0x00u);
        z.push_back(uint8_t(n & 0xFF));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t((~n) & 0xFF));
        z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    const uint32_t adler = adler32(raw.data(), raw.size());
    z.push_back(uint8_t(adler >> 24));
    z.push_back(uint8_t(adler >> 16));
    z.push_back(uint8_t(adler >> 8));
    z.push_back(uint8_t(adler));
    put_chunk(out, "IDAT", z.data(), uint32_t(z.size()));

    put_chunk(out, "IEND", nullptr, 0);
    return out;
}

}