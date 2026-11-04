// 暴力驱动：把真实的游戏包塞进 GameRuntime，随机点击/右键/按键/移动，一直
// tick（可选每帧真的渲染一遍），用来在 ASan 下砸出崩溃。它不是 ctest ——
// 需要真包，跑多久、什么种子都由命令行给。
//
//   oa_monkey <root.pfs> [--frames N] [--seed S] [--render] [--trace FILE]
//
// 关键点：每走一帧先把这帧的输入写进 trace（默认 oa_monkey.trace）。ASan 报
// 错时进程直接死，trace 的最后一行就是复现用的输入序列。
//
// 渲染模式要 SDL 窗口（隐藏窗口即可）——引擎的视频/emote 纹理上传、组离屏
// 合成只有在真渲染时才会跑到，纯 headless 砸不到那部分。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/fs/physfs_fs.h"
#include "core/render/renderer.h"
#include "core/runtime/runtime_game.h"

namespace {

struct Action {
    std::string text;             // trace 用
    oa::runtime::FrameInput input; // 本帧输入
};

// 会在游戏里有含义的键：回车/ESC/空格/Ctrl(跳过)/A/S/W/方向键/翻页/F1..F8。
const int kKeys[] = {13, 27, 32, 17, 65, 83, 87, 37, 38, 39, 40, 33, 34,
                     112, 113, 114, 115, 116, 117, 118, 119};

const char* key_name(int vk) {
    switch (vk) {
        case 13: return "enter";
        case 27: return "esc";
        case 32: return "space";
        case 17: return "ctrl";
        case 37: return "left";
        case 38: return "up";
        case 39: return "right";
        case 40: return "down";
        case 33: return "pgup";
        case 34: return "pgdn";
        default:
            if (vk >= 112 && vk <= 119) return "fkey";
            return "char";
    }
}

class Monkey {
public:
    explicit Monkey(uint64_t seed) : rng_(seed) {}

    Action next(int stage_w, int stage_h) {
        Action a;
        a.input.mouse_x = last_x_;
        a.input.mouse_y = last_y_;
        char buf[128];

        // 移动：经常做（很多 UI 逻辑挂在 hover 上）。
        if (chance(0.45)) {
            last_x_ = int(rng_() % uint64_t(stage_w > 0 ? stage_w : 1280));
            last_y_ = int(rng_() % uint64_t(stage_h > 0 ? stage_h : 720));
            a.input.mouse_x = last_x_;
            a.input.mouse_y = last_y_;
            std::snprintf(buf, sizeof(buf), "move %d,%d", last_x_, last_y_);
            a.text = buf;
        }
        // 左键：边沿 + 保持按下若干帧（拖拽/连续推进都是这个形态）。
        if (chance(0.18)) {
            a.input.left_click_edge = true;
            a.input.left_down = true;
            hold_left_ = 1 + int(rng_() % 3);
            a.text += " click";
        } else if (hold_left_ > 0) {
            a.input.left_down = true;
            --hold_left_;
            a.text += " hold";
        }
        // 右键：取消/返回。
        if (chance(0.05)) {
            a.input.right_click_edge = true;
            a.text += " rclick";
        }
        // 键盘：按下后保持 1..6 帧再抬起来（有的分支等 keyup）。
        if (hold_key_ < 0 && chance(0.10)) {
            hold_key_ = kKeys[rng_() % (sizeof(kKeys) / sizeof(kKeys[0]))];
            hold_key_frames_ = 1 + int(rng_() % 6);
            a.input.key_down_edges.push_back(hold_key_);
            a.input.keys_down.insert(hold_key_);
            std::snprintf(buf, sizeof(buf), " key:%s(%d)", key_name(hold_key_), hold_key_);
            a.text += buf;
        } else if (hold_key_ >= 0) {
            a.input.keys_down.insert(hold_key_);
            if (--hold_key_frames_ <= 0) {
                a.input.key_up_edges.push_back(hold_key_);
                a.input.keys_down.erase(hold_key_);
                std::snprintf(buf, sizeof(buf), " keyup:%d", hold_key_);
                a.text += buf;
                hold_key_ = -1;
            }
        }
        if (a.text.empty()) a.text = "idle";
        return a;
    }

private:
    bool chance(double p) {
        return std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < p;
    }

    std::mt19937_64 rng_;
    int last_x_ = 0;
    int last_y_ = 0;
    int hold_left_ = 0;
    int hold_key_ = -1;
    int hold_key_frames_ = 0;
};

} // namespace

int main(int argc, char** argv) {
    std::string pfs;
    int frames = 3000;
    double minutes = 0.0; // >0 时按墙钟时间跑（帧数只当上限）
    uint64_t seed = 0x5DEECE66Dull;
    bool render = false;
    std::string trace_path = "oa_monkey.trace";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--minutes" && i + 1 < argc) minutes = std::atof(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--render") render = true;
        else if (a == "--trace" && i + 1 < argc) trace_path = argv[++i];
        else if (!a.empty() && a[0] != '-') pfs = a;
    }
    const char* env_pfs = std::getenv("OA_MONKEY_PFS");
    if (pfs.empty() && env_pfs) pfs = env_pfs;
    if (pfs.empty()) {
        std::fprintf(stderr,
                     "usage: oa_monkey <root.pfs> [--frames N] [--minutes M] [--seed S] "
                     "[--render] [--trace FILE]\n"
                     "       --frames 是上限，--minutes 按真实时间收工（0 = 不按时间）\n"
                     "       (or set OA_MONKEY_PFS)\n");
        return 2;
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0); // 崩了也要看到走到哪一步
    std::FILE* trace = std::fopen(trace_path.c_str(), "w");
    if (trace) std::setvbuf(trace, nullptr, _IOLBF, 0); // 崩了也留下最后几行

    SDL_Window* window = nullptr;
    std::unique_ptr<oa::render::RenderEngine> engine;
    std::unique_ptr<oa::runtime::GameRuntime> rt;

    std::printf("[monkey] mounting %s\n", pfs.c_str());
    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs, false);

    // 一个"会话"= 一次完整启动。游戏自己 [exit]/[gotitle] 收尾时整个会话重来：
    // 运行时没有公开的 reset 入口（[reset] 是脚本内部语义），重建 runtime 是唯一
    // 干净的做法；渲染器持有 runtime 指针，所以必须先毁它。
    auto start_session = [&]() -> bool {
        engine.reset();
        if (window) {
            SDL_DestroyWindow(window); // 窗口尺寸跟着工程走，重建最省心
            window = nullptr;
        }
        rt = std::make_unique<oa::runtime::GameRuntime>(fs);
        rt->open_project("windows");
        rt->boot_project();
        // App 默认就开解码池；猴子直接驱动 GameRuntime，要显式开，否则音频
        // 预读/视频解码 worker 这两条线根本没被压到。
        rt->enable_decode_pool();
        if (!render) return true;
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::printf("[monkey] SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        window = SDL_CreateWindow("oa-monkey", int(rt->project().config.stage_width),
                                  int(rt->project().config.stage_height),
                                  SDL_WINDOW_HIDDEN);
        engine = std::make_unique<oa::render::RenderEngine>(fs.get(), rt.get());
        if (!window || !engine->create_renderer(window, "sdl")) {
            std::printf("[monkey] renderer creation failed: %s\n", SDL_GetError());
            engine.reset();
            return false;
        }
        return true;
    };

    std::printf("[monkey] opening project\n");
    if (!start_session()) return 2;
    std::printf("[monkey] booted\n");

    Monkey monkey(seed);
    const int stage_w = int(rt->project().config.stage_width);
    const int stage_h = int(rt->project().config.stage_height);
    std::printf("[monkey] %s frames<=%d minutes=%.1f seed=%llu render=%d trace=%s\n",
                pfs.c_str(), frames, minutes, (unsigned long long)seed, render ? 1 : 0,
                trace_path.c_str());
    const auto wall_start = std::chrono::steady_clock::now();

    size_t errors = 0;
    // 帧时钟也用固定步长（不是墙上时间）：同一个 seed ⇒ 同一串输入 + 同一个
    // 时钟 ⇒ 崩溃可复现。墙上时间会让动画/视频进度漂，同一个 seed 复现不了。
    const uint64_t t0 = 0;
    int sessions = 1;
    int quick_exits = 0;
    int session_start = 0;
    for (int i = 0; i < frames; ++i) {
        if (minutes > 0.0) {
            const double used = std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - wall_start)
                                    .count() / 60.0;
            if (used >= minutes) break;
        }
        const Action a = monkey.next(stage_w, stage_h);
        if (trace) std::fprintf(trace, "%d %s\n", i, a.text.c_str());
        try {
            rt->tick(16, a.input);
            if (engine) engine->render_frame(t0 + uint64_t(i) * 16, 1.0);
        } catch (const std::exception& e) {
            // 剧本/资源错误在暴力输入下是预期内的：记下来接着砸。
            ++errors;
            std::printf("[monkey] frame %d threw: %s\n", i, e.what());
            if (trace) std::fprintf(trace, "%d THROW %s\n", i, e.what());
        }
        // 游戏自己收尾（[exit]/[gotitle]）→ 整个会话重来，长跑要的是连续运行。
        // 连续几次都在开局几十帧内退出（数据坏了/一进来就退）就放弃，别空转。
        if (rt->exit_requested()) {
            if (i - session_start < 60) {
                if (++quick_exits >= 5) {
                    std::printf("[monkey] 连续 %d 次开局即退出，停止\n", quick_exits);
                    break;
                }
            } else {
                quick_exits = 0;
            }
            std::printf("[monkey] session %d ended at frame %d, restarting\n", sessions, i);
            if (!start_session()) break;
            ++sessions;
            session_start = i;
        }
        if (engine && (i % 200) == 0)
            std::printf("[monkey] frame %d sessions %d (%zu errors)\n", i, sessions, errors);
    }
    if (trace) std::fclose(trace);
    if (engine) engine->release_all();
    if (window) SDL_DestroyWindow(window);
    if (render) SDL_Quit();
    const double total_min = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - wall_start)
                                 .count() / 60.0;
    std::printf("[monkey] done: %.1f min, %d sessions, %zu script errors, survived\n",
                total_min, sessions, errors);
    return 0;
}
