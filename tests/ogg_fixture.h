#pragma once
// Synthetic Ogg Vorbis fixtures for media tests: encode a short sine tone with
// libvorbisenc (the library is already a vcpkg dependency). Tests can then
// feed real decodable streams through MediaPlayers / the runtime loader.
#include <cmath>
#include <cstdint>
#include <vector>

#include <vorbis/vorbisenc.h>

namespace oafix {

/// Encode `seconds` of a sine tone into an Ogg Vorbis byte stream.
/// Returns an empty vector on encode failure.
inline std::vector<uint8_t> tone_ogg(double seconds, int channels, long rate, double freq,
                                     float amp) {
    std::vector<uint8_t> out;
    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, channels, rate, 0.3) != 0) {
        vorbis_info_clear(&vi);
        return out;
    }
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    vorbis_dsp_state vd;
    if (vorbis_analysis_init(&vd, &vi) != 0) {
        vorbis_comment_clear(&vc);
        vorbis_info_clear(&vi);
        return out;
    }
    vorbis_block vb;
    vorbis_block_init(&vd, &vb);
    ogg_stream_state os;
    ogg_stream_init(&os, 0x41520001u);
    ogg_packet h1, h2, h3;
    vorbis_analysis_headerout(&vd, &vc, &h1, &h2, &h3);
    ogg_stream_packetin(&os, &h1);
    ogg_stream_packetin(&os, &h2);
    ogg_stream_packetin(&os, &h3);
    auto write_page = [&out](ogg_page& pg) {
        out.insert(out.end(), pg.header, pg.header + pg.header_len);
        out.insert(out.end(), pg.body, pg.body + pg.body_len);
    };
    ogg_page pg;
    while (ogg_stream_flush(&os, &pg)) write_page(pg);

    const double step = 2.0 * 3.14159265358979323846 * freq / double(rate);
    const size_t total = size_t(seconds * double(rate));
    size_t written = 0;
    auto flush_block = [&]() {
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);
            ogg_packet op;
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                ogg_stream_packetin(&os, &op);
                while (ogg_stream_pageout(&os, &pg)) write_page(pg);
            }
        }
    };
    while (written < total) {
        size_t n = total - written;
        if (n > 1024) n = 1024;
        float** buf = vorbis_analysis_buffer(&vd, int(n));
        for (size_t i = 0; i < n; ++i) {
            const float s = amp * float(std::sin(step * double(written + i)));
            for (int c = 0; c < channels; ++c) buf[c][i] = s;
        }
        vorbis_analysis_wrote(&vd, int(n));
        written += n;
        flush_block();
    }
    vorbis_analysis_wrote(&vd, 0);
    flush_block();
    while (ogg_stream_flush(&os, &pg)) write_page(pg);

    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return out;
}

} // namespace oafix
