#pragma once
// Synthetic Ogg/Theora video fixture for media tests (mirror of
// ogg_fixture.h's vorbis encoder): encode plain RGBA frames into a real
// Ogg/Theora byte stream with libtheoraenc + libogg, so tests can feed real
// decodable videos through VideoEngine without any game archive. The engine
// only links the decoder, so this encoder surface lives in the tests.
#include <cstdint>
#include <cstring>
#include <vector>

#include <ogg/ogg.h>
#include <theora/theoraenc.h>

namespace oafix {

/// Encode `frames` RGBA32 frames (each w*h*4 bytes) into an Ogg/Theora
/// container. w and h must be multiples of 16 (Theora macroblocks). Content
/// colors are limited-range BT.601 (studio swing): black -> Y 16, white ->
/// Y 235, so decoded pure black decodes back to RGB 0 and white to ~255.
/// Returns an empty vector on encode failure.
inline std::vector<uint8_t> encode_theora_ogv(
    int w, int h, const std::vector<std::vector<uint8_t>>& frames, int fps = 30) {
    std::vector<uint8_t> out;
    if (frames.empty() || w <= 0 || h <= 0 || (w % 16) != 0 || (h % 16) != 0)
        return out;
    const int cw = w / 2;
    const int ch = h / 2;

    th_info info;
    th_info_init(&info);
    info.frame_width = w;
    info.frame_height = h;
    info.pic_width = w;
    info.pic_height = h;
    info.pic_x = 0;
    info.pic_y = 0;
    info.fps_numerator = fps;
    info.fps_denominator = 1;
    info.aspect_numerator = 1;
    info.aspect_denominator = 1;
    info.colorspace = TH_CS_ITU_REC_470M;
    info.pixel_fmt = TH_PF_420;
    info.target_bitrate = 0;
    info.quality = 63; // superseded by TH_ENCCTL_SET_QUALITY below

    th_enc_ctx* enc = th_encode_alloc(&info);
    if (!enc) {
        th_info_clear(&info);
        return out;
    }
    int quality = 63; // 0..63, best quality keeps test pixels stable
    if (th_encode_ctl(enc, TH_ENCCTL_SET_QUALITY, &quality, sizeof(quality)) != 0) {
        th_encode_free(enc);
        th_info_clear(&info);
        return out;
    }

    ogg_stream_state os;
    if (ogg_stream_init(&os, 0x41520002u) != 0) {
        th_encode_free(enc);
        th_info_clear(&info);
        return out;
    }
    th_comment comment;
    th_comment_init(&comment);
    auto write_page = [&out](ogg_page& pg) {
        out.insert(out.end(), pg.header, pg.header + pg.header_len);
        out.insert(out.end(), pg.body, pg.body + pg.body_len);
    };
    ogg_packet op;
    ogg_page pg;
    while (th_encode_flushheader(enc, &comment, &op) > 0) {
        ogg_stream_packetin(&os, &op);
        while (ogg_stream_pageout(&os, &pg)) write_page(pg);
    }

    // RGB(A) -> limited-range BT.601 Y'CbCr 4:2:0 planes (w x h Y, w/2 x h/2
    // chroma). Test content is neutral-gray (R==G==B: black and white only),
    // so every chroma sample is exactly 128 — hard edges only ever mix
    // neutral samples, keeping U/V flat and the tests focused on luma.
    std::vector<uint8_t> y(w * h), u(cw * ch), v(cw * ch);
    std::fill(u.begin(), u.end(), 128);
    std::fill(v.begin(), v.end(), 128);
    auto convert = [&](const std::vector<uint8_t>& rgba) {
        for (int py = 0; py < h; ++py) {
            for (int px = 0; px < w; ++px) {
                const uint8_t* p = &rgba[(size_t(py) * w + size_t(px)) * 4];
                y[size_t(py) * w + size_t(px)] = uint8_t(
                    ((66 * p[0] + 129 * p[1] + 25 * p[2] + 128) >> 8) + 16);
            }
        }
    };

    th_ycbcr_buffer buf;
    buf[0].width = w;
    buf[0].height = h;
    buf[0].stride = w;
    buf[0].data = y.data();
    buf[1].width = cw;
    buf[1].height = ch;
    buf[1].stride = cw;
    buf[1].data = u.data();
    buf[2] = buf[1];
    buf[2].data = v.data();

    for (size_t f = 0; f < frames.size(); ++f) {
        convert(frames[f]);
        if (th_encode_ycbcr_in(enc, buf) != 0) {
            ogg_stream_clear(&os);
            th_encode_free(enc);
            th_info_clear(&info);
            return out;
        }
        while (th_encode_packetout(enc, 0, &op) == 1) {
            ogg_stream_packetin(&os, &op);
            while (ogg_stream_pageout(&os, &pg)) write_page(pg);
        }
    }
    // Drain the encoder tail (final frame(s) still buffered).
    while (th_encode_packetout(enc, 1, &op) == 1) {
        ogg_stream_packetin(&os, &op);
        while (ogg_stream_pageout(&os, &pg)) write_page(pg);
    }
    while (ogg_stream_flush(&os, &pg)) write_page(pg);

    ogg_stream_clear(&os);
    th_comment_clear(&comment);
    th_encode_free(enc);
    th_info_clear(&info);
    return out;
}

} // namespace oafix
