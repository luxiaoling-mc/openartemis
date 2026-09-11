#pragma once
// Generic tagged binary stream (MessagePack-flavored), the transport of the
// engine save-domain serializers. Design reference: krkrsdl3
// cpp/tjs2/tjsBinarySerializer — little-endian one-byte type-tag stream where
// integers/strings/raw byte blocks/arrays/maps carry their own tags and
// lengths, so no escaping is ever needed and byte runs travel verbatim.
//
// Every document starts with a fixed header:
//     bytes 0..3  magic 'O','A','S','B'
//     bytes 4..7  u32 LE format version (kFormatVersion = 1)
// Writers prepend it automatically; Readers validate it.
//
// Tag table (byte values are stable; never reuse a reserved value):
//   0x00..0x7f  positive fixint (value = tag)
//   0x80..0x8f  map of (tag & 0x0f) entries           0x90..0x9f  array of n
//   0xa0..0xbf  string of (tag & 0x1f) bytes          0xc0  nil
//   0xc1  reserved           0xc2  true               0xc3  false
//   0xc4  str8 (u8 len)      0xc5  str16 (u16 LE)     0xc6  str32 (u32 LE)
//   0xc7  raw8 (u8 len)      0xc8  raw16 (u16 LE)     0xc9  raw32 (u32 LE)
//   0xca  float32 (f32 LE)   0xcb  float64 (f64 LE)
//   0xcc  uint8  0xcd  uint16  0xce  uint32  0xcf  uint64
//   0xd0  int8   0xd1  int16   0xd2  int32   0xd3  int64
//   0xd4..0xd9  reserved
//   0xda  map16 (u16 LE)     0xdb  map32 (u32 LE)
//   0xdc  array16 (u16 LE)   0xdd  array32 (u32 LE)
//   0xde 0xdf  reserved
//   0xe0..0xff  negative fixint (int8 value = tag)
//
// Strings are length-prefixed UTF-8 byte runs; the codec copies them verbatim
// (no charset validation, embedded NULs fine). Raw blocks are opaque octets.
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace oa::util {

constexpr uint32_t kFormatVersion = 1; // header u32; bump = breaking format change
constexpr size_t kHeaderSize = 8;      // magic(4) + u32 LE version

/// True when the four bytes at p are the OA magic 'OASB'.
inline bool is_magic(const uint8_t* p) {
    return p[0] == 'O' && p[1] == 'A' && p[2] == 'S' && p[3] == 'B';
}

// Tag byte values (see table above). Fixnum/fixstr/fixarray/fixmap ranges are
// implied by their low/high byte values; only the fixed tags are named here.
enum : uint8_t {
    kTagNil = 0xc0,
    kTagTrue = 0xc2,
    kTagFalse = 0xc3,
    kTagStr8 = 0xc4,
    kTagStr16 = 0xc5,
    kTagStr32 = 0xc6,
    kTagRaw8 = 0xc7,
    kTagRaw16 = 0xc8,
    kTagRaw32 = 0xc9,
    kTagFloat32 = 0xca,
    kTagFloat64 = 0xcb,
    kTagU8 = 0xcc,
    kTagU16 = 0xcd,
    kTagU32 = 0xce,
    kTagU64 = 0xcf,
    kTagI8 = 0xd0,
    kTagI16 = 0xd1,
    kTagI32 = 0xd2,
    kTagI64 = 0xd3,
    kTagMap16 = 0xda,
    kTagMap32 = 0xdb,
    kTagArray16 = 0xdc,
    kTagArray32 = 0xdd,
};

struct FormatError : std::runtime_error {
    explicit FormatError(const std::string& msg) : std::runtime_error(msg) {}
};

/// Append-only encoder. Construction already writes the document header.
class Writer {
public:
    Writer();
    const std::string& data() const { return out_; }
    size_t size() const { return out_.size(); }

    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    /// Minimal-width integer (fixint / int8..64 / uint8..64 ladder, krkr
    /// tjsBinarySerializer style). Use for counts as well as signed values.
    void i64(int64_t v);
    void f64(double v);
    void str(const std::string& s);
    void raw(const uint8_t* p, size_t n);
    void raw(const std::vector<uint8_t>& b) { raw(b.data(), b.size()); }
    void nil();
    void boolean(bool b);
    void map_begin(size_t entries);
    void array_begin(size_t items);

private:
    void put_byte(uint8_t b) { out_.push_back(char(b)); }
    std::string out_;
};

/// Bounds-checked decoder over one whole document (header validated on
/// construction). Every read throws FormatError on malformed/truncated data;
/// counts/lengths are validated against the remaining bytes before use.
class Reader {
public:
    /// Validates magic + records the header version. Throws FormatError when
    /// the data is not an OA binary document.
    Reader(const uint8_t* data, size_t size);
    explicit Reader(const std::string& bytes);
    explicit Reader(const std::vector<uint8_t>& bytes);

    /// Format version from the document header.
    uint32_t version() const { return header_version_; }
    size_t remaining() const { return size_ - pos_; }
    bool eof() const { return pos_ == size_; }
    /// Next tag byte without consuming it.
    uint8_t peek_tag() const { return require_byte(0); }

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    /// Any integer tag (fixints, int8..64, uint8..64) -> int64; uint64 values
    /// above INT64_MAX and non-integer tags throw FormatError.
    int64_t i64();
    /// float64/float32 tags -> double.
    double f64();
    /// true/false tags only.
    bool boolean();
    /// fixstr/str8/16/32 -> verbatim bytes.
    std::string str();
    /// raw8/16/32 -> opaque octets.
    std::vector<uint8_t> raw();
    /// fixmap/map16/32 -> entry count (validated against remaining bytes).
    size_t map_entries();
    /// fixarray/array16/32 -> item count (validated against remaining bytes).
    size_t array_items();
    /// Skip one complete value (unknown fields, forward compatibility).
    void skip_value();
    /// Fail unless the whole payload was consumed.
    void expect_eof(const char* what);

    /// True when the byte is any integer tag (fixints, int/uint8..64).
    static bool is_int_tag(uint8_t t) {
        return t <= 0x7f || t >= 0xe0 || (t >= kTagU8 && t <= kTagU64) ||
               (t >= kTagI8 && t <= kTagI64);
    }

private:
    void skip_at(size_t depth);
    uint8_t require_byte(size_t offset) const; // peek w/o bounds check msg
    void need(size_t n) const;                 // throw when n > remaining
    void need_byte() const { need(1); }

    const uint8_t* data_;
    size_t size_ = 0;
    size_t pos_ = 0;
    uint32_t header_version_ = 0;
};

} // namespace oa::util
