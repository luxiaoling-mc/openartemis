// 并发压力：把**会起线程的子系统**一起摇起来，专打「worker 线程」与「主线程」的交错。
//
//   oa_stress <root.pfs> [--minutes M] [--threads N] [--seed S] [--render]
//                         [--silent] [--roles video,audio,layers,emote]
//                         [--video P] [--audio P] [--emote P]
//
// 各角色对应的线程：
//   video  —— VideoEngine 的 driver 线程解码 + 推帧，主线程起停/收尾/落纹理
//   audio  —— decode pool worker + 每通道 prefetch 线程 + SDL 设备回调线程，
//             主线程起停/淡入淡出/改音量
//   emote  —— PSB 并行解码线程池 + 画布合成，主线程重建/渲染/销毁
//   render —— 主线程整条渲染链（计划重建、组离屏合成、HostTexture flush）
//
// 契约：这些 API 全部在**本线程**调用（引擎要求），真正并发的是引擎内部的
// worker 线程。--render 时用 runtime 自己的渲染器（和 app/安卓一样：tick 里
// 直接 render_frame），不是另建一个 —— 这样纹理 flush、音频设备、视频音轨
// 都是生产路径。
//
// --threads N：N 条线程各建一个自己的 runtime（各挂各的包、各跑各的 driver 与
// 音频 worker），用来看有没有跨实例共享状态被写坏。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "core/fs/physfs_fs.h"
#include "core/media/video.h"
#include "core/runtime/runtime_game.h"

namespace {

enum Role : unsigned {
    kRoleVideo = 1u << 0,
    kRoleAudio = 1u << 1,
    kRoleLayers = 1u << 2,
    kRoleEmote = 1u << 3,
};
constexpr unsigned kAllRoles = kRoleVideo | kRoleAudio | kRoleLayers | kRoleEmote;

struct Args {
    std::string pfs;
    double minutes = 5.0;
    int threads = 1;
    uint64_t seed = 0x5DEECE66Dull;
    bool render = false;
    bool silent = false;
    int decode_pool = -1; // <0 = 平台默认，0 = 关（全部同步解码）
    unsigned roles = kAllRoles;
    std::vector<std::string> extra_videos;
    std::vector<std::string> extra_audio;
    std::vector<std::string> extra_emotes;
};

unsigned parse_roles(const std::string& csv) {
    unsigned out = 0;
    size_t pos = 0;
    while (pos <= csv.size()) {
        const size_t comma = csv.find(',', pos);
        const std::string name =
            csv.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (name == "video") out |= kRoleVideo;
        else if (name == "audio") out |= kRoleAudio;
        else if (name == "layers") out |= kRoleLayers;
        else if (name == "emote") out |= kRoleEmote;
        else if (name == "all") out |= kAllRoles;
        else if (!name.empty())
            std::fprintf(stderr, "[stress] unknown role '%s' (video,audio,layers,emote,all)\n",
                         name.c_str());
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--minutes" && i + 1 < argc) a.minutes = std::atof(argv[++i]);
        else if (s == "--threads" && i + 1 < argc) a.threads = std::atoi(argv[++i]);
        else if (s == "--seed" && i + 1 < argc) a.seed = std::strtoull(argv[++i], nullptr, 0);
        else if (s == "--render") a.render = true;
        else if (s == "--silent") a.silent = true;
        else if (s == "--decode-pool" && i + 1 < argc) a.decode_pool = std::atoi(argv[++i]);
        else if (s == "--roles" && i + 1 < argc) a.roles = parse_roles(argv[++i]);
        else if (s == "--video" && i + 1 < argc) a.extra_videos.push_back(argv[++i]);
        else if (s == "--audio" && i + 1 < argc) a.extra_audio.push_back(argv[++i]);
        else if (s == "--emote" && i + 1 < argc) a.extra_emotes.push_back(argv[++i]);
        else if (!s.empty() && s[0] != '-') a.pfs = s;
    }
    if (a.threads < 1) a.threads = 1;
    return a;
}

// ---------------------------------------------------------------------------
// 资产发现。IFileSystem::list 只列**文件**（不列子目录），没法递归发现目录，
// 所以按各代工程的约定目录逐个试；找不到的角色自动跳过（--video/--audio/
// --emote 可以手工补）。
// ---------------------------------------------------------------------------
bool has_ext(const std::string& path, const std::vector<std::string>& exts) {
    for (const std::string& ext : exts) {
        const size_t n = ext.size();
        if (path.size() <= n) continue;
        bool same = true;
        for (size_t k = 0; k < n; ++k) {
            char a = path[path.size() - n + k];
            char b = ext[k];
            if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same) return true;
    }
    return false;
}

std::vector<std::string> collect(const oa::fs::IFileSystem& fs,
                                 const std::vector<const char*>& dirs,
                                 const std::vector<std::string>& exts, size_t limit) {
    std::vector<std::string> out;
    for (const char* dir : dirs) {
        const auto entries = fs.list(dir);
        if (!entries) continue;
        for (const std::string& name : *entries) {
            if (!has_ext(name, exts)) continue;
            out.push_back(std::string(dir) + "/" + name);
            if (out.size() >= limit) return out;
        }
    }
    return out;
}

struct Assets {
    std::vector<std::string> videos;
    std::vector<std::string> bgm;
    std::vector<std::string> se;
    std::vector<std::string> voice;
    std::vector<std::string> emotes;
    std::vector<std::string> images;
};

Assets find_assets(const oa::fs::IFileSystem& fs) {
    Assets a;
    a.videos = collect(fs,
                       {"movie", "movie/movie", "image/anime", "image/movie", "mv",
                        "video", "adv/movie", "system/movie", "movie_bai"},
                       {".mp4", ".wmv", ".ogv", ".avi", ".mkv", ".webm"}, 64);
    a.bgm = collect(fs, {"sound/bgm", "sound/BGM", "bgm", "sound"}, {".ogg", ".wav"}, 32);
    a.se = collect(fs,
                   {"sound/se", "sound/sysse", "sound/SE", "sound/se_", "sound"},
                   {".ogg", ".wav"}, 64);
    a.voice = collect(fs, {"sound/vo", "sound/voice", "voice", "sound"}, {".ogg", ".wav"}, 32);
    a.emotes = collect(fs,
                       {"emote", "emote/psb", "image/emote", "adv/emote", "psb",
                        "image/character", "image/fg"},
                       {".psb"}, 32);
    a.images = collect(fs,
                       {"image/bg", "image/fg", "image/character", "image", "bg", "fg"},
                       {".png", ".webp", ".jpg", ".jpeg"}, 32);
    return a;
}

struct Stats {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> video_plays{0};
    std::atomic<uint64_t> video_stops{0};
    std::atomic<uint64_t> overlay_frames{0}; // overlay 节点已挂上宿主纹理的帧数
    std::atomic<uint64_t> audio_ops{0};
    std::atomic<uint64_t> layer_create{0};
    std::atomic<uint64_t> layer_remove{0};
    std::atomic<uint64_t> emote_builds{0};
    std::atomic<uint64_t> errors{0};
};

/// 每个工作线程的随机源 + 各类"进行中"状态。
struct WorkerState {
    std::mt19937_64 rng;
    // video
    bool video_playing = false;
    bool overlay = false; // 本段是 overlay（全屏 [video]，通道 id 为空）
    uint64_t video_stop_at = 0;
    // layers
    std::deque<std::string> my_layers;
    // emote：固定几个 id 反复重建（同 id 重建会替换旧 player，不会越堆越多）
    std::vector<std::string> emote_ids;
    std::vector<uint64_t> emote_due;
};

struct WindowHost {
    SDL_Window* window = nullptr;
};

uint64_t pick(std::mt19937_64& rng, const std::vector<std::string>& v) {
    return v.empty() ? 0 : (rng() % v.size());
}

int rand_int(std::mt19937_64& rng, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + int(rng() % uint64_t(hi - lo + 1));
}

// ---------------------------------------------------------------------------
// 各角色的一步动作。全部由本线程调用。
// ---------------------------------------------------------------------------
void step_video(oa::runtime::GameRuntime& rt, const Assets& assets, WorkerState& st,
                const std::string& channel, uint64_t frame, Stats* stats) {
    if (!st.video_playing) {
        if (assets.videos.empty()) return;
        oa::media::VideoConfig cfg;
        cfg.file = assets.videos[pick(st.rng, assets.videos)];
        cfg.skippable = (st.rng() % 2) == 0;
        cfg.loop_play = (st.rng() % 4) == 0;
        // 1/3 走 overlay（全屏 [video]，通道 id 为空）：这条路径的解码键是
        // kOverlayVideoId，和层视频不是同一个槽。
        st.overlay = (st.rng() % 3) == 0;
        if (st.overlay)
            rt.video().play_overlay(cfg);
        else
            rt.video().play_layer(channel, cfg);
        ++stats->video_plays;
        st.video_playing = true;
        // 停得越早，越容易在"推帧进行中"撞上收尾
        st.video_stop_at = frame + 1 + st.rng() % 40;
    } else if (frame >= st.video_stop_at) {
        if (st.overlay) {
            if (st.rng() % 2)
                rt.video().stop_overlay();
            else
                rt.video().stop_all_videos();
        } else {
            switch (int(st.rng() % 3)) {
                case 0: rt.video().stop_layer(channel); break;
                case 1: rt.video().stop_layer_subtree("stress"); break;
                default: rt.video().stop_all_videos(); break;
            }
        }
        ++stats->video_stops;
        st.video_playing = false;
        st.overlay = false;
    }
}

void step_audio(oa::runtime::GameRuntime& rt, const Assets& assets, WorkerState& st,
                int index, Stats* stats) {
    oa::media::AudioEngine& au = rt.audio();
    const int op = int(st.rng() % 10);
    switch (op) {
        case 0: {
            if (assets.bgm.empty()) return;
            oa::media::BgmConfig cfg;
            cfg.loop_play = (st.rng() % 2) == 0;
            cfg.fade_in_ms = (st.rng() % 3) * 120;
            cfg.gain = int32_t(st.rng() % 1001);
            au.play_bgm(assets.bgm[pick(st.rng, assets.bgm)], cfg);
            break;
        }
        case 1: au.stop_bgm(uint64_t(st.rng() % 400)); break;
        case 2: {
            if (assets.bgm.empty()) return;
            oa::media::BgmConfig cfg;
            cfg.loop_play = true;
            au.crossfade_bgm(assets.bgm[pick(st.rng, assets.bgm)], cfg);
            break;
        }
        case 3: {
            if (assets.se.empty()) return;
            const std::string id = "stress.se." + std::to_string(index) + "." +
                                   std::to_string(st.rng() % 8);
            oa::media::SeConfig cfg;
            cfg.loop_play = (st.rng() % 3) == 0;
            cfg.gain = int32_t(st.rng() % 1001);
            cfg.pan = int32_t(int(st.rng() % 2001) - 1000);
            cfg.skippable = (st.rng() % 2) == 0;
            au.play_se(id, assets.se[pick(st.rng, assets.se)], cfg);
            break;
        }
        case 4: {
            const std::string id = "stress.se." + std::to_string(index) + "." +
                                   std::to_string(st.rng() % 8);
            au.stop_se(id, uint64_t(st.rng() % 300));
            break;
        }
        case 5: {
            const std::string id = "stress.se." + std::to_string(index) + "." +
                                   std::to_string(st.rng() % 8);
            au.fade_se_gain(id, int32_t(st.rng() % 1001), uint64_t(st.rng() % 400));
            break;
        }
        case 6: {
            if (assets.voice.empty()) return;
            const std::string id = "stress.vo." + std::to_string(index);
            oa::media::SeConfig cfg;
            cfg.gain = int32_t(st.rng() % 1001);
            au.play_voice(id, assets.voice[pick(st.rng, assets.voice)], cfg);
            break;
        }
        case 7: au.stop_all_sounds(); break;
        case 8: au.set_master_volume(float(st.rng() % 101) / 100.0f); break;
        default: au.fade_bgm_gain(int32_t(st.rng() % 1001), uint64_t(st.rng() % 400)); break;
    }
    ++stats->audio_ops;
}

void step_layers(oa::runtime::GameRuntime& rt, const Assets& assets, WorkerState& st,
                 int index, uint64_t frame, Stats* stats) {
    // 建 1~3 个（带随机几何/裁剪/掩码），把积累的量控制在 24 个以内。
    const int add = rand_int(st.rng, 1, 3);
    for (int k = 0; k < add; ++k) {
        const std::string id = "stress." + std::to_string(index) + ".l" + std::to_string(frame) +
                               "." + std::to_string(k);
        std::map<std::string, std::string> props;
        if (!assets.images.empty() && (st.rng() % 4) != 0)
            props["file"] = assets.images[pick(st.rng, assets.images)];
        props["width"] = std::to_string(rand_int(st.rng, 16, 640));
        props["height"] = std::to_string(rand_int(st.rng, 16, 480));
        props["x"] = std::to_string(rand_int(st.rng, -200, 1600));
        props["y"] = std::to_string(rand_int(st.rng, -200, 900));
        props["alpha"] = std::to_string(rand_int(st.rng, 0, 255));
        props["rotate"] = std::to_string(rand_int(st.rng, 0, 360));
        props["xscale"] = std::to_string(double(rand_int(st.rng, 10, 300)) / 100.0);
        props["yscale"] = std::to_string(double(rand_int(st.rng, 10, 300)) / 100.0);
        if (st.rng() % 3 == 0) {
            props["clip"] = std::to_string(rand_int(st.rng, 0, 32)) + "," +
                            std::to_string(rand_int(st.rng, 0, 32)) + "," +
                            std::to_string(rand_int(st.rng, 8, 256)) + "," +
                            std::to_string(rand_int(st.rng, 8, 256));
        }
        if (!assets.images.empty() && st.rng() % 5 == 0)
            props["mask"] = assets.images[pick(st.rng, assets.images)];
        rt.scene().create(id, props);
        st.my_layers.push_back(id);
        ++stats->layer_create;
    }
    while (st.my_layers.size() > 24) {
        rt.scene().remove(st.my_layers.front());
        st.my_layers.pop_front();
        ++stats->layer_remove;
    }
    // 随机改属性（走 lyprop 路径，触发计划重建）
    if (!st.my_layers.empty()) {
        const std::string& id = st.my_layers[size_t(st.rng() % st.my_layers.size())];
        std::map<std::string, std::string> p;
        p["alpha"] = std::to_string(rand_int(st.rng, 0, 255));
        p["x"] = std::to_string(rand_int(st.rng, -200, 1600));
        if (st.rng() % 3 == 0) p["visible"] = (st.rng() % 2) ? "1" : "0";
        rt.scene().set_props(id, p);
    }
}

/// emote：同 id 反复重建（替换旧 player → 旧 PSB 资源释放），并把已存在的删掉。
void step_emote(oa::runtime::GameRuntime& rt, const Assets& assets, WorkerState& st,
                int index, uint64_t frame, Stats* stats) {
    if (assets.emotes.empty()) return;
    if (st.emote_ids.empty()) {
        // 两三个固定 id：重建走 apply_emote_static 的 carry-view 路径。
        for (int k = 0; k < 2; ++k)
            st.emote_ids.push_back("stress." + std::to_string(index) + ".emote" +
                                   std::to_string(k));
        st.emote_due.assign(st.emote_ids.size(), frame);
    }
    const size_t slot = size_t(st.rng() % st.emote_ids.size());
    const std::string& id = st.emote_ids[slot];
    if (frame < st.emote_due[slot]) return;
    if (rt.scene().find(id) && (st.rng() % 3) == 0) {
        // 走真正的删除域（LayerDelete）：场景节点 + emote player + 该层视频
        // 通道一起释放 —— 和脚本里 lydel/fgdel 同一条路径。
        oa::runtime::Event del;
        del.kind = oa::runtime::Event::Kind::LayerDelete;
        del.id = id;
        rt.apply_event(del);
        ++stats->layer_remove;
    } else {
        oa::runtime::Event e;
        e.kind = oa::runtime::Event::Kind::Custom;
        e.tag = "emotestatic";
        e.params["id"] = id;
        e.params["files"] = assets.emotes[pick(st.rng, assets.emotes)];
        e.params["width"] = std::to_string(rand_int(st.rng, 256, 1024));
        e.params["height"] = std::to_string(rand_int(st.rng, 256, 1024));
        rt.apply_event(e); // 走解释器同一套域路由（emotestatic → 并行解码 + 画布合成）
        ++stats->emote_builds;
    }
    // PSB 重建要跑并行解码 + 画布合成，比别的角色重得多：间隔放大，
    // 让它稳定地"每隔几秒重建一次"，而不是把整个压测拖成加载测试。
    st.emote_due[slot] = frame + uint64_t(rand_int(st.rng, 120, 420));
}

void worker(const Args& args, const Assets& assets, int index, std::atomic<bool>* stop,
            Stats* stats) {
    auto fs = std::make_shared<oa::fs::PhysFileSystem>(args.pfs, false);
    SDL_Window* window = nullptr;
    size_t emote_layers = 0;
    size_t emote_events = 0;
    uint64_t frames_done = 0;
    {
    oa::runtime::GameRuntime rt(fs);
    rt.open_project("windows");
    rt.boot_project();
    // 解码线程池（app 默认就开）：audio 的预读 worker + 视频解码 worker 都在
    // 这里，是"worker 线程 vs 主线程"的主要来源之一。--decode-pool 0 关掉。
    rt.enable_decode_pool(args.decode_pool);

    if (args.render) {
        const int w = int(rt.project().config.stage_width);
        const int h = int(rt.project().config.stage_height);
        window = SDL_CreateWindow("oa-stress", w, h, SDL_WINDOW_HIDDEN);
    }
    {
        // 多实例时 SDL_InitSubSystem 的引用计数不是线程安全的：attach_window
        // 与 MediaPlayers::init 都会去初始化音频子系统，串行起来。
        static std::mutex sdl_init_mu;
        std::lock_guard<std::mutex> lk(sdl_init_mu);
        const bool attached =
            window && rt.attach_window(static_cast<void*>(window), "sdl");
        if (window && !attached) {
            std::printf("[stress] worker %d: renderer attach failed (%s), 继续无窗模式\n",
                        index, SDL_GetError());
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        if (!attached) rt.media_players().init();
    }

    WorkerState st;
    st.rng.seed(args.seed + uint64_t(index) * 0x9E3779B97F4A7C15ull);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(int64_t(args.minutes * 60000.0));
    const std::string channel = "stress." + std::to_string(index);
    uint64_t frame = 0;

    while (!stop->load() && std::chrono::steady_clock::now() < deadline) {
        ++frame;
        try {
            if (args.roles & kRoleVideo) step_video(rt, assets, st, channel, frame, stats);
            if (args.roles & kRoleAudio) step_audio(rt, assets, st, index, stats);
            if ((args.roles & kRoleLayers) && frame % 3 == 0)
                step_layers(rt, assets, st, index, frame, stats);
            if ((args.roles & kRoleEmote) && frame % 5 == 0)
                step_emote(rt, assets, st, index, frame, stats);

            oa::runtime::FrameInput in;
            in.mouse_x = rand_int(st.rng, 0, 1280);
            in.mouse_y = rand_int(st.rng, 0, 720);
            if (st.rng() % 24 == 0) {
                in.left_click_edge = true;
                in.left_down = true;
            }
            if (st.rng() % 40 == 0) in.right_click_edge = true;
            if (st.rng() % 60 == 0) in.key_down_edges.push_back(int(st.rng() % 256));

            if (args.silent) rt.audio().set_master_volume(0.0f);
            rt.tick(16, in);
            // overlay 真的上屏了吗：节点挂上宿主纹理才算（不靠"我以为推了"）。
            if (st.overlay) {
                if (const oa::render::SceneNode* n = rt.scene().overlay_node())
                    if (n->layer.host_bound && n->layer.texture) ++stats->overlay_frames;
            }
        } catch (const std::exception& e) {
            // 脚本/资源错误在暴力输入下是预期内的：记下来接着摇。
            ++stats->errors;
            if (stats->errors.load() <= 5)
                std::printf("[stress] worker %d frame %llu threw: %s\n", index,
                            (unsigned long long)frame, e.what());
        }
        ++stats->frames;
    }

    // 先把 runtime 收掉（析构里 release_all 释放渲染后端/GL 上下文），再销毁
    // 窗口 —— 反过来会让后端在已经销毁的窗口上收尾。
    emote_layers = rt.emote_layers().size();
    emote_events = rt.emote_layer_events();
    frames_done = frame;
    } // rt 析构
    if (window) SDL_DestroyWindow(window);
    std::printf("[stress] worker %d done: %llu frames, emote layers=%zu events=%zu\n", index,
                (unsigned long long)frames_done, emote_layers, emote_events);
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse(argc, argv);
    if (args.pfs.empty()) {
        std::fprintf(stderr,
                     "usage: oa_stress <root.pfs> [--minutes M] [--threads N] [--seed S]\n"
                     "                  [--render] [--silent] [--decode-pool N]\n"
                     "                  [--roles video,audio,layers,emote]\n"
                     "                  [--video P] [--audio P] [--emote P]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (args.render && !SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("[stress] SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
        return 2;
    }

    Assets assets;
    {
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(args.pfs, false);
        assets = find_assets(*fs);
    }
    for (const auto& v : args.extra_videos) assets.videos.push_back(v);
    for (const auto& a : args.extra_audio) assets.se.push_back(a);
    for (const auto& e : args.extra_emotes) assets.emotes.push_back(e);

    std::printf("[stress] %s threads=%d minutes=%.1f render=%d silent=%d decode_pool=%d\n",
                args.pfs.c_str(), args.threads, args.minutes, args.render ? 1 : 0,
                args.silent ? 1 : 0, args.decode_pool);
    std::printf("[stress] assets: videos=%zu bgm=%zu se=%zu voice=%zu emotes=%zu images=%zu\n",
                assets.videos.size(), assets.bgm.size(), assets.se.size(),
                assets.voice.size(), assets.emotes.size(), assets.images.size());
    if ((args.roles & kRoleVideo) && assets.videos.empty())
        std::printf("[stress] SKIP video（包里没找到视频，可用 --video 指定）\n");
    if ((args.roles & kRoleAudio) && assets.bgm.empty() && assets.se.empty() &&
        assets.voice.empty())
        std::printf("[stress] SKIP audio（包里没找到音频，可用 --audio 指定）\n");
    if ((args.roles & kRoleEmote) && assets.emotes.empty())
        std::printf("[stress] SKIP emote（包里没找到 .psb，可用 --emote 指定）\n");

    std::atomic<bool> stop{false};
    Stats stats;
    std::vector<std::thread> pool;
    for (int i = 0; i < args.threads; ++i)
        pool.emplace_back(worker, std::cref(args), std::cref(assets), i, &stop, &stats);
    for (auto& t : pool) t.join();

    std::printf("[stress] done: frames=%llu video(play/stop)=%llu/%llu overlay_frames=%llu "
                "audio_ops=%llu layers(+/-, emote included)=%llu/%llu emotes=%llu errors=%llu\n",
                (unsigned long long)stats.frames.load(),
                (unsigned long long)stats.video_plays.load(),
                (unsigned long long)stats.video_stops.load(),
                (unsigned long long)stats.overlay_frames.load(),
                (unsigned long long)stats.audio_ops.load(),
                (unsigned long long)stats.layer_create.load(),
                (unsigned long long)stats.layer_remove.load(),
                (unsigned long long)stats.emote_builds.load(),
                (unsigned long long)stats.errors.load());
    return 0;
}
