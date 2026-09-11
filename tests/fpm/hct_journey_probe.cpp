// research/88 diagnostic journey probe over the HCT archive
// (灵感满溢的甜蜜创想凸): boot -> title -> UI click script -> walk story ->
// log every video window (channel/file/audio counters per 100 ticks) and the
// UI layers at each park. REPORT-ONLY (like emote_axis_dump): not registered
// as a ctest. OA_TEST_HCT_PFS gates 77.
//
// Driving knobs:
//   OA_HCT_PROBE_ENTRY=start01..05  Lua title_start2 shortcut injection
//                                   (bypasses the title UI, rr-style).
//   OA_HCT_PROBE_CLICKS="x,y;x,y"   UI journey: click stage points after the
//                                   title park (bt_start 122,135; char rows
//                                   167,381 / ~(280+128,381) / ...).
//   OA_HCT_PROBE_MAXV=n             stop after n video windows closed.
//   OA_HCT_PROBE_TICKS=n            hard tick cap (default 60000).
//   OA_HCT_PROBE_PARK=1             end the run at the first video park.
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
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_save.h"

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

struct VideoSample {
    std::string file;
    bool audio_on = false;
    bool decoded = false;
    uint64_t pos_ms = 0;
    uint64_t frames = 0;
    uint64_t active = 0;
    float peak = 0.0f;
};

VideoSample video_snapshot(oa::runtime::GameRuntime& rt) {
    VideoSample s;
    const auto vs = rt.video().state();
    if (vs.overlay_video && vs.overlay_video->playing) {
        const auto& ch = *vs.overlay_video;
        s.file = ch.file;
        s.audio_on = ch.audio_on;
        s.decoded = ch.decoded;
        s.pos_ms = ch.position_ms;
        s.frames = ch.audio_frames;
        s.active = ch.audio_active_frames;
        s.peak = ch.audio_peak;
    }
    for (const auto& [id, ch] : vs.video_layers) {
        (void)id;
        if (!ch.playing) continue;
        s.file = "layer:" + ch.file;
        s.audio_on = ch.audio_on;
        s.decoded = ch.decoded;
        s.pos_ms = ch.position_ms;
        s.frames = ch.audio_frames;
        s.active = ch.audio_active_frames;
        s.peak = ch.audio_peak;
        break;
    }
    return s;
}

bool any_video_playing(oa::runtime::GameRuntime& rt) {
    return !video_snapshot(rt).file.empty();
}

void dump_click_layers(oa::runtime::GameRuntime& rt, const char* tag, size_t cap) {
    size_t n = 0;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        const auto nm = h->params.find("name");
        const auto cl = h->params.find("click");
        const std::string key = k != h->params.end() ? k->second : std::string();
        const std::string name = nm != h->params.end() ? nm->second : std::string();
        const std::string click = cl != h->params.end() ? cl->second : std::string();
        std::printf("[UI] %s layer='%s' key='%s' name='%s' click='%.40s' "
                    "xy=(%.0f,%.0f %.0fx%.0f) vis=%d\n",
                    tag, l->id.c_str(), key.c_str(), name.c_str(), click.c_str(),
                    l->left, l->top, l->width, l->height, (int)l->visible);
        if (++n >= cap) break;
    }
    if (n == 0) std::printf("[UI] %s (no click layers)\n", tag);
}

bool page_revealed(oa::runtime::GameRuntime& rt) {
    for (const std::string& id : rt.text().visible_content_layers()) {
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        if (ml && (ml->reveal_pending || ml->reveal_index < ml->char_count))
            return false;
    }
    return true;
}
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pfs_path = std::getenv("OA_TEST_HCT_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_HCT_PFS unset; skipping\n");
        return 77;
    }
    const char* entry = std::getenv("OA_HCT_PROBE_ENTRY");
    const char* clicks_env = std::getenv("OA_HCT_PROBE_CLICKS");
    const int max_videos = std::getenv("OA_HCT_PROBE_MAXV")
                               ? std::atoi(std::getenv("OA_HCT_PROBE_MAXV"))
                               : 3;
    const size_t tick_cap = std::getenv("OA_HCT_PROBE_TICKS")
                                ? size_t(std::atol(std::getenv("OA_HCT_PROBE_TICKS")))
                                : 60000;
    const bool park_stop = std::getenv("OA_HCT_PROBE_PARK") != nullptr;

    struct Click {
        int x, y;
    };
    std::vector<Click> ui_clicks;
    if (clicks_env && *clicks_env) {
        std::string s = clicks_env;
        size_t p = 0;
        while (p < s.size()) {
            const size_t semi = s.find(';', p);
            const std::string tok = s.substr(p, semi == std::string::npos ? s.size() - p : semi - p);
            const size_t comma = tok.find(',');
            if (comma != std::string::npos)
                ui_clicks.push_back(
                    {std::atoi(tok.substr(0, comma).c_str()),
                     std::atoi(tok.substr(comma + 1).c_str())});
            if (semi == std::string::npos) break;
            p = semi + 1;
        }
        std::printf("[M] UI click script: %zu clicks\n", ui_clicks.size());
    }

    std::error_code ec;
    std::filesystem::path store_dir =
        std::filesystem::temp_directory_path(ec) /
        ("oa_hct_journey_" + std::to_string(::getpid()));
    std::filesystem::remove_all(store_dir, ec);
    auto store = std::make_shared<oa::runtime::DirSaveStore>(store_dir.string());
    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, true);
    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;

    bool saw_title_init = false;
    rt.interpreter().on_step =
        [&](const std::string&, size_t, const oa::runtime::Instruction& i) {
            const std::string* f = i.get("function");
            if (f && *f == "title_init") saw_title_init = true;
        };

    size_t t = 0;
    size_t clicks = 0;
    int videos_seen = 0;
    std::string last_file;
    bool injected = false;
    size_t ui_idx = 0;
    bool ui_clicking = false;
    bool answered_select = false;
    bool had_select = false;
    size_t ui_click_start = 0;
    size_t last_sample = 0;
    // video-window accounting
    std::string open_file;
    size_t open_tick = 0;
    size_t open_audio = 0;
    bool open_audio_on = false;
    bool reported_park = false;

    for (t = 0; t < tick_cap && !rt.exit_requested(); ++t) {
        const oa::runtime::WaitReason* w = rt.current_wait();
        const bool video_park =
            w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id == "video";
        const VideoSample vs = video_snapshot(rt);
        const std::string now_file = vs.file;

        // ---- open/close transitions ---------------------------------------
        if (now_file != open_file) {
            if (open_file.empty() && !now_file.empty()) {
                open_tick = t;
                open_audio = 0;
                open_audio_on = vs.audio_on;
                reported_park = false;
                std::printf("[M] f=%zu VIDEO OPEN '%s' wait=%s\n", t,
                            now_file.c_str(), wait_desc(w));
            } else if (!open_file.empty() && now_file.empty()) {
                ++videos_seen;
                const size_t dur = t - open_tick;
                std::printf("[M] f=%zu VIDEO CLOSED '%s' dur=%zuticks(~%.2fs) "
                            "audio_on=%d aframes=%llu(%.2fs) active=%llu peak=%.4f "
                            "wait=%s videos_seen=%d\n",
                            t, open_file.c_str(), dur, dur * 0.016,
                            (int)open_audio_on, (unsigned long long)vs.frames,
                            double(vs.frames) / 44100.0,
                            (unsigned long long)vs.active, vs.peak, wait_desc(w),
                            videos_seen);
                if (videos_seen >= max_videos) break;
            }
            open_file = now_file;
        }
        if (!now_file.empty() && t - last_sample >= 100) {
            last_sample = t;
            std::printf("[V] f=%zu file='%s' dec=%d audio=%d pos=%llums "
                        "aframes=%llu(%.2fs) active=%llu peak=%.4f wait=%s\n",
                        t, vs.file.c_str(), (int)vs.decoded, (int)vs.audio_on,
                        (unsigned long long)vs.pos_ms,
                        (unsigned long long)vs.frames, double(vs.frames) / 44100.0,
                        (unsigned long long)vs.active, vs.peak, wait_desc(w));
        }
        if (!now_file.empty() && open_file == now_file) {
            open_audio = vs.frames;
            open_audio_on = vs.audio_on;
        }

        // ---- park on the first video window ------------------------------
        if (park_stop && video_park && !reported_park) {
            reported_park = true;
            dump_click_layers(rt, "videopark", 15);
        }

        // ---- decide this tick's input -------------------------------------
        oa::runtime::FrameInput in;
        bool do_click = false;
        if (video_park) {
            // never click while a movie plays (skip guard)
        } else if (!answered_select && rt.scene().size() > 0) {
            // Auto-answer system select dialogs with the first row (the
            // "watch the digest" choice on the 共通38e entry) — a real user
            // picks an option with one click on a row. Row containers live
            // at <mw>.120.N (N = 1..9) with their top offset; the click
            // handler rows hang off the textured leaf below them.
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                const std::string& id = l->id;
                const std::string prefix = "1.80.120.";
                if (id.rfind(prefix, 0) != 0) continue;
                const std::string rest = id.substr(prefix.size());
                if (rest.size() != 1 || rest[0] < '1' || rest[0] > '9') continue;
                if (!l->visible) continue;
                const int cx = 640;
                const int cy = int(l->top) + 40;
                std::printf("[M] f=%zu SELECT row '%s' top=%.0f click (%d,%d)\n",
                            t, id.c_str(), l->top, cx, cy);
                in.left_click_edge = true;
                in.left_down = true;
                in.mouse_x = cx;
                in.mouse_y = cy;
                do_click = true;
                answered_select = true;
                ++clicks;
                break;
            }
        }
        if (do_click) {
            // consumed below (skip the rest of the input decision)
        } else if (ui_idx < ui_clicks.size() && ui_clicking &&
                   t >= ui_click_start + 60) {
            // click script: next point (after a settle gap)
            ++ui_idx;
            ui_clicking = false;
        }
        auto ui_ready = [&] {
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                const auto* h = rt.scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto nm = h->params.find("name");
                const bool named = nm != h->params.end() &&
                                   (nm->second == "select" || nm->second == "ttl1" ||
                                    nm->second == "adv");
                const auto k = h->params.find("key");
                const bool keyed = k != h->params.end() && !k->second.empty();
                if (l->visible && (named || keyed)) return true;
            }
            return false;
        };
        if (!video_park && ui_idx < ui_clicks.size() && !ui_clicking && ui_ready()) {
            const Click c = ui_clicks[ui_idx];
            in.left_click_edge = true;
            in.left_down = true;
            in.mouse_x = c.x;
            in.mouse_y = c.y;
            do_click = true;
            ui_clicking = true;
            ui_click_start = t;
            ++clicks;
            std::printf("[M] f=%zu UI click (%d,%d) wait=%s\n", t, c.x, c.y,
                        wait_desc(w));
            // click script stops once a video window opens
            if (ui_idx + 1 >= ui_clicks.size()) ui_idx = ui_clicks.size();
        } else if (!video_park && ui_idx >= ui_clicks.size() && entry &&
                   !injected) {
            // entry injection gate below (not input)
        } else if (!video_park && ui_idx >= ui_clicks.size() &&
                   (w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                          w->kind == oa::runtime::WaitReason::Kind::Generic0))) {
            // story walk: click once when the current page is fully revealed
            const bool text_on =
                rt.text().page_has_visible_text("1.80.mw.adv_adv") ||
                rt.text().page_has_visible_text("1.80.mw.adv");
            if (!text_on || page_revealed(rt)) {
                in.left_click_edge = true;
                in.left_down = true;
                in.mouse_x = 640;
                in.mouse_y = 600;
                do_click = true;
                ++clicks;
                if (clicks % 100 == 0)
                    std::printf("[M] f=%zu story click #%zu wait=%s\n", t, clicks,
                                wait_desc(w));
            }
        }
        rt.tick(16, in);
        (void)do_click;
        // select-dialog presence tracking (reset the answer latch)
        {
            bool sel_present = false;
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                const auto* h = rt.scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto nm = h->params.find("name");
                if (nm != h->params.end() && nm->second == "select" && l->visible) {
                    sel_present = true;
                    break;
                }
            }
            if (!sel_present && had_select) {
                answered_select = false;
                std::printf("[M] f=%zu select dialog closed\n", t);
            }
            had_select = sel_present;
        }

        // ---- title park reporting + injection -----------------------------
        if (saw_title_init && t == 900) {
            dump_click_layers(rt, "title@900", 25);
            std::printf("[M] f=%zu title snapshot wait=%s\n", t, wait_desc(rt.current_wait()));
        }
        if (t == 1200) {
            // geometry of every select-tree layer (digest dialog rows)
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                if (l->id.find("120") == std::string::npos) continue;
                std::printf("[SEL] id='%s' xy=(%.0f,%.0f %.0fx%.0f) vis=%d file='%s' clip=%d,%d,%d,%d\n",
                            l->id.c_str(), l->left, l->top, l->width, l->height,
                            (int)l->visible, l->file.c_str(), (int)l->clip_x,
                            (int)l->clip_y, (int)l->clip_w, (int)l->clip_h);
            }
        }
        if (entry && !injected && saw_title_init && t > 600 &&
            !any_video_playing(rt)) {
            const auto* w2 = rt.current_wait();
            const bool parked =
                w2 && w2->kind != oa::runtime::WaitReason::Kind::Stop &&
                !rt.transition().is_in_progress(rt.now_ms());
            if (parked) {
                std::printf("[M] f=%zu injecting title_start2('%s') wait=%s\n", t,
                            entry, wait_desc(w2));
                rt.interpreter().lua_bridge().run_code(
                    ("title_start2('" + std::string(entry) + "')").c_str(),
                    "hct_journey_probe");
                injected = true;
            }
        }
        if (videos_seen >= max_videos) break;
    }
    std::printf("[M] done f=%zu clicks=%zu videos=%d wait=%s exit=%d\n", t,
                clicks, videos_seen, wait_desc(rt.current_wait()),
                (int)rt.exit_requested());
    dump_click_layers(rt, "end", 12);
    return 0;
}
