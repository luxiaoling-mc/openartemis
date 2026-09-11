// research/87: HCT (灵感满溢的甜蜜创想凸 / Hamidashi Creative Totsu)
// movie-audio acceptance — the user-reported stutter title. The opening
// videos of this CJK release all live loose in movie_bai/ (the archive's
// init.system.movie_path = "movie_bai/"; movie_jp/ is the JP set and is not
// mounted by this data). Two phases over the REAL archive (pfs + volume
// chain + the loose movie_bai/ overlay via the archive's own parent dir):
//
//   Phase A (engine level, deterministic): VideoEngine plays the OP
//   movie_bai/o4p6jwsag.mp4 (init.movie.op; HEVC 29.97 fps + AAC 44100 Hz
//   stereo, ~91.8 s) to EOF on the 16 ms tick clock. Asserts the container
//   audio source attaches (audio_on), the produced 44100 Hz stereo frame
//   count tracks the channel clock through playback, the stream is audibly
//   non-silent, and EOF tears the channel down after exactly one finish
//   event (the decodable audio run reaches ~91.82 s — right up to the
//   video EOF; the mp4 tail padding is not trimmed, research/86 §7).
//
//   Phase B (boot journey): boots the project headless. The boot chain
//   plays movie_bai/logo.mp4 (~5 s, digitally SILENT AAC track) and parks
//   on it; the park ends at the logo's own EOF and the boot reaches the
//   title wait. Asserts the logo window decoded + audio attached + silent
//   track (the 0-active case), locking the movie_bai directory choice and
//   the loose-file resolution into regression. The OP itself is not played
//   at boot (it plays from the new-game route, like rr's).
//
// Env: OA_TEST_HCT_PFS = path to the game's root.pfs FILE (the game
// directory's loose movie_bai/ files overlay the pack). Skip code 77 when
// unset. No window, no UI: pure engine acceptance (audio is dropped
// silently headless — decode + channel counters still run).
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
// Phase A: the OP movie straight through the VideoEngine pump.
// ---------------------------------------------------------------------------
namespace {
int engine_phase(std::shared_ptr<const oa::fs::IFileSystem> fs) {
    // movie_bai/o4p6jwsag.mp4 = init.movie.op of this release (the loose
    // movie_bai/ directory overlays the archive through its parent dir).
    const std::string file = "movie_bai/o4p6jwsag.mp4";
    if (!fs->exists(file)) {
        std::fprintf(stderr, "%s missing (loose movie_bai dir not mounted?)\n",
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
    uint64_t final_frames = 0;
    uint64_t final_active = 0;
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
        // Every 1000 ticks (~16 s) the produced frame count must track the
        // channel clock (deterministic delta pacing, ± a few ticks).
        if (tick % 1000 == 0) {
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
        if (tick == 2000) { // mid-flight sample (~32 s into the ~92 s OP)
            mid_active = ch->audio_active_frames;
            mid_peak = ch->audio_peak;
            std::printf("[A] mid tick=%zu pos=%llums audio_frames=%llu "
                        "active=%llu peak=%.4f\n",
                        tick, (unsigned long long)ch->position_ms,
                        (unsigned long long)ch->audio_frames,
                        (unsigned long long)ch->audio_active_frames,
                        ch->audio_peak);
        }
        final_frames = ch->audio_frames;
        final_active = ch->audio_active_frames;
    }
    check(finished, "A: EOF reached (playing cleared)");
    check(finish_events == 1, "A: exactly one finish event");
    check(finish_tick > 5600 && finish_tick < 5900,
          "A: OP ran to its ~91.8 s EOF (finish tick %zu)", finish_tick);
    check(mid_active > 1000, "A: audibly non-silent mid-flight (active frames)");
    check(mid_peak > 1e-3f, "A: audibly non-silent mid-flight (peak)");
    // movie_bai audio track: the container's nominal stream duration is
    // 91.677 s (4,042,95x frames) but the decodable sample run (incl. the
    // mp4 tail padding the engine does not trim, research/86 §7) reaches
    // ~91.82 s: measured final count 4,049,438 = 91.824 s, i.e. the audio
    // plays out right up to the video EOF (~91.825 s) and the channel then
    // retires with the movie (exactly one finish event).
    check(final_frames > 4047000 && final_frames < 4052000,
          "A: audio ran to the movie's end (final frames=%llu, want ~4,049,4xx)",
          (unsigned long long)final_frames);
    check(final_active > final_frames / 10,
          "A: audio still audibly active at EOF");
    const auto tail = v.poll_finish_events();
    check(tail.empty(), "A: finish queue drained");
    v.stop_overlay();
    check(!v.is_overlay_playing(), "A: stop clears the channel");
    std::printf("[A] engine phase done (finish at tick %zu, ~%.2fs virtual; "
                "frames=%llu active=%llu)\n",
                finish_tick, finish_tick * 0.016,
                (unsigned long long)final_frames,
                (unsigned long long)final_active);
    return g_failures;
}
} // namespace

// ---------------------------------------------------------------------------
// Phase B: real boot journey — the boot chain parks on movie_bai/logo.mp4
// (digitally silent AAC track), then reaches the title wait.
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

int journey_phase(std::shared_ptr<const oa::fs::IFileSystem> fs,
                  std::shared_ptr<oa::runtime::SaveStore> store) {
    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();

    struct Logo {
        size_t enter = 0;
        size_t exit = 0;
        bool released = false;
        bool decoded = false;
        bool audio_on = false;
        uint64_t frames = 0;
        uint64_t active = 0;
        float peak = 0.0f;
    } logo;
    size_t t = 0;
    const size_t kTickLimit = 6000; // 96 s virtual: boot + logo + title slack
    bool logo_park_open = false;
    for (t = 0; t < kTickLimit && !rt.exit_requested(); ++t) {
        const oa::runtime::WaitReason* w = rt.current_wait();
        const bool video_park =
            w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id == "video";
        if (video_park) {
            const auto vs = rt.video().state();
            const auto* ch = vs.overlay_video ? &*vs.overlay_video : nullptr;
            const std::string file = ch ? ch->file : std::string();
            if (!logo_park_open && file.find("logo.mp4") != std::string::npos) {
                logo_park_open = true;
                logo.enter = t;
                std::printf("[B] boot logo park start f=%zu file='%s'\n", t,
                            file.c_str());
            }
            if (ch) {
                if (ch->decoded) logo.decoded = true;
                if (ch->audio_on) logo.audio_on = true;
                logo.frames = ch->audio_frames;
                logo.active = ch->audio_active_frames;
                if (ch->audio_peak > logo.peak) logo.peak = ch->audio_peak;
            }
            rt.tick(16, oa::runtime::FrameInput());
            const oa::runtime::WaitReason* w2 = rt.current_wait();
            if (logo_park_open && !logo.released &&
                !(w2 && w2->kind == oa::runtime::WaitReason::Kind::Stop &&
                  w2->id == "video")) {
                logo.released = true;
                logo.exit = t;
                std::printf("[B] boot logo park end f=%zu after %zu ticks "
                            "(frames=%llu active=%llu peak=%.4f)\n",
                            t, t - logo.enter, (unsigned long long)logo.frames,
                            (unsigned long long)logo.active, logo.peak);
            }
            continue;
        }
        rt.tick(16, oa::runtime::FrameInput());
        // Past the boot videos: settle on whatever park the boot chain
        // reaches (this title parks on the title screen after the logo EOF).
        if (logo.released && t > logo.exit + 400) break;
    }
    std::printf("[B] end f=%zu wait=%s\n", t, wait_desc(rt.current_wait()));

    check(logo_park_open, "B: boot logo park seen (movie_bai/logo.mp4)");
    if (logo_park_open) {
        check(logo.released, "B: logo park released by its own EOF");
        check(logo.decoded, "B: logo really decodes");
        check(logo.audio_on, "B: logo container audio attached (silent track)");
        check(logo.exit - logo.enter >= 300,
              "B: logo played into its ~5 s (park %zu ticks)",
              logo.exit - logo.enter);
        check(logo.exit - logo.enter <= 340,
              "B: logo duration sane (~5.06 s = ~316 ticks; park %zu ticks)",
              logo.exit - logo.enter);
        check(logo.frames > 200000,
              "B: logo audio frames counted through its play (frames=%llu)",
              (unsigned long long)logo.frames);
        check(logo.active < 100,
              "B: logo track is digitally silent (active=%llu of %llu)",
              (unsigned long long)logo.active,
              (unsigned long long)logo.frames);
    }
    // The boot chain must have left the video domain (title wait or click
    // park) — the engine is not stuck in a video.
    const oa::runtime::WaitReason* wf = rt.current_wait();
    check(!wf || wf->kind != oa::runtime::WaitReason::Kind::Stop ||
              wf->id != "video",
          "B: boot released the video domain at the end (wait=%s)",
          wait_desc(wf));
    return g_failures;
}
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pfs_path = std::getenv("OA_TEST_HCT_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_HCT_PFS unset; skipping\n");
        return 77;
    }
    std::error_code ec;
    std::filesystem::path store_dir = std::filesystem::temp_directory_path(ec) /
                                      ("oa_hct_audio_" + std::to_string(::getpid()));
    std::filesystem::remove_all(store_dir, ec);
    auto store = std::make_shared<oa::runtime::DirSaveStore>(store_dir.string());
    // The archive's own real parent directory (loose movie_bai/) overlays
    // the pack: the mp4 movies only exist loose in this title (research/87).
    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, true);

    int rc = engine_phase(fs);
    if (rc != 0) return rc;
    rc = journey_phase(fs, store);
    if (g_failures == 0) std::printf("tm_movie_audio_test: all ok\n");
    return g_failures == 0 ? 0 : 1;
}
