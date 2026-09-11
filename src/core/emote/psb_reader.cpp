#include "core/emote/psb_reader.h"

#include <cstdlib>
#include <cstring>
#include <limits>

namespace oa::emote {
namespace {

uint16_t rd_u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
uint64_t rd_compact(const uint8_t* p, int w) {
    uint64_t v = 0;
    for (int i = 0; i < w; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}
uint32_t adler32(const uint8_t* p, size_t n) {
    constexpr uint32_t kMod = 65521;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

// ---------------------------------------------------------------------------
// E-mote PSB header decryption. Retail E-mote exports (e.g.
// 甜蜜女友3) carry the encrypt bit and XOR their 12 header words (file bytes
// 8..56: header_length/offsetEncrypt, the table offsets, the checksum and
// the v4 extra-chunk offsets) with the keystream of a xorshift-style
// 128-bit cipher — the same scheme as the krkr E-mote plugin reference
// (plugins/emoteplayer/emotefile.cpp emote_decrypt; behavioural reference,
// reimplemented below, no code copied):
//     key = { 0x075BCD15, 0x159A55E5, 0x1F123BB5, seed }
// The seed is per-game/per-license. 甜蜜女友3's seed 0x02F0AF34 was
// recovered by an exhaustive 32-bit sweep validated against the header
// adler32. Only the header words are encrypted — the section
// tables they point to are plain — so after a successful patch the normal
// parse proceeds unchanged.
// ---------------------------------------------------------------------------
struct EmoteCipher {
    uint32_t key[4];
    uint32_t v = 0;
};
inline void emote_cipher_init(EmoteCipher* c, uint32_t seed) {
    c->key[0] = 0x075BCD15u;
    c->key[1] = 0x159A55E5u;
    c->key[2] = 0x1F123BB5u;
    c->key[3] = seed;
    c->v = 0;
}
inline uint8_t emote_cipher_next(EmoteCipher* c) {
    if (!c->v) {
        const uint32_t b = c->key[3];
        const uint32_t a = c->key[0] ^ (c->key[0] << 11);
        c->key[0] = c->key[1];
        c->key[1] = c->key[2];
        const uint32_t cc = a ^ b ^ ((a ^ (b >> 11)) >> 8);
        c->key[2] = b;
        c->key[3] = cc;
        c->v = cc;
    }
    const uint8_t r = uint8_t(c->v & 0xff);
    c->v >>= 8;
    return r;
}

/// Seeds for E-mote-encrypted PSB files, keyed by the project they were
/// recovered from. Unknown projects may add theirs (recovery method is
/// documented in the source; OA_EMOTE_SEED overrides for one-off use).
const struct { const char* game; uint32_t seed; } kEmoteDecryptSeeds[] = {
    {"甜蜜女友3", 0x02F0AF34u},
};

/// Try every candidate seed on `buf`'s header words (file bytes 8..56).
/// Returns true and patches the words in place when a seed's decryption
/// satisfies the header adler32 (bytes 8..40 + 44..56 == checksum word at
/// 40) — a definitive 32-bit validation, so a match cannot be coincidental.
bool try_emote_decrypt(std::vector<uint8_t>& buf) {
    if (buf.size() < 56) return false;
    auto check = [&]() {
        uint8_t payload[44];
        std::memcpy(payload, buf.data() + 8, 32);
        std::memcpy(payload + 32, buf.data() + 44, 12);
        return adler32(payload, 44) == rd_u32(buf.data() + 40);
    };
    auto run = [&](uint32_t seed) -> bool {
        EmoteCipher c;
        emote_cipher_init(&c, seed);
        for (size_t i = 0; i < 48; ++i) buf[8 + i] ^= emote_cipher_next(&c);
        if (check()) return true;
        // not this seed: undo and try the next candidate
        EmoteCipher rc;
        emote_cipher_init(&rc, seed);
        for (size_t i = 0; i < 48; ++i) buf[8 + i] ^= emote_cipher_next(&rc);
        return false;
    };
    if (const char* e = std::getenv("OA_EMOTE_SEED")) { // hex override first
        char* end = nullptr;
        const unsigned long v = std::strtoul(e, &end, 0);
        if (end && *end == '\0' && v != 0 && run(uint32_t(v))) return true;
    }
    for (const auto& k : kEmoteDecryptSeeds)
        if (run(k.seed)) return true;
    return false;
}

} // namespace

bool PsbReader::load(const std::vector<uint8_t>& data, std::string* err) {
    return load(data.data(), data.size(), err);
}

bool PsbReader::load(const uint8_t* data, size_t size, std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    data_.assign(data, data + size);
    if (data_.size() < 56) return fail("psb: file too small");
    if (memcmp(data_.data(), "PSB\0", 4) != 0) return fail("psb: bad magic");

    header_.version = rd_u16(data_.data() + 4);
    header_.encrypt = rd_u16(data_.data() + 6);
    const uint32_t hl = header_.version >= 4 ? 56u : header_.version >= 3 ? 44u : 40u;
    if (header_.version < 1 || header_.version > 4)
        return fail("psb: unsupported version");
    if (header_.encrypt & 1) {
        // E-mote retail encryption — XOR the 12 header words
        // (file bytes 8..56) with a per-game seeded keystream; the adler32
        // header checksum validates the candidate. On success the normal
        // parse below reads the decrypted header.
        if (!try_emote_decrypt(data_))
            return fail("psb: encrypted body (no known E-mote seed)");
    }

    header_.header_length = rd_u32(data_.data() + 8);
    header_.offset_names = rd_u32(data_.data() + 12);
    header_.offset_strings = rd_u32(data_.data() + 16);
    header_.offset_strings_data = rd_u32(data_.data() + 20);
    header_.offset_chunk_offsets = rd_u32(data_.data() + 24);
    header_.offset_chunk_lengths = rd_u32(data_.data() + 28);
    header_.offset_chunk_data = rd_u32(data_.data() + 32);
    header_.offset_entries = rd_u32(data_.data() + 36);
    if (header_.version >= 3) {
        header_.checksum = rd_u32(data_.data() + 40);
        // adler32 over the header payload (bytes 8..40 [+44..56 for v4]).
        std::vector<uint8_t> payload;
        payload.insert(payload.end(), data_.begin() + 8, data_.begin() + 40);
        if (header_.version >= 4)
            payload.insert(payload.end(), data_.begin() + 44, data_.begin() + 56);
        if (adler32(payload.data(), payload.size()) != *header_.checksum)
            return fail("psb: header checksum mismatch");
    }
    if (header_.version >= 4) {
        header_.offset_extra_chunk_offsets = rd_u32(data_.data() + 44);
        header_.offset_extra_chunk_lengths = rd_u32(data_.data() + 48);
        header_.offset_extra_chunk_data = rd_u32(data_.data() + 52);
    }
    if (header_.header_length != 0 && header_.header_length != hl)
        return fail("psb: unexpected header length");
    for (uint32_t o : {header_.offset_names, header_.offset_strings,
                       header_.offset_strings_data, header_.offset_chunk_offsets,
                       header_.offset_chunk_lengths, header_.offset_chunk_data,
                       header_.offset_entries})
        if (o >= data_.size()) return fail("psb: table offset out of range");

    // Names: v1 = plain C strings; v2+ = charset/namesData/nameIndexes trie.
    // (v1 not needed by any current asset; reject politely.)
    if (header_.version == 1) return fail("psb: v1 names unsupported");
    {
        std::vector<uint32_t> charset, names_data, name_indexes;
        uint32_t cursor = header_.offset_names;
        if (!array_values(cursor, &charset)) return fail("psb: charset array");
        // array_values walks a value at an offset: the names section stores
        // three *plain arrays*; recompute cursor by parsing heads directly.
        int cw = 0, cnt = 0, ew = 0;
        uint32_t ent = 0;
        cursor = header_.offset_names;
        if (!parse_array_head(cursor, &cw, &cnt, &ew, &ent)) return fail("psb: charset");
        charset.assign(cnt, 0);
        for (int i = 0; i < cnt; ++i) charset[i] = uint32_t(rd_compact(data_.data() + ent + i * ew, ew));
        cursor = ent + size_t(cnt) * ew;
        if (!parse_array_head(cursor, &cw, &cnt, &ew, &ent)) return fail("psb: namesData");
        names_data.assign(cnt, 0);
        for (int i = 0; i < cnt; ++i) names_data[i] = uint32_t(rd_compact(data_.data() + ent + i * ew, ew));
        cursor = ent + size_t(cnt) * ew;
        if (!parse_array_head(cursor, &cw, &cnt, &ew, &ent)) return fail("psb: nameIndexes");
        name_indexes.assign(cnt, 0);
        for (int i = 0; i < cnt; ++i) name_indexes[i] = uint32_t(rd_compact(data_.data() + ent + i * ew, ew));
        names_.reserve(name_indexes.size());
        for (uint32_t idx : name_indexes) {
            if (idx >= names_data.size()) return fail("psb: name index oob");
            uint32_t cur = names_data[idx];
            std::string rev;
            while (cur != 0) {
                if (cur >= names_data.size()) return fail("psb: name trie oob");
                uint32_t code = names_data[cur];
                if (code >= charset.size()) return fail("psb: charset index oob");
                uint32_t byte = cur - charset[code];
                rev.push_back(char(byte & 0xFF));
                cur = code;
            }
            names_.emplace_back(rev.rbegin(), rev.rend());
        }
    }
    // Strings table.
    {
        std::vector<uint32_t> offs;
        if (!array_values(header_.offset_strings, &offs)) return fail("psb: strings");
        strings_.reserve(offs.size());
        for (uint32_t o : offs) {
            size_t s = size_t(header_.offset_strings_data) + o;
            if (s >= data_.size()) return fail("psb: string offset oob");
            size_t e = s;
            while (e < data_.size() && data_[e] != 0) ++e;
            strings_.emplace_back(reinterpret_cast<const char*>(data_.data() + s), e - s);
        }
    }
    // Chunk tables.
    auto read_table = [&](uint32_t off, std::vector<uint32_t>* out) {
        int cw = 0, cnt = 0, ew = 0;
        uint32_t ent = 0;
        if (!parse_array_head(off, &cw, &cnt, &ew, &ent)) return false;
        out->assign(cnt, 0);
        for (int i = 0; i < cnt; ++i)
            (*out)[i] = uint32_t(rd_compact(data_.data() + ent + size_t(i) * ew, ew));
        return true;
    };
    if (!read_table(header_.offset_chunk_offsets, &chunk_offsets_)) return fail("psb: chunks");
    if (!read_table(header_.offset_chunk_lengths, &chunk_lengths_)) return fail("psb: chunk lens");
    if (chunk_offsets_.size() != chunk_lengths_.size()) return fail("psb: chunk mismatch");
    if (header_.version >= 4 && header_.offset_extra_chunk_offsets &&
        header_.offset_extra_chunk_offsets < data_.size()) {
        if (!read_table(*header_.offset_extra_chunk_offsets, &extra_chunk_offsets_))
            return fail("psb: xchunks");
        if (!read_table(*header_.offset_extra_chunk_lengths, &extra_chunk_lengths_))
            return fail("psb: xchunk lens");
        if (extra_chunk_offsets_.size() != extra_chunk_lengths_.size())
            return fail("psb: xchunk mismatch");
    }
    return true;
}

bool PsbReader::parse_array_head(uint32_t off, int* count_width, int* count,
                                 int* entry_width, uint32_t* entries_off) const {
    if (off >= data_.size()) return false;
    const uint8_t kind = data_[off];
    if (kind < uint8_t(Kind::ArrayN1) || kind > uint8_t(Kind::ArrayN8)) return false;
    const int cw = kind - uint8_t(Kind::ArrayN1) + 1;
    if (off + 1 + cw + 1 > data_.size()) return false;
    const uint64_t cnt = rd_compact(data_.data() + off + 1, cw);
    if (cnt > 0xFFFFFFF) return false;
    const uint8_t wm = data_[off + 1 + cw];
    // entry-width marker byte: 0x0C..0x14, width = marker - 0x0C
    if (wm < 0x0C || wm > uint8_t(Kind::ArrayN8)) return false;
    const int ew = wm - 0x0C;
    const uint32_t ent = off + 1 + cw + 1;
    if (uint64_t(ent) + uint64_t(cnt) * ew > data_.size()) return false;
    *count_width = cw;
    *count = int(cnt);
    *entry_width = ew;
    *entries_off = ent;
    return true;
}

Kind PsbReader::kind_at(uint32_t off) const {
    if (off >= data_.size()) return Kind::None;
    return Kind(data_[off]);
}

bool PsbReader::array_values(uint32_t off, std::vector<uint32_t>* values) const {
    int cw = 0, cnt = 0, ew = 0;
    uint32_t ent = 0;
    if (!parse_array_head(off, &cw, &cnt, &ew, &ent)) return false;
    values->assign(cnt, 0);
    for (int i = 0; i < cnt; ++i)
        (*values)[i] = uint32_t(rd_compact(data_.data() + ent + size_t(i) * ew, ew));
    return true;
}

bool PsbReader::object_entries(uint32_t off, std::vector<std::string>* keys,
                               std::vector<uint32_t>* values) const {
    if (off >= data_.size() || data_[off] != uint8_t(Kind::Objects)) return false;
    // [names array][offsets array]; child values are offsets relative to the
    // position right after the second array.
    int cw = 0, cnt = 0, ew = 0;
    uint32_t ent = 0;
    uint32_t cursor = off + 1;
    if (!parse_array_head(cursor, &cw, &cnt, &ew, &ent)) return false;
    std::vector<uint32_t> ni;
    ni.reserve(cnt);
    for (int i = 0; i < cnt; ++i)
        ni.push_back(uint32_t(rd_compact(data_.data() + ent + size_t(i) * ew, ew)));
    cursor = ent + size_t(cnt) * ew;
    if (!parse_array_head(cursor, &cw, &cnt, &ew, &ent)) return false;
    const uint32_t base = ent + size_t(cnt) * ew;
    keys->clear();
    values->clear();
    keys->reserve(ni.size());
    values->reserve(ni.size());
    for (uint32_t idx : ni) {
        if (idx >= names_.size()) return false;
        keys->push_back(names_[idx]);
    }
    for (int i = 0; i < cnt; ++i)
        values->push_back(base + uint32_t(rd_compact(data_.data() + ent + size_t(i) * ew, ew)));
    return true;
}

std::optional<uint32_t> PsbReader::object_member(uint32_t off, std::string_view key) const {
    std::vector<std::string> keys;
    std::vector<uint32_t> values;
    if (!object_entries(off, &keys, &values)) return std::nullopt;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return values[i];
    return std::nullopt;
}

bool PsbReader::list_items(uint32_t off, std::vector<uint32_t>* values) const {
    if (off >= data_.size() || data_[off] != uint8_t(Kind::List)) return false;
    int cw = 0, cnt = 0, ew = 0;
    uint32_t ent = 0;
    if (!parse_array_head(off + 1, &cw, &cnt, &ew, &ent)) return false;
    const uint32_t base = ent + size_t(cnt) * ew;
    values->clear();
    values->reserve(cnt);
    for (int i = 0; i < cnt; ++i)
        values->push_back(base + uint32_t(rd_compact(data_.data() + ent + size_t(i) * ew, ew)));
    return true;
}

bool PsbReader::read_int(uint32_t off, int64_t* out) const {
    if (off >= data_.size()) return false;
    const uint8_t kind = data_[off];
    if (kind == uint8_t(Kind::Null)) return false;
    if (kind >= uint8_t(Kind::NumberN0) && kind <= uint8_t(Kind::NumberN8)) {
        const int w = kind - uint8_t(Kind::NumberN0);
        if (w == 0) {
            *out = 0;
            return true;
        }
        if (off + 1 + w > data_.size()) return false;
        const uint64_t raw = rd_compact(data_.data() + off + 1, w);
        // sign-extend from the encoded width (the shift must run on a signed
        // value; an unsigned shift would leave narrow negatives as huge
        // positives, e.g. int16 -800 -> 64736)
        const int shift = 64 - 8 * w;
        const int64_t signed_raw = int64_t(raw);
        *out = (signed_raw << shift) >> shift;
        return true;
    }
    return false;
}

bool PsbReader::read_double(uint32_t off, double* out) const {
    if (off >= data_.size()) return false;
    const uint8_t kind = data_[off];
    if (kind == uint8_t(Kind::Null)) return false;
    if (kind >= uint8_t(Kind::NumberN0) && kind <= uint8_t(Kind::NumberN8)) {
        int64_t v = 0;
        if (!read_int(off, &v)) return false;
        *out = double(v);
        return true;
    }
    if (kind == uint8_t(Kind::Float)) {
        if (off + 5 > data_.size()) return false;
        uint32_t bits = rd_u32(data_.data() + off + 1);
        float f;
        memcpy(&f, &bits, 4);
        *out = f;
        return true;
    }
    if (kind == uint8_t(Kind::Double)) {
        if (off + 9 > data_.size()) return false;
        uint64_t bits = rd_compact(data_.data() + off + 1, 8);
        double d;
        memcpy(&d, &bits, 8);
        *out = d;
        return true;
    }
    if (kind == uint8_t(Kind::Float0)) {
        *out = 0.0;
        return true;
    }
    return false;
}

std::optional<std::string> PsbReader::read_string(uint32_t off) const {
    if (off >= data_.size()) return std::nullopt;
    const uint8_t kind = data_[off];
    if (kind < uint8_t(Kind::StringN1) || kind > uint8_t(Kind::StringN4)) return std::nullopt;
    const int w = kind - uint8_t(Kind::StringN1) + 1;
    if (off + 1 + w > data_.size()) return std::nullopt;
    const uint64_t idx = rd_compact(data_.data() + off + 1, w);
    if (idx >= strings_.size()) return std::nullopt;
    return strings_[idx];
}

bool PsbReader::read_resource(uint32_t off, int32_t* index, bool* extra) const {
    if (off >= data_.size()) return false;
    const uint8_t kind = data_[off];
    bool x = false;
    int w = 0;
    if (kind >= uint8_t(Kind::ResourceN1) && kind <= uint8_t(Kind::ResourceN4)) {
        w = kind - uint8_t(Kind::ResourceN1) + 1;
    } else if (kind >= uint8_t(Kind::ExtraChunkN1) &&
               kind <= uint8_t(Kind::ExtraChunkN4)) {
        x = true;
        w = kind - uint8_t(Kind::ExtraChunkN1) + 1;
    } else {
        return false;
    }
    if (off + 1 + w > data_.size()) return false;
    *index = int32_t(rd_compact(data_.data() + off + 1, w));
    *extra = x;
    return true;
}

bool PsbReader::chunk_bytes(size_t index, bool extra, std::span<const uint8_t>* out) const {
    const auto& offs = extra ? extra_chunk_offsets_ : chunk_offsets_;
    const auto& lens = extra ? extra_chunk_lengths_ : chunk_lengths_;
    if (index >= offs.size()) return false;
    const uint32_t base =
        extra ? header_.offset_extra_chunk_data.value_or(0) : header_.offset_chunk_data;
    const uint64_t start = uint64_t(base) + offs[index];
    if (start + lens[index] > data_.size()) return false;
    *out = std::span<const uint8_t>(data_.data() + start, lens[index]);
    return true;
}

// ---------------------------------------------------------------------------
// BC3/DXT5 decode: 4x4 blocks, alpha BC2-style 8-bit interpolation + BC1
// color block with 4-color interpolation (DXT5 layout: 8 alpha bytes, then
// 8 color bytes per block).
// ---------------------------------------------------------------------------
namespace {
void decode_color_block(const uint8_t* c, uint8_t out[4][4][4]) {
    const uint16_t c0 = uint16_t(c[0]) | (uint16_t(c[1]) << 8);
    const uint16_t c1 = uint16_t(c[2]) | (uint16_t(c[3]) << 8);
    int r0 = ((c0 >> 11) & 0x1F) << 3;
    int g0 = ((c0 >> 5) & 0x3F) << 2;
    int b0 = (c0 & 0x1F) << 3;
    int r1 = ((c1 >> 11) & 0x1F) << 3;
    int g1 = ((c1 >> 5) & 0x3F) << 2;
    int b1 = (c1 & 0x1F) << 3;
    uint8_t colors[4][3];
    colors[0][0] = uint8_t(r0);
    colors[0][1] = uint8_t(g0);
    colors[0][2] = uint8_t(b0);
    colors[1][0] = uint8_t(r1);
    colors[1][1] = uint8_t(g1);
    colors[1][2] = uint8_t(b1);
    if (c0 > c1) {
        colors[2][0] = uint8_t((2 * r0 + r1) / 3);
        colors[2][1] = uint8_t((2 * g0 + g1) / 3);
        colors[2][2] = uint8_t((2 * b0 + b1) / 3);
        colors[3][0] = uint8_t((r0 + 2 * r1) / 3);
        colors[3][1] = uint8_t((g0 + 2 * g1) / 3);
        colors[3][2] = uint8_t((b0 + 2 * b1) / 3);
    } else {
        colors[2][0] = uint8_t((r0 + r1) / 2);
        colors[2][1] = uint8_t((g0 + g1) / 2);
        colors[2][2] = uint8_t((b0 + b1) / 2);
        colors[3][0] = 0;
        colors[3][1] = 0;
        colors[3][2] = 0;
    }
    const uint32_t bits = rd_u32(c + 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const int idx = (bits >> (2 * (y * 4 + x))) & 3;
            out[y][x][0] = colors[idx][0];
            out[y][x][1] = colors[idx][1];
            out[y][x][2] = colors[idx][2];
            // opaque unless the 1-alpha (c0<=c1) transparent index was picked
            out[y][x][3] = (c0 <= c1 && idx == 3) ? 0 : 255;
        }
    }
}
} // namespace

bool decode_bc3(const uint8_t* src, int width, int height,
                std::vector<uint8_t>* rgba_out) {
    if (width <= 0 || height <= 0 || (width & 3) || (height & 3)) return false;
    rgba_out->assign(size_t(width) * height * 4, 0);
    const int bw = width / 4, bh = height / 4;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + size_t(by * bw + bx) * 16;
            // alpha: 2 endpoint bytes + 6 bit-pair bytes (48 bits, 3b/texel)
            const uint8_t a0 = blk[0], a1 = blk[1];
            uint8_t alphas[8];
            alphas[0] = a0;
            alphas[1] = a1;
            if (a0 > a1) {
                for (int i = 1; i <= 6; ++i)
                    alphas[i + 1] = uint8_t(((6 - i) * a0 + i * a1) / 7);
            } else {
                for (int i = 1; i <= 4; ++i)
                    alphas[i + 1] = uint8_t(((4 - i) * a0 + i * a1) / 5);
                alphas[6] = 0;
                alphas[7] = 255;
            }
            uint8_t alpha[4][4];
            uint64_t abits = 0;
            for (int i = 0; i < 6; ++i) abits |= uint64_t(blk[2 + i]) << (8 * i);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    alpha[y][x] = alphas[(abits >> (3 * (y * 4 + x))) & 7];
            uint8_t color[4][4][4];
            decode_color_block(blk + 8, color);
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const int px = bx * 4 + x, py = by * 4 + y;
                    uint8_t* d = rgba_out->data() + (size_t(py) * width + px) * 4;
                    d[0] = color[y][x][0];
                    d[1] = color[y][x][1];
                    d[2] = color[y][x][2];
                    d[3] = color[y][x][3] ? alpha[y][x] : 0;
                }
            }
        }
    }
    return true;
}

} // namespace oa::emote
