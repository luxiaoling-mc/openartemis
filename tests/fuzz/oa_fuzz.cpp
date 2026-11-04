// 解析面模糊测试。一个源文件，编译期用 OA_FUZZ_<TARGET> 选择目标：
//
//   psb     E-mote PSB 容器（表/对象/资源区，越界读的高发区）
//   iet     剧本解析：文本脚本 + ASB 二进制解码（先 decode_asb 再 parse）
//   image   PNG/JPEG 解码 + PNG tEXt 扫描
//   save    官方存档容器（BOWX/BOWG/BOWS：zlib + 扁平字节区 + 尾部目录）
//
// 两种运行形态：
//
//   默认            本文件自带暴力变异循环（任何编译器都行，配 ASan 最好用）
//   OA_FUZZ_LIBFUZZER  只提供 LLVMFuzzerTestOneInput，main 由 libFuzzer 提供
//                   （clang -fsanitize=fuzzer，覆盖率引导；见 README）
//
// 这里只碰"纯解析"：不建 Interpreter 的 Lua 状态、不跑剧本。畸形输入抛异常
// 是设计内行为（解析器报告失败），所以每次调用都吞异常 —— 只有 ASan 报错、
// 断言、段错误才算发现。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#if defined(OA_FUZZ_PSB)
#include "core/emote/psb_reader.h"
#elif defined(OA_FUZZ_IET)
#include "core/runtime/runtime_iet.h"
#elif defined(OA_FUZZ_IMAGE)
#include "core/media/image.h"
#elif defined(OA_FUZZ_SAVE)
#include "core/runtime/runtime_save.h"
#include "core/runtime/runtime_save_official.h"
#else
#error "OA_FUZZ_<TARGET> 未定义：只能选一个目标（见 tests/fuzz/CMakeLists.txt）"
#endif

namespace {

// 一个输入的全部解析动作。目标内部自己处理"解不出来"（返回 false / 抛异常）。
void exercise(const uint8_t* data, size_t size) {
#if defined(OA_FUZZ_PSB)
    oa::emote::PsbReader reader;
    if (!reader.load(data, size, nullptr)) return;
    // 从根对象往下走一遍：读类型、标量、字符串、资源区、子对象/数组。
    std::vector<std::string> keys;
    std::vector<uint32_t> values;
    if (!reader.object_entries(reader.root_offset(), &keys, &values)) return;
    for (uint32_t off : values) {
        (void)reader.kind_at(off);
        int64_t i = 0;
        double d = 0.0;
        int32_t resource = 0;
        bool extra = false;
        (void)reader.read_int(off, &i);
        (void)reader.read_double(off, &d);
        (void)reader.read_string(off);
        if (reader.read_resource(off, &resource, &extra) && resource >= 0) {
            std::span<const uint8_t> bytes;
            (void)reader.chunk_bytes(size_t(resource), extra, &bytes);
        }
        std::vector<uint32_t> kids;
        (void)reader.list_items(off, &kids);
        (void)reader.array_values(off, &kids);
        std::vector<std::string> sub_keys;
        std::vector<uint32_t> sub_values;
        (void)reader.object_entries(off, &sub_keys, &sub_values);
    }
#elif defined(OA_FUZZ_IET)
    std::string text;
    const bool asb = size >= 4 && data[0] == 'A' && data[1] == 'S' && data[2] == 'B' &&
                     data[3] == 0;
    if (asb) {
        text = oa::runtime::decode_asb(std::vector<uint8_t>(data, data + size));
    } else {
        text.assign(reinterpret_cast<const char*>(data), size);
    }
    const oa::runtime::Script script = oa::runtime::Script::parse("fuzz.iet", text);
    // 让编译器别把整个结果优化掉，同时摸一下 label 表。
    volatile size_t sink = script.labels.size() + script.instructions.size();
    (void)sink;
#elif defined(OA_FUZZ_IMAGE)
    const std::vector<uint8_t> bytes(data, data + size);
    oa::media::Image image;
    if (oa::media::decode_image(bytes, image)) {
        volatile int64_t sink = int64_t(image.w) * image.h + int64_t(image.rgba.size());
        (void)sink;
    }
    (void)oa::media::png_text_chunks(bytes);
#elif defined(OA_FUZZ_SAVE)
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    if (oa::runtime::is_official_save(bytes)) {
        // 官方（原生引擎）容器：zlib + 扁平字节区 + 尾部目录。
        const oa::runtime::OfficialSave save = oa::runtime::decode_official_save(bytes);
        volatile size_t sink = save.variables.size() + save.read_lines.size() +
                               save.field_ids.size();
        (void)sink;
    } else if (size >= 4 && bytes.substr(0, 4) == "OASB") {
        // 本引擎自己的存档文档：两种解码面都走一遍。
        const oa::runtime::SaveData save = oa::runtime::SaveData::decode(std::string(bytes));
        volatile size_t sink = save.local_variables.size() + save.layers.size();
        (void)sink;
        volatile size_t sink2 = oa::runtime::decode_domain_map(std::string(bytes)).size();
        (void)sink2;
    }
#endif
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    try {
        exercise(data, size);
    } catch (...) {
        // 畸形输入：解析器用什么方式报告失败都算正常（见文件头）。
    }
    return 0;
}

#if !defined(OA_FUZZ_LIBFUZZER)
#include "fuzz_loop.h"

int main(int argc, char** argv) {
    return oa::fuzz::brute_loop(argc, argv, [](const uint8_t* data, size_t size) {
        LLVMFuzzerTestOneInput(data, size);
    });
}
#endif // !OA_FUZZ_LIBFUZZER
