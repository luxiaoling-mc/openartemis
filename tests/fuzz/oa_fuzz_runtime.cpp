// 运行时 fuzz：把输入当剧本喂给**真的** GameRuntime（不是只跑解析器）。
//
// 合成一个最小工程（system.ini + system/first.iet），boot 之后随机点/按键 tick
// 若干帧 —— 覆盖解释执行、wait 状态机、事件派发、层命令、Lua 桥。异常一律吞掉
// （畸形剧本报 ScriptError 是正常行为），只有 ASan 报告/断言/信号才算发现。
//
// 每轮都新建一个运行时：状态隔离比复用快更重要 —— 否则上一轮留下的层树/变量
// 会让问题不可复现。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/fs/fs.h"
#include "core/runtime/runtime_game.h"

namespace {

class MapFs : public oa::fs::IFileSystem {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    std::optional<std::vector<uint8_t>> read(std::string_view path) const override {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return std::nullopt;
        return it->second;
    }
    const char* kind() const override { return "fuzz"; }
};

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

// 每帧的输入也从同一份字节里派生：同一份输入 ⇒ 同一条轨迹（可复现）。
oa::runtime::FrameInput input_for(uint64_t seed, int frame, int stage_w, int stage_h) {
    oa::runtime::FrameInput in;
    uint64_t h = mix(seed, uint64_t(frame));
    in.mouse_x = int(h % uint64_t(stage_w > 0 ? stage_w : 1280));
    in.mouse_y = int((h >> 20) % uint64_t(stage_h > 0 ? stage_h : 720));
    if ((h >> 40) % 4 == 0) {
        in.left_click_edge = true;
        in.left_down = true;
    }
    if ((h >> 44) % 16 == 0) in.right_click_edge = true;
    if ((h >> 48) % 8 == 0) {
        static const int kKeys[] = {13, 27, 32, 17, 65, 83, 87, 37, 38, 39, 40, 33, 34};
        const int key = kKeys[(h >> 52) % (sizeof(kKeys) / sizeof(kKeys[0]))];
        in.key_down_edges.push_back(key);
        in.keys_down.insert(key);
    }
    return in;
}

void run_one(const uint8_t* data, size_t size) {
    auto fs = std::make_shared<MapFs>();
    fs->files["system.ini"] = std::vector<uint8_t>{
        // 最小可启动工程：只有 WIDTH/HEIGHT/BOOT 是必需的。
        '[', 'W', 'I', 'N', 'D', 'O', 'W', 'S', ']', '\n', 'W', 'I', 'D', 'T', 'H', '=', '1',
        '2', '8', '0', '\n', 'H', 'E', 'I', 'G', 'H', 'T', '=', '7', '2', '0', '\n', 'B', 'O',
        'O', 'T', '=', 's', 'y', 's', 't', 'e', 'm', '/', 'f', 'i', 'r', 's', 't', '.', 'i',
        'e', 't', '\n'};
    fs->files["system/first.iet"] = std::vector<uint8_t>(data, data + size);

    uint64_t seed = 0x243F6A8885A308D3ull;
    for (size_t i = 0; i < size; ++i) seed = mix(seed, data[i]);

    try {
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        rt.boot_project();
        const int ticks = 24 + int(seed % 24);
        for (int i = 0; i < ticks && !rt.exit_requested(); ++i) {
            rt.tick(16, input_for(seed, i, rt.project().config.stage_width,
                                  rt.project().config.stage_height));
        }
    } catch (...) {
        // 剧本/工程畸形：运行时用异常报告失败，属正常路径。
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    run_one(data, size);
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
