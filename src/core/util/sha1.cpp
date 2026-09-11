#include "core/util/sha1.h"

#include <cstring>

namespace oa::util {

namespace {

inline uint32_t rol(uint32_t v, int bits) {
    return (v << bits) | (v >> (32 - bits));
}

constexpr uint32_t kInit[5] = {
    0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

} // namespace

Sha1::Sha1() { std::memcpy(h_, kInit, sizeof(h_)); }

void Sha1::process_block(const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t tmp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = tmp;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::update(const uint8_t* data, size_t len) {
    if (finished_) return;
    total_bits_ += uint64_t(len) * 8;
    while (len > 0) {
        size_t take = std::min(len, block_.size() - block_len_);
        std::memcpy(block_.data() + block_len_, data, take);
        block_len_ += take;
        data += take;
        len -= take;
        if (block_len_ == block_.size()) {
            process_block(block_.data());
            block_len_ = 0;
        }
    }
}

Sha1Digest Sha1::finalize() {
    if (!finished_) {
        const uint64_t msg_bits = total_bits_; // message length only, before padding
        // padding: 0x80 then zeros until 56 mod 64, then 64-bit big-endian bit count
        uint8_t pad = 0x80;
        update(&pad, 1);
        std::array<uint8_t, 64> zeros{};
        update(zeros.data(), (block_len_ < 56) ? (56 - block_len_) : (120 - block_len_));
        std::array<uint8_t, 8> bits{};
        for (int i = 0; i < 8; ++i) bits[7 - i] = uint8_t(msg_bits >> (i * 8));
        update(bits.data(), bits.size());
        finished_ = true;
    }
    Sha1Digest out{};
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = uint8_t(h_[i] >> 24);
        out[i * 4 + 1] = uint8_t(h_[i] >> 16);
        out[i * 4 + 2] = uint8_t(h_[i] >> 8);
        out[i * 4 + 3] = uint8_t(h_[i]);
    }
    return out;
}

} // namespace oa::util
