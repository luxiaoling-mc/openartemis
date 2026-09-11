#pragma once
// PSB container reader.
// Autonomous implementation; the container facts (v1..v4 header layout,
// name-trie/string/chunk tables, type-byte encoding, adler32 header
// checksum) were cross-checked against the krkr psbfile plugin reading
// logic and the documented PSB container format notes — no code is copied
// from either.
//
// The reader works on a byte buffer and keeps every object VALUE as a raw
// offset plus its type byte; callers walk the tree with the accessors below
// (object entries / list items / scalars / resources).
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::emote {

// PSB object type bytes (the on-disk PSBData type tag set).
enum class Kind : uint8_t {
    None = 0x00,
    Null = 0x01,
    False = 0x02,
    True = 0x03,
    NumberN0 = 0x04, // .. NumberN8 = 0x0C (width = kind - 0x04)
    NumberN8 = 0x0C,
    ArrayN1 = 0x0D, // .. ArrayN8 = 0x14 (count width = kind - 0x0C)
    ArrayN8 = 0x14,
    StringN1 = 0x15, // .. StringN4 = 0x18 (width = kind - 0x14)
    StringN4 = 0x18,
    ResourceN1 = 0x19, // .. ResourceN4 = 0x1C (regular chunk, width = kind - 0x18)
    ResourceN4 = 0x1C,
    Float0 = 0x1D,
    Float = 0x1E,
    Double = 0x1F,
    List = 0x20,  // object list: [array of relative child offsets]
    Objects = 0x21, // object dictionary: [names array][offsets array]
    ExtraChunkN1 = 0x22, // .. ExtraChunkN4 = 0x25 (extra chunks, v4)
    ExtraChunkN4 = 0x25,
};

struct Header {
    uint16_t version = 0;
    uint16_t encrypt = 0;
    uint32_t header_length = 0;
    uint32_t offset_names = 0;
    uint32_t offset_strings = 0;
    uint32_t offset_strings_data = 0;
    uint32_t offset_chunk_offsets = 0;
    uint32_t offset_chunk_lengths = 0;
    uint32_t offset_chunk_data = 0;
    uint32_t offset_entries = 0;
    std::optional<uint32_t> checksum;
    std::optional<uint32_t> offset_extra_chunk_offsets;
    std::optional<uint32_t> offset_extra_chunk_lengths;
    std::optional<uint32_t> offset_extra_chunk_data;
};

class PsbReader {
public:
    /// Parses the container; returns false (with `err` filled) on structural
    /// problems (bad magic, unsupported version, checksum mismatch, tables
    /// outside the file).
    bool load(const uint8_t* data, size_t size, std::string* err = nullptr);
    bool load(const std::vector<uint8_t>& data, std::string* err = nullptr);

    const Header& header() const { return header_; }

    /// Name/string tables (decoded at load).
    const std::vector<std::string>& names() const { return names_; }
    const std::vector<std::string>& strings() const { return strings_; }

    // -- object walkers ------------------------------------------------------
    /// Absolute offset where the document root value lives.
    uint32_t root_offset() const { return header_.offset_entries; }

    /// Kind byte of the value at `off`.
    Kind kind_at(uint32_t off) const;

    /// Objects (dictionary): fills `keys`+`values` with absolute child value
    /// offsets. False when the value is not an object.
    bool object_entries(uint32_t off, std::vector<std::string>* keys,
                        std::vector<uint32_t>* values) const;
    std::optional<uint32_t> object_member(uint32_t off, std::string_view key) const;

    /// List: fills absolute child value offsets.
    bool list_items(uint32_t off, std::vector<uint32_t>* values) const;

    /// Plain u32 arrays (ArrayN) -> values.
    bool array_values(uint32_t off, std::vector<uint32_t>* values) const;

    // -- scalars -------------------------------------------------------------
    bool read_int(uint32_t off, int64_t* out) const;      // NumberN*
    bool read_double(uint32_t off, double* out) const;    // Number/Float/Double (+string fallback)
    std::optional<std::string> read_string(uint32_t off) const; // StringN*
    bool read_resource(uint32_t off, int32_t* index, bool* extra) const;

    // -- resources -----------------------------------------------------------
    /// Absolute byte range of a resource (resolved against the right chunk
    /// data base). `out` spans into the source buffer.
    bool chunk_bytes(size_t index, bool extra, std::span<const uint8_t>* out) const;

private:
    bool parse_array_head(uint32_t off, int* count_width, int* count,
                          int* entry_width, uint32_t* entries_off) const;
    const uint8_t* bytes() const { return data_.data(); }

    std::vector<uint8_t> data_;
    Header header_;
    std::vector<std::string> names_;
    std::vector<std::string> strings_;
    std::vector<uint32_t> chunk_offsets_;
    std::vector<uint32_t> chunk_lengths_;
    std::vector<uint32_t> extra_chunk_offsets_;
    std::vector<uint32_t> extra_chunk_lengths_;
};

// ---------------------------------------------------------------------------
// BC3/DXT5 software decoder (win-spec E-mote atlases are DXT5; krkr only
// decodes RGBA8/RL, so this is an autonomous capability). Straight
// (non-premultiplied) RGBA out, 4 bytes per pixel.
// ---------------------------------------------------------------------------
bool decode_bc3(const uint8_t* src, int width, int height, std::vector<uint8_t>* rgba_out);

} // namespace oa::emote
