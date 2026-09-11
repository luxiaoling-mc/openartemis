#pragma once

#include <cstdint>
#include <map>
#include <vector>
#include <string>
#include <optional>

// 图片
namespace oa::media {

struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgba;
};

bool decode_png(const std::vector<uint8_t>& bytes, Image& out);

/// Decode one still image by magic: JPEG (FF D8) via libjpeg, anything else
/// via decode_png. Returns false (with a stderr note) when the bytes are not
/// a decodable still image. Android/iOS archives (thyt) keep
/// story bgs as real JPEGs, so PNG-only decode left those layers silent.
bool decode_image(const std::vector<uint8_t>& bytes, Image& out);

/// PNG tEXt chunk keywords -> text: only uncompressed tEXt
/// chunks are read, bytes map to chars as-is). Consumed by e:loadPngComments
/// — FPM standing-figure / message-window-face placement ("pos,x,y,w,h")
/// lives in these comments.
std::map<std::string, std::string> png_text_chunks(const std::vector<uint8_t>& bytes);

double band_luma(const Image& img, int y0, int y1);

// Dependency - free minimal PNG encoder(RGBA8, stored / zlib - stored
// deflate blocks). Used by [savess] to write slot thumbnails (placeholder
// frames when no real capture is available). Deterministic output.
/// Encode an RGBA8 image as PNG. Empty on failure.
std::vector<uint8_t> encode_png(uint32_t width, uint32_t height,
    const std::vector<uint8_t>& rgba);

}
