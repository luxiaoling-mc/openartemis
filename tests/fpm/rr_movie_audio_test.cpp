// research/86: rr (常轨脱离ReReCall) movie-audio acceptance — two phases
// over the REAL archive (pfs + volume chain + loose movie/ overlay via the
// archive's own parent directory):
//
//   Phase A (engine level, deterministic): VideoEngine plays the real
//   in-story movie movie/d2z4ydi8wv.mp4 (HEVC + AAC 44.1 kHz stereo) to EOF
//   on the 16 ms tick clock. Asserts the container audio source attaches
//   (audio_on), the produced 44100 Hz stereo frame count tracks the channel
//   clock through playback, the stream is audibly non-silent (activity
//   counters/peak), and EOF tears the channel down after exactly one finish
//   event.
//
//   Phase B (real-game journey): boots the rr project headless. The boot
//   chain itself plays the two opening movies in order — the brand logo
//   movie/logo.mp4 (~5 s, digitally SILENT audio track) then the reported
//   no-sound OP movie/d1e86u7ftq.mp4 (~99 s, real music). The driver ticks
//   through both [video] Stop parks and asserts, on the OP: live container
//   audio from start to EOF (~99 s), audibly non-silent samples, and the
//   audio channel retiring with the movie at EOF. The logo park also proves
//   the silent-track case: audio attached and counting, no activity.
//
// Env: OA_TEST_RR_PFS = path to the rr root.pfs FILE (the game directory's
// loose movie/ files overlay the pack, so the mp4s resolve). Skip code 77
// when unset. No window, no UI: pure engine acceptance (the audio is
// dropped silently headless — decode + channel counters still run).
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "core/fs/physfs_fs.h"
#include "core/media/video.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_save.h"

namespace {
int g_failures = 0;
void check(bool cond, const char* fmt, ...) {
    if (cond) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "FAIL: %s\n", buf);
    ++g_failures;
}
} // namespace

// ---------------------------------------------------------------------------
// Phase A: one real movie straight through the VideoEngine pump.
// ---------------------------------------------------------------------------
namespace {
int engine_phase(std::shared_ptr<const oa::fs::IFileSystem> fs) {
    const std::string file = "movie/d2z4ydi8wv.mp4";
    if (!fs->exists(file)) {
        std::fprintf(stderr, "%s missing (loose movie dir not mounted?)\n",
                     file.c_str());
        return 1;
    }
    oa::media::VideoEngine v;
    v.set_loader([&fs](const std::string& f) -> std::optional<std::vector<uint8_t>> {
        return fs->read(f);
    });
    oa::media::VideoConfig cfg;
    cfg.file = file;
    cfg.skippable = true;
    v.play_overlay(cfg);
    check(v.is_overlay_playing(), "A: overlay playing");
    const auto snap0 = v.state();
    check(snap0.overlay_video && snap0.overlay_video->decoded,
          "A: real decode attached");
    check(snap0.overlay_video && snap0.overlay_video->audio_on,
          "A: container audio attached (audio_on)");

    uint64_t mid_active = 0; // active frames at the mid-flight sample
    float mid_peak = 0.0f;
    size_t finish_events = 0;
    bool finished = false;
    size_t finish_tick = 0;
    const double kTickFrames = 16.0 * 44.1; // 16 ms * 44100 / 1000
    for (size_t tick = 1; tick < 8000 && !finished; ++tick) {
        v.update(16);
        finish_events += v.poll_finish_events().size();
        const auto s = v.state();
        const auto* ch = s.overlay_video ? &*s.overlay_video : nullptr;
        if (!ch || !ch->playing) {
            finish_tick = tick;
            finished = true;
            break;
        }
        // Every 500 ticks (~8 s) the produced frame count must track the
        // channel clock (deterministic delta pacing, ± a few ticks).
        if (tick % 500 == 0) {
            const double want = double(tick) * kTickFrames;
            const double got = double(ch->audio_frames);
            if (got < want - 3000.0 || got > want + 3000.0) {
                std::fprintf(stderr,
                             "A: audio clock drift at tick %zu: got %.0f want "
                             "%.0f\n",
                             tick, got, want);
                ++g_failures;
            }
        }
        if (tick == 1000) { // mid-flight sample (~16 s into a ~45 s movie)
            mid_active = ch->audio_active_frames;
            mid_peak = ch->audio_peak;
            std::printf("[A] mid tick=%zu pos=%llums audio_frames=%llu "
                        "active=%llu peak=%.4f\n",
                        tick, (unsigned long long)ch->position_ms,
                        (unsigned long long)ch->audio_frames,
                        (unsigned long long)ch->audio_active_frames,
                        ch->audio_peak);
        }
    }
    check(finished, "A: EOF reached (playing cleared)");
    check(finish_events == 1, "A: exactly one finish event");
    check(mid_active > 1000, "A: audibly non-silent mid-flight (active frames)");
    check(mid_peak > 1e-3f, "A: audibly non-silent mid-flight (peak)");
    const auto tail = v.poll_finish_events();
    check(tail.empty(), "A: finish queue drained");
    check(finish_tick > 2500, "A: movie ran past ~40 s (finish tick %zu)",
          finish_tick);
    v.stop_overlay();
    check(!v.is_overlay_playing(), "A: stop clears the channel");
    std::printf("[A] engine phase done (finish at tick %zu, ~%.2fs virtual)\n",
                finish_tick, finish_tick * 0.016);
    return g_failures;
}
} // namespace

// ---------------------------------------------------------------------------
// Phase B: real rr boot journey through logo.mp4 (silent track) and the OP
// d1e86u7ftq.mp4 (real sound), asserting movie audio through EOF.
// ---------------------------------------------------------------------------
namespace {
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

/// One observation window over a fullscreen movie park.
struct MovieWindow {
    std::string file;
    size_t enter = 0;  // first tick of this park
    size_t exit = 0;   // tick the park released
    bool released = false;
    bool decoded = false;
    bool audio_on = false;
    uint64_t mid_frames = 0; // audio snapshot past half the movie
    uint64_t mid_active = 0;
    float peak = 0.0f;
    uint64_t last_frames = 0;
    uint64_t last_active = 0;
};

int journey_phase(std::shared_ptr<const oa::fs::IFileSystem> fs,
                  std::shared_ptr<oa::runtime::SaveStore> store) {
    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();

    MovieWindow cur;      // the park currently open (any movie)
    MovieWindow op;       // the reported OP movie (d1e86u7ftq.mp4)
    MovieWindow logo;     // the boot brand logo (logo.mp4, silent track)
    bool raw_started = false;
    size_t t = 0;
    const size_t kTickLimit = 30000; // 480 s virtual: boot + OP (~99 s) + slack
    bool op_done = false;
    for (t = 0; t < kTickLimit && !rt.exit_requested(); ++t) {
        // research/82 probe shortcut: jump the boot chain (parked on the
        // boot logo movie / title) straight into the 莉々子-00 route — its
        // chapter start is the reported opening movie. The interrupted boot
        // logo window is only informative (silent-track case).
        if (!raw_started && t == 200) {
            raw_started = true;
            try {
                rt.interpreter().lua_bridge().run_code("title_start2('莉々子-00')", "rraw");
                std::printf("[B] raw title_start2(莉々子-00) invoked f=%zu\n", t);
            } catch (const std::exception& e) {
                std::printf("[B] raw invoke FAILED: %s\n", e.what());
                break;
            }
        }
        const oa::runtime::WaitReason* w = rt.current_wait();
        const bool video_park =
            w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id == "video";
        if (video_park) {
            const auto vs = rt.video().state();
            const auto* ch = vs.overlay_video ? &*vs.overlay_video : nullptr;
            const std::string file = ch ? ch->file : std::string();
            // A park whose channel file changed starts a new window: the
            // previous movie ended (its own EOF or a script-side stop).
            if (!file.empty() && cur.enter != 0 && file != cur.file) {
                if (!cur.released) cur.released = true; // ended by switch
                if (cur.exit == 0) cur.exit = t;
                if (cur.file.find("logo.mp4") != std::string::npos) logo = cur;
                if (cur.file.find("d1e86u7ftq.mp4") != std::string::npos) op = cur;
                std::printf("[B] park switch f=%zu -> file='%s' (prev '%s' "
                            "%zu ticks)\n",
                            t, file.c_str(), cur.file.c_str(),
                            cur.exit - cur.enter);
                cur = MovieWindow{};
                cur.file = file;
                cur.enter = t;
                continue;
            }
            if (cur.enter == 0 && !file.empty()) {
                cur.file = file;
                cur.enter = t;
                std::printf("[B] park start f=%zu file='%s'\n", t, file.c_str());
            }
            if (ch) {
                if (ch->decoded) cur.decoded = true;
                if (ch->audio_on) cur.audio_on = true;
                if (ch->audio_peak > cur.peak) cur.peak = ch->audio_peak;
                cur.last_frames = ch->audio_frames;
                cur.last_active = ch->audio_active_frames;
                if (cur.mid_frames == 0 && ch->position_ms >= 45000) {
                    cur.mid_frames = ch->audio_frames;
                    cur.mid_active = ch->audio_active_frames;
                    std::printf("[B] mid %s pos=%llums frames=%llu active=%llu "
                                "peak=%.4f\n",
                                file.c_str(), (unsigned long long)ch->position_ms,
                                (unsigned long long)ch->audio_frames,
                                (unsigned long long)ch->audio_active_frames,
                                ch->audio_peak);
                }
                if (cur.enter != 0 && t - cur.enter > 0 &&
                    (t - cur.enter) % 3000 == 0) { // every ~48 s virtual
                    std::printf("[B] heartbeat %s f=%zu pos=%llums frames=%llu "
                                "active=%llu peak=%.4f\n",
                                file.c_str(), t,
                                (unsigned long long)ch->position_ms,
                                (unsigned long long)ch->audio_frames,
                                (unsigned long long)ch->audio_active_frames,
                                ch->audio_peak);
                }
            }
            rt.tick(16, oa::runtime::FrameInput());
            // Released when the wait is no longer this video park.
            const oa::runtime::WaitReason* w2 = rt.current_wait();
            if (!cur.released && cur.enter != 0 && !(w2 && w2->kind ==
                    oa::runtime::WaitReason::Kind::Stop && w2->id == "video")) {
                cur.released = true;
                cur.exit = t;
                const auto vs2 = rt.video().state();
                const bool ch_gone =
                    !vs2.overlay_video || !vs2.overlay_video->playing;
                std::printf("[B] park end f=%zu after %zu ticks (%zu movie "
                            "ticks); channel %s frames=%llu active=%llu peak=%.4f\n",
                            t, t - cur.enter, cur.exit - cur.enter,
                            ch_gone ? "gone" : "still up",
                            (unsigned long long)cur.last_frames,
                            (unsigned long long)cur.last_active, cur.peak);
                if (cur.file.find("d1e86u7ftq.mp4") != std::string::npos) {
                    op = cur;
                    op_done = true;
                }
            }
            if (op_done && t > op.exit + 300) break;
            continue;
        }
        // Non-video wait: keep the runtime moving only through non-blocking
        // parks; boot needs no clicks, so plain ticks suffice.
        rt.tick(16, oa::runtime::FrameInput());
    }
    std::printf("[B] end f=%zu wait=%s\n", t, wait_desc(rt.current_wait()));

    // ---- logo.mp4 park (boot brand logo, digitally silent AAC track; cut
    // short by the raw chapter jump at f=200, so it is informational) ------
    check(logo.enter != 0, "B: boot logo park seen");
    if (logo.enter != 0) {
        check(logo.released, "B: logo park released");
        check(logo.file.find("logo.mp4") != std::string::npos,
              "B: logo file name");
        check(logo.decoded, "B: logo really decodes");
        check(logo.audio_on, "B: logo container audio attached (silent track)");
        check(logo.exit - logo.enter >= 150,
              "B: logo played into its ~5 s before the raw jump (%zu ticks)",
              logo.exit - logo.enter);
        check(logo.last_frames > 100000,
              "B: logo audio frames counted through its play");
        std::printf("[B] logo active=%llu of %llu frames (silent track: ~0 "
                    "expected)\n",
                    (unsigned long long)logo.last_active,
                    (unsigned long long)logo.last_frames);
    }

    // ---- d1e86u7ftq.mp4 park (the reported no-sound OP) -------------------
    check(op.enter != 0, "B: OP park seen");
    if (op.enter != 0) {
        check(op.released, "B: OP park released by EOF");
        check(op.file.find("d1e86u7ftq.mp4") != std::string::npos,
              "B: OP file name");
        check(op.decoded, "B: OP really decodes");
        check(op.audio_on, "B: OP container audio attached (audio_on)");
        const size_t op_ticks = op.exit - op.enter;
        check(op_ticks > 5600, "B: OP ran ~99 s to EOF (park %zu ticks, ~%.1fs)",
              op_ticks, op_ticks * 0.016);
        check(op.mid_frames >= 1900000,
              "B: audio live past ~45 s (frames=%llu)",
              (unsigned long long)op.mid_frames);
        check(op.mid_active > op.mid_frames / 10,
              "B: audio audibly non-silent at the mid sample");
        check(op.peak > 1e-3f, "B: audio peak audible");
        check(op.last_frames > 4000000,
              "B: audio ran into the final seconds (last frames=%llu)",
              (unsigned long long)op.last_frames);
        check(op.last_active > op.last_frames / 10,
              "B: audio still audibly active at EOF");
    }
    return g_failures;
}
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pfs_path = std::getenv("OA_TEST_RR_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_RR_PFS unset; skipping\n");
        return 77;
    }
    std::error_code ec;
    std::filesystem::path store_dir = std::filesystem::temp_directory_path(ec) /
                                      ("oa_rr_audio_" + std::to_string(::getpid()));
    std::filesystem::remove_all(store_dir, ec);
    auto store = std::make_shared<oa::runtime::DirSaveStore>(store_dir.string());
    // The archive's own real parent directory (loose movie/) overlays the
    // pack: the mp4 movies only exist loose in this title (research/86).
    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, true);

    int rc = engine_phase(fs);
    if (rc != 0) return rc;
    rc = journey_phase(fs, store);
    if (g_failures == 0) std::printf("rr_movie_audio_test: all ok\n");
    return g_failures == 0 ? 0 : 1;
}
