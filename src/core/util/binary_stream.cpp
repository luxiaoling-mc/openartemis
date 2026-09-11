#include "core/util/binary_stream.h"

#include <cstring>
#include <limits>

namespace oa::util {

namespace {
constexpr uint8_t kMagicByte0 = 'O';
constexpr uint8_t kMagicByte1 = 'A';
constexpr uint8_t kMagicByte2 = 'S';
constexpr uint8_t kMagicByte3 = 'B';

// ---------------------------------------------------------------------------
// Little-endian primitives shared by Writer and Reader.
// ---------------------------------------------------------------------------
void put_le16(std::string* out, uint16_t v) {
    out->push_back(char(uint8_t(v & 0xff)));
    out->push_back(char(uint8_t(v >> 8)));
}
void put_le32(std::string* out, uint32_t v) {
    out->push_back(char(uint8_t(v & 0xff)));
    out->push_back(char(uint8_t((v >> 8) & 0xff)));
    out->push_back(char(uint8_t((v >> 16) & 0xff)));
    out->push_back(char(uint8_t((v >> 24) & 0xff)));
}
void put_le64(std::string* out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out->push_back(char(uint8_t((v >> (8 * i)) & 0xff)));
}
uint16_t read_le16(const uint8_t* p) {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}
uint32_t read_le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
uint64_t read_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}
} // namespace

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
Writer::Writer() {
    // Header: magic + u32 LE format version.
    out_.reserve(64);
    put_byte(kMagicByte0);
    put_byte(kMagicByte1);
    put_byte(kMagicByte2);
    put_byte(kMagicByte3);
    put_le32(&out_, kFormatVersion);
}

void Writer::u8(uint8_t v) { put_byte(v); }
void Writer::u16(uint16_t v) { put_le16(&out_, v); }
void Writer::u32(uint32_t v) { put_le32(&out_, v); }
void Writer::u64(uint64_t v) { put_le64(&out_, v); }

void Writer::i64(int64_t v) {
    if (v >= 0) {
        if (v <= 0x7f) {
            put_byte(uint8_t(v));
        } else if (v <= 0xff) {
            put_byte(kTagU8);
            put_byte(uint8_t(v));
        } else if (v <= 0xffff) {
            put_byte(kTagU16);
            u16(uint16_t(v));
        } else if (v <= 0xffffffffLL) {
            put_byte(kTagU32);
            u32(uint32_t(v));
        } else {
            put_byte(kTagU64);
            u64(uint64_t(v));
        }
    } else {
        if (v >= -32) {
            put_byte(uint8_t(int8_t(v)));
        } else if (v >= std::numeric_limits<int8_t>::min()) {
            put_byte(kTagI8);
            put_byte(uint8_t(int8_t(v)));
        } else if (v >= std::numeric_limits<int16_t>::min()) {
            put_byte(kTagI16);
            u16(uint16_t(int16_t(v)));
        } else if (v >= std::numeric_limits<int32_t>::min()) {
            put_byte(kTagI32);
            u32(uint32_t(int32_t(v)));
        } else {
            put_byte(kTagI64);
            u64(uint64_t(v));
        }
    }
}

void Writer::f64(double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_byte(kTagFloat64);
    put_le64(&out_, bits);
}

void Writer::str(const std::string& s) {
    const size_t n = s.size();
    if (n <= 31) {
        put_byte(uint8_t(0xa0 | n));
    } else if (n <= 0xff) {
        put_byte(kTagStr8);
        put_byte(uint8_t(n));
    } else if (n <= 0xffff) {
        put_byte(kTagStr16);
        u16(uint16_t(n));
    } else if (n <= 0xffffffffULL) {
        put_byte(kTagStr32);
        u32(uint32_t(n));
    } else {
        throw FormatError("string too long to serialize");
    }
    out_.append(s);
}

void Writer::raw(const uint8_t* p, size_t n) {
    if (n <= 0xff) {
        put_byte(kTagRaw8);
        put_byte(uint8_t(n));
    } else if (n <= 0xffff) {
        put_byte(kTagRaw16);
        u16(uint16_t(n));
    } else if (n <= 0xffffffffULL) {
        put_byte(kTagRaw32);
        u32(uint32_t(n));
    } else {
        throw FormatError("raw block too long to serialize");
    }
    out_.append(reinterpret_cast<const char*>(p), n);
}

void Writer::nil() { put_byte(kTagNil); }
void Writer::boolean(bool b) { put_byte(b ? kTagTrue : kTagFalse); }

void Writer::map_begin(size_t entries) {
    if (entries <= 15) {
        put_byte(uint8_t(0x80 | entries));
    } else if (entries <= 0xffff) {
        put_byte(kTagMap16);
        u16(uint16_t(entries));
    } else {
        put_byte(kTagMap32);
        u32(uint32_t(entries));
    }
}

void Writer::array_begin(size_t items) {
    if (items <= 15) {
        put_byte(uint8_t(0x90 | items));
    } else if (items <= 0xffff) {
        put_byte(kTagArray16);
        u16(uint16_t(items));
    } else {
        put_byte(kTagArray32);
        u32(uint32_t(items));
    }
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
Reader::Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {
    if (size < kHeaderSize) {
        throw FormatError("truncated document (missing binary header)");
    }
    if (!is_magic(data)) {
        throw FormatError("unrecognized save document (bad magic)");
    }
    header_version_ = read_le32(data + 4);
    pos_ = kHeaderSize; // payload starts right after the header
}

Reader::Reader(const std::string& bytes)
    : Reader(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) {}
Reader::Reader(const std::vector<uint8_t>& bytes)
    : Reader(bytes.data(), bytes.size()) {}

void Reader::need(size_t n) const {
    if (n > remaining()) {
        throw FormatError("truncated document (ran out of bytes)");
    }
}
uint8_t Reader::require_byte(size_t offset) const {
    need_byte();
    return data_[pos_ + offset];
}

uint8_t Reader::u8() {
    need_byte();
    return data_[pos_++];
}
uint16_t Reader::u16() {
    need(2);
    const uint16_t v = read_le16(data_ + pos_);
    pos_ += 2;
    return v;
}
uint32_t Reader::u32() {
    need(4);
    const uint32_t v = read_le32(data_ + pos_);
    pos_ += 4;
    return v;
}
uint64_t Reader::u64() {
    need(8);
    const uint64_t v = read_le64(data_ + pos_);
    pos_ += 8;
    return v;
}

int64_t Reader::i64() {
    const uint8_t t = peek_tag();
    if (t <= 0x7f) {
        ++pos_;
        return int64_t(t);
    }
    if (t >= 0xe0) { // negative fixint: int8 sign-extended from the tag byte
        ++pos_;
        return int64_t(int8_t(t));
    }
    ++pos_;
    switch (t) {
        case kTagU8: return int64_t(u8());
        case kTagU16: return int64_t(u16());
        case kTagU32: return int64_t(u32());
        case kTagU64: {
            const uint64_t v = u64();
            if (v > uint64_t(std::numeric_limits<int64_t>::max()))
                throw FormatError("uint64 value out of int64 range");
            return int64_t(v);
        }
        case kTagI8: return int64_t(int8_t(u8()));
        case kTagI16: return int64_t(int16_t(u16()));
        case kTagI32: return int64_t(int32_t(u32()));
        case kTagI64: return int64_t(u64());
        default:
            throw FormatError("expected integer tag");
    }
}

double Reader::f64() {
    const uint8_t t = peek_tag();
    ++pos_;
    if (t == kTagFloat64) {
        const uint64_t bits = u64();
        double v = 0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    if (t == kTagFloat32) {
        const uint32_t bits = u32();
        float f = 0;
        std::memcpy(&f, &bits, sizeof(f));
        return double(f);
    }
    throw FormatError("expected float tag");
}

bool Reader::boolean() {
    const uint8_t t = peek_tag();
    ++pos_;
    if (t == kTagTrue) return true;
    if (t == kTagFalse) return false;
    throw FormatError("expected bool tag");
}

namespace {
size_t str_len_tag(uint8_t t) {
    return t >= 0xa0 && t <= 0xbf ? size_t(t & 0x1f) : SIZE_MAX;
}
} // namespace

std::string Reader::str() {
    const uint8_t t = peek_tag();
    if (str_len_tag(t) != SIZE_MAX) {
        ++pos_;
        const size_t n = size_t(t & 0x1f);
        need(n);
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }
    ++pos_;
    size_t n = 0;
    switch (t) {
        case kTagStr8: n = u8(); break;
        case kTagStr16: n = u16(); break;
        case kTagStr32: n = u32(); break;
        default: throw FormatError("expected string tag");
    }
    need(n);
    std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return s;
}

std::vector<uint8_t> Reader::raw() {
    const uint8_t t = peek_tag();
    ++pos_;
    size_t n = 0;
    switch (t) {
        case kTagRaw8: n = u8(); break;
        case kTagRaw16: n = u16(); break;
        case kTagRaw32: n = u32(); break;
        default: throw FormatError("expected raw tag");
    }
    need(n);
    std::vector<uint8_t> b(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return b;
}

size_t Reader::map_entries() {
    const uint8_t t = peek_tag();
    ++pos_;
    size_t n = 0;
    if (t >= 0x80 && t <= 0x8f) {
        n = size_t(t & 0x0f);
    } else {
        switch (t) {
            case kTagMap16: n = u16(); break;
            case kTagMap32: n = u32(); break;
            default: throw FormatError("expected map tag");
        }
    }
    if (n > remaining()) throw FormatError("map count exceeds document size");
    return n;
}

size_t Reader::array_items() {
    const uint8_t t = peek_tag();
    ++pos_;
    size_t n = 0;
    if (t >= 0x90 && t <= 0x9f) {
        n = size_t(t & 0x0f);
    } else {
        switch (t) {
            case kTagArray16: n = u16(); break;
            case kTagArray32: n = u32(); break;
            default: throw FormatError("expected array tag");
        }
    }
    if (n > remaining()) throw FormatError("array count exceeds document size");
    return n;
}

void Reader::skip_value() { skip_at(0); }

void Reader::skip_at(size_t depth) {
    if (depth > 256) throw FormatError("nesting too deep");
    const uint8_t t = peek_tag();
    // Scalars whose whole encoding is the tag byte itself.
    if (t <= 0x7f || t >= 0xe0 || t == kTagNil || t == kTagTrue || t == kTagFalse) {
        ++pos_;
        return;
    }
    // Byte-run lengths to skip after the tag.
    auto skip_len = [this](uint64_t len) {
        if (len > remaining()) throw FormatError("length exceeds document size");
        pos_ += size_t(len);
    };
    // Containers: recursion over count-validated entries.
    if ((t >= 0x80 && t <= 0x8f) || t == kTagMap16 || t == kTagMap32) {
        const size_t n = map_entries();
        for (size_t i = 0; i < n; ++i) {
            skip_at(depth + 1); // key
            skip_at(depth + 1); // value
        }
        return;
    }
    if ((t >= 0x90 && t <= 0x9f) || t == kTagArray16 || t == kTagArray32) {
        const size_t n = array_items();
        for (size_t i = 0; i < n; ++i) skip_at(depth + 1);
        return;
    }
    if (t >= 0xa0 && t <= 0xbf) { // fixstr
        ++pos_;
        skip_len(t & 0x1f);
        return;
    }
    switch (t) {
        case kTagStr8: case kTagRaw8: ++pos_; skip_len(u8()); return;
        case kTagStr16: case kTagRaw16: ++pos_; skip_len(u16()); return;
        case kTagStr32: case kTagRaw32: ++pos_; skip_len(u32()); return;
        case kTagFloat32: need(4); pos_ += 5; return;
        case kTagFloat64: need(8); pos_ += 9; return;
        case kTagU8: case kTagI8: need(1); pos_ += 2; return;
        case kTagU16: case kTagI16: need(2); pos_ += 3; return;
        case kTagU32: case kTagI32: need(4); pos_ += 5; return;
        case kTagU64: case kTagI64: need(8); pos_ += 9; return;
        default: throw FormatError("unknown tag in document");
    }
}

void Reader::expect_eof(const char* what) {
    if (!eof()) {
        throw FormatError(std::string(what) + ": trailing bytes after document");
    }
}

} // namespace oa::util
