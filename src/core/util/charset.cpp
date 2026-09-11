#include "core/util/charset.h"

#include <algorithm>
#include <cstdint>

namespace oa::util {

bool is_valid_utf8(std::string_view s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const uint8_t c = uint8_t(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        int extra;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
            if (cp < 2) return false; // overlong
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= n) return false;
        for (int k = 1; k <= extra; ++k) {
            const uint8_t cc = uint8_t(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // surrogates / out-of-range
        if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += size_t(extra) + 1;
    }
    return true;
}

std::string_view strip_bom(std::string_view s) {
    if (s.size() >= 3 && uint8_t(s[0]) == 0xEF && uint8_t(s[1]) == 0xBB &&
        uint8_t(s[2]) == 0xBF) {
        return s.substr(3);
    }
    return s;
}

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return out;
}

std::string decode_to_utf8(std::string_view bytes, std::string_view charset) {
    const std::string cs = lower_ascii(charset);
    // Tolerate values like "UTF-8" / "UTF8" / "Shift_JIS" / "SJIS" / "CP932".
    const bool is_utf8 = cs.find("utf") != std::string::npos;
    const std::string_view cleaned = strip_bom(bytes);
    if (is_utf8) {
        // Falls back to Shift_JIS when UTF-8 decoding fails; for now
        // we surface the failure with a marker so SJIS projects are obvious.
        if (is_valid_utf8(cleaned)) return std::string(cleaned);
        return std::string(cleaned) + "\n<!invalid utf8, sjis decode pending>";
    }
    // Shift_JIS / CP932 decode: table generation is planned; until a real SJIS
    // project is targeted, pass bytes through (ASCII subset stays correct).
    return std::string(cleaned);
}

} // namespace oa::util
