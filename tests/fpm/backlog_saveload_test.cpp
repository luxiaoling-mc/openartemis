// L2 M11 backlog save/load regression driver (real fpm, stage-1 repro):
// user flow: story park -> save slot 1 -> load slot 1 -> keep walking -> open
// the backlog. Dumps the FPM Lua `log.stack` structure (entry count, per-entry
// file@block / txno / select / line markers, scr.ip position) across every
// boundary and the blog UI's rendered first-page cache rows, then checks the
// expected "restore-and-continue" semantics (docs/research/33 M11 record):
//   B == A (save does not mutate the stack)
//   D == B (load restores the stack to the saved snapshot)
//   E starts with D and appends only new entries
//   no duplicate (file,block) pair anywhere; blocks monotonic per file
// The driver only observes: it never touches root.pfs or game scripts.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
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

extern "C" {
#include "lua.h"
}

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime_lua.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_save.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
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
bool is_story_wait(const oa::runtime::WaitReason* w) {
    return w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                 w->kind == oa::runtime::WaitReason::Kind::Generic0);
}
std::string sample(oa::runtime::GameRuntime& rt) {
    const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
    if (!ml) return "-";
    for (const auto& u : ml->page)
        if (!u.data.empty()) return u.data.substr(0, 16);
    return "-";
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
bool reach_title(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        if (!(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty())) continue;
        for (const oa::render::Layer* l : rt.scene().draw_order()) {
            const auto* h = rt.scene().find_event_handler(l->id, "click");
            if (!h) continue;
            const auto k = h->params.find("key");
            if (k != h->params.end() && k->second == "bt_start") return true;
        }
    }
    return false;
}
void start_game(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
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
bool park_story(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                size_t budget = 3000) {
    for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
        if (is_story_wait(w) && ml && ml->reveal_index >= ml->char_count && !ml->reveal_pending)
            return true;
    }
    return false;
}
bool reach_story_page(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.text().page_has_visible_text("1.80.mw.adv_adv")) break;
    }
    for (int t = 0; t < 6 && !rt.exit_requested(); ++t) {
        if (!park_story(rt, idle, 2500)) break;
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = 640;
        cl.mouse_y = 600;
        rt.tick(16, cl);
        (void)park_story(rt, idle, 2500);
    }
    return park_story(rt, idle, 2500);
}
void press_tick(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle, int key) {
    oa::runtime::FrameInput k;
    k.key_down_edges = {key};
    k.keys_down.insert(key);
    rt.tick(16, k);
    for (size_t f = 1; f < 40 && !rt.exit_requested(); ++f) rt.tick(16, idle);
}
bool settle_screen(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                   const char* node, size_t settle = 300) {
    size_t held = 0;
    for (size_t f = 0; f < 8000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        if (w && rt.scene().find(node) != nullptr) {
            if (++held > settle) return true;
        } else {
            held = 0;
        }
    }
    return false;
}
void confirm_dialog(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle) {
    size_t held = 0;
    for (size_t f = 0; f < 6000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        if (w && rt.scene().find("600.1.bt.1.0") != nullptr) {
            if (++held > 120) break;
        } else {
            held = 0;
        }
    }
    oa::runtime::FrameInput en;
    en.key_down_edges = {13};
    en.keys_down.insert(13);
    rt.tick(16, en);
}
// -- Lua log.stack observers ------------------------------------------------
std::string lua_str(oa::runtime::GameRuntime& rt, const char* global) {
    lua_State* L = rt.interpreter().lua_bridge().state();
    lua_getglobal(L, global);
    std::string out;
    if (lua_isstring(L, -1)) out = lua_tostring(L, -1);
    lua_pop(L, 1);
    return out;
}
size_t stack_len(oa::runtime::GameRuntime& rt) {
    rt.interpreter().lua_bridge().run_code(
        "_oa_n = table.maxn(log and log.stack or {})", "b11_len");
    lua_State* L = rt.interpreter().lua_bridge().state();
    lua_getglobal(L, "_oa_n");
    size_t n = lua_isnumber(L, -1) ? size_t(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);
    return n;
}
/// Dump `log.stack` entries as "i:file@block|txno|S|L" plus scr.ip position.
std::string dump_stack(oa::runtime::GameRuntime& rt) {
    rt.interpreter().lua_bridge().run_code(R"(
      local t = {}
      local s = log and log.stack or {}
      for i, v in ipairs(s) do
        local crc = v.crc and (v.crc.txno or '?') or '-'
        t[i] = string.format('%d:%s@%s|%s|%s|%s',
               i, tostring(v.file or '?'), tostring(v.block or '?'),
               tostring(crc), v.select and 'S' or '-', v.line and 'L' or '-')
      end
      _oa_dump = table.concat(t, ';')
      local ip = scr and scr.ip or {}
      _oa_ip = string.format('ipfile=%s ipblock=%s ipcount=%s iptextcount=%s',
               tostring(ip.file or '?'), tostring(ip.block or '?'),
               tostring(ip.count or '?'), tostring(ip.textcount or '?'))
    )", "b11_dump");
    return lua_str(rt, "_oa_dump") + "  [" + lua_str(rt, "_oa_ip") + "]";
}
/// Advance story lines until the Lua backlog stack reaches `target` entries
/// (absolute) or the click budget is exhausted / progress stalls. Returns the
/// observed entry count (and prints a stall reason when stuck).
size_t advance_until(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                     size_t target, size_t click_budget, std::string* last_sample) {
    size_t clicks = 0;
    size_t stall = 0;
    std::string last = sample(rt);
    while (clicks < click_budget && !rt.exit_requested()) {
        const size_t n0 = stack_len(rt);
        if (n0 >= target) break;
        if (!park_story(rt, idle, 2500)) {
            std::printf("[b11]     advance stall: not parked (sample='%s' len=%zu)\n",
                        last.c_str(), n0);
            break;
        }
        oa::runtime::FrameInput cl;
        cl.left_click_edge = true;
        cl.left_down = true;
        cl.mouse_x = 640;
        cl.mouse_y = 600;
        rt.tick(16, cl);
        ++clicks;
        for (size_t f = 0; f < 3000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const auto* w = rt.current_wait();
            if (!is_story_wait(w)) continue;
            const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
            if (ml && ml->reveal_index >= ml->char_count && !ml->reveal_pending && f > 2) break;
        }
        if (last_sample) *last_sample = sample(rt);
        const size_t n1 = stack_len(rt);
        if (n1 == n0 && sample(rt) == last) {
            if (++stall > 6) {
                std::printf("[b11]     advance stall: no progress after %zu clicks "
                            "(sample='%s' len=%zu)\n",
                            clicks, sample(rt).c_str(), n1);
                break;
            }
        } else {
            stall = 0;
        }
        last = sample(rt);
    }
    return stack_len(rt);
}
struct Entry {
    std::string file;
    int block = -1;
};
bool parse_entries(const std::string& dump, std::vector<Entry>* out) {
    size_t pos = 0;
    bool ok = true;
    while (pos < dump.size()) {
        const size_t sp = dump.find(';', pos);
        const std::string item = dump.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
        if (item.empty()) break;
        const size_t at = item.find('@');
        if (at == std::string::npos) { ok = false; break; }
        Entry e;
        e.file = item.substr(0, at);
        const size_t bar = item.find('|', at);
        const std::string blk = item.substr(at + 1, bar == std::string::npos ? std::string::npos : bar - at - 1);
        e.block = std::atoi(blk.c_str());
        out->push_back(e);
        if (sp == std::string::npos) break;
        pos = sp + 1;
    }
    return ok;
}
void analyze(const std::string& label, const std::string& dump) {
    std::vector<Entry> v;
    if (!parse_entries(dump, &v) || v.empty()) {
        std::printf("[b11] %-6s (empty/unparsable: '%s')\n", label.c_str(), dump.c_str());
        return;
    }
    bool dup = false;
    bool nonmono = false;
    std::string last_file;
    int last_block = -1;
    std::string prev_key;
    for (const Entry& e : v) {
        const std::string key = e.file + "@" + std::to_string(e.block);
        if (key == prev_key) { dup = true; std::printf("[b11]     DUP adjacent: %s\n", key.c_str()); }
        if (e.file == last_file && last_block >= 0 && e.block <= last_block) {
            nonmono = true;
            std::printf("[b11]     NONMONO %s after %s@%d\n", key.c_str(), last_file.c_str(), last_block);
        }
        if (e.file != last_file) { last_file = e.file; last_block = -1; }
        if (e.block > last_block) last_block = e.block;
        prev_key = key;
    }
    std::printf("[b11] %-6s entries=%zu%s%s\n", label.c_str(), v.size(),
                dup ? " DUP!" : "", nonmono ? " NONMONO!" : "");
}
// ---- engine-side blog row layout dump (L2 M11b) ---------------------------
// Each backlog page row is its own layered message layer "500.z.bt.tx.<i>.1";
// glyphs render at the layer's bound scene node world position + ml.left/top +
// line*line_h. Dumping (node world y, ml geometry, font row metrics) for every
// visible row layer quantifies whether rows land on distinct y (normal) or
// stack on the same y (the user-reported "backlog rows piled together").
struct RowLayout {
    int row = -1;
    double origin = 0; // estimated first text line top: node_world_y + ml.top
    double line_h = 0;
    double node_y = 0;
    double ml_top = 0;
    double size = 0;
    double spacetop = 0;
    double mid = 0;
    double bot = 0;
    size_t lines = 0;
    std::string sample;
    bool visible = false;
};
// every blog visit's row layouts, keyed by visit label (row parity gate)
std::map<std::string, std::vector<RowLayout>> g_row_layouts;
void dump_blog_rows(oa::runtime::GameRuntime& rt, const char* label) {
    std::vector<RowLayout> rows;
    for (const std::string& id : rt.text().visible_content_layers()) {
        const size_t pos = id.find("500.z.bt.tx.");
        if (pos != 0) continue;
        if (id.size() < 15 || id.compare(id.size() - 2, 2, ".1") != 0) continue;
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        if (!ml) continue;
        RowLayout r;
        r.row = std::atoi(id.c_str() + 12);
        r.sample = ml->page.empty() ? "-" : ml->page[0].data.substr(0, 10);
        for (const auto& u : ml->page)
            if (u.kind == oa::render::PageUnit::Kind::Newline) ++r.lines;
        r.ml_top = ml->top;
        r.size = ml->font.size();
        r.spacetop = ml->font.spacetop();
        r.mid = ml->font.spacemiddle();
        r.bot = ml->font.spacebottom();
        r.visible = rt.scene().is_message_layer_visible(id);
        const std::string sid = rt.scene().bound_scene_id(id);
        oa::render::Affine2 wt;
        if (!sid.empty() && rt.scene().world_transform(sid, &wt)) {
            r.node_y = wt.f;
            r.origin = wt.f + ml->top;
        }
        // engine row pitch: line_h = spacetop + ruby + spacemiddle + body +
        // spacebottom (text.cpp layout_page), clamp >= 1
        const double rb = ml->font.rubysize() > 0 ? ml->font.rubysize() : 0;
        r.line_h = ml->font.spacetop() + rb + ml->font.spacemiddle() +
                   ml->font.size() + ml->font.spacebottom();
        if (r.line_h < 1) r.line_h = 1;
        rows.push_back(r);
    }
    std::printf("[b11] blog-rows %s: layers=%zu\n", label, rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const RowLayout& r = rows[i];
        std::printf("[b11]   row%d id-visible=%d node_y=%.1f ml.top=%.1f "
                    "origin=%.1f font(size=%.1f st=%.1f mid=%.1f bot=%.1f "
                    "line_h=%.1f) lines=%zu sample='%s'\n",
                    r.row, r.visible ? 1 : 0, r.node_y, r.ml_top, r.origin,
                    r.size, r.spacetop, r.mid, r.bot, r.line_h, r.lines + 1,
                    r.sample.c_str());
    }
    g_row_layouts[label] = rows;
    // L2 M11b parity gate: after a load the backlog rows must land exactly
    // where they did before the load (per-row ml.top/origin unchanged). The
    // defect shifted every row by the story font's top (4 -> 550).
    if (label != std::string("PRE")) {
        const auto pre = g_row_layouts.find("PRE");
        if (pre != g_row_layouts.end() && !pre->second.empty()) {
            std::vector<RowLayout> pv = pre->second;
            bool same = rows.size() == pv.size();
            for (size_t i = 0; same && i < rows.size() && i < pv.size(); ++i) {
                if (rows[i].row != pv[i].row ||
                    std::fabs(rows[i].origin - pv[i].origin) > 0.6 ||
                    std::fabs(rows[i].ml_top - pv[i].ml_top) > 0.6)
                    same = false;
            }
            std::string msg = "B11-rows ";
            msg += label;
            msg += " row origins match PRE (no load shift)";
            check(same, msg.c_str());
        }
    }
    // overlap scan: two rows with text whose first-line origins nearly coincide
    size_t suspicious = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].sample.empty() || rows[i].sample == "-") continue;
        for (size_t j = i + 1; j < rows.size(); ++j) {
            if (rows[j].sample.empty() || rows[j].sample == "-") continue;
            const double d = std::fabs(rows[i].origin - rows[j].origin);
            if (d < 24.0) { // far below a sane 32-44px row pitch at size 36
                ++suspicious;
                std::printf("[b11]   OVERLAP? row%d vs row%d origin-delta=%.1f\n",
                            rows[i].row, rows[j].row, d);
            }
        }
    }
    std::printf("[b11] blog-rows %s: suspicious-overlaps=%zu\n", label, suspicious);
}
/// Open the backlog (F8), dump FPM cache rows + engine row layout, close (Esc).
void blog_visit(oa::runtime::GameRuntime& rt, oa::runtime::FrameInput& idle,
                const char* label) {
    if (!park_story(rt, idle, 5000)) {
        std::printf("[b11] blog_visit %s: not parked\n", label);
        return;
    }
    {
        // inheritance-source trace: the engine-active message layer just
        // before the backlog opens (row chgmsg layers inherit its geometry)
        const std::string aid = rt.text().active_layer_id();
        const oa::render::MessageLayer* aml = rt.text().layer(aid);
        std::printf("[b11] %s pre-open active='%s' top=%.1f size=%.1f "
                    "spacetop=%.1f mid=%.1f bot=%.1f\n",
                    label, aid.c_str(), aml ? aml->top : -1.0,
                    aml ? aml->font.size() : -1.0,
                    aml ? aml->font.spacetop() : -1.0,
                    aml ? aml->font.spacemiddle() : -1.0,
                    aml ? aml->font.spacebottom() : -1.0);
    }
    press_tick(rt, idle, 119); // F8 = BACKLOG
    bool blog_up = false;
    for (size_t f = 0; f < 6000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        const auto* w = rt.current_wait();
        if (w && rt.scene().find("500.0.drag") != nullptr) { blog_up = true; break; }
    }
    std::printf("[b11] blog_visit %s: opened=%d\n", label, blog_up ? 1 : 0);
    if (blog_up) {
        {
            // active-layer trace after the blog UI settled
            const std::string aid = rt.text().active_layer_id();
            const oa::render::MessageLayer* aml = rt.text().layer(aid);
            std::printf("[b11] %s post-open active='%s' top=%.1f size=%.1f\n",
                        label, aid.c_str(), aml ? aml->top : -1.0,
                        aml ? aml->font.size() : -1.0);
        }
        rt.interpreter().lua_bridge().run_code(R"(
          local b = flg and flg.blog or nil
          local rows = {}
          if b and b.cache then
            local m = table.maxn(b.cache)
            local from = math.max(1, m - 3)
            for i = from, m do
              local c = b.cache[i]
              local ent = log.stack[i] or {}
              local flat = {}
              if c and c.text then
                for ri, row in ipairs(c.text) do
                  local parts = {}
                  for ti, tok in ipairs(row) do
                    if type(tok) == 'string' then parts[#parts+1] = tok
                    elseif type(tok) == 'table' then
                      for ti2, tok2 in ipairs(tok) do
                        if type(tok2) == 'string' then parts[#parts+1] = tok2 end
                      end
                    end
                  end
                  flat[#flat+1] = table.concat(parts)
                end
              end
              rows[#rows+1] = string.format('i=%d %s@%s rows=%d | %s',
                i, tostring(ent.file or '?'), tostring(ent.block or '?'),
                #flat, table.concat(flat, ' / '))
            end
          end
          _oa_blog = table.concat(rows, '\n')
        )", "b11_blog");
        std::printf("[b11] blog cache top rows (%s):\n%s\n", label,
                    lua_str(rt, "_oa_blog").c_str());
        // settle two extra frames so all page rows materialized, then dump
        for (size_t f = 0; f < 120 && !rt.exit_requested(); ++f) rt.tick(16, idle);
        dump_blog_rows(rt, label);
        {
            oa::runtime::FrameInput esc;
            esc.key_down_edges = {27};
            esc.keys_down.insert(27);
            rt.tick(16, esc);
        }
        (void)park_story(rt, idle, 6000);
    }
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_FPM_PFS unset; skipping\n");
        return 77;
    }
    namespace fsf = std::filesystem;
    std::error_code ec;
    const fsf::path store_dir =
        fsf::temp_directory_path(ec) / ("oa_b11_" + std::to_string(getpid()));
    fsf::remove_all(store_dir, ec);
    auto store = std::make_shared<oa::runtime::DirSaveStore>(store_dir.string());

        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    check(reach_title(rt, idle), "B11-0 title parked");
    start_game(rt, idle);
    check(reach_story_page(rt, idle), "B11-1 story body parked");
    const std::string s0 = sample(rt);
    std::printf("[b11] park sample='%s' wait=%s\n", s0.c_str(), ws(rt.current_wait()));

    // ---- A: baseline stack after a long boot walk (deep enough to leave the
    // boot file and exercise multi-file entries when the chapter allows) ----
    std::string a_dump;
    {
        std::string last;
        size_t n = advance_until(rt, idle, 30, 400, &last);
        check(n >= 8, "B11-2 boot walk produced >=8 backlog entries");
        a_dump = dump_stack(rt);
        std::printf("[b11] A(after boot walk n=%zu): %s\n", n, a_dump.c_str());
        analyze("A", a_dump);
    }
    // ---- PRE: open the backlog BEFORE any save/load (row layout baseline) --
    blog_visit(rt, idle, "PRE");
    (void)park_story(rt, idle, 6000);
    {
        std::string a2 = dump_stack(rt);
        analyze("A2", a2);
    }

    // ---- save to slot 1 (F6 -> bt_save01, dlg.save def=1 auto) ----
    const std::string save_sample = sample(rt);
    press_tick(rt, idle, 117); // F6 = SAVE
    check(settle_screen(rt, idle, "500.bt.1.bg.0", 300) && rt.scene().size() > 300,
          "B11-3 F6 opened the save screen");
    check(click_key(rt, "bt_save01"), "B11-4 save slot 1 clicked");
    bool slot_file = false, saveg = false;
    size_t done = 0;
    for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        slot_file = store->exists("savedata_cn/save0001.dat");
        saveg = store->exists("savedata_cn/saveg.dat");
        if (slot_file && saveg && ++done > 200) break;
    }
    check(slot_file && saveg, "B11-5 slot save landed in the store");
    {
        oa::runtime::FrameInput esc;
        esc.key_down_edges = {27};
        esc.keys_down.insert(27);
        rt.tick(16, esc);
    }
    bool closed = false;
    for (size_t f = 0; f < 9000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.scene().find("500.bt.1.bg.0") == nullptr && rt.scene().size() < 200 &&
            is_story_wait(rt.current_wait())) {
            closed = true;
            break;
        }
    }
    check(closed, "B11-6 save screen closed, story wait back");
    std::string b_dump = dump_stack(rt);
    std::printf("[b11] B(after save): %s\n", b_dump.c_str());
    analyze("B", b_dump);

    // ---- load slot 1 (F7 -> bt_save01 -> confirm dialog -> Enter) ----
    press_tick(rt, idle, 118); // F7 = LOAD
    check(settle_screen(rt, idle, "500.bt.1.bg.0", 300) && rt.scene().size() > 300,
          "B11-7 F7 opened the load screen");
    check(click_key(rt, "bt_save01"), "B11-8 load slot 1 clicked");
    bool dlg = false;
    for (size_t f = 0; f < 4000 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (rt.scene().find("600.1.bt.1.0") != nullptr) { dlg = true; break; }
    }
    check(dlg, "B11-9 load confirm dialog opened");
    bool restored = false;
    {
        confirm_dialog(rt, idle);
        for (size_t f = 0; f < 12000 && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const auto* w = rt.current_wait();
            const oa::render::MessageLayer* ml = rt.text().layer("1.80.mw.adv_adv");
            if (sample(rt) == save_sample && rt.scene().find("600.1.bt.1.0") == nullptr &&
                rt.scene().size() < 200 && is_story_wait(w) && ml &&
                ml->reveal_index >= ml->char_count && !ml->reveal_pending) {
                restored = true;
                break;
            }
        }
    }
    check(restored, "B11-10 load restored to the saved page (sample back, parked)");
    std::string d_dump = dump_stack(rt);
    std::printf("[b11] D(post-load, parked): %s\n", d_dump.c_str());
    analyze("D", d_dump);
    // ---- POSTLOAD: backlog right after the load (user repro point) ----
    blog_visit(rt, idle, "POSTLOAD");

    // ---- E: keep walking after the load ----
    std::string e_dump;
    {
        std::string last;
        size_t start_len = 0;
        {
            std::vector<Entry> d;
            (void)parse_entries(d_dump, &d);
            start_len = d.size();
        }
        size_t n = advance_until(rt, idle, start_len + 12, 300, &last);
        check(n >= start_len + 4, "B11-11 post-load walk produced backlog entries");
        e_dump = dump_stack(rt);
        std::printf("[b11] E(post-load walk n=%zu): %s\n", n, e_dump.c_str());
        analyze("E", e_dump);
    }

    // ---- prefix/restore-semantics assertions ----
    {
        std::vector<Entry> a, b, d, e;
        const bool pa = parse_entries(a_dump, &a), pb = parse_entries(b_dump, &b),
                   pd = parse_entries(d_dump, &d), pe = parse_entries(e_dump, &e);
        check(pa && pb && pd && pe, "B11-12 all dumps parse");
        if (pa && pb && pd && pe) {
            check(b.size() == a.size(), "B11-13 save did not mutate the stack count");
            bool b_pref_a = b.size() == a.size();
            for (size_t i = 0; b_pref_a && i < a.size(); ++i)
                if (a[i].file != b[i].file || a[i].block != b[i].block) b_pref_a = false;
            check(b_pref_a, "B11-13b save kept the stack identical (A == B)");
            check(d.size() == b.size(), "B11-14 load restored the saved stack count");
            bool d_eq_b = d.size() == b.size();
            for (size_t i = 0; d_eq_b && i < b.size(); ++i)
                if (d[i].file != b[i].file || d[i].block != b[i].block) d_eq_b = false;
            check(d_eq_b, "B11-14b load stack == saved stack (D == B, no leftover)");
            bool e_pref_d = e.size() >= d.size();
            for (size_t i = 0; e_pref_d && i < d.size(); ++i)
                if (e[i].file != d[i].file || e[i].block != d[i].block) e_pref_d = false;
            check(e_pref_d, "B11-15 post-load stack extends the saved stack (E starts with D)");
            bool no_dup = true;
            for (size_t i = 1; no_dup && i < e.size(); ++i)
                if (e[i].file == e[i - 1].file && e[i].block == e[i - 1].block) no_dup = false;
            check(no_dup, "B11-16 no adjacent duplicate (file,block) after load");
        }
    }

    // ---- POSTWALK: backlog after the post-load walk (user repro point) ----
    blog_visit(rt, idle, "POSTWALK");

    // ---- deep probes: B11_SELECT=1 (choice-boundary dup) / B11_RESTART=1
    // (fresh-boot Continue stack restore) ----
    if (std::getenv("B11_SELECT") || std::getenv("B11_RESTART")) {
        if (std::getenv("B11_SELECT")) {
        // S: fast-skip to the first select (like p3_select), dump the tail of
        // log.stack before/after choosing option 1; a second entry for the
        // same file@block (select_clicknext's set_backlog_next) is a dup.
        {
            rt.interpreter().enqueue_tag("skip", {{"allow", "1"}, {"unread", "1"}});
            rt.interpreter().enqueue_tag("exec", {{"command", "skip"}, {"mode", "1"}});
            bool sel = false;
            size_t sf = 0;
            for (; sf < 120000 && !rt.exit_requested(); ++sf) {
                rt.tick(16, idle);
                // select rows expose a click handler with key "select"
                for (const oa::render::Layer* l : rt.scene().draw_order()) {
                    const auto* h = rt.scene().find_event_handler(l->id, "click");
                    if (!h) continue;
                    const auto k = h->params.find("key");
                    if (k != h->params.end() && k->second == "select") { sel = true; break; }
                }
                if (sel) break;
            }
            check(sel, "B11-S1 deep skip reached the select screen");
            rt.interpreter().enqueue_tag("exec", {{"command", "skip"}, {"mode", "0"}});
            for (size_t f = 0; f < 400 && !rt.exit_requested(); ++f) rt.tick(16, idle);
            std::printf("[b11] S(pre-choice): %s\n", dump_stack(rt).c_str());
            analyze("S", dump_stack(rt));
            // click option 1 (row center 640,321 per csv.mw.select)
            {
                oa::runtime::FrameInput cl;
                cl.left_click_edge = true;
                cl.left_down = true;
                cl.mouse_x = 640;
                cl.mouse_y = 321;
                for (size_t f = 0; f < 900 && !rt.exit_requested(); ++f) {
                    rt.tick(16, cl);
                    cl.left_click_edge = false;
                    cl.left_down = false;
                }
                bool gone = true;
                for (const oa::render::Layer* l : rt.scene().draw_order()) {
                    const auto* h = rt.scene().find_event_handler(l->id, "click");
                    if (!h) continue;
                    const auto k = h->params.find("key");
                    if (k != h->params.end() && k->second == "select") gone = false;
                }
                check(gone, "B11-S2 choice resolved (select rows gone)");
            }
            (void)park_story(rt, idle, 8000);
            std::string post = dump_stack(rt);
            std::printf("[b11] S(post-choice): %s\n", post.c_str());
            analyze("S+", post);
            // adjacent dup right at the choice boundary?
            std::vector<Entry> sp;
            (void)parse_entries(post, &sp);
            bool sdup = false;
            for (size_t i = 1; !sdup && i < sp.size(); ++i)
                if (sp[i].file == sp[i - 1].file && sp[i].block == sp[i - 1].block) sdup = true;
            std::printf("[b11] S select-boundary adjacent dup: %s\n", sdup ? "YES" : "no");
        }
        }
        // R: fresh runtime on the same store, title Continue -> saved story;        // the Lua log.stack must come back from the save (restore chain).
        {
                        auto fs2 = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
            oa::runtime::GameRuntime rt2(fs2);
            rt2.set_save_store(store);
            rt2.open_project("windows");
            rt2.boot_project();
            check(reach_title(rt2, idle), "B11-R1 fresh boot reached the title");
            // wait for the title menu slide to settle (p3_3e pattern: the row
            // node 500.b.3.0 stops moving), then click the Continue row
            {
                double lx = -1e9;
                size_t quiet = 0;
                for (size_t f = 0; f < 12000 && !rt2.exit_requested(); ++f) {
                    rt2.tick(16, idle);
                    if (rt2.transition().is_in_progress(rt2.now_ms())) continue;
                    const oa::render::Layer* row = rt2.scene().find("500.b.3.0");
                    if (!row) continue;
                    double x0 = 0, y0 = 0, rw = 0, rh = 0;
                    if (!rt2.scene().world_rect("500.b.3.0", 404, 120, &x0, &y0, &rw, &rh))
                        continue;
                    if (x0 < 0) continue;
                    quiet = std::fabs(x0 - lx) < 0.5 ? quiet + 1 : 0;
                    lx = x0;
                    if (quiet > 200) break;
                }
            }
            bool clicked = false;
            for (const oa::render::Layer* l : rt2.scene().draw_order()) {
                const auto* h = rt2.scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto key = h->params.find("key");
                if (key == h->params.end() || key->second != "bt_load2") continue;
                double w = l->has_clip ? l->clip_w : 404.0;
                double hh = l->has_clip ? l->clip_h : 120.0;
                double x0 = 0, y0 = 0, rw = 0, rh = 0;
                if (!rt2.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) {
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
                rt2.tick(16, cl);
                clicked = true;
                break;
            }
            check(clicked, "B11-R1b title Continue row (bt_load2) clicked");
            bool rpark = false;
            for (size_t f = 0; f < 25000 && !rt2.exit_requested(); ++f) {
                rt2.tick(16, idle);
                const auto* w = rt2.current_wait();
                const oa::render::MessageLayer* ml = rt2.text().layer("1.80.mw.adv_adv");
                if (ml && is_story_wait(w) && ml->reveal_index >= ml->char_count &&
                    !ml->reveal_pending) {
                    rpark = true;
                    break;
                }
            }
            check(rpark, "B11-R2 fresh-boot Continue parked on the story");
            std::string r_dump = dump_stack(rt2);
            std::printf("[b11] R(fresh-boot Continue): %s\n", r_dump.c_str());
            std::vector<Entry> bv, rv;
            const bool pb = parse_entries(b_dump, &bv), pr = parse_entries(r_dump, &rv);
            bool eq = pb && pr && bv.size() == rv.size();
            for (size_t i = 0; eq && i < bv.size(); ++i)
                if (bv[i].file != rv[i].file || bv[i].block != rv[i].block) eq = false;
            check(eq, "B11-R3 restart Continue restored the saved log.stack (R == B)");
        }
    }

    std::error_code ec2;
    fsf::remove_all(store_dir, ec2);
    if (failures) {
        std::fprintf(stderr, "backlog_saveload_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("backlog_saveload_test: all ok\n");
    return 0;
}
