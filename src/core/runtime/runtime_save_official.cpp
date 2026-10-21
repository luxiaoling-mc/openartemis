// 官方 Artemis 存档容器（BOWX / BOWG / BOWS）只读解码器。
// 布局与实测依据见 runtime_save_official.h 的文件头注释。
//
// 设计约束（与 runtime_save.cpp 的"失败语义原子"一致）：
//   - 这里只做结构校验 + 切字节，任何不一致都抛 FormatError，绝不返回半份数据；
//   - 不修改输入字节，不猜字段语义（图层命令栈、Pluto 值都原样留着）；
//   - 所有长度/偏移都在使用前对边界做一次显式检查（存档来自外部，
//     zlib 头里的 raw_len 与目录偏移都是攻击面）。
#include "core/runtime/runtime_save_official.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <zlib.h>

#include "core/util/binary_stream.h"

namespace oa::runtime {

namespace {

constexpr size_t kHeaderBytes = 12;           // magic + version + raw_len
constexpr uint32_t kMinVersion = 1000;        // 实测 1002 / 1003
constexpr uint32_t kMaxVersion = 1999;
constexpr uint32_t kMaxInflated = 64u << 20;  // 实测最大的 BOWS 约 0.5 MiB
constexpr uint32_t kMaxObjects = 1u << 16;
constexpr uint32_t kMaxFields = 1u << 20;
constexpr uint32_t kMaxEntries = 1u << 20;    // 单张字符串表/已读行表的条目上限
constexpr uint32_t kMaxSpans = 1u << 20;      // BOWG 已读行数组的元素上限

[[noreturn]] void fail(const std::string& what) {
    throw oa::util::FormatError("official save: " + what);
}

/// 有界小端读取器（所有读取都会 throw，不做 UB 越界）。
class Cursor {
public:
    Cursor(std::string_view data, size_t begin, size_t end)
        : data_(data), pos_(begin), end_(end) {
        if (begin > end || end > data.size()) fail("field range outside payload");
    }
    uint32_t u32() {
        need(4);
        const auto* p = reinterpret_cast<const uint8_t*>(data_.data()) + pos_;
        const uint32_t v = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
                           (uint32_t(p[3]) << 24);
        pos_ += 4;
        return v;
    }
    /// u32 长度 + 该长度的原始字节（不做字符集转换：键可能是 CP932）。
    std::string text() {
        const uint32_t n = u32();
        need(n);
        std::string s(data_.substr(pos_, n));
        pos_ += n;
        return s;
    }
    size_t pos() const { return pos_; }
    size_t end() const { return end_; }
    bool at_end() const { return pos_ == end_; }

private:
    void need(size_t n) const {
        if (n > end_ - pos_) fail("truncated field (asked " + std::to_string(n) + ", left " +
                                  std::to_string(end_ - pos_) + ")");
    }
    std::string_view data_;
    size_t pos_;
    size_t end_;
};

/// 目录：object_start → (field_id → field_offset)。
struct Directory {
    std::map<uint32_t, std::map<uint32_t, uint32_t>> objects;
    size_t dir_offset = 0;
};

Directory parse_directory(std::string_view raw) {
    if (raw.size() < kHeaderBytes + 4) fail("payload too small for a footer directory");
    Directory d;
    d.dir_offset = [&] {
        const size_t at = raw.size() - 4;
        const auto* p = reinterpret_cast<const uint8_t*>(raw.data()) + at;
        return size_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
                      (uint32_t(p[3]) << 24));
    }();
    // 目录必须落在负载内部、且至少能容纳 count 本身。
    if (d.dir_offset < 4 || d.dir_offset > raw.size() - 4) {
        fail("footer directory offset " + std::to_string(d.dir_offset) + " outside payload (" +
             std::to_string(raw.size()) + " bytes)");
    }
    Cursor c(raw, d.dir_offset, raw.size() - 4);
    const uint32_t count = c.u32();
    if (count > kMaxObjects) fail("object count " + std::to_string(count) + " over limit");
    size_t fields_total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t start = c.u32();
        const uint32_t nf = c.u32();
        fields_total += nf;
        if (nf > kMaxFields || fields_total > kMaxFields) fail("field count over limit");
        if (start >= d.dir_offset) fail("object start " + std::to_string(start) + " past directory");
        auto& fields = d.objects[start];
        for (uint32_t j = 0; j < nf; ++j) {
            const uint32_t id = c.u32();
            const uint32_t off = c.u32();
            if (off >= d.dir_offset) {
                fail("field " + std::to_string(id) + " offset " + std::to_string(off) +
                     " past directory");
            }
            if (!fields.emplace(id, off).second) {
                fail("duplicate field id " + std::to_string(id) + " in object " +
                     std::to_string(start));
            }
        }
    }
    if (!c.at_end()) fail("footer directory has trailing bytes");
    if (!d.objects.count(0)) fail("root object (start 0) missing");
    return d;
}

/// 一个字段的字节区间：起点是它的 offset，终点是同 object 内**下一个更大的**
/// offset（没有则用 limit）。limit 由调用方给（根 object = 目录偏移）。
std::pair<size_t, size_t> field_span(const Directory& dir, uint32_t object, uint32_t field,
                                     size_t limit) {
    const auto obj = dir.objects.find(object);
    if (obj == dir.objects.end()) {
        fail("object " + std::to_string(object) + " missing from directory");
    }
    const auto it = obj->second.find(field);
    if (it == obj->second.end()) {
        fail("field " + std::to_string(field) + " missing from object " + std::to_string(object));
    }
    const size_t begin = it->second;
    if (limit > dir.dir_offset) limit = dir.dir_offset;
    if (begin > limit) fail("field " + std::to_string(field) + " starts past its limit");
    size_t end = limit;
    for (const auto& [other_id, other_off] : obj->second) {
        (void)other_id;
        if (other_off > begin && other_off < end) end = other_off;
    }
    return {begin, end};
}

std::string inflate_payload(std::string_view compressed, uint32_t declared) {
    if (declared > kMaxInflated) {
        fail("declared size " + std::to_string(declared) + " over limit");
    }
    std::string out(declared, '\0');
    z_stream z{};
    z.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed.data()));
    z.avail_in = static_cast<uInt>(compressed.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    if (::inflateInit(&z) != Z_OK) fail("zlib initialization failed");
    const int rc = ::inflate(&z, Z_FINISH);
    const size_t produced = z.total_out;
    const size_t consumed = z.total_in;
    const size_t left = z.avail_in;
    ::inflateEnd(&z);
    // 三条都要：流必须正常结束、产出必须与头部声明一致、**输入必须被吃干净**
    // （尾部残留 = 容器被截断或拼接，静默忽略会让"读出一半"看起来成功）。
    if (rc != Z_STREAM_END) fail("zlib stream rejected (code " + std::to_string(rc) + ")");
    if (produced != declared) {
        fail("inflated " + std::to_string(produced) + " bytes but header declares " +
             std::to_string(declared));
    }
    if (left != 0 || consumed != compressed.size()) {
        fail("zlib stream has " + std::to_string(left) + " trailing byte(s)");
    }
    return out;
}

/// string-map：u32 count，然后 count × (key, value)。
std::map<std::string, std::string> read_string_map(Cursor& c, const char* what) {
    const uint32_t count = c.u32();
    if (count > kMaxEntries) fail(std::string(what) + " entry count over limit");
    std::map<std::string, std::string> out;
    for (uint32_t i = 0; i < count; ++i) {
        std::string key = c.text();
        std::string value = c.text();
        if (key.empty()) fail(std::string(what) + " has an empty key");
        if (!out.emplace(std::move(key), std::move(value)).second) {
            fail(std::string(what) + " has a duplicate key");
        }
    }
    if (!c.at_end()) fail(std::string(what) + " has trailing bytes");
    return out;
}

/// BOWG field 1：u32 count，然后 count × (script name, u32 n, n × u32 line)。
std::map<std::string, std::vector<uint32_t>> read_read_lines(Cursor& c) {
    const uint32_t count = c.u32();
    if (count > kMaxEntries) fail("read-line table entry count over limit");
    std::map<std::string, std::vector<uint32_t>> out;
    size_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        std::string script = c.text();
        const uint32_t n = c.u32();
        if (n > kMaxSpans) fail("read-line array over limit");
        total += n;
        if (total > kMaxSpans) fail("read-line arrays over the total limit");
        std::vector<uint32_t> lines;
        lines.reserve(n);
        for (uint32_t j = 0; j < n; ++j) lines.push_back(c.u32());
        if (script.empty()) fail("read-line set has an empty script name");
        if (!out.emplace(std::move(script), std::move(lines)).second) {
            fail("read-line table has a duplicate script");
        }
    }
    if (!c.at_end()) fail("read-line table has trailing bytes");
    return out;
}

/// 一个指令：i32 line, string name, u32 nattrs, nattrs × (name, value)。
OfficialCommand read_command(Cursor& c) {
    OfficialCommand out;
    out.line = static_cast<int32_t>(c.u32());
    out.name = c.text();
    const uint32_t n = c.u32();
    if (n > 4096) fail("command attribute count over limit");
    for (uint32_t i = 0; i < n; ++i) {
        std::string name = c.text();
        std::string value = c.text();
        out.attrs.emplace(std::move(name), std::move(value));
    }
    return out;
}

/// 命令栈表（BOWS 字段 24 / 26 以及同类字段）：
/// u32 count, × count: string layer_id, u32 ncmds, ncmds × command。
std::vector<OfficialLayer> read_command_map(Cursor& c, const char* what) {
    const uint32_t count = c.u32();
    if (count > kMaxEntries) fail(std::string(what) + " layer count over limit");
    std::vector<OfficialLayer> out;
    size_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        OfficialLayer layer;
        layer.id = c.text();
        const uint32_t n = c.u32();
        total += n;
        if (n > kMaxEntries || total > kMaxEntries) {
            fail(std::string(what) + " command count over limit");
        }
        layer.commands.reserve(n);
        for (uint32_t j = 0; j < n; ++j) layer.commands.push_back(read_command(c));
        out.push_back(std::move(layer));
    }
    if (!c.at_end()) fail(std::string(what) + " has trailing bytes");
    return out;
}

/// 单个指令（BOWS 字段 29 = 当前 BGM 的 [splay]）。
std::vector<OfficialCommand> read_single_command(Cursor& c, const char* what) {
    std::vector<OfficialCommand> out;
    out.push_back(read_command(c));
    if (!c.at_end()) fail(std::string(what) + " has trailing bytes");
    return out;
}

/// 裸字符串（BOWS 字段 11 = 当前消息图层 id）。
std::string read_bare_string(Cursor& c, const char* what) {
    std::string s = c.text();
    if (!c.at_end()) fail(std::string(what) + " has trailing bytes");
    return s;
}

/// 一个脚本帧（实测形状，见头文件 OfficialFrame 的注释）：
///   u32 index, u32 return_line, string script, u32 line,
///   u32 n_pending, × n_pending 待执行指令, 1 条当前指令。
/// 帧是自定界的（指令自带长度），所以**不需要**再猜下一帧从哪开始。
OfficialFrame read_frame(Cursor& c) {
    OfficialFrame f;
    f.index = c.u32();
    f.return_line = c.u32();
    f.script = c.text();
    f.line = c.u32();
    const uint32_t n = c.u32();
    if (n > 4096) fail("frame pending-tag count over limit");
    f.pending.reserve(n);
    for (uint32_t i = 0; i < n; ++i) f.pending.push_back(read_command(c));
    // 存档瞬间正在执行的那条：标签帧也可能出现（test13 存的是 [rp2]）。
    f.current = read_command(c);
    f.has_current = !f.current.name.empty();
    return f;
}

/// BOWS 字段 4：u32 count + count 个帧。逐帧解、解不动就停：**已解出的帧
/// （尤其第一帧 = 当前位置）全部保留**，由 `truncated` 把"后面还有帧但没认
/// 出来"显式记下来，不整体失败。
std::vector<OfficialFrame> read_frames(Cursor& c, bool* truncated) {
    std::vector<OfficialFrame> out;
    *truncated = false;
    const uint32_t count = c.u32();
    if (count > 64) {
        *truncated = true;
        return out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        try {
            out.push_back(read_frame(c));
        } catch (const std::exception&) {
            *truncated = true;
            break;
        }
    }
    // 帧数不足（解不动）或解完还有剩字节（形状变了）都算截断；恰好一帧不差
    // 吃到字段末尾才算形状对上（4 份真档都是这样）。
    if (out.size() != count || !c.at_end()) *truncated = true;
    return out;
}

/// BOWS 字段 25（文本图层的排版状态）：u32 count, × count:
///   string layer_id, u32 n_groups, × n_groups: (排版组)
/// 每组以 u32 类型开头，紧跟 4 个 f32 = 文字框 (left, top, width, height)；
/// 之后是字体面/字号/字距/颜色等排版细节（两代版本的尾巴长度都不一样：
/// test4 是双字面 + 度量，test13 多了 ruby 面），所以这里**只解每层第一组
/// 的矩形**，剩下的按图层 id 重新定位，不猜尾巴。
///
/// 定位方式：把 f24（文本命令，同为这批图层 id）的顺序当作锚点，在字段里
/// 顺序查找 `<u32 长度><id>`。官方存的顺序与 f24 一致（实测两版都是），
/// 多出来的图层（f25 比 f24 多一条的情况）不影响后面锚点继续命中。
std::map<std::string, OfficialRect> read_text_rects(const std::string& raw, size_t begin,
                                                    size_t end,
                                                    const std::vector<OfficialLayer>& text_layers) {
    std::map<std::string, OfficialRect> out;
    auto at = [&raw](size_t p) {
        return size_t(uint32_t(static_cast<uint8_t>(raw[p]))) |
               (size_t(uint32_t(static_cast<uint8_t>(raw[p + 1]))) << 8) |
               (size_t(uint32_t(static_cast<uint8_t>(raw[p + 2]))) << 16) |
               (size_t(uint32_t(static_cast<uint8_t>(raw[p + 3]))) << 24);
    };
    auto f32 = [&raw](size_t p) {
        const uint32_t bits = uint32_t(static_cast<uint8_t>(raw[p])) |
                              (uint32_t(static_cast<uint8_t>(raw[p + 1])) << 8) |
                              (uint32_t(static_cast<uint8_t>(raw[p + 2])) << 16) |
                              (uint32_t(static_cast<uint8_t>(raw[p + 3])) << 24);
        float v = 0.0f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    };
    size_t cursor = begin;
    for (const auto& layer : text_layers) {
        if (layer.id.empty() || layer.id.size() > 200) continue;
        // 顺序查找 <len><id>：id 里只有可打印字符，长度前缀能挡住"子串恰好在
        // 别的字符串里"的假命中。
        size_t found = std::string::npos;
        for (size_t p = cursor; p + 4 + layer.id.size() <= end; ++p) {
            const uint32_t len = uint32_t(at(p));
            if (len != layer.id.size()) continue;
            if (std::memcmp(raw.data() + p + 4, layer.id.data(), len) != 0) continue;
            found = p;
            break;
        }
        if (found == std::string::npos) continue;
        cursor = found + 4 + layer.id.size();
        if (cursor + 4 + 4 + 16 > end) continue;
        const uint32_t groups = uint32_t(at(cursor));
        if (groups == 0 || groups > 64) continue;
        const size_t g = cursor + 4 + 4; // 跳过组数 + 组类型
        if (g + 16 > end) continue;
        OfficialRect rect;
        rect.left = f32(g);
        rect.top = f32(g + 4);
        rect.width = f32(g + 8);
        rect.height = f32(g + 12);
        // 只收"像样"的框：有限、非负尺寸、量级在画布尺度内（坏数据宁可丢掉，
        // 也不要把文本框甩到屏幕外）。
        const bool finite = std::isfinite(rect.left) && std::isfinite(rect.top) &&
                            std::isfinite(rect.width) && std::isfinite(rect.height);
        const bool sane = finite && std::fabs(rect.left) < 100000.0f &&
                          std::fabs(rect.top) < 100000.0f && rect.width >= 0.0f &&
                          rect.height >= 0.0f && rect.width < 100000.0f &&
                          rect.height < 100000.0f;
        if (!sane || (rect.width == 0.0f && rect.height == 0.0f)) continue;
        out.emplace(layer.id, rect);
    }
    return out;
}

/// 容器解包结果：官方容器头/目录 + 解压后的负载（负载要留着给字段解析）。
struct Parsed {
    OfficialSave base;
    std::string raw;
    Directory dir;
};

Parsed parse_container(std::string_view bytes) {
    if (!is_official_save(bytes)) fail("not a BOW* container (magic mismatch)");
    Parsed out;
    const std::string_view magic = bytes.substr(0, 4);
    out.base.kind = magic == "BOWX" ? OfficialSaveKind::System
                    : magic == "BOWG" ? OfficialSaveKind::Global
                                      : OfficialSaveKind::Slot;
    Cursor head(bytes, 4, bytes.size());
    out.base.version = head.u32();
    if (out.base.version < kMinVersion || out.base.version > kMaxVersion) {
        fail("unsupported version " + std::to_string(out.base.version));
    }
    const uint32_t declared = head.u32();
    out.raw = inflate_payload(bytes.substr(kHeaderBytes), declared);
    out.dir = parse_directory(out.raw);
    for (const auto& [start, fields] : out.dir.objects) {
        auto& ids = out.base.field_ids[start];
        for (const auto& [id, off] : fields) {
            (void)off;
            ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
    }
    return out;
}

} // namespace

bool is_official_save(std::string_view bytes) {
    if (bytes.size() < kHeaderBytes) return false;
    const std::string_view magic = bytes.substr(0, 4);
    return magic == "BOWX" || magic == "BOWG" || magic == "BOWS";
}

OfficialSave decode_official_save(std::string_view bytes) {
    Parsed parsed = parse_container(bytes);
    const std::string& raw = parsed.raw;
    const Directory& dir = parsed.dir;
    OfficialSave out = std::move(parsed.base);
    const size_t dir_limit = dir.dir_offset;
    if (out.kind == OfficialSaveKind::System) {
        // BOWX：只有一张 s.* 表（field 1）。
        const auto span = field_span(dir, 0, 1, dir_limit);
        Cursor c(raw, span.first, span.second);
        out.variables = read_string_map(c, "BOWX variable table");
    } else if (out.kind == OfficialSaveKind::Global) {
        // BOWG：field 1 = 已读行，field 2 = g.* 表。
        {
            const auto span = field_span(dir, 0, 1, dir_limit);
            Cursor c(raw, span.first, span.second);
            out.read_lines = read_read_lines(c);
        }
        const auto span = field_span(dir, 0, 2, dir_limit);
        Cursor c(raw, span.first, span.second);
        out.variables = read_string_map(c, "BOWG variable table");
    } else {
        // BOWS：field 30 = 变量 bank 对象（它的 field 2 才是变量表）。
        const auto bank = field_span(dir, 0, 30, dir_limit);
        const auto bank_start = static_cast<uint32_t>(bank.first);
        if (!dir.objects.count(bank_start)) {
            fail("BOWS variable bank object " + std::to_string(bank_start) +
                 " missing from directory");
        }
        const auto span = field_span(dir, bank_start, 2, bank.second);
        Cursor c(raw, span.first, span.second);
        out.variables = read_string_map(c, "BOWS variable table");
    }
    return out;
}

OfficialSlot decode_official_slot(std::string_view bytes) {
    Parsed parsed = parse_container(bytes);
    if (parsed.base.kind != OfficialSaveKind::Slot) {
        fail("not a BOWS container (this reader only maps numbered saves)");
    }
    OfficialSlot out;
    out.header = std::move(parsed.base);
    const std::string& raw = parsed.raw;
    const Directory& dir = parsed.dir;
    const size_t dir_limit = dir.dir_offset;
    // 变量 bank：与 decode_official_save 同一条路径。
    {
        const auto bank = field_span(dir, 0, 30, dir_limit);
        const auto bank_start = static_cast<uint32_t>(bank.first);
        if (!dir.objects.count(bank_start)) {
            fail("BOWS variable bank object " + std::to_string(bank_start) +
                 " missing from directory");
        }
        const auto span = field_span(dir, bank_start, 2, bank.second);
        Cursor c(raw, span.first, span.second);
        out.header.variables = read_string_map(c, "BOWS variable table");
    }
    // 字段 26 / 24 / 29 / 11 形状已确定 ⇒ 严格解析（字段自带定界，解析器必须
    // 恰好吃完；吃不干净 = 形状变了，宁可报错也不产出半份图层）。
    if (dir.objects.at(0).count(26)) {
        const auto span = field_span(dir, 0, 26, dir_limit);
        Cursor c(raw, span.first, span.second);
        out.image_layers = read_command_map(c, "BOWS image-layer commands");
    }
    if (dir.objects.at(0).count(24)) {
        const auto span = field_span(dir, 0, 24, dir_limit);
        Cursor c(raw, span.first, span.second);
        out.text_layers = read_command_map(c, "BOWS text-layer commands");
    }
    if (dir.objects.at(0).count(29)) {
        const auto span = field_span(dir, 0, 29, dir_limit);
        Cursor c(raw, span.first, span.second);
        out.bgm = read_single_command(c, "BOWS bgm command");
    }
    if (dir.objects.at(0).count(11)) {
        const auto span = field_span(dir, 0, 11, dir_limit);
        Cursor c(raw, span.first, span.second);
        out.current_message_layer = read_bare_string(c, "BOWS current message layer");
    }
    // 字段 25：文本图层排版矩形（以 f24 的图层 id 顺序为锚点解，见函数注释）。
    if (dir.objects.at(0).count(25) && !out.text_layers.empty()) {
        const auto span = field_span(dir, 0, 25, dir_limit);
        out.text_rects = read_text_rects(raw, span.first, span.second, out.text_layers);
    }
    // 字段 4（场景脚本栈）：逐帧解，形状对上就一帧不差地吃到末尾。
    if (dir.objects.at(0).count(4)) {
        const auto span = field_span(dir, 0, 4, dir_limit);
        Cursor c(raw, span.first, span.second);
        out.frames = read_frames(c, &out.frames_truncated);
    }
    // 字段 3：系统脚本帧，与字段 4 的单帧同形（多出来的 16 字节是"待执行 0 +
    // 当前指令 = 空名"）。只用来在 frames 为空时兜底位置。
    if (dir.objects.at(0).count(3)) {
        const auto span = field_span(dir, 0, 3, dir_limit);
        Cursor c(raw, span.first, span.second);
        try {
            OfficialFrame f = read_frame(c);
            if (!f.script.empty()) {
                out.system_frame = std::move(f);
                out.has_system_frame = true;
            }
        } catch (const std::exception&) {
            // 老版本的系统帧没有"当前指令"尾巴：退回只读帧头。
            Cursor again(raw, span.first, span.second);
            try {
                OfficialFrame f;
                f.index = again.u32();
                f.return_line = again.u32();
                f.script = again.text();
                f.line = again.u32();
                if (!f.script.empty()) {
                    out.system_frame = std::move(f);
                    out.has_system_frame = true;
                }
            } catch (const std::exception&) {
                // 形状不认识就当没有：系统帧只用来兜底位置，不是必需品。
            }
        }
    }
    return out;
}

Value infer_official_value(const std::string& raw) {
    if (raw.empty()) return Value::make_string(raw);
    // 只认"干净的数值写法"：前后空白、显式 '+' 一律算字符串（官方写出来的
    // 数值没有这些；出现它们通常意味着这串东西不是拿来做算术的）。
    const char front = raw.front();
    const bool clean = front != ' ' && front != '\t' && front != '\n' && front != '\r' &&
                       front != '+';
    // "001" 这种前导零写法保持字符串：它更像编号，转成 1 会改掉原文本
    // （"0.5" 不在此列，那是小数）。
    const bool leading_zero_notation =
        raw.size() > 1 && raw[0] == '0' && raw[1] != '.';
    const bool has_dot = raw.find('.') != std::string::npos;
    const bool has_exponent = raw.find_first_of("eE") != std::string::npos;
    if (clean && !leading_zero_notation && !has_dot && !has_exponent) {
        const char* p = raw.c_str();
        char* end = nullptr;
        errno = 0;
        const long long v = std::strtoll(p, &end, 10);
        if (errno == 0 && end == p + raw.size()) return Value::make_int(int64_t(v));
        return Value::make_string(raw);
    }
    if (clean && (has_dot || has_exponent)) {
        const char* p = raw.c_str();
        char* end = nullptr;
        errno = 0;
        const double v = std::strtod(p, &end);
        // std::isfinite 顺带挡掉 "inf"/"nan"（strtod 会吃掉它们，但游戏不会
        // 把这两个词当数存）。
        if (errno == 0 && end == p + raw.size() && std::isfinite(v)) {
            return Value::make_float(v);
        }
    }
    return Value::make_string(raw);
}

bool official_key_is_host_path(const std::string& key) {
    // 实测两份官方存档都带这两个键，值形如
    // "C:\\Users\\koichi\\Documents\\…" / "C:\\artemis\\testgame\\test13"。
    return key == "s.datapath" || key == "s.savepath" || key == "datapath" ||
           key == "savepath";
}

OfficialImportStats import_official_variables(
    const OfficialSave& doc, const std::function<void(const std::string&, Value)>& set) {
    OfficialImportStats stats;
    for (const auto& [key, raw] : doc.variables) {
        if (official_key_is_host_path(key)) {
            ++stats.skipped_host_path;
            continue;
        }
        // 键缺域前缀时按容器种类补：官方本来写全前缀（"s.bgmvol"），
        // 这里只是兜住少数只写裸名的发行版。
        std::string name = key;
        if (name.size() < 2 || name[1] != '.') {
            name = (doc.kind == OfficialSaveKind::System ? "s." : "g.") + name;
        }
        Value value = infer_official_value(raw);
        // 官方把 Lua 表（g.config / g.script / lua.scr …）存成 native Pluto
        // 的二进制。原样搬过来的话，游戏 Lua 调 pluto.unpersist 时本引擎会
        // 认不出（它读的是自己的标签流），而且是**静默**失败——unpersist 的
        // 契约是"读不出来就给空表"，于是 g.config 变成 {}、conf.language 丢
        // 失，test4 每次启动都以为自己是首次运行、反复弹语言选择。所以在
        // 导入这一步就转成本引擎的文档格式。
        if (looks_like_official_pluto(raw)) {
            if (auto converted = convert_official_pluto(raw)) {
                value = Value::make_string(*converted);
                ++stats.pluto_converted;
            } else {
                ++stats.pluto_failed;
            }
        }
        set(name, std::move(value));
        ++stats.applied;
    }
    for (const auto& [script, lines] : doc.read_lines) {
        (void)script;
        stats.read_lines += lines.size();
    }
    return stats;
}

// ---------------------------------------------------------------------------
// 官方 Pluto 值（native engine 的 pluto.persist 输出）→ 本引擎的 pluto 文档
// ---------------------------------------------------------------------------
// 官方引擎的 `pluto` 是真正的 Pluto 库（hoelzro/pluto），二进制形状是
//   [u32 first][u32 ref][u32 type] …
// 而本引擎的 `pluto.persist` 用的是自己的标签流（core/util/binary_stream.h，
// 带 "OASB" 头）。两者不通用，所以官方存档里那些"持久化的 Lua 表"变量
// （g.script / lua.scr / lua.log …）搬过来以后，游戏 Lua 再调
// pluto.unpersist 会读不出来。这里在**导入时**转一次：官方值 → 值树 →
// 本引擎文档。转不动就保留原始字节（调用方决定怎么处理）。
namespace {

constexpr int kPlutoMaxDepth = 128;
constexpr size_t kPlutoMaxNodes = 262144;

struct PlutoNode;
using PlutoPtr = std::shared_ptr<PlutoNode>;

struct PlutoNode {
    enum class Kind { Nil, Bool, Number, Text, Table };
    Kind kind = Kind::Nil;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<std::pair<PlutoPtr, PlutoPtr>> entries; // table: key → value
};

class PlutoReader {
public:
    PlutoReader(std::string_view data, unsigned word) : data_(data), word_(word) {}

    PlutoPtr read() {
        PlutoPtr node = read_value(0);
        if (pos_ != data_.size()) throw oa::util::FormatError("pluto: trailing bytes");
        return node;
    }

private:
    uint64_t take(unsigned n) {
        if (n > data_.size() - pos_) throw oa::util::FormatError("pluto: truncated value");
        uint64_t v = 0;
        for (unsigned i = 0; i < n; ++i) {
            v |= uint64_t(static_cast<uint8_t>(data_[pos_++])) << (8 * i);
        }
        return v;
    }

    PlutoPtr read_value(int depth) {
        if (depth > kPlutoMaxDepth) throw oa::util::FormatError("pluto: nesting too deep");
        if (++nodes_ > kPlutoMaxNodes) throw oa::util::FormatError("pluto: too many values");
        const uint32_t first = static_cast<uint32_t>(take(4));
        const uint32_t ref = static_cast<uint32_t>(take(4));
        if (first == 0) {
            if (ref == 0) return std::make_shared<PlutoNode>(); // nil
            const auto it = refs_.find(ref);
            if (it == refs_.end()) throw oa::util::FormatError("pluto: dangling reference");
            return it->second;
        }
        if (first != 1 || ref == 0) throw oa::util::FormatError("pluto: invalid object header");
        if (refs_.count(ref)) throw oa::util::FormatError("pluto: duplicate object id");
        const uint32_t type = static_cast<uint32_t>(take(4));
        auto node = std::make_shared<PlutoNode>();
        switch (type) {
            case 1: { // boolean
                const uint32_t b = static_cast<uint32_t>(take(4));
                if (b > 1) throw oa::util::FormatError("pluto: invalid boolean");
                node->kind = PlutoNode::Kind::Bool;
                node->boolean = b != 0;
                break;
            }
            case 3: { // number (IEEE double)
                const uint64_t bits = take(8);
                double d = 0.0;
                std::memcpy(&d, &bits, sizeof(d));
                node->kind = PlutoNode::Kind::Number;
                node->number = d;
                break;
            }
            case 4: { // string, 长度是**原生 size_t**（x64 = 8 字节，32 位 = 4）
                const uint64_t n = take(word_);
                if (n > data_.size() - pos_) throw oa::util::FormatError("pluto: bad string length");
                node->kind = PlutoNode::Kind::Text;
                node->text.assign(data_.substr(pos_, static_cast<size_t>(n)));
                pos_ += static_cast<size_t>(n);
                break;
            }
            case 5: { // table：先元表（nil/table），再 key/value 对直到 nil key
                if (take(4) != 0) throw oa::util::FormatError("pluto: persist hook unsupported");
                node->kind = PlutoNode::Kind::Table;
                (void)read_value(depth + 1); // 元表：对存档无意义，读掉即可
                refs_[ref] = node;           // 先登记：表可以自引用
                for (;;) {
                    PlutoPtr key = read_value(depth + 1);
                    if (key->kind == PlutoNode::Kind::Nil) break;
                    node->entries.emplace_back(std::move(key), read_value(depth + 1));
                }
                return node;
            }
            default:
                // 6/8（函数/线程）与 101（永久对象）需要 VM，存档里不该出现。
                throw oa::util::FormatError("pluto: unsupported value type " +
                                            std::to_string(type));
        }
        refs_[ref] = node;
        return node;
    }

    std::string_view data_;
    size_t pos_ = 0;
    size_t nodes_ = 0;
    unsigned word_ = 8;
    std::map<uint32_t, PlutoPtr> refs_;
};

/// 值树 → 本引擎 pluto 文档（标签流；数字沿用 runtime_lua.cpp 的规则：
/// 整数值走最小宽度整数，其余 bit-exact f64）。
void emit_pluto(oa::util::Writer& w, const PlutoPtr& v) {
    switch (v->kind) {
        case PlutoNode::Kind::Nil:
            w.nil();
            return;
        case PlutoNode::Kind::Bool:
            w.boolean(v->boolean);
            return;
        case PlutoNode::Kind::Number: {
            double ipart = 0.0;
            if (std::isfinite(v->number) && std::modf(v->number, &ipart) == 0.0 &&
                std::fabs(v->number) < 9007199254740992.0) {
                w.i64(static_cast<int64_t>(v->number));
            } else {
                w.f64(v->number);
            }
            return;
        }
        case PlutoNode::Kind::Text:
            w.str(v->text);
            return;
        case PlutoNode::Kind::Table: {
            w.map_begin(v->entries.size());
            for (const auto& [key, value] : v->entries) {
                emit_pluto(w, key);
                emit_pluto(w, value);
            }
            return;
        }
    }
}

} // namespace

bool looks_like_official_pluto(const std::string& raw) {
    if (raw.size() < 12) return false;
    const auto u32 = [&raw](size_t off) {
        uint32_t v = 0;
        std::memcpy(&v, raw.data() + off, sizeof(v));
        return v;
    };
    if (u32(0) != 1 || u32(4) == 0) return false;
    const uint32_t type = u32(8);
    return type >= 1 && type <= 5; // boolean / number / string / table
}

std::optional<std::string> convert_official_pluto(std::string_view raw) {
    // 两代 sizeof(size_t)：x64 引擎写 8 字节长度，32 位引擎/旧档可能是 4。
    for (unsigned word : {8u, 4u}) {
        try {
            PlutoReader reader(raw, word);
            const PlutoPtr node = reader.read();
            if (node->kind != PlutoNode::Kind::Table) continue; // 游戏只 unpersist 表
            oa::util::Writer w;
            emit_pluto(w, node);
            return w.data();
        } catch (const std::exception&) {
            continue;
        }
    }
    return std::nullopt;
}

} // namespace oa::runtime
