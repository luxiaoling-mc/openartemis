// P5b save-domain tests (research/17): first-boot system-save behavior
// (missing saveg/system.dat = silent, empty g./s. domains), syssave/sysload
// round trips across runtime instances, numbered saves (local vars + script
// position + scene-lite + audio snapshot) and their restore, savess/takess
// thumbnail writing, file-command delete/copy, autosave-on-input-wait and the
// SaveData/domain-map binary formats (research/64). Persistence goes through an
// in-memory store (deterministic, isolated) plus one real-filesystem tmp-dir
// round trip.
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
#include "core/runtime/runtime_save.h"
#include "core/util/binary_stream.h"
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

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
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
        const auto it = files.find(rel);
        if (it == files.end()) return false;
        files.erase(it);
        return true;
    }
    bool exists(const std::string& rel) const override {
        return files.count(rel) > 0;
    }
};

std::string project_ini() {
    return "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
           "BOOT = system/first.iet\nSAVEPATH = save\n";
}

// Story script saved mid-flow; loader script issues [load].
const char* kStoryIet = R"(*main
[var name="foo" data="hello"]
[var name="n" data="42"]
[lyc id="bg" file="bg.png"]
[lyprop id="bg" alpha="128"]
[lyevent id="bg" type="click" function="fn_bg" name="ttl"]
[splay file="bgm_a.ogg" loop="1" gain="700" pan="-100"]
[seplay id="se1" file="click.ogg" gain="400"]
[print data="page one"]
[save file="save01.dat"]
[@]
[print data="page two"]
[@]
[stop]
)";
const char* kLoaderIet = "*main\n[load file=\"save01.dat\"]\n[stop]\n";

std::shared_ptr<MemFs> story_fs() {
    auto fs = std::make_shared<MemFs>();
    fs->files["system.ini"] = project_ini();
    fs->files["system/first.iet"] =
        "*main\n[call file=\"story.iet\" label=\"main\"]\n";
    fs->files["story.iet"] = kStoryIet;
    return fs;
}

std::shared_ptr<MemFs> loader_fs() {
    auto fs = std::make_shared<MemFs>();
    fs->files["system.ini"] = project_ini();
    fs->files["system/first.iet"] = kLoaderIet;
    fs->files["story.iet"] = kStoryIet;
    return fs;
}

// ---------------------------------------------------------------------------
// T1/T2: system saves (saveg.dat/system.dat)
// ---------------------------------------------------------------------------
void test_syssave_sysload() {
    auto store = std::make_shared<MemSaveStore>();
    {
        auto fs = std::make_shared<MemFs>();
        fs->files["system.ini"] = project_ini();
        fs->files["system/first.iet"] = "*main\n[stop]\n";
        oa::runtime::GameRuntime rt(fs);
        rt.set_save_store(store);
        rt.open_project("windows");
        rt.boot_project();
        // First boot with no save files: sysload stays silent; domains empty.
        check(!store->exists("save/saveg.dat"), "T1 first boot has no saveg.dat");
        check(!rt.interpreter().variables().get("s.bgmvol").has_value(),
              "T1 no default system vars from files (Lua seeds its own)");
        // Persist g./s. domains like a no-file [save].
        rt.interpreter().set_variable("g.alpha", oa::runtime::Value::make_string("1"));
        rt.interpreter().set_variable("g.saveslot",
                                      oa::runtime::Value::make_int(3));
        rt.interpreter().set_variable("s.bgmvol",
                                      oa::runtime::Value::make_int(700));
        check(rt.syssave(), "T2 syssave writes the system files");
        check(store->exists("save/saveg.dat") && store->exists("save/system.dat"),
              "T2 saveg.dat + system.dat written under savepath");
    }
    {
        // New runtime, same store: boot sysload restores the persisted domains.
        auto fs = std::make_shared<MemFs>();
        fs->files["system.ini"] = project_ini();
        fs->files["system/first.iet"] = "*main\n[stop]\n";
        oa::runtime::GameRuntime rt(fs);
        rt.set_save_store(store);
        rt.open_project("windows");
        rt.boot_project();
        const auto& vars = rt.interpreter().variables();
        const auto ga = vars.get("g.alpha");
        check(ga && ga->kind == oa::runtime::ValueKind::String && ga->str_val == "1",
              "T2 sysload restored g.alpha");
        const auto sl = vars.get("g.saveslot");
        check(sl && sl->kind == oa::runtime::ValueKind::Int && sl->int_val == 3,
              "T2 sysload restored g.saveslot (int)");
        const auto bv = vars.get("s.bgmvol");
        check(bv && bv->kind == oa::runtime::ValueKind::Int && bv->int_val == 700,
              "T2 sysload restored s.bgmvol");
        // [save] without file == syssave (engine event path).
        rt.interpreter().set_variable("g.more", oa::runtime::Value::make_string("x"));
        oa::runtime::Event e;
        e.kind = oa::runtime::Event::Kind::ConfigEvent;
        e.tag = "save";
        rt.apply_save_event(e);
        const auto more = store->read("save/saveg.dat");
        check(more.has_value() &&
                  oa::runtime::decode_domain_map(std::string(more->begin(), more->end()))
                          .count("more") == 1,
              "T2 [save] without file persists the g. domain");
    }
}

// ---------------------------------------------------------------------------
// T3: numbered save + load restore (vars / position / scene-lite / audio)
// ---------------------------------------------------------------------------
void test_numbered_save_load() {
    auto store = std::make_shared<MemSaveStore>();
    auto fs = story_fs();
    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    size_t f = 0;
    bool saved = false;
    for (; f < 600 && !rt.exit_requested(); ++f) {
        rt.tick(16, idle);
        if (store->exists("save/save01.dat")) {
            saved = true;
            break;
        }
    }
    check(saved, "T3 numbered save file written (save/save01.dat)");
    std::printf("[T3] save frame=%zu wait=%s pos=%s:%zu\n", f,
                wait_str(rt.current_wait()).c_str(),
                rt.interpreter().current_script()
                    ? rt.interpreter().current_script()->c_str()
                    : "(none)",
                rt.interpreter().current_line());
    check(rt.interpreter().current_script() &&
              *rt.interpreter().current_script() == "story.iet",
          "T3 save taken inside story.iet");
    // Sanity of the binary layout: header (magic "OASB" + u32 LE version 1)
    // then a decodable payload.
    const auto bytes = store->read("save/save01.dat");
    check(bytes.has_value(), "T3 save content readable");
    if (bytes) {
        check(bytes->size() >= 8 && (*bytes)[0] == 'O' && (*bytes)[1] == 'A' &&
                  (*bytes)[2] == 'S' && (*bytes)[3] == 'B',
              "T3 save starts with the OASB binary magic");
        const uint32_t hdr_version = uint32_t((*bytes)[4]) | (uint32_t((*bytes)[5]) << 8) |
                                     (uint32_t((*bytes)[6]) << 16) |
                                     (uint32_t((*bytes)[7]) << 24);
        check(hdr_version == 1, "T3 binary header version 1");
        oa::runtime::SaveData data = oa::runtime::SaveData::decode(
            std::string(bytes->begin(), bytes->end()));
        check(data.version == 1, "T3 format version 1");
        check(data.local_variables.count("foo") == 1, "T3 local var foo in save");
        check(data.current_script == "story.iet", "T3 script position in save");
        check(data.has_audio && data.audio.bgm.has_value() &&
                  data.audio.bgm->gain == 700,
              "T3 audio snapshot carries the bgm gain");
        check(data.has_audio && data.audio.se.size() == 1 &&
                  data.audio.se[0].id == "se1" && data.audio.se[0].gain == 400,
              "T3 audio snapshot carries the se channel");
        bool found_bg = false;
        for (const auto& l : data.layers) {
            if (l.id == "bg") found_bg = true;
        }
        check(found_bg, "T3 scene snapshot carries the bg layer");
    }

    // A fresh runtime boots a loader and restores the save.
    auto fs2 = loader_fs();
    oa::runtime::GameRuntime rt2(fs2);
    rt2.set_save_store(store);
    rt2.set_media_loader([](const std::string& name)
                             -> std::optional<std::vector<uint8_t>> {
        if (name == "bgm_a.ogg" || name == "bgm_b.ogg") {
            return oafix::tone_ogg(2.0, 1, 44100, 220.0, 0.2f);
        }
        if (name == "click.ogg") return oafix::tone_ogg(0.5, 1, 44100, 880.0, 0.3f);
        return std::nullopt;
    });
    rt2.open_project("windows");
    rt2.boot_project();
    // Boot parked the loader at [stop] with the [load] event still queued;
    // apply the restore directly (no media pump before the assertions).
    check(rt2.load_game_from("save01.dat", -1), "T3 load restored from file");
    std::printf("[T3] direct-load pos=%s:%zu wait=%s\n",
                rt2.interpreter().current_script()
                    ? rt2.interpreter().current_script()->c_str()
                    : "(none)",
                rt2.interpreter().current_line(), wait_str(rt2.current_wait()).c_str());
    const auto foo = rt2.interpreter().variables().get("foo");
    check(foo && foo->kind == oa::runtime::ValueKind::String && foo->str_val == "hello",
          "T3 local var foo restored after load");
    check(rt2.interpreter().current_script() &&
              *rt2.interpreter().current_script() == "story.iet",
          "T3 interpreter position restored to story.iet");
    // Scene-lite restore.
    const oa::render::Layer* bg = rt2.scene().find("bg");
    check(bg != nullptr, "T3 scene layer bg restored");
    if (bg) {
        const auto a = bg->props.find("alpha");
        check(a != bg->props.end() && a->second == "128",
              "T3 layer props restored (alpha=128)");
    }
    // Scene-lite event-registration restore (misc-B: layer handler rows are
    // part of the  Scene snapshot).
    {
        const auto* h = rt2.scene().find_event_handler("bg", "click");
        check(h != nullptr, "T3 restored click handler on bg");
        if (h) {
            const auto fn = h->params.find("function");
            check(fn != h->params.end() && fn->second == "fn_bg",
                  "T3 restored handler keeps its function extra");
            check(h->enabled, "T3 restored handler enabled");
        }
    }
    // Audio restore (channels replay from their start; gains raw).
    check(rt2.audio().is_bgm_playing(), "T3 bgm restored playing");
    check(rt2.audio().state().bgm_channel &&
              rt2.audio().state().bgm_channel->raw_gain == 700,
          "T3 bgm gain restored");
    check(rt2.audio().state().bgm_channel &&
              rt2.audio().state().bgm_channel->loop_file.has_value(),
          "T3 bgm A-B loop file re-derived on restore (bgm_a -> bgm_b)");
    check(rt2.audio().is_se_playing("se1"), "T3 se1 restored playing");
    // Keep ticking a bit: resumed flow must not crash and text was cleared
    // ( clear_scene_text on load).
    for (int i = 0; i < 20; ++i) rt2.tick(16, idle);
}

// ---------------------------------------------------------------------------
// T4: savess/takess + [file] commands + autosave
// ---------------------------------------------------------------------------
void test_screenshots_and_files() {
    auto store = std::make_shared<MemSaveStore>();
    auto fs = std::make_shared<MemFs>();
    fs->files["system.ini"] = project_ini();
    fs->files["system/first.iet"] = "*main\n[stop]\n";
    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    rt.tick(16, idle);

    // [takess] without a host capture then [savess]: placeholder PNG.
    oa::runtime::Event take;
    take.kind = oa::runtime::Event::Kind::ConfigEvent;
    take.tag = "takess";
    oa::runtime::Event save_shot;
    save_shot.kind = oa::runtime::Event::Kind::ConfigEvent;
    save_shot.tag = "savess";
    save_shot.params["file"] = "save01";
    save_shot.params["width"] = "32";
    save_shot.params["height"] = "18";
    // Events go through the normal dispatch pipeline (they land in the host
    // stream from interpreter execution only; drive via the public API).
    rt.apply_save_event(take);
    rt.apply_save_event(save_shot);
    check(store->exists("save/save01.png"), "T4 savess wrote save/save01.png");
    if (auto png = store->read("save/save01.png")) {
        check(png->size() > 33 && (*png)[1] == 'P' && (*png)[2] == 'N' &&
                  (*png)[3] == 'G',
              "T4 thumbnail is a PNG");
        // IHDR width at bytes 16..19.
        const uint32_t w = (uint32_t((*png)[16]) << 24) | (uint32_t((*png)[17]) << 16) |
                           (uint32_t((*png)[18]) << 8) | uint32_t((*png)[19]);
        check(w == 32, "T4 thumbnail width from IHDR");
    }
    // [file command=delete target=...] removes a save-area file.
    oa::runtime::Event del;
    del.kind = oa::runtime::Event::Kind::Custom;
    del.tag = "file";
    del.params["command"] = "delete";
    del.params["target"] = "save01.png";
    rt.apply_save_event(del);
    check(!store->exists("save/save01.png"), "T4 [file delete] removed the thumbnail");

    // [autosave allow=2] triggers a save when an input wait parks.
    auto fs2 = std::make_shared<MemFs>();
    fs2->files["system.ini"] = project_ini();
    fs2->files["system/first.iet"] = "*main\n[@]\n[stop]\n";
    oa::runtime::GameRuntime rt2(fs2);
    rt2.set_save_store(store);
    oa::runtime::Event auto_cfg;
    auto_cfg.kind = oa::runtime::Event::Kind::ConfigEvent;
    auto_cfg.tag = "autosave";
    auto_cfg.params["allow"] = "2";
    rt2.apply_save_event(auto_cfg);
    rt2.open_project("windows");
    rt2.boot_project();
    bool autosaved = false;
    for (int i = 0; i < 60 && !rt2.exit_requested(); ++i) {
        rt2.tick(16, idle);
        if (store->exists("save/autosave.dat")) {
            autosaved = true;
            break;
        }
    }
    check(autosaved, "T5 [autosave allow=2] wrote autosave.dat on an input wait");
}

// ---------------------------------------------------------------------------
// T6: SaveData binary round trips + type fidelity + format guards
// ---------------------------------------------------------------------------
void test_formats() {
    oa::runtime::SaveData d;
    d.local_variables["foo"] = oa::runtime::Value::make_string("hello");
    d.local_variables["n"] = oa::runtime::Value::make_int(42);
    d.local_variables["big"] = oa::runtime::Value::make_int(9007199254740993LL);
    d.local_variables["neg"] = oa::runtime::Value::make_int(-1234567890123456789LL);
    d.local_variables["f"] = oa::runtime::Value::make_float(-0.5);
    d.local_variables["pi"] = oa::runtime::Value::make_float(3.141592653589793);
    d.local_variables["flag"] = oa::runtime::Value::make_bool(true);
    d.local_variables["nul"] = oa::runtime::Value::make_null();
    std::string raw_str = "bin\x00\x01\x02";  // NUL + control bytes in a string
    d.local_variables["bin"] = oa::runtime::Value::make_string(raw_str);
    d.current_script = "story.iet";
    d.current_line = 12;
    d.call_stack.emplace_back("system/first.iet", 3);
    d.call_stack.emplace_back("story.iet", 77);
    d.has_scene = true;
    d.root_props["alpha"] = "200";
    oa::runtime::LayerSnap ls;
    ls.id = "bg";
    ls.props["alpha"] = "128";
    oa::runtime::LayerEventHandlerSnap h;
    h.type = "click";
    h.enabled = true;
    h.penetration = false;
    h.handler = "fn_bg";
    h.file = "story.iet";
    h.label = "ttl";
    h.call = false;
    h.params["function"] = "fn_bg";
    ls.handlers.push_back(h);
    d.layers.push_back(ls);
    d.has_audio = true;
    oa::runtime::AudioChannelSnap bgm;
    bgm.id = "bgm";
    bgm.file = "bgm_a.ogg";
    bgm.loop_play = true;
    bgm.gain = 700;
    bgm.pan = -100;
    d.audio.bgm = bgm;
    oa::runtime::AudioChannelSnap se1;
    se1.id = "se1";
    se1.file = "click.ogg";
    se1.gain = 400;
    d.audio.se.push_back(se1);
    const std::string doc = d.encode();
    check(doc.size() >= 8 && oa::util::is_magic(
              reinterpret_cast<const uint8_t*>(doc.data())),
          "T6 encoded document carries the binary magic");
    oa::runtime::SaveData r = oa::runtime::SaveData::decode(doc);
    check(r.version == 1, "T6 header version decoded into SaveData.version");
    check(r.local_variables.at("foo").kind == oa::runtime::ValueKind::String &&
              r.local_variables.at("foo").str_val == "hello",
          "T6 string variable round trip");
    check(r.local_variables.at("n").kind == oa::runtime::ValueKind::Int &&
              r.local_variables.at("n").int_val == 42,
          "T6 int variable round trip");
    check(r.local_variables.at("big").kind == oa::runtime::ValueKind::Int &&
              r.local_variables.at("big").int_val == 9007199254740993LL,
          "T6 int64 beyond 2^53 round trips exactly");
    check(r.local_variables.at("neg").kind == oa::runtime::ValueKind::Int &&
              r.local_variables.at("neg").int_val == -1234567890123456789LL,
          "T6 negative int64 round trips exactly");
    check(r.local_variables.at("f").kind == oa::runtime::ValueKind::Float &&
              r.local_variables.at("f").float_val == -0.5,
          "T6 float variable round trip");
    check(r.local_variables.at("pi").kind == oa::runtime::ValueKind::Float &&
              r.local_variables.at("pi").float_val == 3.141592653589793,
          "T6 float64 bit-exact round trip");
    check(r.local_variables.at("flag").kind == oa::runtime::ValueKind::Bool &&
              r.local_variables.at("flag").bool_val,
          "T6 bool variable round trip");
    check(r.local_variables.at("nul").kind == oa::runtime::ValueKind::Null,
          "T6 null variable round trip");
    check(r.local_variables.at("bin").kind == oa::runtime::ValueKind::String &&
              r.local_variables.at("bin").str_val == raw_str,
          "T6 arbitrary-byte string round trips verbatim");
    check(r.current_script == "story.iet" && r.current_line == 12 &&
              r.call_stack.size() == 2 && r.call_stack[0].first == "system/first.iet" &&
              r.call_stack[0].second == 3 && r.call_stack[1].first == "story.iet" &&
              r.call_stack[1].second == 77,
          "T6 position + stack round trip");
    check(r.has_scene && r.layers.size() == 1 && r.layers[0].id == "bg" &&
              r.layers[0].props.at("alpha") == "128",
          "T6 scene round trip");
    check(r.layers[0].handlers.size() == 1 &&
              r.layers[0].handlers[0].type == "click" &&
              r.layers[0].handlers[0].params.at("function") == "fn_bg",
          "T6 layer event handler round trip");
    check(r.root_props.at("alpha") == "200", "T6 root props round trip");
    check(r.has_audio && r.audio.bgm && r.audio.bgm->file == "bgm_a.ogg" &&
              r.audio.bgm->gain == 700 && r.audio.bgm->pan == -100 &&
              r.audio.bgm->loop_play,
          "T6 bgm channel round trip");
    check(r.has_audio && r.audio.se.size() == 1 && r.audio.se[0].id == "se1" &&
              r.audio.se[0].gain == 400,
          "T6 se channel round trip");

    // Minimal document (no scene/audio) round trips with them absent.
    oa::runtime::SaveData minimal;
    minimal.local_variables["x"] = oa::runtime::Value::make_string("1");
    oa::runtime::SaveData m2 = oa::runtime::SaveData::decode(minimal.encode());
    check(!m2.has_scene && !m2.has_audio && m2.local_variables.at("x").str_val == "1",
          "T6 minimal save without scene/audio round trips");

    bool threw = false;
    {
        std::string newer = "OASB";
        newer.push_back(char(2));  // u32 LE version 2
        newer.push_back(0);
        newer.push_back(0);
        newer.push_back(0);
        newer.push_back(char(0xc0));  // nil payload would be fine; version wins
        try {
            (void)oa::runtime::SaveData::decode(newer);
        } catch (...) {
            threw = true;
        }
    }
    check(threw, "T6 newer header version rejected");
    // Bad magic.
    threw = false;
    try {
        (void)oa::runtime::SaveData::decode(std::string("NOPE") + "\x01\x00\x00\x00" + "\xc0");
    } catch (...) {
        threw = true;
    }
    check(threw, "T6 bad magic rejected");
    // Truncated document (header only).
    threw = false;
    try {
        (void)oa::runtime::SaveData::decode("OASB\x01\x00\x00\x00");
    } catch (...) {
        threw = true;
    }
    check(threw, "T6 truncated document rejected");
    // Unknown top-level fields are skipped (forward tolerance).
    {
        oa::util::Writer w;
        w.map_begin(2);
        w.str("current_script");
        w.str("x.iet");
        w.str("brand_new_field");
        w.map_begin(1);
        w.str("k");
        w.str("v");
        oa::runtime::SaveData u = oa::runtime::SaveData::decode(w.data());
        check(u.current_script == "x.iet" && !u.has_scene && !u.has_audio,
              "T6 unknown top-level field skipped");
    }

    // Domain map format (values now carry the full type set).
    std::map<std::string, oa::runtime::Value> m;
    m["alpha"] = oa::runtime::Value::make_string("1");
    m["saveslot"] = oa::runtime::Value::make_int(3);
    m["big"] = oa::runtime::Value::make_int(9007199254740993LL);
    m["flag"] = oa::runtime::Value::make_bool(true);
    m["f"] = oa::runtime::Value::make_float(0.25);
    const auto back = oa::runtime::decode_domain_map(oa::runtime::encode_domain_map(m));
    check(back.size() == 5 && back.at("alpha").str_val == "1" &&
              back.at("saveslot").int_val == 3 &&
              back.at("big").int_val == 9007199254740993LL &&
              back.at("flag").bool_val && back.at("f").float_val == 0.25,
          "T6 domain map round trip with full value types");
}

// ---------------------------------------------------------------------------
// T7: real filesystem round trip in an isolated tmp dir
// ---------------------------------------------------------------------------
void test_real_dir_store() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() /
                          ("oa_save_test_" + std::to_string(getpid()));
    fs::remove_all(root, ec);
    auto store = std::make_shared<oa::runtime::DirSaveStore>(root.string());
    {
        auto mfs = std::make_shared<MemFs>();
        mfs->files["system.ini"] = project_ini();
        mfs->files["system/first.iet"] = "*main\n[stop]\n";
        oa::runtime::GameRuntime rt(mfs);
        rt.set_save_store(store);
        rt.open_project("windows");
        rt.boot_project();
        rt.interpreter().set_variable("g.tag", oa::runtime::Value::make_string("ok"));
        check(rt.syssave(), "T7 real dir syssave ok");
        check(fs::exists(root / "save" / "saveg.dat", ec), "T7 saveg.dat on disk");
    }
    {
        auto mfs = std::make_shared<MemFs>();
        mfs->files["system.ini"] = project_ini();
        mfs->files["system/first.iet"] = "*main\n[stop]\n";
        oa::runtime::GameRuntime rt(mfs);
        rt.set_save_store(store);
        rt.open_project("windows");
        rt.boot_project();
        const auto v = rt.interpreter().variables().get("g.tag");
        check(v && v->kind == oa::runtime::ValueKind::String && v->str_val == "ok",
              "T7 real dir sysload restored g.tag");
    }
    fs::remove_all(root, ec);
}

} // namespace

int main() {
    test_syssave_sysload();
    test_numbered_save_load();
    test_screenshots_and_files();
    test_formats();
    test_real_dir_store();
    if (failures) {
        std::fprintf(stderr, "save_domain_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("save_domain_test: all ok\n");
    return 0;
}
