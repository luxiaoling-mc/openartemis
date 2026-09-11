// research/83: real-fpm automode voice-sync probe — drives fpm/root.pfs
// headless into the story body, enables automode through the REAL Lua path
// (automode_start(), i.e. the same code the MW auto button runs), then
// releases every input and ticks. Logs every page park release with the
// playing sound channels at release time (id + started_at + current frame).
//
// Purpose: evidence for "automode must not page-turn while the current
// line's voice is still playing" — pre-fix runs should show releases while a
// voice channel is playing (the old fixed-900 ms timer), post-fix runs
// should release only after the voice channel ended.
//
// Env: OA_TEST_FPM_PFS (exit 77 when unset); OA_AM_PROBE_PAGES = page budget.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"

namespace {
const char* kind_str(oa::runtime::WaitReason::Kind k) {
    using K = oa::runtime::WaitReason::Kind;
    switch (k) {
        case K::Generic: return "generic";
        case K::Generic0: return "wt0";
        case K::Timed: return "timed";
        case K::Stop: return "stop";
        case K::Se: return "se";
        case K::VideoLayer: return "video";
        case K::ScenarioTween: return "stween";
        case K::KeyWait: return "key";
    }
    return "?";
}
std::string wait_str(const oa::runtime::WaitReason* w) {
    if (!w) return "none";
    std::string s = kind_str(w->kind);
    if (!w->id.empty()) s += "('" + w->id + "')";
    return s;
}
bool is_story_park(const oa::runtime::WaitReason* w) {
    using K = oa::runtime::WaitReason::Kind;
    return w && (w->kind == K::Generic || w->kind == K::Generic0);
}
std::string page_sample(oa::runtime::GameRuntime& rt) {
    const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
    if (!ml) return "-";
    for (const auto& u : ml->page)
        if (!u.data.empty()) return u.data.substr(0, 22);
    return "-";
}
// "id@started_at" of every currently playing se/voice channel.
std::string playing_now(oa::runtime::GameRuntime& rt) {
    std::string out;
    const auto& st = rt.audio().state();
    for (const auto& [id, c] : st.se_channels) {
        (void)id;
        if (!c.playing) continue;
        if (!out.empty()) out += ",";
        out += "se:" + c.id + "@" + std::to_string(c.started_at_ms);
    }
    for (const auto& [id, c] : st.voice_channels) {
        (void)id;
        if (!c.playing) continue;
        if (!out.empty()) out += ",";
        out += "vo:" + c.id + "@" + std::to_string(c.started_at_ms);
    }
    return out;
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_FPM_PFS unset; skipping\n");
        return 77;
    }
    const long page_budget = std::getenv("OA_AM_PROBE_PAGES")
                                 ? std::atol(std::getenv("OA_AM_PROBE_PAGES"))
                                 : 12;
    std::string target;
    try {
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        rt.boot_project();
        oa::runtime::FrameInput idle;

        // ---- boot to title, click bt_start (title_drain_test pattern) -----
        bool saw_title_init = false;
        rt.interpreter().on_step = [&](const std::string&, size_t,
                                       const oa::runtime::Instruction& i) {
            const std::string* f = i.get("function");
            if (f && *f == "title_init") saw_title_init = true;
        };
        size_t boot = 0;
        for (; boot < 2400 && !rt.exit_requested(); ++boot) {
            rt.tick(16, idle);
            if (!saw_title_init) continue;
            const auto* w = rt.current_wait();
            if (!(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty()))
                continue;
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                const auto* h = rt.scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto k = h->params.find("key");
                if (k == h->params.end() || k->second != "bt_start") continue;
                const auto n = h->params.find("name");
                if (n != h->params.end() && n->second == "ttl1") {
                    target = l->id;
                    break;
                }
            }
            if (target.empty()) {
                for (const oa::render::Layer* l : rt.scene().draw_order()) {
                    const auto* h = rt.scene().find_event_handler(l->id, "click");
                    if (!h) continue;
                    const auto k = h->params.find("key");
                    if (k == h->params.end() || k->second != "bt_start") continue;
                    target = l->id;
                    break;
                }
            }
            if (!target.empty()) break;
        }
        std::printf("[am] boot=%zu target=%s wait=%s\n", boot, target.c_str(),
                    wait_str(rt.current_wait()).c_str());
        if (target.empty()) {
            std::fprintf(stderr, "automode_probe: title start button not reached\n");
            return 1;
        }
        for (size_t stl = 0; stl < 4000 && !rt.exit_requested(); ++stl) {
            rt.tick(16, idle);
            const auto* wq = rt.current_wait();
            if (!(wq && wq->kind == oa::runtime::WaitReason::Kind::Stop && wq->id.empty()))
                continue;
            if (!rt.transition().is_in_progress(rt.now_ms())) break;
        }
        // click the start button (edge held one frame, then released)
        {
            bool done = false;
            for (size_t f = 0; f < 900 && !rt.exit_requested(); ++f) {
                oa::runtime::FrameInput in;
                if (!done) {
                    in.left_click_edge = true;
                    in.left_down = true;
                    done = true;
                }
                rt.tick(16, in);
                if (rt.scene().find("500") == nullptr) break;
            }
        }
        // ---- start the story: click the parked button candidates while the
        //      title layer (500) exists, then idle until the body text shows
        //      (ux_journey_test pattern) ------------------------------------
        {
            oa::runtime::FrameInput in;
            const int cands[][2] = {{140, 136}, {80, 124}, {200, 150}};
            for (int ci = 0; ci < 3 && rt.scene().find("500"); ++ci) {
                in.left_click_edge = true;
                in.left_down = true;
                in.mouse_x = cands[ci][0];
                in.mouse_y = cands[ci][1];
                for (size_t f = 0; f < 500 && !rt.exit_requested(); ++f) {
                    rt.tick(16, in);
                    in.left_click_edge = false;
                    in.left_down = false;
                    in.mouse_x = -1;
                    in.mouse_y = -1;
                    if (rt.scene().find("500") == nullptr) break;
                }
            }
        }
        std::printf("[am] after-start wait=%s\n", wait_str(rt.current_wait()).c_str());

        // ---- wait for the story body (no further clicks) -------------------
        bool body_reached = false;
        size_t f0 = 0;
        for (; f0 < 8000 && !rt.exit_requested(); ++f0) {
            rt.tick(16, idle);
            if (rt.text().page_has_visible_text("1.80.mw.adv_adv")) {
                body_reached = true;
                break;
            }
        }
        if (!body_reached) {
            std::fprintf(stderr, "automode_probe: story body not reached\n");
            return 1;
        }
        std::printf("[am] BODY at frame=%zu wait=%s page='%s'\n", f0,
                    wait_str(rt.current_wait()).c_str(), page_sample(rt).c_str());
        // park the click machine at the current body page (click once if the
        // body text appeared mid-block, then settle on a fully revealed park)
        if (!is_story_park(rt.current_wait())) {
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = 640;
            cl.mouse_y = 600;
            for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
                rt.tick(16, f == 0 ? cl : idle);
                if (is_story_park(rt.current_wait())) break;
            }
        }
        // Wait for the current page to finish revealing (no clicks), then
        // enable automode through the real Lua entry (MW auto button path).
        for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            bool rev_done = true;
            for (const std::string& id : rt.text().visible_content_layers()) {
                const oa::render::MessageLayer* ml = rt.text().layer(id);
                if (ml && (ml->reveal_pending || ml->reveal_index < ml->char_count))
                    rev_done = false;
            }
            if (rev_done) break;
        }
        std::printf("[am] pre-auto frame=%zu page='%s' wait=%s playing=[%s]\n", f0,
                    page_sample(rt).c_str(), wait_str(rt.current_wait()).c_str(),
                    playing_now(rt).c_str());
        rt.interpreter().lua_bridge().run_code(
            "if type(automode_start)=='function' then automode_start() end",
            "automode_probe");
        for (size_t f = 0; f < 60; ++f) rt.tick(16, idle);
        std::printf("[am] automode active=%d wait=%s playing=[%s]\n",
                    (int)rt.automode_active(), wait_str(rt.current_wait()).c_str(),
                    playing_now(rt).c_str());

        // ---- observe automode paging with zero input -----------------------
        size_t parked_at = 0; // frame the current story park began
        std::set<std::string> park_voices; // channels playing when parked
        size_t pages = 0;
        size_t rel_while_voice = 0;
        const size_t kMaxFrames = 6000 * 60; // safety
        for (size_t f = 0; f < kMaxFrames && !rt.exit_requested(); ++f) {
            const auto* w = rt.current_wait();
            const std::string page = page_sample(rt);
            if (is_story_park(w) && parked_at == 0) {
                parked_at = f;
                park_voices.clear();
                const auto& st = rt.audio().state();
                for (const auto& [id, c] : st.se_channels)
                    if (c.playing) park_voices.insert("se:" + id);
                for (const auto& [id, c] : st.voice_channels)
                    if (c.playing) park_voices.insert("vo:" + id);
                if (pages < size_t(page_budget)) {
                    std::printf("[am] PARK frame=%zu page='%s' parked_with=[%s]\n", f,
                                page.c_str(), playing_now(rt).c_str());
                }
            }
            if (parked_at != 0 && !is_story_park(w)) {
                const uint64_t dur = uint64_t(f - parked_at) * 16;
                // channels that were playing when this park began AND are
                // still playing now -> the page turned while a voice ran.
                std::string still;
                const auto& st = rt.audio().state();
                for (const auto& id : park_voices) {
                    const bool is_vo = id.rfind("vo:", 0) == 0;
                    const std::string cid = id.substr(3);
                    const auto& map = is_vo ? st.voice_channels : st.se_channels;
                    const auto it = map.find(cid);
                    if (it != map.end() && it->second.playing) {
                        if (!still.empty()) still += ",";
                        still += id + "@" + std::to_string(it->second.started_at_ms);
                    }
                }
                const bool while_voice = !still.empty();
                if (while_voice) ++rel_while_voice;
                if (++pages <= size_t(page_budget) || while_voice) {
                    std::printf("[am] RELEASE frame=%zu park_ms=%llu page='%s' "
                                "park_voice_still=[%s] playing=[%s] wait=%s\n",
                                f, (unsigned long long)dur, page.c_str(), still.c_str(),
                                playing_now(rt).c_str(), wait_str(w).c_str());
                }
                parked_at = 0;
                park_voices.clear();
                if (pages >= size_t(page_budget)) break;
            }
            // bail out when a non-story park persists (title/stop end state)
            if (w && !is_story_park(w) && parked_at == 0 && f > 2000 &&
                w->kind == oa::runtime::WaitReason::Kind::Stop) {
                break;
            }
            rt.tick(16, idle);
        }
        std::printf("[am] pages=%zu releases_while_park_voice_playing=%zu\n", pages,
                    rel_while_voice);
        // research/83 acceptance: with the default conf.autostop=1 (voice-wait
        // on), every automode page flip must wait for the parked line's voice
        // channel to stop — zero releases while a park voice still plays.
        if (pages < 3) {
            std::fprintf(stderr, "automode_probe: too few automode page turns "
                                 "(%zu) for a verdict\n",
                         pages);
            return 1;
        }
        if (rel_while_voice != 0) {
            std::fprintf(stderr,
                         "automode_probe: %zu page release(s) while the parked "
                         "line's voice was still playing (expected 0)\n",
                         rel_while_voice);
            return 1;
        }
        std::printf("automode_probe: ok (%zu automode pages, 0 releases while "
                    "voice playing)\n",
                    pages);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
    return 0;
}
