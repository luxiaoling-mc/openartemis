#pragma once
// Text encoding helpers (UTF-8 today; Shift_JIS decode is a placeholder
// until a real SJIS project is exercised — CHARSET defaults to
// Shift_JIS, so this is planned, not optional).
#include <optional>
#include <string>
#include <string_view>

namespace oa::util {

/// Returns true when the whole buffer is valid UTF-8 (no BOM required).
bool is_valid_utf8(std::string_view s);

/// Strips a UTF-8 BOM when present.
std::string_view strip_bom(std::string_view s);

/// Decode `bytes` into a UTF-8 std::string. `charset` values accepted
/// (case-insensitive): "utf-8"/"utf8" and "shift_jis"/"sjis"/"cp932" /
/// "ms932". Unknown charsets fall back to Shift_JIS (default).
/// If the input is already valid UTF-8 it is returned unchanged.
std::string decode_to_utf8(std::string_view bytes, std::string_view charset);

} // namespace oa::util
