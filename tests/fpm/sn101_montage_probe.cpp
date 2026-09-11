// research/101 diagnostic (report-only; NOT a ctest): snll (サクラノ詩)
// prologue recap-montage probe on the real runtime — headless, virtual
// 16 ms ticks, so every script wait runs at its nominal duration and the
// montage timeline is fps-independent (the windowed Xvfb+llvmpipe host
// collapses to ~0.5 fps during the montage because of the layer-video
// mask bakes, making windowed dwell times meaningless).
//
// Journey: boot (warm save root -> no language page) -> title bt_start ->
// the opening pages -> the auto recap montage (script/c20_01a.ast block
// 0003_00005: 回想_白枠 CG lv700 + ノイズa movie lv610 with its _m mask
// partner + ~60 hard bg/fg cuts each followed by extrans(500) + the
// シャッター夜_縦 wipe) -> the 敬启 letter page, where it probes
// click-advance liveness (research/101 symptom 4) and walks a few letter
// pages. Along the way it logs:
//   - the per-image bg-signature timeline (engine frames + clock, dwell),
//     with the wait-kind skeleton (nominal durations) — symptom 2 rhythm;
//   - the BGM/SE bus + host decode-player state (bgm34/bgm40/se809),
//     symptom 1;
//   - the strip channel state incl. mask_on/bound and, when bound, the
//     composited veil statistics of its frames (alpha>64 coverage etc.) —
//     symptom 3 fog-flash visibility evidence;
//   - channel lifecycle vs scene deletion (title sakura channel must stop
//     when the story wipes the title group; the strip channel must stop at
//     its cgdel) — the perpetual-pump fix (symptom 2 perf side).
//
// Env: OA_TEST_SNLL_PFS = snll root.pfs (sibling root.pfs.000 volume + loose
// movie/ dir resolve through the engine fs). OA_SN101_SAVESRC = an optional
// warm save-root directory (a windowed run that already picked a language);
// without it the first-boot language page stalls the journey (probe skips).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
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
#include "core/render/layer.h"
#include "core/render/layer_kind.h" // layer-model S5: 绑定判定经内容角色

namespace {
using K = oa::runtime::WaitReason::Kind;
const char* ws(const oa::runtime::WaitReason* w) {
    if (!w) return "none";
    switch (w->kind) {
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
bool story_wait(const oa::runtime::WaitReason* w) {
    return w && (w->kind == K::Generic || w->kind == K::Generic0);
}
std::string sample(oa::runtime::GameRuntime& rt) {
    for (const std::string& id : rt.text().visible_content_layers()) {
        if (id.find(".mw.") == std::string::npos) continue;
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        if (!ml) continue;
        for (const auto& u : ml->page) {
            if (u.kind != oa::render::PageUnit::Kind::Newline && !u.data.empty())
                return u.data.substr(0, 14);
        }
    }
    return std::string();
}
bool text_ready(oa::runtime::GameRuntime& rt) {
    for (const std::string& id : rt.text().visible_content_layers()) {
        if (id.find(".mw.") == std::string::npos) continue;
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        if (ml && ml->char_count > 0 && !ml->reveal_pending &&
            ml->reveal_index >= ml->char_count)
            return true;
    }
    return false;
}
bool has_key(oa::runtime::GameRuntime& rt, const char* key) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k != h->params.end() && k->second == key) return true;
    }
    return false;
}
bool row_center(oa::runtime::GameRuntime& rt, const std::string& key, int* x, int* y) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end() || k->second != key) continue;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 60.0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 60.0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (!rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) return false;
        *x = int(x0 + rw / 2);
        *y = int(y0 + rh / 2);
        return true;
    }
    return false;
}
// Minimal host emulation for [trans]/[flip] drained events: the windowed
// host (main.cpp -> RenderEngine::process_event) starts transitions when it
// applies drained events; headless probes must do the same or every
// Stop{"trans"} wait releases instantly (no transition state exists) and
// the montage runs ~16x faster than its script timing. Other drained event
// kinds are host-visual only and are ignored here.
void host_apply_drained(oa::runtime::GameRuntime& rt) {
    for (const oa::runtime::Event& e : rt.drain_events()) {
        if (e.kind == oa::runtime::Event::Kind::Trans) {
            rt.transition_begin(e.params); // capture callback marks captured
        }
    }
}
void host_step(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& in) {
    rt.tick(16, in);
    host_apply_drained(rt);
}

void tick_idle(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle, size_t n) {
    for (size_t f = 0; f < n && !rt.exit_requested(); ++f) host_step(rt, idle);
}
void click_at(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle, int x, int y,
              size_t after = 200) {
    oa::runtime::FrameInput cl;
    cl.left_click_edge = true;
    cl.left_down = true;
    cl.mouse_x = x;
    cl.mouse_y = y;
    host_step(rt, cl);
    host_apply_drained(rt);
    tick_idle(rt, idle, after);
}
bool park_story(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                size_t budget = 3000) {
    for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
        host_step(rt, idle);
        if (story_wait(rt.current_wait()) && text_ready(rt)) return true;
    }
    return false;
}

// lv0 story-bg signature (files of .bg. layers under 1.0.bx.by.bs).
std::string bg_sig(oa::runtime::GameRuntime& rt) {
    std::string out;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        if (l->id.rfind("1.0.bx.by.bs.", 0) != 0) continue;
        if (l->id.find(".bg.") == std::string::npos) continue;
        if (l->file.empty()) continue;
        if (!out.empty()) out += "|";
        out += l->id + "=" + l->file;
    }
    return out;
}
void audio_dump(oa::runtime::GameRuntime& rt, const char* tag) {
    const auto st = rt.audio().state();
    if (st.bgm_channel) {
        const auto& b = *st.bgm_channel;
        std::printf("[snp] %s bgm='%s' playing=%d loop=%d gain=%d fade=%s "
                    "players=%zu clock=%llu\n",
                    tag, b.file.c_str(), b.playing ? 1 : 0, b.loop_play ? 1 : 0,
                    b.raw_gain, b.fade ? "Y" : "-",
                    rt.media_players().active_players(),
                    (unsigned long long)st.clock_ms);
    } else {
        std::printf("[snp] %s bgm=(none) players=%zu\n", tag,
                    rt.media_players().active_players());
    }
    int n = 0;
    for (const auto& [id, ch] : rt.audio().state().se_channels) {
        if (!ch.playing) continue;
        if (n++ < 8)
            std::printf("[snp] %s se id='%s' file='%s' loop=%d\n", tag,
                        id.c_str(), ch.file.c_str(), ch.loop_play ? 1 : 0);
    }
}
void video_dump(oa::runtime::GameRuntime& rt, const char* tag, bool veil_stats) {
    const auto st = rt.video().state();
    for (const auto& [id, ch] : st.video_layers) {
        bool bound = false;
        // S5 (research/108): 绑定判定 = 层的内容来源状态(视频帧域 = Video 角色);
        // 保留命名空间 file 比对已删除。
        for (const oa::render::Layer* l : rt.scene().draw_order()) {
            if (l->id == id && l->visible &&
                oa::render::kind_of(*l) == oa::render::LayerKind::Video)
                bound = true;
        }
        std::string veil;
        if (veil_stats && ch.playing && ch.decoded) {
            int w = 0, h = 0;
            const uint8_t* px = nullptr;
            if (rt.video().video_frame(id, &w, &h, &px, nullptr) && px) {
                uint64_t a32 = 0, a64 = 0, a128 = 0;
                double asum = 0;
                const size_t n = size_t(w) * size_t(h);
                for (size_t i = 0; i < n; ++i) {
                    const int a = px[i * 4 + 3];
                    if (a > 32) ++a32;
                    if (a >= 64) ++a64;
                    if (a >= 128) ++a128;
                    asum += a;
                }
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              " veil: a>32=%.1f%% a>=64=%.1f%% a>=128=%.2f%% "
                              "mean=%.1f",
                              100.0 * double(a32) / double(n),
                              100.0 * double(a64) / double(n),
                              100.0 * double(a128) / double(n),
                              asum / double(n));
                veil = buf;
            }
        }
        std::printf("[snp] %s video id='%s' file='%s' playing=%d decoded=%d "
                    "loop=%d mask_on=%d bound=%d pos=%llums%s\n",
                    tag, id.c_str(), ch.file.c_str(), ch.playing ? 1 : 0,
                    ch.decoded ? 1 : 0, ch.loop_play ? 1 : 0, ch.mask_on ? 1 : 0,
                    bound ? 1 : 0, (unsigned long long)ch.position_ms,
                    veil.c_str());
    }
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_SNLL_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_SNLL_PFS unset; skipping\n");
        return 77;
    }
    namespace fsf = std::filesystem;
    std::error_code ec;
    const fsf::path store_dir =
        fsf::temp_directory_path(ec) / ("oa_sn101_" + std::to_string(::getpid()));
    fsf::remove_all(store_dir, ec);
    if (const char* src = std::getenv("OA_SN101_SAVESRC"); src && *src) {
        fsf::copy(src, store_dir,
                  fsf::copy_options::recursive | fsf::copy_options::update_existing, ec);
    }
    auto store = std::make_shared<oa::runtime::DirSaveStore>(store_dir.string());

    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();
    rt.set_transition_capture_callback([&]() { rt.mark_transition_captured(); });
    oa::runtime::FrameInput idle;

    // ---- boot -> title ----------------------------------------------------
    int f_title = -1;
    for (size_t f = 0; f < 60000 && !rt.exit_requested(); ++f) {
        host_step(rt, idle);
        if (has_key(rt, "bt_start") && rt.current_wait() &&
            rt.current_wait()->kind == K::Stop && rt.current_wait()->id.empty()) {
            f_title = int(f);
            break;
        }
    }
    if (f_title < 0) {
        std::printf("[snp] FAIL: title never parked (sample='%s' wait=%s)\n",
                    sample(rt).c_str(), ws(rt.current_wait()));
        return 1;
    }
    std::printf("[snp] title parked f=%d (sample='%s')\n", f_title, sample(rt).c_str());
    audio_dump(rt, "title");

    // ---- new game -> pre-montage page walk --------------------------------
    int cx = 0, cy = 0;
    if (!row_center(rt, "bt_start", &cx, &cy)) {
        cx = 721;
        cy = 840;
    }
    click_at(rt, idle, cx, cy, 400);
    int pre_parks = 0;
    std::string pre_last;
    bool montage = false;
    for (size_t f = 0; f < 40000 && !rt.exit_requested() && !montage; ++f) {
        host_step(rt, idle);
        const std::string smp = sample(rt);
        if (smp != pre_last && !smp.empty()) {
            pre_last = smp;
            ++pre_parks;
            std::printf("[snp] pre park %d text='%s' wait=%s\n", pre_parks,
                        smp.c_str(), ws(rt.current_wait()));
        }
        for (const oa::render::Layer* l : rt.scene().draw_order()) {
            if (l->file.find("回想") != std::string::npos) montage = true;
        }
        for (const auto& [id, ch] : rt.video().state().video_layers) {
            if (ch.file.find("ノイズ") != std::string::npos && ch.playing)
                montage = true;
        }
        if (montage) break;
        if (story_wait(rt.current_wait()) && text_ready(rt)) {
            click_at(rt, idle, 960, 930, 60);
        }
    }
    if (!montage) {
        std::printf("[snp] FAIL: montage never reached (parks=%d)\n", pre_parks);
        return 1;
    }
    std::printf("[snp] MONTAGE START f=%llu (pre parks=%d)\n",
                (unsigned long long)rt.now_ms() / 16, pre_parks);
    audio_dump(rt, "mont-start");
    video_dump(rt, "mont-start", false);

    // ---- the montage ------------------------------------------------------
    struct {
        int imgs = 0;
        std::string bg_last;
        uint64_t bg_last_f = 0;
        uint64_t bg_last_ms = 0;
        std::string bgm_last;
        int last_wk = -999;
        std::string last_wid;
        uint64_t vdump_at = 0;
        int veil_samples = 0;
        double veil_total = 0;
        double veil_min = 0, veil_max = 0, mean_min = 1e9, mean_max = 0;
        uint64_t mont_f0 = 0, mont_ms0 = 0;
    } m;
    m.mont_f0 = rt.now_ms() / 16;
    m.mont_ms0 = rt.now_ms();
    for (size_t f = 0; f < 400000 && !rt.exit_requested(); ++f) {
        host_step(rt, idle);
        const uint64_t fr = rt.now_ms() / 16;
        // leave when a fresh story text parks (the letter page after the wipe)
        const std::string smp = sample(rt);
        if (!smp.empty() && smp != pre_last) {
            std::printf("[snp] MONTAGE END f=%llu imgs=%d veil_cov>=64: "
                        "avg=%.2f%% min=%.2f%% max=%.2f%% (samples=%d) mean-a: "
                        "min=%.1f max=%.1f text='%s'\n",
                        (unsigned long long)fr, m.imgs,
                        m.veil_samples ? m.veil_total / m.veil_samples : 0.0,
                        m.veil_min, m.veil_max, m.veil_samples, m.mean_min,
                        m.mean_max, smp.c_str());
            break;
        }
        // wait skeleton
        const auto* w = rt.current_wait();
        const int wk = w ? int(w->kind) : -1;
        const std::string wid = w ? w->id : std::string();
        if (wk != m.last_wk || wid != m.last_wid) {
            m.last_wk = wk;
            m.last_wid = wid;
            uint64_t dur = 0;
            if (w && w->kind == K::Timed) dur = w->milliseconds;
            std::printf("[snp] f=%llu wait-> %s id='%s' dur=%llums\n",
                        (unsigned long long)fr, ws(w), wid.c_str(),
                        (unsigned long long)dur);
        }
        // optional: click DURING the montage (user-mash variant) at fixed
        // image counts — input=1 transitions are click-skippable; later
        // parks must stay alive (symptom-4 breadth).
        if (std::getenv("OA_SN101_CLICKDURING") &&
            (m.imgs == 8 || m.imgs == 16 || m.imgs == 24 || m.imgs == 32 ||
             m.imgs == 40 || m.imgs == 48)) {
            click_at(rt, idle, 960, 930, 20);
            std::printf("[snp] click-during-montage at img %d f=%llu\n", m.imgs,
                        (unsigned long long)fr);
        }
        // image signature changes
        const std::string sig = bg_sig(rt);
        if (sig != m.bg_last && !sig.empty()) {
            const uint64_t nowms = rt.now_ms();
            if (m.imgs > 0)
                std::printf("[snp] img %3d f=%llu t=%llums dwell=%llums wait=%s\n",
                            m.imgs, (unsigned long long)fr,
                            (unsigned long long)nowms,
                            (unsigned long long)(nowms - m.bg_last_ms), ws(w));
            else
                std::printf("[snp] img 0 f=%llu t=%llums wait=%s\n",
                            (unsigned long long)fr, (unsigned long long)nowms,
                            ws(w));
            m.bg_last = sig;
            m.bg_last_ms = nowms;
            ++m.imgs;
        }
        // bgm/SE changes
        const auto st = rt.audio().state();
        std::string bgmfile;
        if (st.bgm_channel) bgmfile = st.bgm_channel->file;
        if (bgmfile != m.bgm_last) {
            if (!m.bgm_last.empty() || !bgmfile.empty())
                std::printf("[snp] f=%llu bgm '%s' -> '%s' playing=%d\n",
                            (unsigned long long)fr, m.bgm_last.c_str(),
                            bgmfile.c_str(),
                            st.bgm_channel && st.bgm_channel->playing ? 1 : 0);
            m.bgm_last = bgmfile;
        }
        // periodic channel + veil stats
        if (fr >= m.vdump_at) {
            m.vdump_at = fr + 400; // every ~6.4 s of engine time
            video_dump(rt, "mont", true);
        }
        // whole-montage veil census: sample every 100 engine frames; keep
        // min/mean/max of the per-frame visible coverage (alpha>=64) plus
        // the per-frame mean alpha.
        if (fr % 100 == 0) {
            for (const auto& [id, ch] : rt.video().state().video_layers) {
                if (ch.file.find("ノイズ") == std::string::npos || !ch.playing ||
                    !ch.decoded)
                    continue;
                int w = 0, h = 0;
                const uint8_t* px = nullptr;
                if (rt.video().video_frame(id, &w, &h, &px, nullptr) && px) {
                    const size_t n = size_t(w) * size_t(h);
                    uint64_t a64 = 0;
                    double asum = 0;
                    for (size_t i = 0; i < n; ++i) {
                        if (px[i * 4 + 3] >= 64) ++a64;
                        asum += px[i * 4 + 3];
                    }
                    const double cov = 100.0 * double(a64) / double(n);
                    const double mean = asum / double(n);
                    m.veil_total += cov;
                    if (m.veil_samples == 0 || cov < m.veil_min) m.veil_min = cov;
                    if (cov > m.veil_max) m.veil_max = cov;
                    if (m.veil_samples == 0 || mean < m.mean_min) m.mean_min = mean;
                    if (mean > m.mean_max) m.mean_max = mean;
                    ++m.veil_samples;
                }
                break;
            }
        }
        if (fr - m.mont_f0 > 60 * 60 * 4) {
            std::printf("[snp] montage cap hit imgs=%d\n", m.imgs);
            break;
        }
    }
    audio_dump(rt, "mont-end");
    video_dump(rt, "mont-end", false);

    // ---- letter page input-liveness ---------------------------------------
    std::printf("[snp] POST park text='%s' wait=%s\n", sample(rt).c_str(),
                ws(rt.current_wait()));
    (void)park_story(rt, idle, 6000);
    std::printf("[snp] POST parked text='%s' wait=%s clickable-keys:\n",
                sample(rt).c_str(), ws(rt.current_wait()));
    {
        std::map<std::string, int> keys;
        for (const oa::render::Layer* l : rt.scene().draw_order()) {
            const auto* h = rt.scene().find_event_handler(l->id, "click");
            if (!h) continue;
            const auto k = h->params.find("key");
            if (k != h->params.end()) ++keys[k->second];
        }
        int n = 0;
        for (const auto& [k, c] : keys)
            if (n++ < 40) std::printf("  key='%s' x%d\n", k.c_str(), c);
    }
    // probe clicks: park must advance to a new text
    int post_parks = 1;
    std::string post_last = sample(rt);
    int fails = 0;
    for (int probe = 0; probe < 8 && !rt.exit_requested(); ++probe) {
        click_at(rt, idle, 960, 930, 400);
        bool advanced = false;
        for (size_t f = 0; f < 2400 && !rt.exit_requested(); ++f) {
            host_step(rt, idle);
            const std::string smp = sample(rt);
            if (smp != post_last && !smp.empty()) {
                post_last = smp;
                ++post_parks;
                std::printf("[snp] POST advance OK -> park %d text='%s' wait=%s\n",
                            post_parks, smp.c_str(), ws(rt.current_wait()));
                advanced = true;
                break;
            }
        }
        if (!advanced) {
            ++fails;
            std::printf("[snp] probe %d: no advance (text='%s' wait=%s)\n", probe,
                        post_last.c_str(), ws(rt.current_wait()));
            if (fails >= 2) break;
        }
        if (post_parks >= 5) break;
    }
    std::printf("[snp] RESULT post_parks=%d fails=%d -> %s\n", post_parks, fails,
                post_parks >= 3 ? "INPUT-OK" : (fails ? "INPUT-DEAD" : "SHORT"));

    // ---- letter phase: walk pages with bg rows; dock button + Esc probes --
    // (symptom-4 breadth: are the dock/menu buttons alive after the montage?
    //  symptom-5 context: do letter-page bg swaps run any transition?)
    int dock_ok = 0;
    bool dock_ui_seen = false;
    for (int p = 0; p < 14 && !rt.exit_requested(); ++p) {
        if (!park_story(rt, idle, 3000)) break;
        const std::string smp = sample(rt);
        std::printf("[snp] letter park %d text='%s' wait=%s bg-sig=%s\n", p + 1,
                    smp.c_str(), ws(rt.current_wait()), bg_sig(rt).c_str());
        if (p + 1 == 3 && !dock_ui_seen) {
            int dx = 0, dy = 0;
            if (row_center(rt, "bt_save", &dx, &dy)) {
                const size_t base = rt.scene().size();
                click_at(rt, idle, dx, dy, 300);
                bool opened = false;
                for (size_t f = 0; f < 1200 && !rt.exit_requested(); ++f) {
                    host_step(rt, idle);
                    const auto* w2 = rt.current_wait();
                    if (rt.scene().size() > base + 40 || rt.scene().find("500.bt.1.bg.0") ||
                        (w2 && w2->kind == K::Stop && w2->id.empty())) {
                        opened = true;
                        break;
                    }
                }
                dock_ok = opened ? 1 : 0;
                dock_ui_seen = true;
                std::printf("[snp] dock bt_save click -> %s (scene %zu -> %zu)\n",
                            opened ? "OPENED" : "no-ui", base, rt.scene().size());
                // close with Esc (key 27)
                oa::runtime::FrameInput esc;
                esc.key_down_edges = {27};
                esc.keys_down.insert(27);
                host_step(rt, esc);
                tick_idle(rt, idle, 300);
                (void)park_story(rt, idle, 3000);
            }
        }
        if (p + 1 >= 3) {
            // advance only when the text is fully revealed
            click_at(rt, idle, 960, 930, 120);
            std::string nxt;
            for (size_t f = 0; f < 1800 && !rt.exit_requested(); ++f) {
                host_step(rt, idle);
                nxt = sample(rt);
                if (!nxt.empty() && nxt != smp) break;
            }
            std::printf("[snp] letter advance -> text='%s'\n", nxt.c_str());
        }
    }
    std::printf("[snp] dock probe -> %s\n", dock_ok ? "DOCK-OK" : "DOCK-UNVERIFIED");
    audio_dump(rt, "final");
    return 0;
}
