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
#include "core/runtime/runtime_save.h"

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

namespace {
struct KeyRect { std::string key; int cx, cy; double w, h; };
std::vector<KeyRect> list_keys(oa::runtime::GameRuntime& rt) {
    std::vector<KeyRect> out;
    std::set<std::string> seen;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end()) continue;
        if (!seen.insert(k->second).second) continue;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (rt.scene().world_rect(l->id, w > 0 ? w : 80, hh > 0 ? hh : 40, &x0, &y0,
                                  &rw, &rh))
            out.push_back({k->second, int(x0 + rw / 2), int(y0 + rh / 2), rw, rh});
        else
            out.push_back({k->second, 0, 0, 0, 0});
    }
    return out;
}
bool click_key(oa::runtime::GameRuntime& rt, const std::string& key) {
    for (const auto& v : list_keys(rt))
        if (v.key == key) {
            oa::runtime::FrameInput cl;
            cl.left_click_edge = true;
            cl.left_down = true;
            cl.mouse_x = v.cx;
            cl.mouse_y = v.cy;
            rt.tick(16, cl);
            for (size_t f = 0; f < 10; ++f) rt.tick(16, {});
            std::printf("[es] clicked '%s' at (%d,%d)\n", key.c_str(), v.cx, v.cy);
            return true;
        }
    std::printf("[es] key '%s' NOT clickable\n", key.c_str());
    return false;
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
        if (const char* sd = std::getenv("OA_ES_SAVE")) {
            if (*sd) {
                auto store = std::make_shared<oa::runtime::DirSaveStore>(sd);
                rt.set_save_store(store);
            }
        }
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
        // ---- start the story by clicking the real start button ------------
        click_key(rt, "bt_start");
        for (size_t f = 0; f < 600; ++f) rt.tick(16, idle);
        std::printf("[am] after-start wait=%s\n", wait_str(rt.current_wait()).c_str());
        for (size_t f = 0; f < 120; ++f) rt.tick(16, idle);
        for (const auto& k : list_keys(rt))
            std::printf("[es] key '%s' at (%d,%d) %.0fx%.0f\n", k.key.c_str(), k.cx,
                        k.cy, k.w, k.h);
        std::printf("[es] text='%s' layers=%zu\n", page_sample(rt).c_str(),
                    rt.text().visible_content_layers().size());

        // ---- wait for the story body (no further clicks) -------------------
        bool body_reached = false;
        size_t f0 = 0;
        for (; f0 < 20000 && !rt.exit_requested(); ++f0) {
            oa::runtime::FrameInput in = idle;
            if (f0 > 120 && f0 % 180 == 0) {
                in.left_click_edge = true;
                in.left_down = true;
                in.mouse_x = 960;
                in.mouse_y = 700;
            }
            rt.tick(16, in);
            if (page_sample(rt) != "-") {
                body_reached = true;
                break;
            }
        }
        if (!body_reached)
            std::fprintf(stderr, "[es] body text not seen yet; keep walking\n");
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

        // ---- walk pages with Enter, dumping the emote state at each park ----
        const char* pb = std::getenv("OA_ES_PAGES");
        const size_t page_budget = pb && *pb ? size_t(std::atoi(pb)) : 80;
        size_t pages = 0;
        std::string last_text;
        for (size_t f = 0; f < 800000 && !rt.exit_requested() && pages < page_budget; ++f) {
            const auto* w = rt.current_wait();
            const std::string page = page_sample(rt);
            bool revealed = true;
            for (const std::string& id : rt.text().visible_content_layers()) {
                const oa::render::MessageLayer* ml = rt.text().layer(id);
                if (ml && (ml->reveal_pending || ml->reveal_index < ml->char_count))
                    revealed = false;
            }
            if (is_story_park(w)) {
              if (revealed && !page.empty() && page != "-" && page != last_text) {
                last_text = page;
                ++pages;
                std::printf("[es] PAGE %zu '%s'\n", pages, page.c_str());
                for (const auto& [lid, st] : rt.emote_layers()) {
                    if (!st.player) continue;
                    auto v = st.player->variables();
                    auto g = [&](const char* k) {
                        auto it = v.find(k);
                        return it == v.end() ? 0.0 : it->second;
                    };
                    std::printf("[es]   %s eye=%.2f brow=%.2f browsp=%.2f mouth=%.2f "
                                "mouthsp=%.2f cheek=%.2f talk=%.2f eyeUD=%.2f eyeLR=%.2f "
                                "hitomi=%.2f\n",
                                lid.c_str(), g("face_eye_open"), g("face_eyebrow"),
                                g("face_eyebrow_sp"), g("face_mouth"), g("face_mouth_sp"),
                                g("face_cheek"), g("face_talk"), g("face_eye_UD"),
                                g("face_eye_LR"), g("face_hitomi_sp"));
                }
              }
                oa::runtime::FrameInput enter;
                enter.key_down_edges = {13};
                enter.keys_down.insert(13);
                rt.tick(16, enter);
                for (int k = 0; k < 30 && !rt.exit_requested(); ++k) rt.tick(16, idle);
            } else {
                rt.tick(16, idle);
            }
        }
        std::printf("[es] pages=%zu\n", pages);

    } catch (const std::exception& e) {
        std::fprintf(stderr, "RUNTIME EXCEPTION: %s\n", e.what());
        return 1;
    }
    return 0;
}
