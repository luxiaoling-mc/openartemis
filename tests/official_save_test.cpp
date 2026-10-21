// 官方（原生 SDK）存档容器 BOWX/BOWG/BOWS 的回归测试。
//
// 覆盖：容器头 + zlib + 尾部字段目录的解析、三种容器的字段取法、值类型推断、
// host-path 键规则、导入时的域前缀兜底，以及所有"坏容器"路径必须抛
// FormatError（不许半份数据/不许越界读）。
//
// 夹具是**自造**的（同 zlib 现压），所以不依赖任何游戏资产；另外留一个
// 环境变量开关（OA_TEST_OFFICIAL_SAVE_DIR）用真实官方 savedata 再过一遍：
//   OA_TEST_OFFICIAL_SAVE_DIR="…/test4/savedata" ./official_save_test
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

#include "core/fs/fs.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_iet.h"
#include "core/runtime/runtime_lua.h"
#include "core/runtime/runtime_save.h"
#include "core/runtime/runtime_save_official.h"
#include "core/util/binary_stream.h"

namespace {

int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// --- 夹具构造 -------------------------------------------------------------

void put_u32(std::string& out, uint32_t v) {
    out.push_back(char(v & 0xff));
    out.push_back(char((v >> 8) & 0xff));
    out.push_back(char((v >> 16) & 0xff));
    out.push_back(char((v >> 24) & 0xff));
}

void put_string(std::string& out, const std::string& bytes) {
    put_u32(out, uint32_t(bytes.size()));
    out += bytes;
}

std::string string_map(const std::vector<std::pair<std::string, std::string>>& rows) {
    std::string out;
    put_u32(out, uint32_t(rows.size()));
    for (const auto& [k, v] : rows) {
        put_string(out, k);
        put_string(out, v);
    }
    return out;
}

std::string zlib_compress(const std::string& raw) {
    uLongf n = compressBound(uLong(raw.size()));
    std::string out(n, '\0');
    const int rc = compress2(reinterpret_cast<Bytef*>(out.data()), &n,
                             reinterpret_cast<const Bytef*>(raw.data()), uLong(raw.size()), 9);
    check(rc == Z_OK, "fixture compression");
    out.resize(n);
    return out;
}

/// objects: object_start → (field_id → field_offset)，偏移都是"数据区内的绝对
/// 偏移"。目录在数据区尾部，最后 4 字节再放目录偏移。
std::string build_container(
    const std::string& magic, uint32_t version, const std::string& data_region,
    const std::map<uint32_t, std::map<uint32_t, uint32_t>>& objects) {
    std::string data = data_region;
    const uint32_t dir = uint32_t(data.size());
    put_u32(data, uint32_t(objects.size()));
    for (const auto& [start, fields] : objects) {
        put_u32(data, start);
        put_u32(data, uint32_t(fields.size()));
        for (const auto& [id, off] : fields) {
            put_u32(data, id);
            put_u32(data, off);
        }
    }
    put_u32(data, dir);
    std::string out = magic;
    put_u32(out, version);
    put_u32(out, uint32_t(data.size()));
    out += zlib_compress(data);
    return out;
}

/// 单字段容器（BOWX 只有 field 1；数据区 = 该字段）。
std::string bowx(const std::map<std::string, std::string>& vars, uint32_t version = 1003) {
    std::vector<std::pair<std::string, std::string>> rows(vars.begin(), vars.end());
    return build_container("BOWX", version, string_map(rows), {{0, {{1, 0}}}});
}

/// BOWG：field 1 = 已读行，field 2 = g.* 表。
std::string bowg(const std::map<std::string, std::string>& vars,
                 const std::vector<std::pair<std::string, std::vector<uint32_t>>>& reads,
                 uint32_t version = 1003) {
    std::string lines;
    put_u32(lines, uint32_t(reads.size()));
    for (const auto& [script, rows] : reads) {
        put_string(lines, script);
        put_u32(lines, uint32_t(rows.size()));
        for (uint32_t v : rows) put_u32(lines, v);
    }
    std::string data = lines;
    const uint32_t vars_off = uint32_t(data.size());
    std::vector<std::pair<std::string, std::string>> rows(vars.begin(), vars.end());
    data += string_map(rows);
    return build_container("BOWG", version, data, {{0, {{1, 0}, {2, vars_off}}}});
}

/// BOWS：root 的 field 30 指向变量 bank 对象（该对象的 field 2 才是变量表）。
std::string bows(const std::map<std::string, std::string>& vars, uint32_t version = 1003) {
    std::string data(4, '\0'); // 让 bank 起始偏移 ≠ 0（root 也占 0）
    const uint32_t bank = uint32_t(data.size());
    std::vector<std::pair<std::string, std::string>> rows(vars.begin(), vars.end());
    data += string_map(rows);
    std::map<uint32_t, std::map<uint32_t, uint32_t>> objects;
    objects[0] = {{30, bank}};
    objects[bank] = {{2, bank}};
    return build_container("BOWS", version, data, objects);
}

bool throws_format_error(const std::string& bytes) {
    try {
        oa::runtime::decode_official_save(bytes);
    } catch (const oa::util::FormatError&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

// --- 用例 -----------------------------------------------------------------

void test_bowx_roundtrip() {
    const std::map<std::string, std::string> vars{
        {"s.bgmvol", "80"},
        {"s.datapath", "C:\\Users\\koichi\\Documents\\game"},
        {"s.status.automode", "0"},
        {"s.current_message_layer", "1.80.mw.adv_adv"},
        {"s.empty", ""},
        {"s.cp932", "\x93\xfa\x96\x7b"}, // CP932 原文（"日本"）
    };
    const auto doc = oa::runtime::decode_official_save(bowx(vars));
    check(doc.kind == oa::runtime::OfficialSaveKind::System, "BOWX kind");
    check(doc.version == 1003, "BOWX version");
    check(doc.variables == vars, "BOWX variables survive byte-exactly");
    check(doc.read_lines.empty(), "BOWX has no read-line set");
    check(!doc.field_ids.empty() && doc.field_ids.at(0) == std::vector<uint32_t>{1},
          "BOWX field ids");

    // 1002（旧版本）同样接受。
    const auto old = oa::runtime::decode_official_save(bowx(vars, 1002));
    check(old.version == 1002 && old.variables == vars, "BOWX v1002 accepted");
}

void test_bowg() {
    const std::map<std::string, std::string> vars{
        {"g.reset", "0"},
        {"g.logo", "1"},
        {"g.script", std::string("\x01\x00\x00\x00\x02\x00", 6)}, // 二进制块原样
    };
    const std::vector<std::pair<std::string, std::vector<uint32_t>>> reads{
        {"scenario/main/aiy00010.asb", {2824, 2823, 2822}},
        {"system/first.iet", {13, 12, 11, 10}},
        {"system/logo.iet", {}},
    };
    const auto doc = oa::runtime::decode_official_save(bowg(vars, reads));
    check(doc.kind == oa::runtime::OfficialSaveKind::Global, "BOWG kind");
    check(doc.variables == vars, "BOWG variables");
    check(doc.read_lines.size() == reads.size(), "BOWG read-line set count");
    check(doc.read_lines.at("scenario/main/aiy00010.asb") ==
              std::vector<uint32_t>({2824, 2823, 2822}),
          "BOWG read-line rows");
    check(doc.read_lines.at("system/logo.iet").empty(), "BOWG empty read-line set");
}

void test_bows() {
    const std::map<std::string, std::string> vars{
        {"game.ver", "1.0.1"},
        {"lua.scr", std::string("\x01\x00\x00\x00\x01\x00\x00\x00\x05\x00", 10)},
        {"hide", "0"},
    };
    const auto doc = oa::runtime::decode_official_save(bows(vars));
    check(doc.kind == oa::runtime::OfficialSaveKind::Slot, "BOWS kind");
    check(doc.variables == vars, "BOWS variable bank");
    check(doc.field_ids.size() == 2, "BOWS object count (root + bank)");
}

void test_rejects() {
    const auto good = bowx({{"s.a", "1"}});
    check(!oa::runtime::is_official_save("OASB"), "OASB is not official");
    check(!oa::runtime::is_official_save("BOW"), "short magic is not official");
    check(throws_format_error(std::string("OASB\x01\x00\x00\x00", 8)), "OASB rejected");
    check(throws_format_error(""), "empty rejected");
    check(throws_format_error(good.substr(0, 8)), "truncated header rejected");
    check(throws_format_error(good.substr(0, good.size() - 2)), "truncated stream rejected");

    { // 版本在支持区间外
        std::string bad = good;
        bad[4] = char(0x63); // 999
        bad[5] = char(0x03);
        check(throws_format_error(bad), "version 999 rejected");
    }
    { // 头部声明长度与实际解压长度不符
        std::string bad = good;
        bad[8] = char(bad[8] + 1);
        check(throws_format_error(bad), "declared size mismatch rejected");
    }
    { // zlib 流之后的垃圾字节
        std::string bad = good;
        bad.push_back('\x00');
        check(throws_format_error(bad), "trailing garbage after zlib rejected");
    }
    { // 目录偏移越界
        const std::string data = string_map({{"s.a", "1"}});
        std::string raw = data;
        put_u32(raw, 4);        // count
        put_u32(raw, 0);        // object start
        put_u32(raw, 1);        // fields
        put_u32(raw, 1);        // field id
        put_u32(raw, 0);        // field offset
        put_u32(raw, 0xffffff); // 目录偏移 → 越界
        std::string out = "BOWX";
        put_u32(out, 1003);
        put_u32(out, uint32_t(raw.size()));
        out += zlib_compress(raw);
        check(throws_format_error(out), "directory offset out of range rejected");
    }
    { // 同 object 内重复 field id
        const std::string data = string_map({{"s.a", "1"}});
        std::string raw = data;
        put_u32(raw, 1);
        put_u32(raw, 0);
        put_u32(raw, 2);
        put_u32(raw, 1);
        put_u32(raw, 0);
        put_u32(raw, 1);
        put_u32(raw, 0);
        put_u32(raw, uint32_t(data.size()));
        std::string out = "BOWX";
        put_u32(out, 1003);
        put_u32(out, uint32_t(raw.size()));
        out += zlib_compress(raw);
        check(throws_format_error(out), "duplicate field id rejected");
    }
    { // 字符串表尾部多余字节
        std::string data = string_map({{"s.a", "1"}});
        data.push_back('\x00');
        const auto doc = build_container("BOWX", 1003, data, {{0, {{1, 0}}}});
        check(throws_format_error(doc), "trailing bytes in string map rejected");
    }
    { // 空键
        const auto doc = bowx({{"", "1"}});
        check(throws_format_error(doc), "empty variable key rejected");
    }
    { // 字符串长度越界
        std::string data;
        put_u32(data, 1);
        put_u32(data, 0xffff); // key 长度远超数据区
        data += "x";
        const auto doc = build_container("BOWX", 1003, data, {{0, {{1, 0}}}});
        check(throws_format_error(doc), "string length overrun rejected");
    }
    { // 缺少根 object
        const auto doc = build_container("BOWX", 1003, "xxxx", {{4, {{1, 4}}}});
        check(throws_format_error(doc), "missing root object rejected");
    }
    { // 缺字段（BOWX 只有 field 2）
        const std::string data = string_map({{"s.a", "1"}});
        const auto doc = build_container("BOWX", 1003, data, {{0, {{2, 0}}}});
        check(throws_format_error(doc), "missing field 1 rejected");
    }
    { // BOWS 的 field 30 指向不存在的 object
        const std::string data = string_map({{"a", "1"}});
        const auto doc = build_container("BOWS", 1003, data, {{0, {{30, 0}}}});
        check(throws_format_error(doc), "BOWS missing variable bank rejected");
    }
}

void test_value_inference() {
    using oa::runtime::infer_official_value;
    check(infer_official_value("80").kind == oa::runtime::ValueKind::Int, "\"80\" → int");
    check(infer_official_value("80").int_val == 80, "\"80\" value");
    check(infer_official_value("0").kind == oa::runtime::ValueKind::Int, "\"0\" → int");
    check(infer_official_value("0").int_val == 0, "\"0\" is zero");
    check(infer_official_value("-3").int_val == -3, "negative int");
    check(infer_official_value("001").kind == oa::runtime::ValueKind::String,
          "leading-zero notation stays a string");
    check(infer_official_value("+3").kind == oa::runtime::ValueKind::String,
          "explicit plus stays a string");
    check(infer_official_value("1.5").kind == oa::runtime::ValueKind::Float, "\"1.5\" → float");
    check(infer_official_value("0.0").kind == oa::runtime::ValueKind::Float, "\"0.0\" → float");
    check(infer_official_value("1.80.mw.adv_adv").kind == oa::runtime::ValueKind::String,
          "dotted version-ish string stays a string");
    check(infer_official_value("").kind == oa::runtime::ValueKind::String, "empty stays string");
    check(infer_official_value(" 12").kind == oa::runtime::ValueKind::String,
          "leading space stays a string");
    check(infer_official_value("inf").kind == oa::runtime::ValueKind::String,
          "inf stays a string");
    check(infer_official_value("nan").kind == oa::runtime::ValueKind::String,
          "nan stays a string");
    const std::string blob("\x01\x00\x00\x00", 4);
    check(infer_official_value(blob).kind == oa::runtime::ValueKind::String,
          "binary blob stays a string");
    check(infer_official_value(blob).str_val == blob, "binary blob is byte-exact");
    // "0" 必须是数值 0 而不是真值字符串：这是导入时最要命的一条。
    check(!infer_official_value("0").to_bool(), "\"0\" must not be truthy");
}

std::string official_pluto_table(); // 定义见下方槽位夹具（两处共用）

void test_host_paths_and_import() {
    check(oa::runtime::official_key_is_host_path("s.datapath"), "s.datapath is a host path");
    check(oa::runtime::official_key_is_host_path("s.savepath"), "s.savepath is a host path");
    check(!oa::runtime::official_key_is_host_path("s.bgmvol"), "s.bgmvol is not");

    oa::runtime::OfficialSave doc;
    doc.kind = oa::runtime::OfficialSaveKind::System;
    doc.variables = {
        {"s.bgmvol", "80"},
        {"s.datapath", "C:\\elsewhere"},
        {"sep", "5"},              // 缺域前缀 → 补 "s."
        {"g.keep", "1"},           // 已有别的域前缀 → 原样
    };
    doc.read_lines = {{"script.iet", {1, 2, 3}}};
    std::map<std::string, oa::runtime::Value> out;
    const auto stats = oa::runtime::import_official_variables(
        doc, [&out](const std::string& k, oa::runtime::Value v) { out[k] = v; });
    check(stats.applied == 3, "import applies 3 keys");
    check(stats.skipped_host_path == 1, "import skips the host path key");
    check(stats.read_lines == 3, "import counts read lines");
    check(out.count("s.datapath") == 0, "host path key never reaches the store");
    check(out.at("s.bgmvol").int_val == 80, "typed value imported");
    check(out.count("s.sep") == 1, "missing prefix gets the container's domain");
    check(out.count("g.keep") == 1, "existing domain prefix is preserved");

    // Pluto 表（g.config / g.script …）：导入时就得转成本引擎的文档格式。
    // 不转的话游戏侧 pluto.unpersist 会**静默**拿到空表，conf.language 丢失，
    // test4 每次启动都重弹语言选择。
    oa::runtime::OfficialSave global;
    global.kind = oa::runtime::OfficialSaveKind::Global;
    global.variables = {{"g.config", official_pluto_table()},
                        {"g.plain", "not binary"},
                        {"g.number", "42"}};
    std::map<std::string, oa::runtime::Value> gout;
    const auto gstats = oa::runtime::import_official_variables(
        global, [&gout](const std::string& k, oa::runtime::Value v) { gout[k] = v; });
    check(gstats.pluto_converted == 1, "import converts the official Pluto table");
    check(gstats.pluto_failed == 0, "no Pluto conversion failure on a valid table");
    check(gout.at("g.config").str_val.rfind("OASB", 0) == 0,
          "g.config arrives as an engine pluto document");
    check(gout.at("g.plain").str_val == "not binary", "non-Pluto bytes stay untouched");
    check(gout.at("g.number").int_val == 42, "plain numbers are still typed");
}

/// 真实官方存档（可选）：OA_TEST_OFFICIAL_SAVE_DIR 指向一份 savedata 目录。
void test_real_fixture() {
    const char* dir = std::getenv("OA_TEST_OFFICIAL_SAVE_DIR");
    if (!dir || !*dir) {
        std::printf("official_save_test: real-fixture pass skipped "
                    "(set OA_TEST_OFFICIAL_SAVE_DIR to a savedata directory)\n");
        return;
    }
    const std::string root(dir);
    const auto load = [&](const char* name, std::string* out) {
        std::FILE* f = std::fopen((root + "/" + name).c_str(), "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        out->resize(size_t(n));
        const size_t got = n ? std::fread(out->data(), 1, size_t(n), f) : 0;
        std::fclose(f);
        out->resize(got);
        return got == size_t(n);
    };
    std::string system_doc;
    std::string global_doc;
    if (!load("system.dat", &system_doc) || !load("saveg.dat", &global_doc)) {
        std::printf("official_save_test: %s has no system.dat/saveg.dat; skipped\n",
                    root.c_str());
        return;
    }
    const auto system = oa::runtime::decode_official_save(system_doc);
    check(system.kind == oa::runtime::OfficialSaveKind::System, "real system.dat kind");
    check(!system.variables.empty(), "real system.dat variables");
    check(system.variables.count("s.bgmvol") == 1, "real system.dat has s.bgmvol");

    const auto global = oa::runtime::decode_official_save(global_doc);
    check(global.kind == oa::runtime::OfficialSaveKind::Global, "real saveg.dat kind");
    check(!global.variables.empty(), "real saveg.dat variables");
    check(!global.read_lines.empty(), "real saveg.dat read-line sets");
    // 已读行必须像"行号"（用来兜住数组切分错位：错位后会读成大整数）。
    size_t rows = 0;
    for (const auto& [script, lines] : global.read_lines) {
        (void)script;
        rows += lines.size();
        for (uint32_t line : lines) check(line < 1000000, "read-line looks like a line number");
    }
    check(rows > 0, "real saveg.dat read-lines are not all empty");

    // 全局变量里的 Pluto 表（g.config = 设置 + 语言）：导入时必须转换。
    // 这是"官方档里语言已经选好、进游戏不该再问"的关键——不转的话
    // pluto.unpersist 静默返回 {}，conf.language 消失，每次启动都重弹选择。
    std::map<std::string, oa::runtime::Value> imported;
    const auto gstats = oa::runtime::import_official_variables(
        global, [&imported](const std::string& k, oa::runtime::Value v) { imported[k] = v; });
    if (imported.count("g.config")) {
        check(gstats.pluto_converted > 0, "real saveg.dat Pluto tables converted");
        check(imported.at("g.config").str_val.rfind("OASB", 0) == 0,
              "real g.config becomes an engine pluto document");
        check(imported.at("g.config").str_val.size() > 256,
              "converted g.config is not truncated");
    }

    // 编号存档（官方 save/qsave）：有就一起验。
    for (const char* slot_name : {"save0001.dat", "AiyokunoEustia0001.dat"}) {
        std::string slot_doc;
        if (!load(slot_name, &slot_doc)) continue;
        const auto slot = oa::runtime::decode_official_slot(slot_doc);
        check(slot.header.kind == oa::runtime::OfficialSaveKind::Slot, "real slot kind");
        check(!slot.header.variables.empty(), "real slot variables");
        check(!slot.image_layers.empty(), "real slot image layers");
        check(!slot.text_layers.empty(), "real slot text layers");
        check(!slot.frames.empty() && !slot.frames.front().script.empty(),
              "real slot has a script frame");
        // 帧 = 头 + 待执行指令 + 当前指令，并且必须一帧不差地吃到字段末尾。
        check(!slot.frames_truncated, "real slot frames parse exactly to the field end");
        check(slot.frames.front().has_current && !slot.frames.front().current.name.empty(),
              "real slot frame names the tag being executed");
        check(slot.has_system_frame && !slot.system_frame.script.empty(),
              "real slot system frame");
        // 文本排版矩形：message 层必须有框（否则读档后文字会落回默认位置）。
        check(!slot.text_rects.empty(), "real slot carries text geometry");
        check(slot.text_rects.count(slot.current_message_layer) == 1,
              "real slot message layer has a text box");
        // BGM 只在存档里真有 splay 时才有：有的档（test13）根本不带字段 29。
        if (!slot.bgm.empty()) {
            check(!slot.bgm.front().attrs.at("file").empty(), "real slot bgm names a file");
        }
        const auto save = oa::runtime::SaveData::decode(slot_doc);
        check(!save.current_script.empty(), "real slot imports a position");
        check(!save.layers.empty(), "real slot imports layers");
        size_t boxes = 0;
        for (const auto& t : save.text_layers) boxes += t.positioned ? 1 : 0;
        check(boxes > 0, "real slot imports text box geometry");
        // 事件的"回调参数"必须一起搬：官方把 function/lua/key… 放在 attrs 里，
        // 只搬 type/handler 的旧实现会让点击落到一个空回调上。
        size_t handlers = 0, with_params = 0, with_handler = 0;
        for (const auto& l : save.layers) {
            for (const auto& h : l.handlers) {
                ++handlers;
                if (!h.params.empty()) ++with_params;
                if (!h.handler.empty()) ++with_handler;
            }
        }
        check(handlers > 0, "real slot imports lyevent rows");
        check(with_handler > 0, "real slot handler rows keep their handler kind");
        check(with_params > 0, "real slot handler rows keep their callback params");
        size_t stack = 0;
        for (const auto& f : slot.frames) {
            (void)f;
            ++stack;
        }
        std::printf("official_save_test: %s → %s:%zu, %zu var(s), %zu layer(s), %zu text "
                    "layer(s)/%zu box(es), %zu frame(s), bgm %s\n",
                    slot_name, save.current_script.c_str(), save.current_line,
                    save.local_variables.size(), save.layers.size(), save.text_layers.size(),
                    boxes, stack,
                    save.has_audio ? save.audio.bgm->file.c_str() : "(none)");
        break; // 一份就够（同目录里通常只有一个槽位夹具）
    }

    std::printf("official_save_test: real fixtures ok (%zu s. keys, %zu g. keys, "
                "%zu scripts / %zu read-lines)\n",
                system.variables.size(), global.variables.size(), global.read_lines.size(),
                rows);
}

/// 真实 g.config → 本引擎的 pluto.unpersist：设置表必须读得出来。
/// 回归 test4「进游戏还要选语言」：官方把设置表（含 language/ui_lang）存成
/// native Pluto 的二进制，导入时若原样搬运，本引擎的 unpersist 会**静默**
/// 返回空表 → conf.language 丢失 → 每次启动都判定首次运行。
void test_real_gconfig_unpersist() {
    const char* dir = std::getenv("OA_TEST_OFFICIAL_SAVE_DIR");
    if (!dir || !*dir) return;
    std::string bytes;
    {
        std::FILE* f = std::fopen((std::string(dir) + "/saveg.dat").c_str(), "rb");
        if (!f) return;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        bytes.resize(size_t(n));
        const size_t got = n ? std::fread(bytes.data(), 1, size_t(n), f) : 0;
        std::fclose(f);
        bytes.resize(got);
    }
    if (bytes.empty()) return;
    const auto doc = oa::runtime::decode_official_save(bytes);
    std::map<std::string, oa::runtime::Value> vars;
    oa::runtime::import_official_variables(
        doc, [&vars](const std::string& k, oa::runtime::Value v) { vars[k] = std::move(v); });
    const auto it = vars.find("g.config");
    if (it == vars.end()) {
        std::printf("official_save_test: saveg.dat carries no g.config; config probe skipped\n");
        return;
    }
    oa::runtime::LuaBridge eng(oa::runtime::LuaHost{});
    eng.run_code(R"(
function probe_gconfig(e, p)
  local t = pluto.unpersist({}, p.blob)
  assert(type(t) == 'table', 'unpersist returns a table')
  assert(next(t) ~= nil, 'config table is not empty')
  if t.language ~= nil then
    assert(type(t.language) == 'string' and #t.language > 0, 'main language is a string')
  end
  -- 多语言发行版（有 ui_lang）一定会把主语言一起存下来；这两者都读出来，
  -- lang_init 才不会按"首次启动"再问一遍。
  if t.ui_lang ~= nil then
    assert(t.language ~= nil, 'multilingual config keeps the main language')
  end
  return true
end)", "gconfig_probe");
    check(eng.call_function("probe_gconfig", {{"blob", it->second.str_val}}),
          "real g.config unpersists into a non-empty config table");
    std::printf("official_save_test: real g.config → %zu-byte engine pluto doc, "
                "unpersist ok\n",
                it->second.str_val.size());
}

// --- 端到端：真的走 sysload / syssave -------------------------------------
struct MemFs : oa::fs::IFileSystem {
    std::map<std::string, std::string> files;
    std::optional<std::vector<uint8_t>> read(std::string_view path) const override {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return std::nullopt;
        return std::vector<uint8_t>(it->second.begin(), it->second.end());
    }
    bool exists(std::string_view path) const override {
        return files.count(std::string(path)) > 0;
    }
    const char* kind() const override { return "mem"; }
};

class MemSaveStore final : public oa::runtime::SaveStore {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    bool write(const std::string& rel, const std::vector<uint8_t>& d) override {
        files[rel] = d;
        return true;
    }
    std::optional<std::vector<uint8_t>> read(const std::string& rel) const override {
        const auto it = files.find(rel);
        if (it == files.end()) return std::nullopt;
        return it->second;
    }
    bool remove(const std::string& rel) override { return files.erase(rel) > 0; }
    bool exists(const std::string& rel) const override { return files.count(rel) > 0; }
};

std::string as_bytes(const std::vector<uint8_t>& v) {
    return std::string(v.begin(), v.end());
}

/// 官方 savedata 落到存档根 → boot 必须把 s./g. 灌进来，覆盖存档时官方原件
/// 必须被备份成 `<file>.official.bak`，主文件变成 OASB。
void test_sysload_end_to_end() {
    const auto sys_doc = bowx({
        {"s.bgmvol", "80"},
        {"s.status.automode", "0"},
        {"s.datapath", "C:\\Users\\someone\\elsewhere"},
    });
    const std::string pluto_blob("\x01\x00\x00\x00\x02\x00\x00\x00\x04\x00", 10);
    const auto g_doc = bowg({{"g.script", pluto_blob}, {"g.reset", "0"}},
                            {{"system/first.iet", {2, 1, 0}}});

    auto store = std::make_shared<MemSaveStore>();
    store->files["save/system.dat"] = {sys_doc.begin(), sys_doc.end()};
    store->files["save/saveg.dat"] = {g_doc.begin(), g_doc.end()};

    auto fs = std::make_shared<MemFs>();
    fs->files["system.ini"] =
        "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
        "BOOT = system/first.iet\nSAVEPATH = save\n";
    fs->files["system/first.iet"] = "*main\n[stop]\n";

    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();

    const auto& vars = rt.interpreter().variables();
    const auto bgmvol = vars.get("s.bgmvol");
    check(bgmvol.has_value(), "sysload imported s.bgmvol");
    check(bgmvol && bgmvol->kind == oa::runtime::ValueKind::Int && bgmvol->int_val == 80,
          "s.bgmvol imported as int 80");
    const auto automode = vars.get("s.status.automode");
    check(automode && automode->kind == oa::runtime::ValueKind::Int &&
              automode->int_val == 0 && !automode->to_bool(),
          "\"0\" imported as falsy int (not a truthy string)");
    check(!vars.get("s.datapath").has_value(), "host path key not imported");
    const auto gscript = vars.get("g.script");
    check(gscript && gscript->kind == oa::runtime::ValueKind::String &&
              gscript->str_val == pluto_blob,
          "g.* value imported byte-exactly (Pluto blob untouched)");

    // 第一次 syssave：官方原件必须留一份，主文件必须换成本引擎的 OASB。
    check(rt.syssave(), "syssave succeeds over an official save");
    check(store->files.count("save/system.dat.official.bak") == 1,
          "official system.dat backed up before overwrite");
    check(store->files.count("save/saveg.dat.official.bak") == 1,
          "official saveg.dat backed up before overwrite");
    check(!oa::runtime::is_official_save(as_bytes(store->files.at("save/system.dat"))),
          "system.dat rewritten in the engine's own format");
    const auto backup = as_bytes(store->files.at("save/system.dat.official.bak"));
    check(oa::runtime::is_official_save(backup), "backup still decodes as an official save");
    check(oa::runtime::decode_official_save(backup).variables.size() == 3,
          "backup keeps all official keys");

    // 第二次 syssave 不许再覆盖备份（只保第一份原件）。
    const auto saved_backup = store->files.at("save/system.dat.official.bak");
    rt.interpreter().set_variable("s.bgmvol", oa::runtime::Value::make_int(10));
    check(rt.syssave(), "second syssave succeeds");
    check(store->files.at("save/system.dat.official.bak") == saved_backup,
          "second syssave leaves the backup untouched");
}

/// BOWS 槽位夹具：变量 bank + 字段 26/24/25/29/11/4/3。
struct SlotParts {
    std::map<std::string, std::string> vars;
    std::string image_layers;  // 字段 26 的已编码命令栈
    std::string text_layers;   // 字段 24
    std::string text_rects;    // 字段 25（排版矩形）
    std::string bgm;           // 字段 29（单条指令）
    std::string msg_layer;     // 字段 11（裸字符串）
    std::string frames;        // 字段 4（count + frames）
    std::string system_frame;  // 字段 3
};

void put_i32(std::string& out, int32_t v) { put_u32(out, uint32_t(v)); }

std::string cmd_bytes(int32_t line, const std::string& name,
                      const std::vector<std::pair<std::string, std::string>>& attrs) {
    std::string out;
    put_i32(out, line);
    put_string(out, name);
    put_u32(out, uint32_t(attrs.size()));
    for (const auto& [k, v] : attrs) {
        put_string(out, k);
        put_string(out, v);
    }
    return out;
}

std::string command_map(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& layers) {
    std::string out;
    put_u32(out, uint32_t(layers.size()));
    for (const auto& [id, cmds] : layers) {
        put_string(out, id);
        put_u32(out, uint32_t(cmds.size()));
        for (const auto& c : cmds) out += c;
    }
    return out;
}

std::string frame_bytes(uint32_t index, uint32_t ret, const std::string& script,
                        uint32_t line, const std::vector<std::string>& pending,
                        const std::string& current) {
    std::string out;
    put_u32(out, index);
    put_u32(out, ret);
    put_string(out, script);
    put_u32(out, line);
    put_u32(out, uint32_t(pending.size()));
    for (const auto& c : pending) out += c;
    out += current; // 存档瞬间正在执行的那条（官方一定写）
    return out;
}

/// 字段 25（文本排版状态）：u32 count, × (string id, u32 n_groups, 组)。
/// 组 = u32 类型 + 4 个 f32 矩形 + "尾巴"；尾巴不需要解，这里塞几个字节
/// 证明解析器不依赖它。
std::string text_geometry(const std::vector<std::pair<std::string, std::array<float, 4>>>& rows) {
    std::string out;
    put_u32(out, uint32_t(rows.size()));
    for (const auto& [id, rect] : rows) {
        put_string(out, id);
        put_u32(out, 1); // 排版组数
        put_u32(out, 2); // 组类型（实测恒为 2）
        for (float v : rect) {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            put_u32(out, bits);
        }
        put_u32(out, 1); // 尾巴：真实存档里是字号/字面/颜色…，解析器不读
        put_u32(out, 0);
    }
    return out;
}

std::string bows_slot(const SlotParts& parts) {
    std::vector<std::pair<std::string, std::string>> rows(parts.vars.begin(), parts.vars.end());
    std::string data(4, '\0'); // bank 起点 ≠ 0（root 也占 0）
    const uint32_t bank = uint32_t(data.size());
    data += string_map(rows);
    std::map<uint32_t, std::map<uint32_t, uint32_t>> objects;
    auto add = [&](uint32_t field, const std::string& block) {
        objects[0][field] = uint32_t(data.size());
        data += block;
    };
    add(26, parts.image_layers);
    add(24, parts.text_layers);
    add(25, parts.text_rects);
    add(29, parts.bgm);
    add(11, parts.msg_layer);
    add(4, parts.frames);
    add(3, parts.system_frame);
    objects[0][30] = bank;
    objects[bank] = {{2, bank}};
    return build_container("BOWS", 1003, data, objects);
}

/// 官方 Pluto 表：[1][ref][5][0] + 元表 nil + ("k"=1.0, "s"=true) + nil。
std::string official_pluto_table() {
    std::string out;
    put_u32(out, 1);
    put_u32(out, 1); // ref 1
    put_u32(out, 5); // table
    put_u32(out, 0); // persist flag
    put_u32(out, 0); // 元表：nil
    put_u32(out, 0);
    put_u32(out, 1); // key: ref 2, string
    put_u32(out, 2);
    put_u32(out, 4);
    put_u32(out, 1); // 长度（word=8 的低 4 字节）
    put_u32(out, 0); // 长度高 4 字节
    out += "k";
    put_u32(out, 1); // value: ref 3, number 1.0
    put_u32(out, 3);
    put_u32(out, 3);
    const double one = 1.0;
    std::string bits(sizeof(one), '\0');
    std::memcpy(bits.data(), &one, sizeof(one));
    out += bits;
    put_u32(out, 1); // key: ref 4, "s"
    put_u32(out, 4);
    put_u32(out, 4);
    put_u32(out, 1);
    put_u32(out, 0);
    out += "s";
    put_u32(out, 1); // value: ref 5, bool true
    put_u32(out, 5);
    put_u32(out, 1);
    put_u32(out, 1);
    put_u32(out, 0); // 表结束（nil key）
    put_u32(out, 0);
    return out;
}

void test_bows_slot() {
    SlotParts parts;
    parts.vars = {
        {"game.ver", "1.0.1"},
        {"lua.scr", official_pluto_table()},
        {"hide", "0"},
    };
    parts.image_layers = command_map({
        {"bg", {cmd_bytes(-1, "lyc", {{"file", ":bg/room"}, {"id", "bg"}}),
                cmd_bytes(-1, "lyprop", {{"alpha", "255"}, {"top", "0"}})}},
        {"fg", {cmd_bytes(-1, "lyc", {{"file", ":fg/girl"}}),
                cmd_bytes(-1, "lyevent", {{"type", "click"}, {"mode", "disable"},
                                          {"handler", "calllua"},
                                          {"function", "on_click"},
                                          {"key", "13"}})}},
    });
    parts.text_layers = command_map({
        {"1.80.mw.adv_adv", {cmd_bytes(-1, "font", {{"size", "40"}, {"color", "FFFFFF"}}),
                             cmd_bytes(-1, "print", {{"data", "hello"}}),
                             cmd_bytes(-1, "rt", {{"omitblankline", "1"}})}},
    });
    parts.text_rects = text_geometry({{"1.80.mw.adv_adv", {10.0f, 20.0f, 300.0f, 40.0f}}});
    parts.bgm = cmd_bytes(-1, "splay",
                          {{"file", ":bgm/a.ogg"}, {"gain", "800"}, {"loop", "1"}, {"pan", "0"}});
    parts.msg_layer = [] {
        std::string s;
        put_string(s, "1.80.mw.adv_adv");
        return s;
    }();
    parts.frames = [] {
        std::string s;
        put_u32(s, 2); // 2 帧：当前 + 调用者
        s += frame_bytes(1, 7, "story.iet", 12,
                         {cmd_bytes(-1, "calllua", {{"function", "x"}})},
                         cmd_bytes(-1, "print", {{"data", "x"}}));
        s += frame_bytes(0, 3, "other.iet", 5, {}, cmd_bytes(-1, "click", {}));
        return s;
    }();
    parts.system_frame =
        frame_bytes(0, 4, "system/first.iet", 2, {}, cmd_bytes(-1, "stop", {}));

    const std::string doc = bows_slot(parts);
    const auto slot = oa::runtime::decode_official_slot(doc);
    check(slot.header.kind == oa::runtime::OfficialSaveKind::Slot, "BOWS slot kind");
    check(slot.header.variables.size() == 3, "BOWS slot variables");
    check(slot.image_layers.size() == 2, "BOWS image layer count");
    check(slot.text_layers.size() == 1, "BOWS text layer count");
    check(slot.frames.size() == 2, "BOWS frame count");
    check(!slot.frames_truncated, "BOWS frames fully parsed");
    check(slot.frames.front().script == "story.iet" && slot.frames.front().line == 12,
          "BOWS current frame");
    check(slot.frames.front().pending.size() == 1 &&
              slot.frames.front().pending.front().name == "calllua",
          "BOWS pending tags parsed");
    check(slot.frames.front().has_current && slot.frames.front().current.name == "print",
          "BOWS current tag parsed");
    check(slot.frames.back().has_current && slot.frames.back().current.name == "click",
          "BOWS second frame current tag parsed");
    check(slot.has_system_frame && slot.system_frame.script == "system/first.iet",
          "BOWS system frame");
    check(slot.system_frame.has_current && slot.system_frame.current.name == "stop",
          "BOWS system frame current tag");
    check(slot.text_rects.count("1.80.mw.adv_adv") == 1 &&
              slot.text_rects.at("1.80.mw.adv_adv").width == 300.0f &&
              slot.text_rects.at("1.80.mw.adv_adv").top == 20.0f,
          "BOWS text geometry parsed");
    check(slot.current_message_layer == "1.80.mw.adv_adv", "BOWS current message layer");
    check(!slot.bgm.empty() && slot.bgm.front().attrs.at("file") == ":bgm/a.ogg",
          "BOWS bgm command");

    // 槽位 → SaveData 的映射
    const auto data = oa::runtime::SaveData::decode(doc);
    check(data.current_script == "story.iet" && data.current_line == 12,
          "slot position imported");
    check(data.call_stack.size() == 1 && data.call_stack.front().first == "other.iet" &&
              data.call_stack.front().second == 7,
          "slot call stack uses the caller's return line");
    check(data.local_variables.size() == 3, "slot variables imported");
    check(data.local_variables.at("game.ver").str_val == "1.0.1", "string variable kept");
    check(data.local_variables.at("lua.scr").str_val.rfind("OASB", 0) == 0,
          "official Pluto table converted to the engine's pluto doc");
    check(data.has_scene && data.layers.size() == 2, "slot layers imported");
    check(data.layers.front().props.at("file") == ":bg/room", "lyc file prop");
    check(data.layers.front().props.at("alpha") == "255", "lyprop merged over lyc");
    check(data.layers.front().props.count("id") == 0, "layer id is not a prop");
    check(data.layers.back().handlers.size() == 1 &&
              data.layers.back().handlers.front().type == "click" &&
              !data.layers.back().handlers.front().enabled &&
              data.layers.back().handlers.front().handler == "calllua" &&
              data.layers.back().handlers.front().params.at("function") == "on_click" &&
              data.layers.back().handlers.front().params.at("key") == "13" &&
              data.layers.back().handlers.front().filter_params.at("id") == "fg",
          "lyevent → handler row (known fields to row, the rest to params)");
    check(data.has_text && data.text_layers.size() == 1, "slot text layers imported");
    check(data.text_layers.front().text == "hello", "print data → text page");
    check(data.text_layers.front().active, "current message layer is active");
    check(data.text_layers.front().positioned && data.text_layers.front().left == 10.0 &&
              data.text_layers.front().width == 300.0,
          "text box geometry imported from field 25");
    check(data.has_audio && data.audio.bgm && data.audio.bgm->file == ":bgm/a.ogg",
          "slot bgm imported");
    check(data.audio.bgm->gain == 800 && data.audio.bgm->loop_play, "bgm gain/loop");
}

void test_pluto_conversion() {
    const auto doc = oa::runtime::convert_official_pluto(official_pluto_table());
    check(doc.has_value(), "official Pluto table converts");
    if (doc) {
        check(doc->rfind("OASB", 0) == 0, "converted doc has the engine header");
        oa::util::Reader r(*doc);
        check(r.map_entries() == 2, "converted doc is a 2-entry map");
    }
    check(!oa::runtime::convert_official_pluto("not a pluto value").has_value(),
          "garbage does not convert");
    check(!oa::runtime::convert_official_pluto("").has_value(), "empty does not convert");
    // 顶层不是表：游戏只 unpersist 表，不转换。
    std::string scalar;
    put_u32(scalar, 1);
    put_u32(scalar, 1);
    put_u32(scalar, 3); // number
    const double two = 2.0;
    std::string bits(sizeof(two), '\0');
    std::memcpy(bits.data(), &two, sizeof(two));
    scalar += bits;
    check(!oa::runtime::convert_official_pluto(scalar).has_value(),
          "scalar Pluto value is not converted");
}

} // namespace

int main() {
    test_bowx_roundtrip();
    test_bowg();
    test_bows();
    test_bows_slot();
    test_pluto_conversion();
    test_rejects();
    test_value_inference();
    test_host_paths_and_import();
    test_sysload_end_to_end();
    test_real_fixture();
    test_real_gconfig_unpersist();
    if (failures) {
        std::fprintf(stderr, "official_save_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("official_save_test: ok\n");
    return 0;
}
