// Headless emote performance probe (report-only, not a ctest).
// Boots a REAL game archive headlessly (GameRuntime::tick, virtual 16 ms
// ticks — the runtime_frames_test driving model), injects a live E-mote
// layer through the game's own Lua bridge (e:createEmoteLayer — exactly the
// call the game framework makes for [fg] portraits), drives the file's own
// idle/foreground timelines and measures:
//   * per-tick wall time before/while an emote layer is live (steady idle +
//     expression churn), the runtime's real headless cost;
//   * player-level micro costs: advance_ms (timeline step + compose +
//     throttled CPU raster), collect_pose_parts (GPU-path pose geometry),
//     and a full CPU raster (render_now);
//   * fresh load() cost (PSB parse + atlas decode), cold and warm.
// No window, no renderer; no story walk needed (the layer is injected at
// the title park, so the probe never depends on game-specific UI).
//
// usage: emote_perf_probe <root.pfs> [psb-name-substring]
// knobs: OA_PERF_PSB      (psb substring; default "jsa_6")
//        OA_PERF_BASETICKS (baseline ticks, default 300)
//        OA_PERF_STEADY    (steady emote ticks, default 600)
//        OA_PERF_BOOTCAP   (layer-materialize tick cap, default 600)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/fs/physfs_fs.h"
#include "core/render/content_role.h"
#include "core/render/layer.h"
#include "core/render/renderer.h"
#include "core/runtime/runtime.h"

namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) {
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      Clock::now() - t0)
                      .count()) /
           1e6;
}
int env_int(const char* name, int dflt) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : dflt;
}
const char* env_str(const char* name, const char* dflt) {
    const char* v = std::getenv(name);
    return v && *v ? v : dflt;
}

struct TickStat {
    double total_ms = 0;
    double max_ms = 0;
    long n = 0;
    void add(double ms) {
        total_ms += ms;
        if (ms > max_ms) max_ms = ms;
        ++n;
    }
    double avg() const { return n ? total_ms / double(n) : 0; }
};

struct PartsStat {
    double total_ms = 0;
    double max_ms = 0;
    long n = 0;
    size_t last_parts = 0;
    void add(double ms, size_t parts) {
        total_ms += ms;
        if (ms > max_ms) max_ms = ms;
        ++n;
        last_parts = parts;
    }
    double avg() const { return n ? total_ms / double(n) : 0; }
};

const char* wait_desc(const oa::runtime::WaitReason* w) {
    if (!w) return "run";
    switch (w->kind) {
        case oa::runtime::WaitReason::Kind::Stop: return "stop";
        case oa::runtime::WaitReason::Kind::Timed: return "timed";
        case oa::runtime::WaitReason::Kind::Generic: return "click";
        case oa::runtime::WaitReason::Kind::Generic0: return "click0";
        default: return "other";
    }
}

/// First emote PSB under image/fg whose name contains `sub` (the games keep
/// fg PSBs in the volume chain; PhysFileSystem merges them into one listing,
/// so a directory walk finds them without knowing the volume).
std::string find_psb(oa::fs::IFileSystem& fs, const std::string& sub) {
    std::vector<std::string> queue = {"image/fg", "image\\fg"};
    std::set<std::string> seen;
    while (!queue.empty()) {
        const std::string dir = queue.back();
        queue.pop_back();
        if (seen.count(dir)) continue;
        seen.insert(dir);
        auto items = fs.list(dir);
        if (!items) continue;
        for (const std::string& n : *items) {
            const std::string full = dir + "/" + n;
            if (n.find('.') == std::string::npos) {
                queue.push_back(full);
                continue;
            }
            std::string tail = n.size() >= 4 ? n.substr(n.size() - 4) : n;
            for (char& c : tail) c = char(::tolower(c));
            if (tail != ".psb") continue;
            if (!sub.empty() && n.find(sub) == std::string::npos) continue;
            // normalize to backslash form (the archive's own convention)
            std::string out = full;
            for (char& c : out)
                if (c == '/') c = '\\';
            return out;
        }
    }
    return "";
}
} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: emote_perf_probe <root.pfs> [psb-substring]\n");
        return 2;
    }
    const char* pfs_path = argv[1];
    const std::string psb_sub =
        argc > 2 ? argv[2] : env_str("OA_PERF_PSB", "");
    const int base_ticks = env_int("OA_PERF_BASETICKS", 300);
    const int steady_ticks = env_int("OA_PERF_STEADY", 600);
    const int boot_cap = env_int("OA_PERF_BOOTCAP", 600);

    std::shared_ptr<oa::fs::IFileSystem> fs =
        std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
    oa::runtime::GameRuntime rt(fs);
    rt.open_project("windows");
    rt.boot_project();

    oa::runtime::FrameInput in;
    TickStat base_stat, steady_stat;

    // ---- phase 1: baseline (pre-emote) ------------------------------------
    for (int i = 0; i < base_ticks && !rt.exit_requested(); ++i) {
        const auto t0 = Clock::now();
        rt.tick(16, in);
        base_stat.add(ms_since(t0));
    }

    // ---- phase 2: inject a live emote layer (the game's own Lua call) -----
    const std::string psb = find_psb(*fs, psb_sub);
    if (psb.empty()) {
        std::printf("[perf] no fg psb matching '%s' in the archive\n",
                    psb_sub.c_str());
        return 3;
    }
    std::printf("[perf] injecting e:createEmoteLayer with '%s'\n", psb.c_str());
    {
        std::string code = "e:createEmoteLayer{id='perftest', files={[[";
        code += psb;
        code += "]]}, width=1920, height=1620}";
        rt.interpreter().lua_bridge().run_code(code.c_str(), "emote_perf_probe");
    }
    for (int i = 0; i < boot_cap && rt.emote_layers().empty(); ++i)
        rt.tick(16, in);
    if (rt.emote_layers().empty()) {
        std::printf("[perf] emote layer did not materialize\n");
        return 4;
    }
    for (const auto& [id, st] : rt.emote_layers())
        std::printf("[perf] emote layer id='%s' file='%s' %dx%d rev=%llu\n",
                    id.c_str(), st.file.c_str(), st.width, st.height,
                    (unsigned long long)st.revision);

    // ---- phase 3: drive the file's own timelines --------------------------
    // idle = a looping timeline (loopEnd>0, the 待機 breathing loop);
    // expression = the first non-looping timeline (a one-shot gesture).
    for (auto& [id, st] : rt.emote_layers()) {
        if (!st.player) continue;
        oa::emote::EmotePlayer& p = *st.player;
        const oa::emote::EmoteFile& f = p.file();
        std::string idle, expr;
        for (const auto& t : f.timelines) {
            if (idle.empty() && t.loopEnd > 0) idle = t.label;
            if (expr.empty() && t.loopEnd <= 0 && !t.variables.empty())
                expr = t.label;
        }
        std::printf("[perf] layer '%s': timelines=%zu idle='%s' expr='%s'\n",
                    id.c_str(), f.timelines.size(), idle.c_str(), expr.c_str());
        if (!idle.empty()) p.fade_in_timeline(idle);
    }

    // steady: idle breathing only (the standing-portrait baseline state)
    for (int i = 0; i < steady_ticks && !rt.exit_requested(); ++i) {
        const auto t0 = Clock::now();
        rt.tick(16, in);
        steady_stat.add(ms_since(t0));
    }

    // expression churn: replay the one-shot every 2 s (the [fg] tag flow)
    TickStat churn_stat;
    for (int i = 0; i < steady_ticks && !rt.exit_requested(); ++i) {
        const auto t0 = Clock::now();
        if (i % 120 == 0) {
            for (auto& [id, st] : rt.emote_layers()) {
                if (!st.player) continue;
                for (const auto& t : st.player->file().timelines) {
                    if (t.loopEnd > 0 || t.variables.empty()) continue;
                    st.player->play_timeline(t.label);
                    break;
                }
            }
        }
        rt.tick(16, in);
        churn_stat.add(ms_since(t0));
    }

    // ---- phase 4: player micro costs --------------------------------------
    for (const auto& [id, st] : rt.emote_layers()) {
        if (!st.player) continue;
        oa::emote::EmotePlayer& p = *st.player;

        // advance_ms: the runtime's own per-tick call. Measured twice:
        //   raster-included (external_pose off — headless / OA_EMOTE_GPU=0,
        //   amortizes the throttled pose raster into the average), and
        //   pure (external_pose on — the windowed GPU-path per-tick cost:
        //   timeline step + compose only).
        TickStat adv;
        p.set_external_pose(false);
        for (int i = 0; i < 300; ++i) {
            const auto t0 = Clock::now();
            p.advance_ms(16);
            adv.add(ms_since(t0));
        }
        TickStat adv_pure;
        p.set_external_pose(true);
        for (int i = 0; i < 300; ++i) {
            const auto t0 = Clock::now();
            p.advance_ms(16);
            adv_pure.add(ms_since(t0));
        }
        // collect_pose_parts: the GPU-path pose geometry per revision
        PartsStat col;
        std::vector<oa::emote::EmoteDrawPart> parts;
        for (int i = 0; i < 100; ++i) {
            const auto t0 = Clock::now();
            std::string err;
            p.collect_pose_parts(&parts, &err);
            col.add(ms_since(t0), parts.size());
        }
        // full CPU raster (external_pose off): the OA_EMOTE_GPU=0 path
        TickStat ras;
        p.set_external_pose(false);
        for (int i = 0; i < 20; ++i) {
            const auto t0 = Clock::now();
            p.render_now();
            ras.add(ms_since(t0));
        }
        std::printf(
            "[perf] layer '%s': advance_ms avg=%.3f max=%.3f ms (raster incl) | "
            "advance_pure avg=%.3f max=%.3f ms | "
            "collect_parts avg=%.3f max=%.3f ms (parts=%zu) | cpu_raster "
            "avg=%.2f max=%.2f ms (%dx%d)\n",
            id.c_str(), adv.avg(), adv.max_ms, adv_pure.avg(), adv_pure.max_ms,
            col.avg(), col.max_ms,
            col.last_parts, ras.avg(), ras.max_ms, p.width(), p.height());
    }

    // ---- phase 5: fresh load cost (PSB parse + atlas decode + first pose) -
    {
        auto bytes = fs->read(psb);
        if (bytes) {
            // decomposition: psb parse | atlas decode | player load (which
            // includes both plus the initial CPU pose raster)
            const auto tp = Clock::now();
            oa::emote::EmoteFile f;
            std::string err;
            const bool pok = f.load(*bytes, &err);
            const double parse_ms = ms_since(tp);
            double atlas_ms = 0;
            for (auto& s : f.sources) {
                if (!s->rgbaDecoded) {
                    const auto ta = Clock::now();
                    f.ensure_atlas(s.get(), &err);
                    atlas_ms += ms_since(ta);
                }
            }
            size_t atlas_bytes = 0;
            for (const auto& s : f.sources)
                atlas_bytes += s->rgba.size();
            const auto t0 = Clock::now();
            oa::emote::EmotePlayer p;
            const bool ok = p.load(*bytes, 1920, 1620, &err);
            const double load_ms = ms_since(t0);
            const auto t1 = Clock::now();
            oa::emote::EmotePlayer p2;
            const bool ok2 = p2.load(*bytes, 1920, 1620, &err);
            const double load2_ms = ms_since(t1);
            std::printf(
                "[perf] fresh load '%s': total=%.1f (warm=%.1f) ok=%d/%d | "
                "decomp: psb_parse=%.1f atlas_decode=%.1f (%zu src, %.1f MB) "
                "first_pose+raster≈%.1f\n",
                psb.c_str(), load_ms, load2_ms, int(ok), int(ok2), parse_ms,
                atlas_ms, f.sources.size(), double(atlas_bytes) / 1048576.0,
                load_ms - parse_ms - atlas_ms);
            if (!ok) std::printf("[perf]   load err: %s\n", err.c_str());
            (void)pok;
        } else {
            std::printf("[perf] fresh load: fs.read('%s') failed\n",
                        psb.c_str());
        }
    }

    // ---- phase 3b: windowed GPU-compositing frame cost (optional) ---------
    // OA_PERF_GPU=1: create a hidden SDL window + the real RenderEngine and
    // measure the full host frame path (collect_pose_parts ->
    // emote_render_parts -> draw_scene -> present), i.e. the windowed
    // emote-pump cost the app pays at the pose cadence.
    if (env_int("OA_PERF_GPU", 0)) {
        const int sw = rt.project_.config.stage_width;
        const int sh = rt.project_.config.stage_height;
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::printf("[perf] GPU mode: SDL_Init failed: %s\n", SDL_GetError());
        } else {
            SDL_Window* win = SDL_CreateWindow("oa-perf", sw, sh,
                                               SDL_WINDOW_HIDDEN);
            oa::render::RenderEngine re(fs.get(), &rt);
            if (!win || !re.create_renderer(win, "sdl")) {
                std::printf("[perf] GPU mode: renderer creation failed: %s\n",
                            SDL_GetError());
            } else {
                std::map<std::string, uint64_t> seen_rev;
                TickStat gpu_steady, gpu_churn;
                int frames = env_int("OA_PERF_GPU_FRAMES", 600);
                for (int i = 0; i < frames * 2 && !rt.exit_requested(); ++i) {
                    if (i == frames) {
                        // switch on expression churn for the second half
                        for (auto& [id, st] : rt.emote_layers()) {
                            if (!st.player) continue;
                            for (const auto& t : st.player->file().timelines) {
                                if (t.loopEnd > 0 || t.variables.empty()) continue;
                                st.player->play_timeline(t.label);
                                break;
                            }
                        }
                    }
                    const auto t0 = Clock::now();
                    rt.tick(16, in);
                    // ---- host emote pump (emote_pump_frames core) ----
                    const bool gpu = true;
                    for (const auto& [id, st] : rt.emote_layers()) {
                        if (!st.player) continue;
                        st.player->set_external_pose(gpu);
                        const uint64_t rev = st.player->revision();
                        if (rev == 0 || seen_rev[id] == rev) continue;
                        std::vector<oa::emote::EmoteDrawPart> parts;
                        std::string err;
                        const oa::render::TextureKey tkey =
                            oa::render::EmoteContent::canvas_key(id);
                        if (st.player->collect_pose_parts(&parts, &err) &&
                            re.emote_render_parts(tkey, st.player->file(),
                                                  parts, st.width, st.height))
                            seen_rev[id] = rev;
                    }
                    if (i >= frames && i % 120 == 0) {
                        // replay the one-shot during churn
                        for (auto& [id, st] : rt.emote_layers()) {
                            if (!st.player) continue;
                            for (const auto& t : st.player->file().timelines) {
                                if (t.loopEnd > 0 || t.variables.empty()) continue;
                                st.player->play_timeline(t.label);
                                break;
                            }
                        }
                    }
                    // ---- scene draw + present (render_beigin..render_end) --
                    re.render_beigin();
                    re.draw_scene(rt.scene(), 1.0);
                    re.progress_transition();
                    re.render_end();
                    const double ms = ms_since(t0);
                    if (i < frames) gpu_steady.add(ms);
                    else gpu_churn.add(ms);
                }
                std::printf("[perf] GPU frame cost: steady(avg=%.3f max=%.3f "
                            "n=%ld) churn(avg=%.3f max=%.3f n=%ld)\n",
                            gpu_steady.avg(), gpu_steady.max_ms, gpu_steady.n,
                            gpu_churn.avg(), gpu_churn.max_ms, gpu_churn.n);
            }
            if (win) SDL_DestroyWindow(win);
            SDL_Quit();
        }
    }

    // ---- phase 4b: dump the current pose as PPM (pixel-compare runs) ------
    {
        const char* dump = std::getenv("OA_PERF_DUMP");
        if (dump && *dump) {
            for (const auto& [id, st] : rt.emote_layers()) {
                if (!st.player) continue;
                st.player->set_external_pose(false);
                st.player->render_now();
                const int W = st.player->width(), H = st.player->height();
                const std::vector<uint8_t>& rgba = st.player->rgba();
                std::string path = std::string(dump) + "_" + id + ".ppm";
                FILE* f = std::fopen(path.c_str(), "wb");
                if (f && W > 0 && H > 0) {
                    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
                    std::vector<uint8_t> rgb(size_t(W) * size_t(H) * 3);
                    for (size_t i = 0; i < size_t(W) * size_t(H); ++i) {
                        // composite over mid-grey so the alpha channel is
                        // visible in the RGB dump
                        const int a = rgba[i * 4 + 3];
                        for (int c = 0; c < 3; ++c)
                            rgb[i * 3 + c] = uint8_t(
                                (rgba[i * 4 + c] * a + 128 * (255 - a)) / 255);
                    }
                    std::fwrite(rgb.data(), 1, rgb.size(), f);
                    std::fclose(f);
                    std::printf("[perf] dumped '%s' (%dx%d)\n", path.c_str(), W, H);
                } else if (f) {
                    std::fclose(f);
                }
                st.player->set_external_pose(true);
            }
        }
    }

    std::printf(
        "[perf] SUMMARY baseline(avg=%.3f max=%.3f n=%ld) emote_steady"
        "(avg=%.3f max=%.3f n=%ld) emote_churn(avg=%.3f max=%.3f n=%ld)\n",
        base_stat.avg(), base_stat.max_ms, base_stat.n, steady_stat.avg(),
        steady_stat.max_ms, steady_stat.n, churn_stat.avg(), churn_stat.max_ms,
        churn_stat.n);
    return 0;
}
