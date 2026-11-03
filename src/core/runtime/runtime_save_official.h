#pragma once
// 官方（原生 Artemis SDK / 发行版 exe）存档容器：BOWX / BOWG / BOWS。 与本引擎自己的 OASB 文档是**两套完全不同的东西**：官方容器 = 12 字节头 + 一段 zlib 流，解压后的负载不是结构化文档，而是「扁平字节区 + 尾部目录」：
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/runtime/runtime_iet.h" // oa::runtime::Value

namespace oa::runtime {

enum class OfficialSaveKind {
    System, ///< BOWX：s.* 域（音量/窗口/跳过等设置）
    Global, ///< BOWG：g.* 域 + 已读行
    Slot,   ///< BOWS：编号存档（变量 bank；图层命令栈尚未映射）
};

/// 官方存档容器解出来的内容（只读）。
struct OfficialSave {
    OfficialSaveKind kind = OfficialSaveKind::System;
    uint32_t version = 0; ///< 容器头里的版本（1002/1003…）
    /// BOWX/BOWG/BOWS 的变量表：键含域前缀（"s.bgmvol" / "g.script" /
    /// 编号存档里则是 "lua.scr" 这类本地名），值保持**原样字节**。
    std::map<std::string, std::string> variables;
    /// 仅 BOWG：脚本 → 已读行号集合（实测大致递减，即"访问过的行"）。
    std::map<std::string, std::vector<uint32_t>> read_lines;
    /// 诊断用：object_start → 该 object 的字段 id 列表（升序）。BOWS 的
    /// 图层命令栈就是靠这张表定位的（field 24/25/26/28…），导入器不用它。
    std::map<uint32_t, std::vector<uint32_t>> field_ids;
};

/// 前 4 字节是否为 BOWX / BOWG / BOWS。
bool is_official_save(std::string_view bytes);

/// 解一个官方容器；结构不合法一律抛 oa::util::FormatError
/// （截断/魔数不符/zlib 失败/raw_len 不符/目录越界/字段越界/条目数超限）。
OfficialSave decode_official_save(std::string_view bytes);

// ---------------------------------------------------------------------------
// BOWS 编号存档：官方把"现场"存成**每图层的命令栈**（不是脚本位置）
// ---------------------------------------------------------------------------
/// 一条被保存下来的指令：源码行号 + 标签名 + 属性（官方是 name → value）。
struct OfficialCommand {
    int32_t line = -1;
    std::string name;
    std::map<std::string, std::string> attrs;
};

/// 一个图层（键 = 图层 id）在存档时刻的命令栈（按执行顺序）。
struct OfficialLayer {
    std::string id;
    std::vector<OfficialCommand> commands;
};

/// 一个脚本帧。官方按"内层在前"排列（frames[0] = 当前执行位置）。 字段 4 的实测形状（test4 Rev.3139 / test13 两代共 4 份真档逐字节对齐， 解析后恰好吃到字段末尾）： u32 index, u32 return_line, strin...
struct OfficialFrame {
    uint32_t index = 0;
    uint32_t return_line = 0;
    std::string script;
    uint32_t line = 0;
    std::vector<OfficialCommand> pending;
    OfficialCommand current;   ///< 存档瞬间正在执行的指令
    bool has_current = false;
};

/// 文本图层的排版矩形（BOWS 字段 25）。官方把"文字框"存在排版状态里，
/// 而 [font] 指令本身往往只带颜色（test4 就是这样）——不给这个框，读档后
/// 恢复出来的文本会落到默认位置。
struct OfficialRect {
    float left = 0, top = 0, width = 0, height = 0;
};

struct OfficialSlot {
    OfficialSave header;                     ///< 变量 bank（BOWS 字段 30）
    std::vector<OfficialLayer> image_layers; ///< 字段 26：lyc / lyprop / lyevent…
    std::vector<OfficialLayer> text_layers;  ///< 字段 24：font / print / rt…
    /// 字段 25：文本图层的排版矩形（键与 text_layers 的图层 id 同一套）。
    /// 只解每个图层的第一个矩形（官方按"排版组"存，第一组就是当前文字框）。
    std::map<std::string, OfficialRect> text_rects;
    std::vector<OfficialCommand> bgm;        ///< 字段 29：当前 BGM（0/1 条 splay）
    std::vector<OfficialFrame> frames;       ///< 字段 4：场景脚本栈
    OfficialFrame system_frame;              ///< 字段 3：系统脚本帧
    bool has_system_frame = false;
    std::string current_message_layer;       ///< 字段 11：当前消息图层 id
    /// 栈里后续帧形状不认识（只取已解部分）。实测 4 份真档都是 0；
    /// 留这条是为了"官方换形状"时不整体失败，也不假装完整。
    bool frames_truncated = false;
};

/// 解一个 BOWS 槽位。**尽力而为**：字段 26/24/25/29/11 严格解析（形状已知且
/// 自带定界），字段 4/3 逐帧解、某帧解不动就保留已解部分并记
/// `frames_truncated`（第一帧 = 当前位置，够恢复）。容器/目录/bank 层的不合法
/// 仍然抛 FormatError。
OfficialSlot decode_official_slot(std::string_view bytes);

/// 官方值里的 Pluto 表（native engine `pluto.persist` 的二进制）→ 本引擎
/// `pluto` 的文档格式（binary_stream 标签流）。失败（不是 Pluto 值 / 顶层不是
/// 表 / 用了不支持的 VM 类型）返回 nullopt —— 调用方应保留原始字节。
std::optional<std::string> convert_official_pluto(std::string_view raw);

// --------------------------------------------------------------------------- 导入到引擎变量域（sysload 用）...
Value infer_official_value(const std::string& raw);

/// 官方存档里**不可迁移**的键：它们是写档那台机器上的绝对路径
/// （"C:\Users\koichi\…"），照抄进来会把本机的存档/资源路径指到别处。
bool official_key_is_host_path(const std::string& key);

/// 官方 Pluto 文档的字节形状：u32 first(=1)、u32 ref(!=0)、u32 type(1..5)。
/// 用来在导入时**先判断再转换**，免得把所有二进制变量都当 Pluto 试一遍。
bool looks_like_official_pluto(const std::string& raw);

struct OfficialImportStats {
    size_t applied = 0;          ///< 实际写入的变量数
    size_t skipped_host_path = 0;///< 因 host path 规则跳过的条目
    size_t read_lines = 0;       ///< 已读行的总行数（当前引擎无已读状态存储）
    size_t pluto_converted = 0;  ///< 转成本引擎 pluto 文档的表变量数（g.config 等）
    size_t pluto_failed = 0;     ///< 看着像 Pluto 但转不动（保留原始字节）
};

/// 把 doc.variables 灌进 set()（通常是 interpreter_->set_variable）。
/// 键缺域前缀时按容器种类补（BOWX→"s." / BOWG→"g."），已有前缀则原样使用。
OfficialImportStats import_official_variables(
    const OfficialSave& doc,
    const std::function<void(const std::string&, Value)>& set);

} // namespace oa::runtime
