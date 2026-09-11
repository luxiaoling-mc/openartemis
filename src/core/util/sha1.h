#pragma once
// Minimal SHA-1 (FIPS 180-1), used for pf8 PFS key derivation:
// key = SHA1(pfs_file_bytes[7 .. 7+index_size]).
#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <vector>

namespace oa::util {

using Sha1Digest = std::array<uint8_t, 20>;

class Sha1 {
public:
    Sha1();
    void update(const uint8_t* data, size_t len);
    Sha1Digest finalize();

private:
    uint32_t h_[5];
    uint64_t total_bits_ = 0;
    std::array<uint8_t, 64> block_{};
    size_t block_len_ = 0;
    bool finished_ = false;
    void process_block(const uint8_t* block);
};

inline Sha1Digest sha1_of(const void* data, size_t len) {
    Sha1 h;
    h.update(static_cast<const uint8_t*>(data), len);
    return h.finalize();
}

} // namespace oa::util
