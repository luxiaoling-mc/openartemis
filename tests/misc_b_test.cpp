// Misc-B package tests (research/20): var query bridges (get_sound_info /
// file_update_time / file_exists save=1), deferred [takess] capture +
// savess resize, and the skip/automode -> media sync (AudioEngine skipping
// gate + Se-wait skip branch under the P1c2 control machine). Deterministic,
// headless; screenshots persist through an isolated tmp DirSaveStore.
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/fs/fs.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_save.h"
#include "ogg_fixture.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
const char* kind_str(oa::runtime::WaitReason::Kind k) {
    using K = oa::runtime::WaitReason::Kind;
    switch (k) {
        case K::Generic: return "generic";
        case K::Generic0: return "wt0";
        case K::Timed: return "timed";
        case K::Stop: return "stop";
        case K::Se: return "se";
        case K::VideoLayer: return "video";
        case K::ScenarioTween: return "scenario-tween";
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

struct MemFs : oa::fs::IFileSystem {
    std::map<std::string, std::string> files;
    std::optional<std::vector<uint8_t>> read(std::string_view path) const override {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return std::nullopt;
        return std::vector<uint8_t>(it->second.begin(), it->second.end());
    }
    bool exists(std::string_view path) const override {
        return files.count(std::string(path)) > 0;
    }
    const char* kind() const override { return "mem"; }
};

class MemSaveStore final : public oa::runtime::SaveStore {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    bool write(const std::string& rel, const std::vector<uint8_t>& d) override {
        files[rel] = d;
        return true;
    }
    std::optional<std::vector<uint8_t>> read(const std::string& rel) const override {
        const auto it = files.find(rel);
        if (it == files.end()) return std::nullopt;
        return it->second;
    }
    bool remove(const std::string& rel) override {
        return files.erase(rel) > 0;
    }
    bool exists(const std::string& rel) const override {
        return files.count(rel) > 0;
    }
};

std::shared_ptr<MemFs> boot_fs(const char* script) {
    auto fs = std::make_shared<MemFs>();
    fs->files["system.ini"] =
        "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
        "BOOT = system/first.iet\nSAVEPATH = save\n";
    fs->files["system/first.iet"] = std::string("*main\n") + script;
    return fs;
}

// ---------------------------------------------------------------------------
// W1: var system=get_sound_info (BGM slot + SE pseudo array; id query)
// ---------------------------------------------------------------------------
void test_get_sound_info_bridge() {
    auto fs = boot_fs("[stop]\n");
    oa::runtime::GameRuntime rt(fs);
    rt.open_project("windows");
    rt.boot_project();
    // Play channels directly on the engine (media tags would need assets).
    oa::media::BgmConfig bgm;
    bgm.loop_play = true;
    bgm.gain = 700;
    bgm.pan = -100;
    rt.audio().play_bgm("bgm.ogg", bgm);
    oa::media::SeConfig se;
    se.gain = 400;
    se.pan = 100;
    rt.audio().play_se("z9", "a.ogg", se);
    rt.audio().play_se("a1", "b.ogg", se);

    const auto set = [&](const std::string& sys, std::map<std::string, std::string> p) {
        p["system"] = sys;
        rt.interpreter().apply_var(p);
    };
    set("get_sound_info", {{"name", "t.snd"}});
    const auto& vars = rt.interpreter().variables();
    const auto num = [&](const std::string& n) -> long long {
        const auto v = vars.get(n);
        return v && v->kind == oa::runtime::ValueKind::Int ? v->int_val : -999999;
    };
    const auto str = [&](const std::string& n) -> std::string {
        const auto v = vars.get(n);
        return v && v->kind == oa::runtime::ValueKind::String ? v->str_val : std::string();
    };
    check(num("t.snd.playing") == 1, "W1 bgm playing=1");
    check(num("t.snd.gain") == 700 && num("t.snd.pan") == -100, "W1 bgm gain/pan raw");
    check(num("t.snd.size") == 2, "W1 se pseudo-array size");
    check(str("t.snd.0.id") == "a1" && str("t.snd.1.id") == "z9",
          "W1 se sorted by id (a1 then z9)");
    check(num("t.snd.0.gain") == 400, "W1 se gain carried");
    // id query
    set("get_sound_info", {{"name", "t.one"}, {"id", "z9"}});
    check(num("t.one.playing") == 1 && num("t.one.gain") == 400,
          "W1 id query finds the se");
    set("get_sound_info", {{"name", "t.miss"}, {"id", "none"}});
    check(num("t.miss.playing") == 0, "W1 missing id -> only playing=0");
}

// ---------------------------------------------------------------------------
// W2: var system=file_update_time (format tokens; noexist default)
// ---------------------------------------------------------------------------
void test_file_update_time_bridge() {
    auto fs = boot_fs("[stop]\n");
    oa::runtime::GameRuntime rt(fs);
    rt.open_project("windows");
    rt.boot_project();
    // Canned mtime for deterministic formatting.
    rt.interpreter().hooks().file_mtime = [](const std::string&)
        -> std::optional<std::array<int64_t, 6>> {
        return std::array<int64_t, 6>{2026, 9, 4, 7, 17, 5};
    };
    rt.interpreter().apply_var({{"system", "file_update_time"},
                                {"name", "t.tmp"},
                                {"file", "save/save0001.dat"},
                                {"format", "yyyy/MM/dd\nhh:mm"}});
    const auto v = rt.interpreter().variables().get("t.tmp");
    check(v && v->kind == oa::runtime::ValueKind::String && v->str_val == "2026/09/04\n07:17",
          "W2 file_update_time format (yyyy/MM/dd newline hh:mm)");
    // Missing file hook (nullopt) -> noexist default.
    rt.interpreter().hooks().file_mtime = [](const std::string&)
        -> std::optional<std::array<int64_t, 6>> { return std::nullopt; };
    rt.interpreter().apply_var({{"system", "file_update_time"},
                                {"name", "t.missing"},
                                {"file", "save/none.dat"},
                                {"noexist", "-1"}});
    const auto m = rt.interpreter().variables().get("t.missing");
    check(m && m->kind == oa::runtime::ValueKind::String && m->str_val == "-1",
          "W2 absent file returns the noexist value");
}

// ---------------------------------------------------------------------------
// W3: file_exists save=1 sees engine-written save files
// ---------------------------------------------------------------------------
void test_file_exists_save_bridge() {
    auto store = std::make_shared<MemSaveStore>();
    auto fs = boot_fs("[stop]\n");
    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();
    // Write a numbered save through the store directly (engine face) and
    // query with the var bridge.
    oa::runtime::SaveData d;
    d.current_script = "story.iet";
    const std::string json = d.encode();
    store->write("save/save01.dat",
                 std::vector<uint8_t>(json.begin(), json.end()));
    rt.interpreter().apply_var({{"system", "file_exists"},
                                {"name", "t.f"},
                                {"file", "save/save01.dat"},
                                {"save", "1"}});
    const auto v = rt.interpreter().variables().get("t.f");
    check(v && v->kind == oa::runtime::ValueKind::Bool && v->bool_val,
          "W3 file_exists save=1 true for an engine save");
    rt.interpreter().apply_var({{"system", "file_exists"},
                                {"name", "t.g"},
                                {"file", "save/nope.dat"},
                                {"save", "1"}});
    const auto g = rt.interpreter().variables().get("t.g");
    check(g && g->kind == oa::runtime::ValueKind::Bool && !g->bool_val,
          "W3 file_exists save=1 false for a missing save");
}

// ---------------------------------------------------------------------------
// W4: deferred [takess] + savess resize
// ---------------------------------------------------------------------------
void test_takess_deferred_and_resize() {
    namespace fsf = std::filesystem;
    std::error_code ec;
    const fsf::path root = fsf::temp_directory_path() /
                           ("oa_miscb_" + std::to_string(::getpid()));
    fsf::remove_all(root, ec);
    auto store = std::make_shared<oa::runtime::DirSaveStore>(root.string());
    auto fs = boot_fs("[stop]\n");
    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();
    // 2x1 frame: left half red, right half blue.
    rt.set_frame_capture([](uint32_t* w, uint32_t* h)
                             -> std::optional<std::vector<uint8_t>> {
        *w = 2;
        *h = 1;
        std::vector<uint8_t> rgba(8);
        rgba[0] = 255; rgba[1] = 0; rgba[2] = 0; rgba[3] = 255;
        rgba[4] = 0; rgba[5] = 0; rgba[6] = 255; rgba[7] = 255;
        return rgba;
    });
    oa::runtime::Event take;
    take.kind = oa::runtime::Event::Kind::ConfigEvent;
    take.tag = "takess";
    rt.apply_save_event(take);
    // Capture is deferred until the host's end-of-frame hook fires.
    rt.post_frame_capture();
    oa::runtime::Event shot;
    shot.kind = oa::runtime::Event::Kind::ConfigEvent;
    shot.tag = "savess";
    shot.params["file"] = "thumb";
    shot.params["width"] = "4"; // upscale 2x1 -> 4x1
    shot.params["height"] = "2";
    rt.apply_save_event(shot);
    const auto png = store->read("save/thumb.png");
    check(png.has_value(), "W4 savess wrote the resized thumbnail");
    if (png) {
        const uint32_t w = (uint32_t((*png)[16]) << 24) | (uint32_t((*png)[17]) << 16) |
                           (uint32_t((*png)[18]) << 8) | uint32_t((*png)[19]);
        const uint32_t h = (uint32_t((*png)[20]) << 24) | (uint32_t((*png)[21]) << 16) |
                           (uint32_t((*png)[22]) << 8) | uint32_t((*png)[23]);
        check(w == 4 && h == 2, "W4 resized thumbnail dims (4x2)");
    }
    fsf::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// W5: P1c2 skip machine reaches the audio engine + releases Se waits
// ---------------------------------------------------------------------------
void test_skip_reaches_media() {
    auto store = std::make_shared<MemSaveStore>();
    auto fs = boot_fs(
        "[skip allow=\"1\"]\n"
        "[seplay id=\"s\" file=\"s.ogg\" loop=\"1\"]\n"
        "[wait se=\"s\"]\n"
        "[stop]\n");
    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.set_media_loader([](const std::string& name)
                            -> std::optional<std::vector<uint8_t>> {
        if (name == "s.ogg") return oafix::tone_ogg(1.0, 1, 44100, 440.0, 0.3f);
        return std::nullopt;
    });
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    bool parked = false;
    for (int i = 0; i < 60 && !rt.exit_requested(); ++i) {
        rt.tick(16, idle);
        const oa::runtime::WaitReason* w = rt.current_wait();
        if (w && w->kind == oa::runtime::WaitReason::Kind::Se && w->id == "s") {
            parked = true;
            break;
        }
    }
    check(parked, "W5 script parked in the Se wait without skip");
    check(!rt.audio().state().is_skipping, "W5 no skip active yet");
    // Enable the P1c2 skip machine from the parked queue.
    rt.interpreter().enqueue_tag("exec", {{"command", "skip"}, {"mode", "1"}});
    bool skipping = false;
    bool released = false;
    for (int i = 0; i < 60 && !rt.exit_requested(); ++i) {
        rt.tick(16, idle);
        if (rt.audio().state().is_skipping) skipping = true;
        const oa::runtime::WaitReason* w = rt.current_wait();
        if (skipping && w && w->kind == oa::runtime::WaitReason::Kind::Stop &&
            w->id.empty()) {
            released = true;
            break;
        }
        if (!skipping && w == nullptr) break;
    }
    std::printf("[W5] skip reached engine=%s released=%s final=%s\n",
                skipping ? "yes" : "no", released ? "yes" : "no",
                wait_str(rt.current_wait()).c_str());
    check(skipping, "W5 control skip reaches the audio engine (set_skipping)");
    check(released, "W5 skip-active releases the [wait se] (skip branch)");
}

void test_lytween_cancel_completion_survival() {
    // research/63b: [lytweendel]-cancelled handler tweens deliver their
    // completion only while the tween layer survived the frame's script
    // work. Config page switches run config_delsample() (lytweendel) before
    // the csvbtn3 [lydel 500]+rebuild of the SAME frame; delivering there
    // would reprint the sample preview onto the next page (snll/NekoMiko
    // sample-preview bleed across every config page). Same-frame same-page
    // cancels (FPM r10e slider drag) and natural finishes keep delivering.
    auto fs = boot_fs(
        "[lua]\n"
        "function round_del(e)\n"
        "  e:tag{'chgmsg', id='m.del', layered='1'} e:tag{'rp'}\n"
        "  e:tag{'print', data='FIRED-DEL'} e:tag{'/chgmsg'}\n"
        "end\n"
        "function round_keep(e)\n"
        "  e:tag{'chgmsg', id='m.keep', layered='1'} e:tag{'rp'}\n"
        "  e:tag{'print', data='FIRED-KEEP'} e:tag{'/chgmsg'}\n"
        "end\n"
        "function round_nat(e)\n"
        "  e:tag{'chgmsg', id='m.nat', layered='1'} e:tag{'rp'}\n"
        "  e:tag{'print', data='FIRED-NAT'} e:tag{'/chgmsg'}\n"
        "end\n"
        "function arm_cases(engine)\n"
        "  e = engine\n"
        "  -- cancelled round + same-frame layer teardown -> no delivery\n"
        "  e:tag{'lyc2', id='g.del', width='10', height='10', x='0', y='0'}\n"
        "  e:tag{'lytween', id='g.del', param='alpha', from='254', to='255',\n"
        "        time='100000', handler='calllua', ['function']='round_del'}\n"
        "  e:tag{'lytweendel', id='g.del'}\n"
        "  e:tag{'lydel', id='g.del'}\n"
        "  -- cancelled round, layer kept -> R10e delivery preserved\n"
        "  e:tag{'lyc2', id='g.keep', width='10', height='10', x='0', y='0'}\n"
        "  e:tag{'lytween', id='g.keep', param='alpha', from='254', to='255',\n"
        "        time='100000', handler='calllua', ['function']='round_keep'}\n"
        "  e:tag{'lytweendel', id='g.keep'}\n"
        "  -- natural finish stays a normal delivery\n"
        "  e:tag{'lyc2', id='g.nat', width='10', height='10', x='0', y='0'}\n"
        "  e:tag{'lytween', id='g.nat', param='alpha', from='254', to='255',\n"
        "        time='1', handler='calllua', ['function']='round_nat'}\n"
        "end\n"
        "[/lua]\n"
        "[calllua function=\"arm_cases\"]\n"
        "[stop]\n");
    oa::runtime::GameRuntime rt(fs);
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    for (int i = 0; i < 240 && !rt.exit_requested(); ++i) rt.tick(16, idle);
    auto content = [&](const char* id) {
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        if (!ml) return std::string("-no-layer-");
        for (const auto& u : ml->page)
            if (!u.data.empty()) return u.data;
        return std::string("-empty-");
    };
    std::printf("[63b] del='%s' keep='%s' nat='%s'\n", content("m.del").c_str(),
                content("m.keep").c_str(), content("m.nat").c_str());
    const std::string del = content("m.del");
    check(del == "-no-layer-" || del == "-empty-",
          "63b cancelled round with torn-down layer does NOT deliver");
    check(content("m.keep") == "FIRED-KEEP",
          "63b cancelled round with live layer still delivers (R10e)");
    check(content("m.nat") == "FIRED-NAT", "63b natural finish delivers");
}

void test_natural_tween_completion_survival() {
    // research/71: natural finishes of handler tweens are staged exactly like
    // [lytweendel] cancels (research/63b) and delivered only while the
    // tween's layer still exists at the next frame-end flush. snll config:
    // the sample preview round (lytween on the 500.sample timer layer) ends
    // naturally ~300 ms after arming; if the user switches the config page
    // the same frame the round ended, the queued completion must not drain
    // into the NEW page (it would reprint 500.z.text and re-arm a fresh
    // round there — "切页时预览文字显示"). An alive layer (normal page-2
    // rounds) delivers exactly as before, one frame later.
    auto fs = boot_fs(
        "[lua]\n"
        "function round_71(e)\n"
        "  e:tag{'chgmsg', id='m.71', layered='1'} e:tag{'rp'}\n"
        "  e:tag{'print', data='FIRED-71'} e:tag{'/chgmsg'}\n"
        "end\n"
        "function round_cal(e)\n"
        "  e:tag{'chgmsg', id='m.cal', layered='1'} e:tag{'rp'}\n"
        "  e:tag{'print', data='CAL'} e:tag{'/chgmsg'}\n"
        "end\n"
        "function arm_nat71(engine)\n"
        "  e = engine\n"
        "  e:tag{'lyc2', id='g.71', width='10', height='10', x='0', y='0'}\n"
        "  e:tag{'lytween', id='g.71', param='alpha', from='254', to='255',\n"
        "        time='1', handler='calllua', ['function']='round_71'}\n"
        "end\n"
        "function arm_natcal(engine)\n"
        "  e = engine\n"
        "  e:tag{'lyc2', id='g.71', width='10', height='10', x='0', y='0'}\n"
        "  e:tag{'lytween', id='g.71', param='alpha', from='254', to='255',\n"
        "        time='1', handler='calllua', ['function']='round_cal'}\n"
        "end\n"
        "[/lua]\n"
        "[stop]\n");
    oa::runtime::GameRuntime rt(fs);
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    auto content = [&](const char* id) {
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        if (!ml) return std::string("-no-layer-");
        for (const auto& u : ml->page)
            if (!u.data.empty()) return u.data;
        return std::string("-empty-");
    };
    // Calibration on a dedicated round: find the first tick the delivery
    // lands (finish tick = delivery tick - 2 in this engine: the finish
    // stages at an advance, the flush happens at the NEXT advance, and the
    // queued completion drains in the tick after that).
    int finish_tick = -1;
    {
        rt.interpreter().enqueue_tag("calllua",
                                      {{"function", "arm_natcal"}});
        for (int i = 1; i <= 40 && !rt.exit_requested(); ++i) {
            rt.tick(16, idle);
            if (content("m.cal") == "CAL") {
                finish_tick = i - 2;
                std::printf("[71] calibration: delivery at tick %d -> finish "
                            "tick %d\n",
                            i, finish_tick);
                break;
            }
        }
        check(finish_tick >= 1, "71 calibration found the finish tick");
        if (finish_tick < 1) return;
    }
    // ---- case A: the layer is torn down right after the natural finish
    // (before the staged flush) -> the completion must NOT deliver ----
    rt.interpreter().enqueue_tag("calllua",
                                      {{"function", "arm_nat71"}});
    for (int i = 0; i < finish_tick && !rt.exit_requested(); ++i)
        rt.tick(16, idle);
    rt.interpreter().enqueue_tag("lydel", {{"id", "g.71"}});
    for (int i = 0; i < 40 && !rt.exit_requested(); ++i) rt.tick(16, idle);
    const std::string a = content("m.71");
    std::printf("[71] natural+teardown content='%s'\n", a.c_str());
    check(a == "-no-layer-" || a == "-empty-",
          "71 naturally-finished round whose layer died before the flush "
          "does NOT deliver");
    // ---- case B: layer survives -> normal delivery ----
    rt.interpreter().enqueue_tag("calllua",
                                      {{"function", "arm_nat71"}});
    for (int i = 0; i < 60 && !rt.exit_requested(); ++i) rt.tick(16, idle);
    const std::string b = content("m.71");
    std::printf("[71] natural+alive content='%s'\n", b.c_str());
    check(b == "FIRED-71", "71 naturally-finished round with a live layer "
                            "still delivers");
}

void test_var_get_layer_info_flush() {
    // research/69 (snll text-page sample preview): Lua e:tag events are
    // queued while a var system=get_layer_info tag applies synchronously, so
    // a read following e:tag{'lyc2'...} in the SAME Lua call used to see the
    // pre-lyc2 scene (queried layer missing -> empty dump -> snll
    // config_samplestart's alpha==255 guard failed on first page-2 entry and
    // the sample round never armed; preview appeared only after a slider
    // drag). Ordered semantics (L2 M10c/R10): a sync var evaluates at its
    // position in the tag stream — apply_var now runs the queued prefix
    // (flush_pending_text_tags) before reading the layer.
    auto fs = boot_fs("[stop]\n");
    oa::runtime::GameRuntime rt(fs);
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    rt.tick(16, idle);
    // Queue a layer creation the way a Lua e:tag would (deferred), then
    // apply the get_layer_info var synchronously.
    rt.interpreter().enqueue_tag("lyc2",
                                 {{"id", "probe.g"}, {"width", "10"},
                                  {"height", "10"}, {"x", "5"}, {"y", "6"}});
    rt.interpreter().apply_var({{"id", "probe.g"},
                                {"system", "get_layer_info"},
                                {"name", "q.ly"},
                                {"style", "map"}});
    const auto v = rt.interpreter().variables().get("q.ly.visible");
    const auto vx = rt.interpreter().variables().get("q.ly.left");
    check(v.has_value() && v->kind == oa::runtime::ValueKind::String &&
              v->str_val == "1",
          "69 get_layer_info after a queued lyc2 sees the created layer");
    check(vx.has_value() && vx->kind == oa::runtime::ValueKind::String &&
              vx->str_val == "5",
          "69 get_layer_info reads the lyc2 geometry (left=5)");
    // Control: without any queued prefix the same read still works.
    rt.interpreter().enqueue_tag("lyc2",
                                 {{"id", "probe.h"}, {"width", "1"},
                                  {"height", "1"}, {"x", "0"}, {"y", "0"}});
    rt.tick(16, idle); // drain queue
    rt.interpreter().apply_var({{"id", "probe.h"},
                                {"system", "get_layer_info"},
                                {"name", "r.ly"},
                                {"style", "map"}});
    const auto rv = rt.interpreter().variables().get("r.ly.visible");
    check(rv.has_value() && rv->kind == oa::runtime::ValueKind::String &&
              rv->str_val == "1",
          "69 plain get_layer_info read unchanged");
}

// ---------------------------------------------------------------------------
// W6: automode pacing + voice-sync gate (research/83). The FPM-family Lua
// layer (autoskip.lua automode_start/clickAutomode) queues
// [automode syncse=<current voice ids>] at every automode click park when
// conf.autostop==1 and clears it (syncse="") on page release / automode
// stop; the interval comes from the s.automodewait var (written by
// clickAutomode). The engine must hold the auto page flip while a listed
// voice channel still plays and must pace by s.automodewait (900 ms
// fallback), never by a fixed timer alone.
// ---------------------------------------------------------------------------
struct AutoBoot {
    std::shared_ptr<MemFs> fs;
    oa::runtime::GameRuntime rt;
    oa::runtime::FrameInput idle;
    AutoBoot(const char* body, bool voice) : fs(boot_fs(body)), rt(fs) {
        rt.open_project("windows");
        rt.boot_project();
        // Swap the loader AFTER boot: open_project installs the project-fs
        // loader, and the media players resolve assets lazily on the first
        // tick — install the synthetic tone loader before any tick (same
        // pattern as media_runtime_test R2-R4).
        rt.set_media_loader([voice](const std::string& name)
                                -> std::optional<std::vector<uint8_t>> {
            if (voice && name == "v.ogg")
                return oafix::tone_ogg(2.0, 1, 44100, 440.0, 0.3f);
            return std::nullopt;
        });
    }
    // Tick until a Generic/Generic0 wait parks (budget frames).
    size_t park_generic(size_t budget = 300) {
        for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const auto* w = rt.current_wait();
            if (w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                      w->kind == oa::runtime::WaitReason::Kind::Generic0))
                return f;
        }
        return size_t(-1);
    }
    // Tick until a non-Generic wait parks (release of the auto gate).
    size_t wait_release(size_t budget = 1200) {
        for (size_t f = 0; f < budget && !rt.exit_requested(); ++f) {
            rt.tick(16, idle);
            const auto* w = rt.current_wait();
            if (w && !(w->kind == oa::runtime::WaitReason::Kind::Generic ||
                       w->kind == oa::runtime::WaitReason::Kind::Generic0))
                return f;
        }
        return size_t(-1);
    }
};

void test_automode_voice_gate() {
    // W6a: a voiced park under [automode syncse] must NOT flip at the 200 ms
    // pacing interval — the 2 s tone holds the page until its EOF, and the
    // release tick arrives with the channel already stopped. An unknown
    // second id (never started) must not block.
    AutoBoot b(
        "[automode allow=\"1\"]\n"
        "[exec command=\"automode\" mode=\"1\"]\n"
        "[var name=\"s.automodewait\" data=\"200\"]\n"
        "[seplay id=\"1\" file=\"v.ogg\"]\n"
        "[@]\n"
        "[stop]\n",
        true);
    const size_t pk = b.park_generic();
    check(pk != size_t(-1), "W6a parked in the generic click wait");
    if (pk == size_t(-1)) return;
    check(b.rt.audio().is_sound_playing("1"), "W6a voice playing at the park");
    // the Lua clickAutomode equivalent, queued while parked
    b.rt.interpreter().enqueue_tag("automode", {{"syncse", "1,2"}});
    // crossing the 200 ms interval (park + ~30 ticks) must stay parked
    size_t hold_breaks = 0;
    for (size_t f = 0; f < 30; ++f) {
        b.rt.tick(16, b.idle);
        const auto* w = b.rt.current_wait();
        if (!(w && (w->kind == oa::runtime::WaitReason::Kind::Generic ||
                    w->kind == oa::runtime::WaitReason::Kind::Generic0)))
            ++hold_breaks;
        if (!b.rt.audio().is_sound_playing("1")) ++hold_breaks;
    }
    check(hold_breaks == 0,
          "W6a gate holds the page past the pacing interval while the voice "
          "plays");
    bool playing_at_release = false;
    bool reached_stop = false;
    const size_t rl = b.wait_release();
    if (rl != size_t(-1)) {
        playing_at_release = b.rt.audio().is_sound_playing("1");
        const auto* w = b.rt.current_wait();
        if (w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty())
            reached_stop = true;
    }
    std::printf("[W6a] park@%zu release@%zu playing_at_release=%d stop=%d\n", pk, rl,
                (int)playing_at_release, (int)reached_stop);
    check(rl != size_t(-1) && rl > pk + 60,
          "W6a released only after the voice channel ended (not at 200 ms)");
    check(!playing_at_release, "W6a release tick has the voice channel stopped");
    check(reached_stop, "W6a flow continued to [stop] after the voice gate");
}

void test_automode_no_gate_when_cleared() {
    // W6b: [automode syncse=""] (FPM clickEnd clears the gate on page
    // release) or no syncse at all = plain pacing by s.automodewait — the
    // page flips at ~200 ms even while the voice still plays.
    AutoBoot b(
        "[automode allow=\"1\"]\n"
        "[exec command=\"automode\" mode=\"1\"]\n"
        "[var name=\"s.automodewait\" data=\"200\"]\n"
        "[seplay id=\"1\" file=\"v.ogg\"]\n"
        "[@]\n"
        "[stop]\n",
        true);
    const size_t pk = b.park_generic();
    check(pk != size_t(-1), "W6b parked in the generic click wait");
    if (pk == size_t(-1)) return;
    b.rt.interpreter().enqueue_tag("automode", {{"syncse", ""}});
    bool playing_at_release = false;
    const size_t rl = b.wait_release();
    if (rl != size_t(-1)) playing_at_release = b.rt.audio().is_sound_playing("1");
    std::printf("[W6b] park@%zu release@%zu playing_at_release=%d\n", pk, rl,
                (int)playing_at_release);
    check(rl != size_t(-1) && rl > pk + 8 && rl < pk + 60,
          "W6b cleared gate flips on the 200 ms pacing interval");
    check(playing_at_release, "W6b voice still playing at the paced release (no gate)");
}

void test_automode_pacing_interval() {
    // W6c: the interval reads s.automodewait (2500 ms here) — not a fixed
    // constant; W6d: with the var unset the 900 ms historical default
    // applies. [wt0] parks (Generic0) ride the same automode gate.
    {
        AutoBoot b(
            "[automode allow=\"1\"]\n"
            "[exec command=\"automode\" mode=\"1\"]\n"
            "[var name=\"s.automodewait\" data=\"2500\"]\n"
            "[@]\n"
            "[stop]\n",
            false);
        const size_t pk = b.park_generic();
        const size_t rl = b.wait_release();
        std::printf("[W6c] var=2500 park@%zu release@%zu\n", pk, rl);
        check(pk != size_t(-1) && rl != size_t(-1) && rl > pk + 120 && rl < pk + 240,
              "W6c pacing uses s.automodewait=2500 ms");
    }
    {
        AutoBoot b(
            "[automode allow=\"1\"]\n"
            "[exec command=\"automode\" mode=\"1\"]\n"
            "[wt0]\n"
            "[stop]\n",
            false);
        const size_t pk = b.park_generic();
        const size_t rl = b.wait_release();
        std::printf("[W6d] default park@%zu release@%zu\n", pk, rl);
        check(pk != size_t(-1) && rl != size_t(-1) && rl > pk + 40 && rl < pk + 100,
              "W6d fallback 900 ms pacing on a [wt0] park without the var");
    }
}

} // namespace

int main() {
    test_get_sound_info_bridge();
    test_file_update_time_bridge();
    test_file_exists_save_bridge();
    test_takess_deferred_and_resize();
    test_skip_reaches_media();
    test_lytween_cancel_completion_survival();
    test_natural_tween_completion_survival();
    test_var_get_layer_info_flush();
    test_automode_voice_gate();
    test_automode_no_gate_when_cleared();
    test_automode_pacing_interval();
    if (failures) {
        std::fprintf(stderr, "misc_b_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("misc_b_test: all ok\n");
    return 0;
}
