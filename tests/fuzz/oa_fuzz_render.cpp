// 渲染 fuzz：随机图层树 → 帧计划 →（能建窗口就）真栅格化。
//
// 覆盖的是渲染主干：层树物化/祖先自建、属性解析（含非法值/边界值）、组作用域、
// clip 与 mask 合成、变换/不透明度链、帧计划构建与栅格化。
//
// 计划必须始终自洽：GroupBegin/GroupEnd 成对且中途深度不为负、绘制项坐标有限。
// 不满足就直接 abort —— 暴力模式下等于"发现 bug"，libFuzzer 下会记成崩溃样本。
//
// 窗口用隐藏窗口；建不出来（无显示/驱动缺失）就退化成只跑计划构建，仍然有效。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <SDL3/SDL.h>

#include "core/fs/fs.h"
#include "core/render/layer.h"
#include "core/render/render_host.h"
#include "core/render/renderer.h"

namespace {

class EmptyFs : public oa::fs::IFileSystem {
public:
    std::optional<std::vector<uint8_t>> read(std::string_view) const override {
        return std::nullopt;
    }
    const char* kind() const override { return "fuzz"; }
};

class FuzzHost : public oa::render::RenderHost {
public:
    int stage_width() const override { return 1280; }
    int stage_height() const override { return 720; }
    std::string resolve_magic_path(const std::string& name) const override { return name; }
    std::optional<std::string> host_variable(const std::string&) const override {
        return std::nullopt;
    }
    std::optional<std::vector<uint8_t>> read_save_file(const std::string&) const override {
        return std::nullopt;
    }
    uint64_t now_ms() const override { return 0; }
    void post_frame_capture() override {}
    void transition_begin(const std::map<std::string, std::string>&) override {}
    void bind_text_metrics(std::function<std::tuple<double, double, double>()>) override {}
};

// 进程内只建一次渲染后端：窗口/SDL 初始化代价大，每轮重建会让 fuzz 退化。
struct Backend {
    EmptyFs fs;
    FuzzHost host;
    std::unique_ptr<oa::render::RenderEngine> engine;
    bool tried = false;

    oa::render::RenderEngine* get() {
        if (tried) return engine.get();
        tried = true;
        if (!SDL_Init(SDL_INIT_VIDEO)) return nullptr;
        SDL_Window* win = SDL_CreateWindow("oa-fuzz-render", host.stage_width(),
                                           host.stage_height(), SDL_WINDOW_HIDDEN);
        if (!win) return nullptr;
        engine = std::make_unique<oa::render::RenderEngine>(&fs, &host);
        if (!engine->create_renderer(win, "sdl")) {
            engine.reset();
            return nullptr;
        }
        return engine.get();
    }
};

Backend& backend() {
    static Backend b; // 故意不析构：进程退出时 SDL 已关，反而省掉顺序问题
    return b;
}

struct Rng {
    uint64_t s = 0x243F6A8885A308D3ull;
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    int in(int lo, int hi) { return lo + int(next() % uint64_t(hi - lo + 1)); }
    bool chance(int percent) { return int(next() % 100) < percent; }
};

// 属性值刻意混入非法/边界文本：解析器必须自己扛住。
const char* kNumTokens[] = {"0",   "1",     "-1",   "1280", "720",  "0.5",
                            "-3",  "1e9",   "nan",  "inf",  "",     "abc",
                            "999999999999999999999", "-0.0", "255"};
const char* kBoolTokens[] = {"1", "0", "true", "false", "", "yes", "-1"};
const char* kColors[] = {"#ff00ff00", "#80ffffff", "#00000000", "#zzzzzz", "", "ffffffff",
                         "#ffffffff00"};

const char* pick(Rng& r, const char* const* table, size_t n) {
    return table[r.next() % n];
}
template <size_t N>
const char* pick(Rng& r, const char* const (&table)[N]) {
    return table[r.next() % N];
}

void build_scene(oa::render::Compositor& scene, Rng& r) {
    // id 带层级：走 ensure_path 的祖先自建与子树裁剪。
    static const char* kIds[] = {"a",   "b",     "c",      "a.b", "a.b.c",
                                 "d",   "d.e.f", "bg",     "fg.face", "g1.g2.g3.g4"};
    const int layers = 1 + r.in(0, 9);
    for (int i = 0; i < layers; ++i) {
        const std::string id = pick(r, kIds);
        std::map<std::string, std::string> props;
        if (r.chance(80)) props["x"] = pick(r, kNumTokens);
        if (r.chance(80)) props["y"] = pick(r, kNumTokens);
        if (r.chance(60)) props["width"] = pick(r, kNumTokens);
        if (r.chance(60)) props["height"] = pick(r, kNumTokens);
        if (r.chance(50)) props["anchorx"] = pick(r, kNumTokens);
        if (r.chance(50)) props["anchory"] = pick(r, kNumTokens);
        if (r.chance(45)) props["xscale"] = pick(r, kNumTokens);
        if (r.chance(45)) props["yscale"] = pick(r, kNumTokens);
        if (r.chance(25)) props["zoom"] = pick(r, kNumTokens);
        if (r.chance(45)) props["rotate"] = pick(r, kNumTokens);
        if (r.chance(20)) props["reversex"] = pick(r, kBoolTokens);
        if (r.chance(20)) props["reversey"] = pick(r, kBoolTokens);
        if (r.chance(55)) props["alpha"] = pick(r, kNumTokens);
        if (r.chance(25)) props["visible"] = pick(r, kBoolTokens);
        if (r.chance(30)) {
            // clip 的正常写法 + 越界/负尺寸/坏格式各来一些
            static const char* kClips[] = {"0,0,1280,720", "10,10,100,100", "-50,-50,40,40",
                                           "0,0,0,0", "5,5,-20,-20", "", "1,2", "a,b,c,d"};
            props["clip"] = pick(r, kClips);
        }
        if (r.chance(25)) props["color"] = pick(r, kColors);
        if (r.chance(20)) props["mask"] = r.chance(50) ? "" : "pc/x/mask.png";
        if (r.chance(15)) props["blend"] = r.chance(50) ? "add" : "multiply";
        if (r.chance(10)) props["intermediate_render"] = "1";
        scene.create(id, props);
        if (r.chance(30)) {
            std::map<std::string, std::string> more;
            more["x"] = pick(r, kNumTokens);
            more["alpha"] = pick(r, kNumTokens);
            scene.set_props(id, more); // lyprop 增量合并
        }
        if (r.chance(8)) scene.remove(id); // 子树裁剪
    }
    if (r.chance(20)) scene.clear_scene(); // 清场后下一轮重建
}

// 计划自洽性：组必须成对、深度不为负、绘制项坐标有限。
void check_plan(const oa::render::RenderPlan& plan) {
    int depth = 0;
    for (const auto& item : plan.items()) {
        if (item.op == oa::render::PlanOp::GroupBegin) {
            ++depth;
        } else if (item.op == oa::render::PlanOp::GroupEnd) {
            if (--depth < 0) {
                std::fprintf(stderr, "[fuzz] 计划不自洽：GroupEnd 早于 GroupBegin\n");
                std::abort();
            }
        } else {
            const double vals[] = {item.world.a, item.world.b, item.world.c,
                                   item.world.d, item.world.e, item.world.f};
            for (double v : vals) {
                if (!std::isfinite(v)) {
                    std::fprintf(stderr, "[fuzz] 计划不自洽：变换含非有限值\n");
                    std::abort();
                }
            }
        }
    }
    if (depth != 0) {
        std::fprintf(stderr, "[fuzz] 计划不自洽：组没有闭合（depth=%d）\n", depth);
        std::abort();
    }
}

void run_one(const uint8_t* data, size_t size) {
    Rng r;
    r.s = 0x243F6A8885A308D3ull ^ (uint64_t(size) * 0x9E3779B97F4A7C15ull);
    for (size_t i = 0; i < size; ++i) {
        r.s ^= uint64_t(data[i]);
        r.s *= 0x100000001B3ull; // FNV-1a
        r.s = (r.s << 17) | (r.s >> 47);
    }

    oa::render::Compositor scene;
    build_scene(scene, r);
    const double fade = r.chance(30) ? 0.0 : (r.chance(30) ? 0.5 : 1.0);

    if (auto* engine = backend().get()) {
        engine->draw_scene(scene, fade); // 计划 + 栅格化（含组离屏合成）
        engine->render_frame(uint64_t(r.next() % 1000), fade);
    } else {
        oa::render::PlanInput in;
        in.scene = &scene;
        in.text = nullptr;
        in.sizes = nullptr;
        in.stage_w = 1280;
        in.stage_h = 720;
        in.global_fade = fade;
        oa::render::RenderPlan plan;
        oa::render::build_render_plan(in, &plan);
        check_plan(plan);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    try {
        run_one(data, size);
    } catch (...) {
        // 场景命令本身不抛；真抛了也是正常失败路径。
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
