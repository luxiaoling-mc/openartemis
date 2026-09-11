// P0 (U6b gate, research/41) + P2 (research/43): headless NekoMiko boot +
// journey driver. Runs the REAL NekoMiko root.pfs (qureate, Artemis engine)
// through
//   boot -> (first boot: language select 简体中文) -> brand logos -> title
//   -> はじめから -> gamestart (name dialog, logical completion) -> 確定
//   select -> chapter-01 story start,
// recording how deep the flow gets (clicks / chapter / sample / final wait)
// and asserting the stable gates:
//   P0-1 a select row was chosen (language select on first boot)
//   P0-2 title parked and はじめから clicked
//   P0-3 story body text reached (chapter 01 first page)
//   P0-4 story turns advanced past the pre-story pages (clicks > 0)
//   P0-5 flow intact: no engine exit requested
// Since research/43 the chapter-01 story pages TURN: the stall root cause
// was an empty message layer (speaker-name slot with zero characters) whose
// reveal_pending flag never cleared — it wedged every Generic click wait.
// Fixed in the text engine (empty layers can no longer stay "revealing").
// With OA_P2DEEP=1 the journey keeps turning until an emote (立絵) layer
// event fires (real fg scenes reached after ~45 story clicks) and then
// measures idle-timeline revision growth.
// Env: OA_TEST_NEKOMIKO_PFS (skip 77 unset; auto default like OA_TEST_FPM_PFS).
// Interaction rules follow the game's own framework semantics: pointer rows
// (select options / buttons) are hovered before clicking (cursor model), the
// pointer stays on the row while a choice animates, and plain story page
// turns use Enter/decide (NekoMiko's pointer CLICK rides the framework's
// button-cursor + push-CLICK chain instead).
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <set>
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
int failures = 0;
char g_last_fail[512] = {0};
void check(bool cond, const char* fmt, ...) {
    if (!cond) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(g_last_fail, sizeof(g_last_fail), fmt, ap);
        va_end(ap);
        std::fprintf(stderr, "FAIL: %s\n", g_last_fail);
        ++failures;
    }
}
const char* ws(const oa::runtime::WaitReason* w) {
    using K = oa::runtime::WaitReason::Kind;
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
bool is_page_wait(const oa::runtime::WaitReason* w) {
    // Story page-turn waits: Artemis parks text lines on Generic (@/wt0).
    // Timed waits gate fades/animation flows — a real player does not click
    // through those, and NekoMiko's select framework (cursor model) expects
    // clicks only on live rows.
    if (!w) return false;
    return w->kind == oa::runtime::WaitReason::Kind::Generic ||
           w->kind == oa::runtime::WaitReason::Kind::Generic0;
}
std::string sample(oa::runtime::GameRuntime& rt) {
    // NekoMiko's active story message may be the engine-canonical adv slot
    // or the game-visible layer; read the first non-empty page across the
    // visible content layers (same rule the TXT smoke probe uses).
    for (const std::string& lid : rt.text().visible_content_layers()) {
        const oa::render::MessageLayer* ml = rt.text().layer(lid);
        if (!ml) continue;
        for (const auto& u : ml->page)
            if (!u.data.empty()) return u.data.substr(0, 24);
    }
    const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
    if (!ml) return "-";
    for (const auto& u : ml->page)
        if (!u.data.empty()) return u.data.substr(0, 24);
    return "-";
}
// P2 (research/43): optional fine-grained page-turn trace. With OA_P2TRACE
// set, every engine instruction executed after arming is captured; the page
// turn block dumps it (proves whether the click segment / clickEnd ran).
struct P2Trace {
    std::vector<std::string> steps;
    bool armed = false;
    void arm(oa::runtime::GameRuntime& rt) {
        steps.clear();
        armed = true;
        auto* it = &rt.interpreter();
        it->on_step = [this, it](const std::string& script, size_t line,
                                 const oa::runtime::Instruction& ins) {
            if (!armed) return;
            if (steps.size() < 800) {
                char buf[240];
                std::string extra;
                for (const auto& [k, v] : ins.params) {
                    if (k == "function" || k == "fn") {
                        extra = " " + k + "=" + v;
                        break;
                    }
                }
                std::snprintf(buf, sizeof(buf), "%s:%zu %s%s", script.c_str(), line,
                              ins.tag.c_str(), extra.c_str());
                steps.emplace_back(buf);
            }
        };
    }
    void disarm(oa::runtime::GameRuntime& rt) {
        armed = false;
        rt.interpreter().on_step = nullptr;
    }
    void dump(const char* what) {
        std::printf("[p2trace] --- %s ---\n", what);
        for (const auto& s : steps) std::printf("[p2trace] %s\n", s.c_str());
    }
    bool saw(const char* needle) const {
        for (const auto& s : steps)
            if (s.find(needle) != std::string::npos) return true;
        return false;
    }
} g_trace;
bool reveal_done(const oa::runtime::GameRuntime& rt) {
    for (const std::string& id : rt.text().visible_content_layers()) {
        if (!rt.text().page_has_visible_text(id)) continue;
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        if (ml && (ml->reveal_pending || ml->reveal_index < ml->char_count))
            return false;
    }
    return true;
}
struct Boot {
    oa::runtime::GameRuntime rt;
    std::string chapter;
    Boot(std::shared_ptr<const oa::fs::IFileSystem> fs,
         std::shared_ptr<oa::runtime::SaveStore> store, std::string* ch_out)
        : rt(fs) {
        rt.set_save_store(store);
        rt.open_project("windows");

        if (ch_out) {
            auto& hooks = rt.interpreter().hooks();
            const auto orig = hooks.resource_read;
            hooks.resource_read = [this, ch_out, orig](const std::string& r)
                -> std::optional<std::vector<uint8_t>> {
                if (r.rfind("script/", 0) == 0 || r.find("/script/") != std::string::npos) {
                    *ch_out = r.substr(r.rfind('/') + 1);
                }
                return orig(r);
            };
        }
        rt.boot_project();
    }
};
bool has_key_handler(oa::runtime::GameRuntime& rt, const std::string& key) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k != h->params.end() && k->second == key) return true;
    }
    return false;
}
bool click_key(oa::runtime::GameRuntime& rt, const std::string& key) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end() || k->second != key) continue;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? double(l->width) : 404.0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? double(l->height) : 120.0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (!rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) {
            x0 = l->left;
            y0 = l->top;
            rw = w;
            rh = hh;
        }
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = int(x0 + rw / 2);
        cl.mouse_y = int(y0 + rh / 2);
        rt.tick(16, cl);
        return true;
    }
    return false;
}
struct SelectOpt {
    std::string layer;
    std::string no;
    double cx = 0, cy = 0;
};
bool find_select_opts(oa::runtime::GameRuntime& rt, std::vector<SelectOpt>* out) {
    std::map<int, SelectOpt> by_no;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto it = h->params.find("name");
        if (it == h->params.end() || it->second != "select") continue;
        SelectOpt o;
        o.layer = l->id;
        const auto no = h->params.find("no");
        if (no != h->params.end()) o.no = no->second;
        double w = l->has_clip ? l->clip_w : 560.0;
        double hh = l->has_clip ? l->clip_h : 74.0;
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) {
            o.cx = x0 + rw / 2;
            o.cy = y0 + rh / 2;
        }
        by_no[std::atoi(o.no.c_str())] = o;
    }
    for (auto& [k, v] : by_no) out->push_back(v);
    return !out->empty();
}
void click_point(oa::runtime::GameRuntime& rt, int x, int y) {
    oa::runtime::FrameInput cl;
    cl.left_click_edge = true;
    cl.left_down = true;
    cl.mouse_x = x;
    cl.mouse_y = y;
    rt.tick(16, cl);
}
void hover_point(oa::runtime::GameRuntime& rt, int x, int y, size_t frames) {
    oa::runtime::FrameInput mv;
    mv.mouse_x = x;
    mv.mouse_y = y;
    for (size_t i = 0; i < frames; ++i) rt.tick(16, mv);
}
// ---- E15 (research/53) helpers -------------------------------------------
// Framework button rows (csvbtn3/lyevent): click handlers with params
// name=<btn table> + key=<csv row key>. Used to drive the save UI and the
// yes/no dialog of the save-flow regression below.
struct RowHit {
    double cx = 0, cy = 0;
    std::string key;
};
bool find_rows(oa::runtime::GameRuntime& rt, const std::string& name,
               const std::string& key_prefix, std::vector<RowHit>* out) {
    bool found = false;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto it = h->params.find("name");
        if (it == h->params.end() || it->second != name) continue;
        const auto k = h->params.find("key");
        if (k == h->params.end()) continue;
        if (!key_prefix.empty() && k->second.rfind(key_prefix, 0) != 0) continue;
        RowHit o;
        o.key = k->second;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? double(l->width) : 200.0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? double(l->height) : 60.0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) {
            o.cx = x0 + rw / 2;
            o.cy = y0 + rh / 2;
        }
        if (out) out->push_back(o);
        found = true;
    }
    return found;
}
// E15 (research/53) stage state machine. User crash: save (F6 save UI,
// slot 2, confirm) then right-click EXIT back to the game while a real
// story choice (sel_01) is on screen, then click the option quickly after
// the save UI closes. The UI session leaves scr.ip on the choice block, so
// the story re-runs the choice rows (select_text appends to the live
// scr.select) while the engine rebuilds the rows; a click inside that
// rebuild started the select-exit chain with rows still live and the
// clicknext crashed (select.lua:482/484 family). Regression: run this stage
// and require the story to advance without a Lua crash.
struct SaveBackStage {
    int at_select = 3; // which real story select (3 = sel_01)
    int state = 0;
    size_t f = 0;
    double ax = 0, ay = 0; // option row anchor at arm time
    double bx = 0, by = 0;
    bool done = false;
    const char* why = "ok";
} g_sb;
bool g_sb_armed = false;
size_t g_tick_no = 0;
template <typename In>
static void tickg(oa::runtime::GameRuntime& rt, In&& in) {
    try {
        rt.tick(16, std::forward<In>(in));
        ++g_tick_no;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "TICK CRASH tick=%zu: %s\n", g_tick_no, e.what());
        try {
            std::fprintf(stderr, "%s\n", rt.interpreter().lua_bridge().stack_trace(15).c_str());
        } catch (...) {}
        throw;
    }
}
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0); // diagnostics survive aborts
    const char* pfs_path = std::getenv("OA_TEST_NEKOMIKO_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_NEKOMIKO_PFS unset; skipping\n");
        return 77;
    }
    namespace fsf = std::filesystem;
    std::error_code ec;
    fsf::path store_dir;
    if (const char* sd = std::getenv("OA_TEST_STORE_DIR")) {
        store_dir = sd; // shared store: successive runs keep config/saves
    } else {
        store_dir = fsf::temp_directory_path(ec) /
                    ("oa_nekomiko_p0_" + std::to_string(::getpid()));
        fsf::remove_all(store_dir, ec);
    }
    auto store = std::make_shared<oa::runtime::DirSaveStore>(store_dir.string());
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
    std::string chapter;
    Boot boot(fs, store, &chapter);
    oa::runtime::GameRuntime& rt = boot.rt;
    oa::runtime::FrameInput idle;

    const auto t0 = std::chrono::steady_clock::now();
    auto wall = [&]() -> double {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    // ---- unified journey driver -------------------------------------------
    // Phases are not hardcoded stages: each frame reacts to what the game
    // currently shows (select rows first, then the title button, then story
    // pages), so the driver survives first-boot (language select) and normal
    // boots alike.
    int hold_x = 0, hold_y = 0; // pointer anchor during settle windows
    bool title_clicked = false;
    bool story_body = false;
    size_t clicks = 0, selects_seen = 0, hover_frames = 0, settle_left = 0;
    const int kClickX = 960, kClickY = 660; // neutral (1920x1080 stage)
    std::string last_sample = "-";
    size_t t = 0;
    if (std::getenv("OA_SELBACK")) {
        if (const char* a = std::getenv("OA_SELBACK_AT")) g_sb.at_select = std::atoi(a);
    }
    bool plain_armed = false; // OA_SELBACK_PLAIN: arm on a plain story page
    for (t = 0; t < 40000 && !rt.exit_requested(); ++t) {
        // ---- E15 (research/53): engine-skip to the chapter select (fast
        // regression runs), then the save/EXIT/click stage ------------------
        static bool skip_on = false, skip_done = false;
        if (std::getenv("OA_SELBACK_SKIP") && !skip_done && story_body &&
            chapter.rfind("01_第1章", 0) == 0 && clicks >= 3 && !skip_on) {
            skip_on = true;
            rt.interpreter().enqueue_tag("skip", {{"allow", "1"}, {"unread", "1"}});
            rt.interpreter().enqueue_tag("exec", {{"command", "skip"}, {"mode", "1"}});
            std::printf("[selback] skip to chapter select armed f=%zu\n", t);
        }
        if (skip_on && !skip_done) {
            tickg(rt, idle);
            std::vector<SelectOpt> chk;
            if (find_select_opts(rt, &chk)) {
                skip_done = true;
                skip_on = false;
                rt.interpreter().enqueue_tag("exec",
                                             {{"command", "skip"}, {"mode", "0"}});
                std::printf("[selback] skip halted at select f=%zu (opts=%zu)\n", t,
                            chk.size());
            } else if (rt.exit_requested() || t > 60000) {
                skip_done = true;
                skip_on = false;
                std::printf("[selback] skip no select; continue f=%zu\n", t);
            }
            continue;
        }
        // E15 (research/53) OA_SELBACK_PLAIN: the user's second report — a
        // crash on 普通正文页 存档→右键 (no choice screen). Arm the same
        // save/EXIT stage at a parked plain story page in chapter 01.
        static std::string prev_ch;
        const bool just_entered_01 =
            prev_ch != chapter && chapter.rfind("01_第1章", 0) == 0 &&
            !prev_ch.empty();
        if (prev_ch != chapter) prev_ch = chapter;
        const std::string cur_sample = sample(rt);
        const bool page_parked =
            is_page_wait(rt.current_wait()) && cur_sample != "　" &&
            cur_sample != "-";
        if (std::getenv("OA_SELBACK_PLAIN") && !g_sb_armed && !plain_armed &&
            chapter.rfind("01_第1章", 0) == 0 &&
            ((just_entered_01 && page_parked) || (clicks >= 6 && page_parked)) &&
            story_body) {
            plain_armed = true;
            g_sb_armed = true;
            g_sb.state = 1;
            g_sb.f = 0;
            g_sb.ax = 960;
            g_sb.ay = 438;
            std::printf("[selback] PLAIN armed at story page f=%zu ch='%s' "
                        "sample='%s'\n",
                        t, chapter.c_str(), sample(rt).c_str());
            hover_point(rt, 0, 0, 1);
            continue;
        }
        // research/53 user-3: 482 with NO save at all — right-click x2,
        // left-click, then a decide. Arm the stage at the FIRST parked plain
        // chapter-01 page (page 1: the 確定-click zombie {0} is still the top
        // stack frame there; the rclick UI session's close pops it pre-fix —
        // the ef51afd stale-drop reclaims it at the rclick row handler).
        if (std::getenv("OA_SELBACK_RCLK") && !g_sb_armed && !plain_armed &&
            chapter.rfind("01_第1章", 0) == 0 && page_parked &&
            clicks <= 6 && story_body) {
            plain_armed = true;
            g_sb_armed = true;
            g_sb.state = 1;
            g_sb.f = 0;
            g_sb.ax = 960;
            g_sb.ay = 660; // neutral message-area point (1920x1080 stage)
            std::printf("[selback] RCLK armed at story page f=%zu ch='%s' "
                        "sample='%s'\n",
                        t, chapter.c_str(), sample(rt).c_str());
            hover_point(rt, 0, 0, 1);
            continue;
        }
        // save-UI stage machine (runs one action per outer frame).
        if (g_sb_armed && !g_sb.done) {
            auto tickmv = [&](int x, int y) {
                oa::runtime::FrameInput mv;
                mv.mouse_x = x;
                mv.mouse_y = y;
                tickg(rt, mv);
            };
            auto tickidle = [&] { tickg(rt, idle); };
            auto press = [&](int key) {
                oa::runtime::FrameInput k;
                k.key_down_edges = {key};
                k.keys_down.insert(key);
                tickg(rt, k);
            };
            auto clickat = [&](double x, double y) {
                oa::runtime::FrameInput cl;
                cl.left_click_edge = true;
                cl.left_down = true;
                cl.mouse_x = int(x);
                cl.mouse_y = int(y);
                tickg(rt, cl);
            };
            auto findrow = [&](const char* nm, const char* pre,
                               const std::string& exact) -> bool {
                std::vector<RowHit> rows;
                if (!find_rows(rt, nm, pre, &rows)) return false;
                for (const auto& r : rows) {
                    if (exact.empty() || r.key == exact) {
                        g_sb.bx = r.cx;
                        g_sb.by = r.cy;
                        return true;
                    }
                }
                return false;
            };
            ++g_sb.f;
            if (std::getenv("OA_SELBACK_PLAIN") &&
                ((t >= 1150 && t <= 1172) || (t >= 1260 && t <= 1300))) {
                const char* cs = rt.interpreter().current_script()
                                     ? rt.interpreter().current_script()->c_str()
                                     : "";
                std::string cst;
                for (const auto& fr : rt.interpreter().call_stack()) {
                    cst += fr.script + ":" + std::to_string(fr.return_line) + " ";
                }
                std::printf("[selback] poswatch f=%zu pos=%s:%zu q=%d st[%s]\n",
                            t, cs, rt.interpreter().current_line(),
                            (int)rt.interpreter().has_queued_tags(), cst.c_str());
            }
            // E15: watch when the ENGINE stream parks at the script.asb
            // select-[stop] area (position <=1) with a Stop wait — the
            // pre-crash state the user's trace shows.
            {
                const oa::runtime::WaitReason* pw = rt.current_wait();
                const char* cs = rt.interpreter().current_script()
                                     ? rt.interpreter().current_script()->c_str()
                                     : "";
                if (pw && pw->kind == oa::runtime::WaitReason::Kind::Stop &&
                    std::string(cs) == "system/script.asb" &&
                    rt.interpreter().current_line() <= 1) {
                    static bool park_logged = false;
                    if (!park_logged) {
                        park_logged = true;
                        std::string cst;
                        for (const auto& fr : rt.interpreter().call_stack()) {
                            cst += fr.script + ":" +
                                   std::to_string(fr.return_line) + " ";
                        }
                        std::printf("[selback] ENGINE PARKED at select-[stop] "
                                    "pos=%s:%zu wait_id='%s' f=%zu stack[%s]\n",
                                    cs, rt.interpreter().current_line(),
                                    pw->id.c_str(), t, cst.c_str());
                    }
                }
            }
            // research/53 user-3: no-save rclick stage (right-click x2, then a
            // left click at the message area, then idle-watch for the 482).
            if (std::getenv("OA_SELBACK_RCLK")) {
                if (g_sb.f % 25 == 0) {
                    const oa::runtime::WaitReason* pw = rt.current_wait();
                    std::string cs;
                    for (const auto& fr : rt.interpreter().call_stack()) {
                        cs += fr.script + ":" +
                              std::to_string(fr.return_line) + " ";
                    }
                    std::printf("[selback] rclk-watch f=%zu wait=%s id='%s' "
                                "pos=%s:%zu q=%d stack[%s] sample='%s'\n",
                                t, ws(pw), pw ? pw->id.c_str() : "-",
                                rt.interpreter().current_script()
                                    ? rt.interpreter().current_script()->c_str()
                                    : "?",
                                rt.interpreter().current_line(),
                                (int)rt.interpreter().has_queued_tags(),
                                cs.c_str(), sample(rt).c_str());
                }
                switch (g_sb.state) {
                    case 1:
                        press(2); // right-click #1
                        g_sb.state = 2;
                        g_sb.f = 0;
                        std::printf("[selback] rclick #1 f=%zu\n", t);
                        break;
                    case 2:
                        if (g_sb.f > 40) {
                            press(2); // right-click #2
                            g_sb.state = 3;
                            g_sb.f = 0;
                            std::printf("[selback] rclick #2 f=%zu\n", t);
                        } else {
                            tickidle();
                        }
                        break;
                    case 3:
                        if (g_sb.f > 40) {
                            clickat(g_sb.ax, g_sb.ay); // left click (message)
                            g_sb.state = 4;
                            g_sb.f = 0;
                            std::printf("[selback] lclick @(%.0f,%.0f) f=%zu\n",
                                        g_sb.ax, g_sb.ay, t);
                        } else {
                            tickidle();
                        }
                        break;
                    case 4: // idle-watch: the user crash lands seconds later
                        if (g_sb.f == 60) {
                            // research/53 user-3: the lclick closed the
                            // rclick UI; pre-fix that pops the zombie {0} and
                            // parks the ENGINE at the consumed select-[stop]
                            // (script.asb:0). The user's next decide releases
                            // it into select_exit -> 482 — send that decide.
                            press(13);
                            std::printf("[selback] decide after UI f=%zu\n", t);
                        } else if (g_sb.f > 420) {
                            g_sb.done = true;
                            g_sb.why = "ok";
                            std::printf("[selback] rclk stage done f=%zu\n", t);
                        } else {
                            tickidle();
                        }
                        break;
                    default:
                        g_sb.why = "rclk-state?";
                        g_sb.done = true;
                        break;
                }
                if (g_sb.done && g_sb.why != std::string("ok"))
                    std::printf("[selback] stage failed: %s f=%zu\n", g_sb.why, t);
                continue;
            }
            switch (g_sb.state) {
                case 1: // open the save UI: F6 at the choice park, or the
                        // mw-dock save button on a plain story page (F6 is a
                        // page-advance while a text click-wait holds)
                    if (std::getenv("OA_SELBACK_PLAIN")) {
                        if (findrow("adv", "bt_save", "")) {
                            tickmv(int(g_sb.bx), int(g_sb.by));
                            g_sb.state = 30;
                            g_sb.f = 0;
                            std::printf("[selback] dock bt_save @(%.0f,%.0f) f=%zu\n",
                                        g_sb.bx, g_sb.by, t);
                        } else if (g_sb.f > 200) {
                            g_sb.why = "no-dock-save";
                            g_sb.done = true;
                        } else {
                            tickidle();
                        }
                    } else {
                        press(117);
                        g_sb.state = 2;
                        g_sb.f = 0;
                        std::printf("[selback] F6 pressed f=%zu\n", t);
                    }
                    break;
                case 2: // save page: hover slot 2 then click it
                    if (findrow("save", "bt_save0", "bt_save02")) {
                        tickmv(int(g_sb.bx), int(g_sb.by));
                        g_sb.state = 3;
                        g_sb.f = 0;
                        std::printf("[selback] save slot2 @(%.0f,%.0f) f=%zu\n",
                                    g_sb.bx, g_sb.by, t);
                    } else if (g_sb.f > 400) {
                        g_sb.why = "no-save-ui";
                        g_sb.done = true;
                    } else {
                        tickidle();
                    }
                    break;
                case 3:
                    clickat(g_sb.bx, g_sb.by);
                    g_sb.state = 4;
                    g_sb.f = 0;
                    std::printf("[selback] save slot2 clicked f=%zu\n", t);
                    break;
                case 30: // plain: dock save clicked (state 1 -> 30 -> 2)
                    clickat(g_sb.bx, g_sb.by);
                    g_sb.state = 2;
                    g_sb.f = 0;
                    std::printf("[selback] dock save clicked f=%zu\n", t);
                    break;
                case 4: // overwrite confirm dialog (defaults skip it)
                    if (findrow("dlg", "bt_yes", "")) {
                        tickmv(int(g_sb.bx), int(g_sb.by));
                        g_sb.state = 5;
                        g_sb.f = 0;
                    } else if (g_sb.f > 60) {
                        g_sb.state = 6;
                        g_sb.f = 0;
                        std::printf("[selback] save w/o dialog f=%zu\n", t);
                    } else {
                        tickidle();
                    }
                    break;
                case 5:
                    clickat(g_sb.bx, g_sb.by);
                    g_sb.state = 6;
                    g_sb.f = 0;
                    std::printf("[selback] save confirmed f=%zu\n", t);
                    break;
                case 6: // save flow runs; then EXIT (right click = 回退)
                    if (std::getenv("OA_SELBACK_DBLSAVE")) {
                        // overwrite probe: slot2 now holds a save -> the
                        // second click hits the overwrite-confirm dialog
                        if (g_sb.f > 40 && findrow("save", "bt_save0", "bt_save02")) {
                            tickmv(int(g_sb.bx), int(g_sb.by));
                            g_sb.state = 31;
                            g_sb.f = 0;
                            std::printf("[selback] save2 slot hover f=%zu\n", t);
                        } else if (g_sb.f > 400) {
                            g_sb.why = "save-stall";
                            g_sb.done = true;
                        } else {
                            tickidle();
                        }
                    } else {
                        size_t exf = 100;
                        if (const char* e = std::getenv("OA_SELBACK_EXITF"))
                            exf = size_t(std::atoi(e));
                        if (g_sb.f == exf) {
                            press(2);
                            std::printf("[selback] EXIT pressed f=%zu\n", t);
                        } else if (g_sb.f > exf + 60) {
                            g_sb.state = 7;
                            g_sb.f = 0;
                            std::printf("[selback] EXIT settled f=%zu\n", t);
                        } else if (g_sb.f > 400) {
                            g_sb.why = "save-stall";
                            g_sb.done = true;
                        } else {
                            tickidle();
                        }
                    }
                    break;
                case 31: // second slot click -> overwrite dialog
                    clickat(g_sb.bx, g_sb.by);
                    g_sb.state = 32;
                    g_sb.f = 0;
                    std::printf("[selback] save2 slot clicked f=%zu\n", t);
                    break;
                case 32:
                    if (findrow("dlg", "bt_yes", "")) {
                        tickmv(int(g_sb.bx), int(g_sb.by));
                        g_sb.state = 33;
                        g_sb.f = 0;
                        std::printf("[selback] save2 dlg yes @(%.0f,%.0f) f=%zu\n",
                                    g_sb.bx, g_sb.by, t);
                    } else if (g_sb.f > 200) {
                        g_sb.state = 34;
                        g_sb.f = 0;
                        std::printf("[selback] save2 no-dlg (direct) f=%zu\n", t);
                    } else {
                        tickidle();
                    }
                    break;
                case 33:
                    clickat(g_sb.bx, g_sb.by);
                    g_sb.state = 34;
                    g_sb.f = 0;
                    std::printf("[selback] save2 confirmed f=%zu\n", t);
                    break;
                case 34: // overwrite save flow, then EXIT
                    {
                        size_t exf = 90;
                        if (g_sb.f == exf) {
                            press(2);
                            std::printf("[selback] save2 EXIT pressed f=%zu\n", t);
                        } else if (g_sb.f > exf + 60) {
                            g_sb.state = 7;
                            g_sb.f = 0;
                            std::printf("[selback] save2 EXIT settled f=%zu\n", t);
                        } else if (g_sb.f > 400) {
                            g_sb.why = "save-stall";
                            g_sb.done = true;
                        } else {
                            tickidle();
                        }
                    }
                    break;
                case 7: // wait until the save UI is gone
                    if (!find_rows(rt, "save", "bt_save0", nullptr)) {
                        g_sb.state = 8;
                        g_sb.f = 0;
                        std::printf("[selback] save UI closed f=%zu\n", t);
                    } else if (g_sb.f > 400) {
                        g_sb.why = "save-ui-stuck";
                        g_sb.done = true;
                    } else {
                        tickidle();
                    }
                    break;
                case 8: // race: click the old option spot right away, while
                        // the re-show estag chain may still be rebuilding
                        // (this reproduced the clicknext crash pre-fix)
                    if (std::getenv("OA_SELBACK_PLAIN")) {
                        // plain-story mode: the save/EXIT leaves the ENGINE
                        // parked at script.asb *select [stop] while Lua sits
                        // on chapter-01 page 1 (scr.select=nil). A decide
                        // edge (Enter/CLICK) releases the stop and the inline
                        // stream falls into select_exit -> 482 (user crash).
                        if (g_sb.f == 20) {
                            const oa::runtime::WaitReason* pw = rt.current_wait();
                            std::string cs;
                            for (const auto& fr : rt.interpreter().call_stack()) {
                                cs += fr.script + ":" +
                                      std::to_string(fr.return_line) + " ";
                            }
                            std::printf(
                                "[selback] preEnter wait=%s id='%s' pos=%s:%zu "
                                "queue=%d callstack[%s]\n",
                                ws(pw), pw ? pw->id.c_str() : "-",
                                rt.interpreter().current_script()
                                    ? rt.interpreter().current_script()->c_str()
                                    : "?",
                                rt.interpreter().current_line(),
                                (int)rt.interpreter().has_queued_tags(),
                                cs.c_str());
                            oa::runtime::FrameInput k;
                            k.key_down_edges = {13};
                            k.keys_down.insert(13);
                            tickg(rt, k);
                            std::printf("[selback] plain Enter after EXIT f=%zu\n", t);
                        } else if (g_sb.f > 160) {
                            g_sb.done = true;
                            std::printf("[selback] plain stage done f=%zu\n", t);
                        } else {
                            tickidle();
                        }
                    } else if (g_sb.f == 2) {
                        hover_point(rt, int(g_sb.ax), int(g_sb.ay), 2);
                        std::printf("[selback] race hover option @(%.0f,%.0f) f=%zu\n",
                                    g_sb.ax, g_sb.ay, t);
                    } else if (g_sb.f == 4) {
                        click_point(rt, int(g_sb.ax), int(g_sb.ay));
                        std::printf("[selback] race click option f=%zu\n", t);
                    } else if (g_sb.f > 260) {
                        g_sb.done = true;
                        std::printf("[selback] stage done f=%zu\n", t);
                    } else {
                        tickidle();
                    }
                    break;
                default:
                    g_sb.why = "state?";
                    g_sb.done = true;
                    break;
            }
            if (g_sb.done && g_sb.why != std::string("ok"))
                std::printf("[selback] stage failed: %s f=%zu\n", g_sb.why, t);
            continue;
        }
        if (settle_left > 0) { // post-click settle: hold the pointer where the
            // click landed (a real player keeps the mouse on the row while
            // the choice animation plays; returning it to (0,0) fires a
            // rollout that clears the game's row state mid-chain).
            --settle_left;
            oa::runtime::FrameInput mv;
            mv.mouse_x = hold_x;
            mv.mouse_y = hold_y;
            tickg(rt, mv);
            if (settle_left == 0)
                std::printf("[neko] settle done f=%zu ch='%s' wait=%s sample='%s'\n", t,
                            chapter.c_str(), ws(rt.current_wait()), sample(rt).c_str());
            continue;
        }
        tickg(rt, idle);
        if (std::getenv("OA_SELBACK_PLAIN") && !g_sb_armed) {
            const auto& cs = rt.interpreter().call_stack();
            if (!cs.empty()) {
                const auto& top = cs.back();
                if (top.script == "system/script.asb" && top.return_line <= 1) {
                    static size_t zc = 0;
                    if (++zc == 1) {
                        std::string cst;
                        for (const auto& fr : cs) {
                            cst += fr.script + ":" + std::to_string(fr.return_line) +
                                   " ";
                        }
                        std::printf("[selback] TOP0-APPEARS f=%zu pos=%s:%zu "
                                    "stack[%s] ch='%s'\n",
                                    t,
                                    rt.interpreter().current_script()
                                        ? rt.interpreter().current_script()->c_str()
                                        : "?",
                                    rt.interpreter().current_line(), cst.c_str(),
                                    chapter.c_str());
                    }
                }
            }
        }
        if (std::getenv("OA_SELBACK_PLAIN") && g_sb.done) {
            static size_t stall = 0;
            static size_t last_clicks = 0;
            if (clicks == last_clicks) {
                if (++stall == 1500) { // story not advancing after the stage
                    const oa::runtime::WaitReason* pw = rt.current_wait();
                    std::string cs;
                    for (const auto& fr : rt.interpreter().call_stack()) {
                        cs += fr.script + ":" + std::to_string(fr.return_line) + " ";
                    }
                    std::printf(
                        "[selback] STALL ch='%s' wait=%s id='%s' pos=%s:%zu "
                        "queue=%d callstack[%s] sample='%s'\n",
                        chapter.c_str(), ws(pw),
                        pw ? pw->id.c_str() : "-",
                        rt.interpreter().current_script()
                            ? rt.interpreter().current_script()->c_str()
                            : "?",
                        rt.interpreter().current_line(),
                        (int)rt.interpreter().has_queued_tags(), cs.c_str(),
                        sample(rt).c_str());
                }
            } else {
                stall = 0;
                last_clicks = clicks;
            }
        }
        if (wall() > 280) { // bounded dev/CI wall: depth printed, gates above
            std::printf("[neko] wall cap reached f=%zu\n", t);
            break;
        }
        if (t % 2000 == 0)
            std::printf("[neko] f=%zu ch='%s' wait=%s clicks=%zu sel=%zu body=%d "
                        "sample='%s' wall=%.1fs\n",
                        t, chapter.c_str(), ws(rt.current_wait()), clicks, selects_seen,
                        (int)story_body, sample(rt).c_str(), wall());

        std::vector<SelectOpt> opts;
        if (find_select_opts(rt, &opts)) {
            // E15 (research/53): OA_SELBACK — run the save/EXIT/click stage at
            // the target story select instead of clicking an option.
            if (std::getenv("OA_SELBACK") && !g_sb_armed &&
                selects_seen + 1 == size_t(g_sb.at_select)) {
                g_sb_armed = true;
                g_sb.state = 1;
                g_sb.f = 0;
                g_sb.ax = opts[0].cx;
                g_sb.ay = opts[0].cy;
                std::printf("[selback] armed at select #%zu (idle) f=%zu\n",
                            selects_seen + 1, t);
                hover_point(rt, 0, 0, 1); // park the pointer away
                continue;
            }
            // hover the first option (cursor model), then click it
            if (hover_frames < 40) {
                hover_point(rt, int(opts[0].cx), int(opts[0].cy), 2);
                hover_frames += 2;
                continue;
            }
            hover_frames = 0;
            ++selects_seen;
            std::printf("[neko] select #%zu -> option '%s' @(%.0f,%.0f) f=%zu ch='%s'\n",
                        selects_seen, opts[0].no.c_str(), opts[0].cx, opts[0].cy, t,
                        chapter.c_str());
            click_point(rt, int(opts[0].cx), int(opts[0].cy));
            ++clicks;
            hold_x = int(opts[0].cx);
            hold_y = int(opts[0].cy);
            // E9 crash hunt: OA_SELDBL re-clicks the same row a few frames
            // later (fast double-click) — candidate trigger for a duplicated
            // select_click/select_exit chain (select_clicknext re-entry after
            // select_reset cleared scr.select).
            if (std::getenv("OA_SELDBL") && selects_seen >= 3) {
                size_t delay = 4;
                if (const char* d = std::getenv("OA_SELDBL_DELAY")) delay = size_t(std::atoi(d));
                // hold the pointer on the row between the presses (a real
                // double click never parks the mouse elsewhere)
                for (size_t r = 0; r < delay; ++r)
                    tickg(rt, [&] {
                        oa::runtime::FrameInput mv;
                        mv.mouse_x = int(opts[0].cx);
                        mv.mouse_y = int(opts[0].cy);
                        return mv;
                    }());
                click_point(rt, int(opts[0].cx), int(opts[0].cy));
                ++clicks;
                std::printf("[neko] select #%zu double-click repeat (delay %zu) f=%zu\n",
                            selects_seen, delay, t);
            }
            // give the choice flow room to resolve (exit transition, branch
            // jump); no further synthetic input during this window
            settle_left = 90;
            continue;
        }
        const auto* w = rt.current_wait();
        const bool parked_title =
            w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty() &&
            has_key_handler(rt, "bt_start");
        if (parked_title && !title_clicked) {
            title_clicked = click_key(rt, "bt_start");
            hold_x = 237;
            hold_y = 968; // NekoMiko はじめから (500.d.2) world center
            std::printf("[neko] title bt_start clicked f=%zu ch='%s'\n", t,
                        chapter.c_str());
            settle_left = 60; // avoid double input on the same park
            continue;
        }
        if (!title_clicked) continue; // story cannot start before the title click
        const bool p2trace = std::getenv("OA_P2TRACE") != nullptr;
        if (is_page_wait(w) && reveal_done(rt)) {
            // E15 (research/53): OA_SELBACK_PLAIN — arm the save stage at the
            // FIRST parked plain page of chapter 01 (user's 482 log: ip =
            // 01_第1章 block=1 at the crash). The top-of-loop arm misses the
            // page-1 park because the driver's turn loop holds the loop top.
            if (std::getenv("OA_SELBACK_PLAIN") && !g_sb_armed && !plain_armed &&
                chapter.rfind("01_第1章", 0) == 0 && story_body &&
                sample(rt) != "　") {
                plain_armed = true;
                g_sb_armed = true;
                g_sb.state = 1;
                g_sb.f = 0;
                g_sb.ax = 960;
                g_sb.ay = 438;
                std::printf("[selback] PLAIN armed at story page f=%zu ch='%s' "
                            "sample='%s'\n",
                            t, chapter.c_str(), sample(rt).c_str());
                hover_point(rt, 0, 0, 1);
                continue;
            }
            // Page turn = Enter/decide. NekoMiko's pointer CLICK goes through
            // the framework's button-cursor + push-CLICK chain (rows carry
            // function=btn_clickex); plain story page turns here use the
            // decide path so row-driven UI (selects above) stays exclusive
            // to real row clicks. This matches how the fpm journeys advance
            // dialog parks (research/41).
            oa::runtime::FrameInput key;
            key.key_down_edges = {13};
            key.keys_down.insert(13);
            if (p2trace) {
                g_trace.arm(rt);
                std::printf("[p2trace] preEnter queue=%d fromQueue=%d\n",
                            (int)rt.interpreter().has_queued_tags(),
                            (int)rt.interpreter().last_wait_from_queue());
            }
            tickg(rt, key);
            ++clicks;
            if (p2trace) {
                std::printf("[p2trace] postEnter queue=%d\n",
                            (int)rt.interpreter().has_queued_tags());
                // watch the cursor over the next frames
                for (int pf = 0; pf < 6; ++pf) {
                    tickg(rt, idle);
                    const auto* pw = rt.current_wait();
                    std::printf("[p2trace] f+%d pos=%s:%zu queue=%d fromQ=%d wait=%s\n",
                                pf + 1,
                                rt.interpreter().current_script()
                                    ? rt.interpreter().current_script()->c_str()
                                    : "?",
                                rt.interpreter().current_line(),
                                (int)rt.interpreter().has_queued_tags(),
                                (int)rt.interpreter().last_wait_from_queue(),
                                ws(pw));
                }
            }
            if (clicks <= 6)
                std::printf("[neko] page turn #%zu f=%zu ch='%s' sample='%s'\n", clicks, t,
                            chapter.c_str(), sample(rt).c_str());
            if (sample(rt) != "-") story_body = true;
            // P2 (research/43): with OA_P2DEEP the journey keeps turning until
            // an emote (立絵) layer event fires or the click budget runs out
            // (used to reach a real fg scene through natural scenario flow).
            // E9 select-crash hunt: OA_SELDEEP ignores the emote break and
            // keeps turning/clicking through the story's choice branches
            // (select rows are hovered+clicked above), until the click
            // budget, the wall cap, an exit request, or a tick crash.
            const size_t deep = std::getenv("OA_P2DEEP")   ? 600
                                : std::getenv("OA_SELDEEP") ? 320
                                                            : 40;
            const bool emote_seen = rt.emote_layer_events() > 0;
            const bool seldeep = std::getenv("OA_SELDEEP") != nullptr;
            if (story_body && (clicks >= deep || (emote_seen && !seldeep))) {
                std::printf("[neko] journey target reached f=%zu ch='%s' sample='%s'"
                            " emote=%zu\n",
                            t, chapter.c_str(), sample(rt).c_str(),
                            rt.emote_layer_events());
                break;
            }
            // wait for the page content/wait to change before turning again
            bool changed = false;
            bool tail_dumped = false;
            for (size_t s = 0; s < 3000 && !rt.exit_requested(); ++s) {
                if (p2trace) {
                    g_trace.steps.clear(); // capture what runs each frame
                    auto* it = &rt.interpreter();
                    it->on_step = [](const std::string& scr, size_t ln,
                                     const oa::runtime::Instruction& ins) {
                        if (g_trace.steps.size() < 400) {
                            std::string extra;
                            for (const auto& [k, v] : ins.params) {
                                if (k == "function" || k == "fn" || k == "file" ||
                                    k == "label" || k == "data")
                                    extra += " " + k + "=" + v;
                            }
                            char buf[400];
                            std::snprintf(buf, sizeof(buf), "%s:%zu %s%s", scr.c_str(),
                                          ln, ins.tag.c_str(), extra.c_str());
                            g_trace.steps.push_back(buf);
                        }
                    };
                }
                tickg(rt, idle);
                const std::string now = sample(rt);
                const auto* w2 = rt.current_wait();
                if (now != last_sample || !is_page_wait(w2) ||
                    rt.transition().is_in_progress(rt.now_ms())) {
                    if (p2trace && !tail_dumped && is_page_wait(rt.current_wait()) &&
                        !rt.interpreter().has_queued_tags()) {
                        tail_dumped = true;
                        std::printf("[p2trace] --- tail into park (sample '%s') ---\n",
                                    now.c_str());
                        const size_t n = g_trace.steps.size();
                        for (size_t k = n > 14 ? n - 14 : 0; k < n; ++k)
                            std::printf("[p2trace] %s\n", g_trace.steps[k].c_str());
                    }
                    last_sample = now;
                    changed = true;
                    break;
                }
            }
            if (p2trace) rt.interpreter().on_step = nullptr;
            if (p2trace) {
                g_trace.disarm(rt);
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                              "turn #%zu changed=%d wait=%s sample='%s' clickEnd=%d "
                              "main=%d steps=%zu",
                              clicks, (int)changed, ws(rt.current_wait()),
                              sample(rt).c_str(), (int)g_trace.saw("clickEnd"),
                              (int)g_trace.saw("scriptMainloop"), g_trace.steps.size());
                g_trace.dump(buf);
                if (!changed && clicks >= 2 && g_trace.steps.size() < 40) {
                    // parked with almost no engine activity: wait did not resolve
                    std::printf("[p2trace] RESULT: wait-not-resolved\n");
                }
            }
            continue;
        }
        if (sample(rt) != "-") story_body = true;
    }
    std::printf("[neko] end f=%zu ticks=%zu clicks=%zu selects=%zu body=%d ch='%s' "
                "sample='%s' wait=%s exit=%d wall=%.1fs\n",
                t, g_tick_no, clicks, selects_seen, (int)story_body, chapter.c_str(),
                sample(rt).c_str(), ws(rt.current_wait()), (int)rt.exit_requested(),
                wall());
    // stable tail: flow parked in a sane wait, no exit request
    for (size_t s = 0; s < 240 && !rt.exit_requested(); ++s) tickg(rt, idle);

    // P2 (research/43): deep-run emote breathing evidence — with the fg on
    // stage hold idle for a few seconds and report revision growth + a pixel
    // diff (the idle timeline's breathing re-renders throttled).
    if (std::getenv("OA_P2DEEP") && !rt.emote_layers().empty()) {
        for (auto& [id, st] : rt.emote_layers()) {
            if (!st.player) continue;
            const std::vector<uint8_t> a = st.player->rgba();
            const uint64_t rev0 = st.player->revision();
            // ~half a breathing period (5 s loop): poses must differ clearly
            for (size_t s = 0; s < 160; ++s) tickg(rt, idle);
            const std::vector<uint8_t> b = st.player->rgba();
            uint64_t diff = 0;
            const size_t px = std::min(a.size(), b.size()) / 4;
            for (size_t i = 0; i < px; ++i) {
                if (std::abs(int(a[i * 4]) - int(b[i * 4])) > 24 ||
                    std::abs(int(a[i * 4 + 1]) - int(b[i * 4 + 1])) > 24 ||
                    std::abs(int(a[i * 4 + 2]) - int(b[i * 4 + 2])) > 24)
                    ++diff;
            }
            std::printf("[neko] emote '%s' rev %llu -> %llu diffPx=%llu (%.2f%%)\n",
                        id.c_str(), (unsigned long long)rev0,
                        (unsigned long long)st.player->revision(),
                        (unsigned long long)diff,
                        px ? 100.0 * double(diff) / double(px) : 0.0);
        }
    }

    check(selects_seen >= 1, "P0-1 select row chosen (language/確定) (sel=%zu)",
          selects_seen);
    check(title_clicked, "P0-2 title parked and はじめから clicked");
    check(story_body, "P0-3 story body text reached");
    check(clicks > 0, "P0-4 story pages advanced (clicks=%zu)", clicks);
    check(!rt.exit_requested(), "P0-5 flow intact: no engine exit requested");
    std::printf("[neko] depth chapter='%s' sample='%s' wait=%s layers=%zu\n",
                chapter.c_str(), sample(rt).c_str(), ws(rt.current_wait()),
                rt.scene().size());
    if (failures) {
        std::fprintf(stderr, "FAILURES: %d (last: %s)\n", failures, g_last_fail);
        return 1;
    }
    std::printf("all ok\n");
    return 0;
}
