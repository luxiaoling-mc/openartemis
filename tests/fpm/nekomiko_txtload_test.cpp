// research/72 (NekoMiko U6c): save -> load -> keep turning — the story text
// must keep updating after a real dock-driven load. User report: after 读档
// the plot advances (backgrounds/立绘/flow) but the message text freezes on
// the pre-save page.
//
// Root cause (engine-side): the save is taken while the engine parks INSIDE
// the save-page UI flow (system/save.asb:4). On load the interpreter resumes
// there, and the UI session's residual chains (chgmsg 500.pageno etc.) re-run
// on top of the load's cleared message-layer stack, leaving the engine's
// ACTIVE message layer on a UI layer (500.pageno) instead of the story text
// layer. The game's page-turn dispatcher (system/script.asb) clears the
// previous page with an rp that targets the layer active at the click park —
// with the wrong active it clears the UI layer, the next page's text APPENDS
// behind the old page and reveal_index stays frozen: visible text dead.
// Fix: when a page wait (Generic/Generic0) releases, anchor the text engine's
// active layer back to the layer holding the visible page content (see
// runtime.cpp advance_wait + text.cpp anchor_active_to_content).
//
// This driver reproduces the exact user flow with the real NekoMiko root.pfs:
//   journey to chapter-01 -> parked plain page (sample S)
//   dock bt_save -> save tab -> slot 1 -> save file lands
//   page load tab -> slot 1 -> confirm dialog -> engine [load] runs
//   story re-parks (sample S back) -> 3 page turns
// and asserts the post-load turns keep replacing the page text (no frozen
// sample, reveal complete at every park). Env: OA_TEST_NEKOMIKO_PFS (skip 77).
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <map>
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
    if (!w) return false;
    return w->kind == oa::runtime::WaitReason::Kind::Generic ||
           w->kind == oa::runtime::WaitReason::Kind::Generic0;
}
std::string sample(oa::runtime::GameRuntime& rt) {
    for (const std::string& lid : rt.text().visible_content_layers()) {
        const oa::render::MessageLayer* ml = rt.text().layer(lid);
        if (!ml) continue;
        for (const auto& u : ml->page)
            if (!u.data.empty()) return u.data.substr(0, 24);
    }
    return "-";
}
bool reveal_done(const oa::runtime::GameRuntime& rt) {
    for (const std::string& id : rt.text().visible_content_layers()) {
        if (!rt.text().page_has_visible_text(id)) continue;
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        if (ml && (ml->reveal_pending || ml->reveal_index < ml->char_count))
            return false;
    }
    return true;
}
void tickg(oa::runtime::GameRuntime& rt, const oa::runtime::FrameInput& in) {
    try {
        rt.tick(16, in);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "TICK CRASH: %s\n", e.what());
        std::fflush(stderr);
        std::exit(3);
    }
}
// --- scene/UI helpers (journey pattern from nekomiko_p0_test, research/41) --
bool has_key_handler(oa::runtime::GameRuntime& rt, const std::string& key) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k != h->params.end() && k->second == key) return true;
    }
    return false;
}
struct Row {
    std::string name;
    std::string key;
    double cx = 0, cy = 0;
};
bool collect_rows(oa::runtime::GameRuntime& rt, std::vector<Row>* out) {
    bool any = false;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h) continue;
        Row r;
        const auto nm = h->params.find("name");
        if (nm != h->params.end()) r.name = nm->second;
        const auto k = h->params.find("key");
        if (k == h->params.end()) continue;
        r.key = k->second;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? double(l->width) : 80.0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? double(l->height) : 60.0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) {
            r.cx = x0 + rw / 2;
            r.cy = y0 + rh / 2;
        }
        out->push_back(r);
        any = true;
    }
    return any;
}
bool find_row(oa::runtime::GameRuntime& rt, const std::string& key_prefix,
              const std::string& name, Row* out, int which = 0) {
    std::vector<Row> rows;
    if (!collect_rows(rt, &rows)) return false;
    int n = 0;
    for (const Row& r : rows) {
        if (!name.empty() && r.name != name) continue;
        if (r.key.rfind(key_prefix, 0) != 0) continue;
        if (n++ == which) {
            *out = r;
            return true;
        }
    }
    return false;
}
bool click_key(oa::runtime::GameRuntime& rt, const std::string& key) {
    std::vector<Row> rows;
    if (!collect_rows(rt, &rows)) return false;
    for (const Row& r : rows) {
        if (r.key != key) continue;
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = int(r.cx);
        cl.mouse_y = int(r.cy);
        tickg(rt, cl);
        return true;
    }
    return false;
}
void hover_click_hold(oa::runtime::GameRuntime& rt, const Row& r, size_t hold) {
    oa::runtime::FrameInput mv;
    mv.mouse_x = int(r.cx);
    mv.mouse_y = int(r.cy);
    for (size_t f = 0; f < 25; ++f) tickg(rt, mv);
    oa::runtime::FrameInput cl;
    cl.left_click_edge = true;
    cl.left_down = true;
    cl.mouse_x = int(r.cx);
    cl.mouse_y = int(r.cy);
    tickg(rt, cl);
    oa::runtime::FrameInput holdin;
    holdin.mouse_x = int(r.cx);
    holdin.mouse_y = int(r.cy);
    for (size_t f = 0; f < hold && !rt.exit_requested(); ++f) tickg(rt, holdin);
}
struct SelectOpt {
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
bool parked_story(oa::runtime::GameRuntime& rt) {
    const auto* w = rt.current_wait();
    return is_page_wait(w) && reveal_done(rt) && sample(rt) != "-" && sample(rt) != "　";
}
size_t wait_parked(oa::runtime::GameRuntime& rt, const oa::runtime::FrameInput& idle,
                   size_t budget) {
    for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
        tickg(rt, idle);
        if (parked_story(rt)) return f;
    }
    return budget;
}
bool has_save_file(const std::filesystem::path& dir) {
    std::error_code e2;
    if (!std::filesystem::exists(dir, e2)) return false;
    for (const auto& ent : std::filesystem::recursive_directory_iterator(dir, e2)) {
        if (!e2 && ent.is_regular_file(e2) && ent.path().extension() == ".dat") return true;
        e2.clear();
    }
    return false;
}
/// One page turn (Enter at a parked story page); returns the parked sample.
std::string page_turn(oa::runtime::GameRuntime& rt, const oa::runtime::FrameInput& idle,
                      const char* tag) {
    const std::string before = sample(rt);
    oa::runtime::FrameInput key;
    key.key_down_edges = {13};
    key.keys_down.insert(13);
    tickg(rt, key);
    std::string last = sample(rt);
    for (size_t s = 0; s < 9000 && !rt.exit_requested(); ++s) {
        tickg(rt, idle);
        const std::string now = sample(rt);
        const auto* w = rt.current_wait();
        if (now != last || !is_page_wait(w) ||
            rt.transition().is_in_progress(rt.now_ms())) {
            last = now;
            if (is_page_wait(rt.current_wait())) break;
        }
    }
    std::printf("[nm72] turn[%s]: before='%s' after='%s' wait=%s\n", tag,
                before.c_str(), last.c_str(), ws(rt.current_wait()));
    return last;
}
std::string pos(oa::runtime::GameRuntime& rt) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s:%zu",
                  rt.interpreter().current_script()
                      ? rt.interpreter().current_script()->c_str()
                      : "?",
                  rt.interpreter().current_line());
    return buf;
}
} // namespace

struct Boot {
    oa::runtime::GameRuntime rt;
    std::string chapter;
    Boot(std::shared_ptr<const oa::fs::IFileSystem> fs,
         std::shared_ptr<oa::runtime::SaveStore> store)
        : rt(fs) {
        rt.set_save_store(store);
        rt.open_project("windows");
        auto& hooks = rt.interpreter().hooks();
        const auto orig = hooks.resource_read;
        hooks.resource_read = [this, orig](const std::string& r)
            -> std::optional<std::vector<uint8_t>> {
            if (r.rfind("script/", 0) == 0 || r.find("/script/") != std::string::npos) {
                chapter = r.substr(r.rfind('/') + 1);
            }
            return orig(r);
        };
        rt.boot_project();
    }
};

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pfs_path = std::getenv("OA_TEST_NEKOMIKO_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_NEKOMIKO_PFS unset; skipping\n");
        return 77;
    }
    namespace fsf = std::filesystem;
    std::error_code ec;
    const fsf::path store_dir =
        fsf::temp_directory_path(ec) / ("oa_nm_txtload_" + std::to_string(::getpid()));
    fsf::remove_all(store_dir, ec);
    auto store = std::make_shared<oa::runtime::DirSaveStore>(store_dir.string());
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
    Boot boot(fs, store);
    oa::runtime::GameRuntime& rt = boot.rt;
    oa::runtime::FrameInput idle;

    bool title_clicked = false;
    bool story_body = false;
    bool want_arm = false;
    size_t clicks = 0;
    size_t hover_frames = 0;
    std::string last_sample = "-";
    enum class St { Walk, Arm, DockSave, SavePage, SaveDlg, SaveWait,
                    LoadTab, LoadSlot, LoadDlg, LoadWait, PostLoadWalk } st = St::Walk;
    size_t stf = 0;
    int post_turns = 0;
    std::string save_sample = "-";
    std::string post_prev = "-";
    bool all_turns_live = true;

    for (size_t t = 0; t < 90000 && !rt.exit_requested(); ++t) {
        if (st == St::Walk) {
            std::vector<SelectOpt> opts;
            if (find_select_opts(rt, &opts)) {
                if (hover_frames < 40) {
                    oa::runtime::FrameInput mv;
                    mv.mouse_x = int(opts[0].cx);
                    mv.mouse_y = int(opts[0].cy);
                    for (int k = 0; k < 2; ++k) tickg(rt, mv);
                    hover_frames += 2;
                    continue;
                }
                hover_frames = 0;
                ++clicks;
                std::printf("[nm72] select -> option '%s' ch='%s'\n",
                            opts[0].no.c_str(), boot.chapter.c_str());
                oa::runtime::FrameInput cl;
                cl.left_click_edge = true;
                cl.left_down = true;
                cl.mouse_x = int(opts[0].cx);
                cl.mouse_y = int(opts[0].cy);
                tickg(rt, cl);
                last_sample = sample(rt);
                for (int q = 0; q < 90 && !rt.exit_requested(); ++q) {
                    oa::runtime::FrameInput mv;
                    mv.mouse_x = int(opts[0].cx);
                    mv.mouse_y = int(opts[0].cy);
                    tickg(rt, mv);
                }
                continue;
            }
            const auto* w = rt.current_wait();
            const bool parked_title =
                w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty() &&
                has_key_handler(rt, "bt_start");
            if (parked_title && !title_clicked) {
                title_clicked = click_key(rt, "bt_start");
                std::printf("[nm72] title bt_start clicked ch='%s'\n",
                            boot.chapter.c_str());
                for (int q = 0; q < 60 && !rt.exit_requested(); ++q) tickg(rt, idle);
                continue;
            }
            if (title_clicked && parked_story(rt)) {
                const std::string cur = sample(rt);
                if (cur != "-" && cur != "　") story_body = true;
                const bool in_ch01 = boot.chapter.rfind("01_第1章", 0) == 0;
                if (want_arm && in_ch01 && story_body) {
                    st = St::Arm;
                    stf = 0;
                    std::printf("[nm72] === ARM ch='%s' sample='%s' clicks=%zu\n",
                                boot.chapter.c_str(), sample(rt).c_str(), clicks);
                    continue;
                }
                if (!want_arm && story_body && in_ch01 && clicks >= 3) {
                    want_arm = true;
                    (void)page_turn(rt, idle, "normal-prearm");
                    ++clicks;
                    continue;
                }
                oa::runtime::FrameInput key;
                key.key_down_edges = {13};
                key.keys_down.insert(13);
                tickg(rt, key);
                ++clicks;
                bool changed = false;
                for (size_t s = 0; s < 3000 && !rt.exit_requested(); ++s) {
                    tickg(rt, idle);
                    const std::string now = sample(rt);
                    const auto* w2 = rt.current_wait();
                    if (now != last_sample || !is_page_wait(w2) ||
                        rt.transition().is_in_progress(rt.now_ms())) {
                        last_sample = now;
                        changed = true;
                        break;
                    }
                }
                if (clicks <= 12)
                    std::printf("[nm72] page turn #%zu ch='%s' sample='%s'\n", clicks,
                                boot.chapter.c_str(), sample(rt).c_str());
                continue;
            }
            if (sample(rt) != "-") story_body = true;
            tickg(rt, idle);
            continue;
        }

        // ---------------- UI stage machine ----------------
        ++stf;
        switch (st) {
            case St::Arm: {
                save_sample = sample(rt);
                std::printf("[nm72] save-point sample='%s' pos=%s\n", save_sample.c_str(),
                            pos(rt).c_str());
                check(save_sample != "-" && save_sample != "　", "NM72-1 story page parked");
                st = St::DockSave;
                stf = 0;
                break;
            }
            case St::DockSave: {
                Row r;
                if (find_row(rt, "bt_save", "", &r)) {
                    std::printf("[nm72] dock bt_save @(%.0f,%.0f)\n", r.cx, r.cy);
                    hover_click_hold(rt, r, 250);
                    st = St::SavePage;
                    stf = 0;
                } else if (stf > 900) {
                    std::printf("[nm72] no dock bt_save row\n");
                    return 2;
                } else {
                    tickg(rt, idle);
                }
                break;
            }
            case St::SavePage: {
                Row r;
                if (find_row(rt, "bt_save0", "", &r, 0)) {
                    Row s1;
                    if (find_row(rt, "bt_save01", "", &s1)) {
                        std::printf("[nm72] save slot bt_save01 @(%.0f,%.0f)\n", s1.cx,
                                    s1.cy);
                        hover_click_hold(rt, s1, 300);
                    } else {
                        hover_click_hold(rt, r, 300);
                    }
                    st = St::SaveDlg;
                    stf = 0;
                } else if (stf > 1200) {
                    std::printf("[nm72] save page never opened\n");
                    return 2;
                } else {
                    tickg(rt, idle);
                }
                break;
            }
            case St::SaveDlg: { // optional overwrite dialog (fresh store: none)
                Row d;
                if (find_row(rt, "bt_yes", "dlg", &d)) {
                    hover_click_hold(rt, d, 300);
                    st = St::SaveWait;
                    stf = 0;
                } else if (stf > 500) {
                    st = St::SaveWait;
                    stf = 0;
                } else {
                    tickg(rt, idle);
                }
                break;
            }
            case St::SaveWait: {
                if (has_save_file(store_dir) && stf > 200) {
                    std::printf("[nm72] save file landed; switching to the load tab\n");
                    st = St::LoadTab;
                    stf = 0;
                } else if (stf > 6000) {
                    std::printf("[nm72] save never landed\n");
                    return 2;
                } else {
                    tickg(rt, idle);
                }
                break;
            }
            case St::LoadTab: {
                Row r;
                if (find_row(rt, "bt_load", "save", &r)) {
                    std::printf("[nm72] page tab bt_load @(%.0f,%.0f)\n", r.cx, r.cy);
                    hover_click_hold(rt, r, 300);
                    st = St::LoadSlot;
                    stf = 0;
                } else if (stf > 600) {
                    return 2;
                } else {
                    tickg(rt, idle);
                }
                break;
            }
            case St::LoadSlot: {
                Row r;
                if (find_row(rt, "bt_load01", "", &r) ||
                    find_row(rt, "bt_save01", "", &r)) {
                    std::printf("[nm72] load slot '%s' @(%.0f,%.0f)\n", r.key.c_str(),
                                r.cx, r.cy);
                    hover_click_hold(rt, r, 400);
                    st = St::LoadDlg;
                    stf = 0;
                } else if (stf > 900) {
                    st = St::LoadDlg;
                    stf = 0;
                } else {
                    tickg(rt, idle);
                }
                break;
            }
            case St::LoadDlg: {
                Row d;
                if (find_row(rt, "bt_yes", "dlg", &d)) {
                    std::printf("[nm72] load confirm dlg yes\n");
                    hover_click_hold(rt, d, 400);
                    st = St::LoadWait;
                    stf = 0;
                } else if (stf > 700) {
                    oa::runtime::FrameInput en;
                    en.key_down_edges = {13};
                    en.keys_down.insert(13);
                    tickg(rt, en);
                    st = St::LoadWait;
                    stf = 0;
                } else {
                    tickg(rt, idle);
                }
                break;
            }
            case St::LoadWait: {
                if (stf > 200) {
                    const bool page_up = ([] (oa::runtime::GameRuntime& r) {
                        Row d;
                        return find_row(r, "bt_ret", "load", &d) ||
                               find_row(r, "bt_ret", "save", &d);
                    })(rt);
                    if (!page_up && parked_story(rt)) {
                        std::printf("[nm72] === post-UI-load park pos=%s sample='%s'\n",
                                    pos(rt).c_str(), sample(rt).c_str());
                        check(sample(rt) == save_sample,
                              "NM72-2 load restored the saved page (sample back)");
                        post_prev = sample(rt);
                        st = St::PostLoadWalk;
                        stf = 0;
                    } else if (page_up && stf > 400) {
                        oa::runtime::FrameInput k;
                        k.key_down_edges = {27};
                        k.keys_down.insert(27);
                        tickg(rt, k);
                        stf = 0;
                    } else {
                        tickg(rt, idle);
                    }
                } else {
                    tickg(rt, idle);
                }
                break;
            }
            case St::PostLoadWalk: {
                if (++post_turns <= 3) {
                    char tag[32];
                    std::snprintf(tag, sizeof(tag), "postload%d", post_turns);
                    const std::string after = page_turn(rt, idle, tag);
                    // the core user symptom: the page text must KEEP TURNING
                    // (pre-fix: sample frozen on the pre-save page forever)
                    check(after != post_prev, "NM72-3 post-load turn %d replaced the "
                          "page text ('%s' -> '%s')", post_turns, post_prev.c_str(),
                          after.c_str());
                    if (after == post_prev) all_turns_live = false;
                    check(parked_story(rt) && reveal_done(rt),
                          "NM72-4 post-load turn %d parked revealed", post_turns);
                    post_prev = after;
                } else {
                    std::printf("[nm72] === POST-WALK DONE (all turns live=%d) ===\n",
                                (int)all_turns_live);
                    check(!rt.exit_requested(), "NM72-5 no engine exit request");
                    std::printf("[nm72] final sample='%s' pos=%s wait=%s\n",
                                sample(rt).c_str(), pos(rt).c_str(), ws(rt.current_wait()));
                    if (failures) {
                        std::fprintf(stderr, "nekomiko_txtload_test: %d failure(s) "
                                    "(last: %s)\n", failures, g_last_fail);
                        return 1;
                    }
                    std::printf("all ok\n");
                    return 0;
                }
                break;
            }
            default:
                break;
        }
    }
    std::printf("[nm72] loop exhausted st=%d\n", (int)st);
    return 2;
}
