// P5a runtime-level media tests: interpreter tags -> engine ops (parameter
// fidelity incl. string-preserved ids), the Se/Video wait rows of the wait
// matrix, real Ogg Vorbis EOF -> finish-handler -> wait release, host
// notifications and the media volume var bridge. Synthetic ogg assets are
// generated in-process with libvorbisenc (tests/ogg_fixture.h).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/fs/fs.h"
#include "core/render/content_role.h" // layer-model S5: 内容来源/读取域键断言
#include "core/render/layer_kind.h"
#include "core/runtime/runtime.h"
#include "ogg_fixture.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool approx(double a, double b, double eps = 1e-3) {
    const double d = a - b;
    return d > -eps && d < eps;
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

oa::runtime::Event tag_event(const char* tag,
                            std::initializer_list<std::pair<const char*, const char*>> kv) {
    oa::runtime::Event e;
    e.kind = oa::runtime::Event::Kind::ConfigEvent;
    e.tag = tag;
    for (const auto& [k, v] : kv) e.params[k] = v;
    const auto id = e.params.find("id");
    e.id = id == e.params.end() ? std::string() : id->second;
    return e;
}

struct Boot {
    std::shared_ptr<MemFs> fs;
    std::unique_ptr<oa::runtime::GameRuntime> rt;
    explicit Boot(const char* script) {
        fs = std::make_shared<MemFs>();
        fs->files["system.ini"] =
            "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
            "BOOT = system/first.iet\nSAVEPATH = save\n";
        fs->files["system/first.iet"] = std::string("*main\n") + script;
        rt = std::make_unique<oa::runtime::GameRuntime>(fs);
        rt->open_project("windows");
        rt->boot_project();
    }
};

// ---------------------------------------------------------------------------
// R1: tag -> engine parameter fidelity (no interpreter, direct dispatch)
// ---------------------------------------------------------------------------
void test_tag_parsing() {
    MemFs fs;
    // No open_project: tag dispatch only touches the audio/video engines,
    // which need neither the interpreter nor the project filesystem.
    oa::runtime::GameRuntime rt(std::make_shared<MemFs>(fs));
    auto& audio = rt.audio();

    // [splay file=... loop=0 gain=800 pan=-100 time=120]
    check(rt.apply_media_event(tag_event("splay", {{"file", "t.ogg"}, {"loop", "0"},
                                                   {"gain", "800"}, {"pan", "-100"},
                                                   {"time", "120"}})),
          "R1 splay consumed");
    check(audio.state().bgm_channel.has_value(), "R1 splay starts the BGM channel");
    if (audio.state().bgm_channel) {
        const auto& c = *audio.state().bgm_channel;
        check(!c.loop_play, "R1 splay loop=0 honored");
        check(c.raw_gain == 800, "R1 splay gain=800 kept raw");
        check(c.raw_pan == -100, "R1 splay pan=-100 kept raw");
        check(c.fade.has_value() && c.fade->duration_ms == 120,
              "R1 splay time=120 becomes the fade-in");
    }
    // splay without loop: defaults to loop=1 ( BgmConfig).
    rt.apply_media_event(tag_event("splay", {{"file", "l.ogg"}}));
    check(audio.state().bgm_channel && audio.state().bgm_channel->loop_play,
          "R1 splay default loop = on");

    // [seplay id="1.80" ...]: ids keep their string form (no tail-zero loss).
    rt.apply_media_event(tag_event("seplay", {{"id", "1.80"}, {"file", "f.ogg"},
                                              {"loop", "1"}, {"gain", "500"},
                                              {"pan", "-1000"}, {"time", "100"},
                                              {"skippable", "1"}}));
    const auto& se = audio.state().se_channels.at("1.80");
    check(se.id == "1.80", "R1 seplay keeps the string id '1.80'");
    check(se.loop_play && se.skippable, "R1 seplay loop/skippable parsed");
    check(se.raw_gain == 500 && se.raw_pan == -1000, "R1 seplay gain/pan raw");
    // time=100 starts a fade-in, so the linear gain begins at 0 (target 0.5)
    // while the pan is instant (-1.0).
    check(se.current_gain == 0.0f && se.fade.has_value() &&
              approx(se.fade->target_gain, 0.5f),
          "R1 seplay fade-in starts silent toward gain 0.5");
    check(approx(se.current_pan, -1.0f), "R1 seplay pan applies immediately");
    // seplay without loop/skippable: both off (SeConfig defaults).
    rt.apply_media_event(tag_event("seplay", {{"id", "1.80"}, {"file", "g.ogg"}}));
    check(!audio.state().se_channels.at("1.80").loop_play, "R1 seplay default loop = off");

    // [voice file=...] without id: auto voice:serial ids.
    rt.apply_media_event(tag_event("voice", {{"file", "v.ogg"}}));
    rt.apply_media_event(tag_event("voice", {{"file", "v.ogg"}}));
    check(audio.state().voice_channels.count("voice:1") == 1 &&
              audio.state().voice_channels.count("voice:2") == 1,
          "R1 voice auto ids voice:1, voice:2");

    // [sestop time=250] fades the channel.
    rt.apply_media_event(tag_event("sestop", {{"id", "1.80"}, {"time", "250"}}));
    check(audio.state().se_channels.count("1.80") == 1,
          "R1 sestop with fade keeps the channel (fading)");
    check(audio.state().se_channels.at("1.80").fade.has_value(), "R1 fade-out armed");

    // [allsoundstop]
    rt.apply_media_event(tag_event("allsoundstop", {}));
    check(!audio.is_bgm_playing() && audio.state().se_channels.empty() &&
              audio.state().voice_channels.empty(),
          "R1 allsoundstop clears bgm/se/voice");

    // volume setters clamp at the engine level ( AudioStateBackend).
    audio.set_bgm_volume(2.0f);
    check(audio.state().bgm_volume == 1.0f, "R1 engine volumes clamp");
}

// ---------------------------------------------------------------------------
// R2: [wait se=] releases on the host completion notification
// ---------------------------------------------------------------------------
void test_se_wait_notify() {
    // looped sound: no natural EOF within the test; the end-of-play report
    // (host/sink stop) ends it.
    Boot b("[seplay id=\"s1\" file=\"s1.ogg\" loop=\"1\"]\n"
           "[wait se=\"s1\"]\n"
           "[stop]\n");
    b.rt->set_media_loader([&](const std::string& name)
                               -> std::optional<std::vector<uint8_t>> {
        if (name == "s1.ogg") return oafix::tone_ogg(0.5, 1, 44100, 440.0, 0.4f);
        return std::nullopt;
    });
    oa::runtime::FrameInput idle;
    bool parked = false;
    size_t frame = 0;
    for (; frame < 300 && !b.rt->exit_requested(); ++frame) {
        b.rt->tick(16, idle);
        const auto* w = b.rt->current_wait();
        if (w && w->kind == oa::runtime::WaitReason::Kind::Se && w->id == "s1") {
            parked = true;
            break;
        }
    }
    std::printf("[R2] parked=%s frame=%zu wait=%s\n", parked ? "yes" : "no", frame,
                wait_str(b.rt->current_wait()).c_str());
    check(parked, "R2 [wait se] parked on the looped se");
    check(b.rt->audio().is_sound_playing("s1"), "R2 the looped se is playing while waiting");
    b.rt->sound_finished(oa::media::SoundCategory::Se, "s1");
    check(!b.rt->audio().is_sound_playing("s1"),
          "R2 end-of-play report stops the tracked channel");
    bool reached = false;
    for (; frame < 400 && !b.rt->exit_requested(); ++frame) {
        b.rt->tick(16, idle);
        const auto* w = b.rt->current_wait();
        if (w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty()) {
            reached = true;
            break;
        }
    }
    check(reached, "R2 flow continued to [stop] after the end-of-play report");
    std::printf("[R2] final wait=%s frame=%zu\n", wait_str(b.rt->current_wait()).c_str(),
                frame);
}

// ---------------------------------------------------------------------------
// R3: [wait se= time=N] counts from the SE play start (no EOF required)
// ---------------------------------------------------------------------------
void test_se_wait_time() {
    Boot b("[seplay id=\"s2\" file=\"long.ogg\"]\n"
           "[wait se=\"s2\" time=\"300\"]\n"
           "[stop]\n");
    b.rt->set_media_loader([&](const std::string& name)
                               -> std::optional<std::vector<uint8_t>> {
        if (name == "long.ogg") return oafix::tone_ogg(10.0, 2, 44100, 220.0, 0.3f);
        return std::nullopt;
    });
    oa::runtime::FrameInput idle;
    bool parked = false;
    size_t frame = 0;
    for (; frame < 300 && !b.rt->exit_requested(); ++frame) {
        b.rt->tick(16, idle);
        const auto* w = b.rt->current_wait();
        if (w && w->kind == oa::runtime::WaitReason::Kind::Se && w->id == "s2") {
            parked = true;
            break;
        }
    }
    check(parked, "R3 parked on the se wait");
    bool reached = false;
    for (; frame < 600 && !b.rt->exit_requested(); ++frame) {
        b.rt->tick(16, idle);
        const auto* w = b.rt->current_wait();
        if (w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty()) {
            reached = true;
            break;
        }
    }
    std::printf("[R3] release after ~300ms: frame=%zu wait=%s playing=%s\n", frame,
                wait_str(b.rt->current_wait()).c_str(),
                b.rt->audio().is_sound_playing("s2") ? "yes" : "no");
    check(reached, "R3 [wait se time=N] released on the elapsed time");
    check(frame >= 15 && frame <= 200, "R3 released in the time window (not instantly)");
}

// ---------------------------------------------------------------------------
// R4: natural EOF of a non-loop se releases the wait and dispatches handlers
// ---------------------------------------------------------------------------
void test_se_wait_eof() {
    Boot b("[seplay id=\"s4\" file=\"short.ogg\"]\n"
           "[setonsoundfinish id=\"s4\" handler=\"calllua\" function=\"voice_end\"]\n"
           "[wait se=\"s4\"]\n"
           "[stop]\n");
    b.rt->set_media_loader([&](const std::string& name)
                               -> std::optional<std::vector<uint8_t>> {
        if (name == "short.ogg") return oafix::tone_ogg(0.2, 1, 44100, 880.0, 0.3f);
        return std::nullopt;
    });
    oa::runtime::FrameInput idle;
    size_t frame = 0;
    bool reached = false;
    size_t dispatched_at = 0;
    for (; frame < 400 && !b.rt->exit_requested(); ++frame) {
        b.rt->tick(16, idle);
        if (b.rt->media_handler_dispatches() == 1 && dispatched_at == 0) {
            dispatched_at = frame;
        }
        const auto* w = b.rt->current_wait();
        if (w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty()) {
            reached = true;
            break;
        }
    }
    std::printf("[R4] eof release: frame=%zu handler_dispatch@%zu wait=%s\n", frame,
                dispatched_at, wait_str(b.rt->current_wait()).c_str());
    check(reached, "R4 non-loop EOF released the [wait se]");
    check(dispatched_at != 0 && dispatched_at < frame,
          "R4 natural EOF dispatched the registered finish handler");
}

// ---------------------------------------------------------------------------
// R5: fullscreen [video] parks Stop{video} and completes logically (M9 decodes)
// ---------------------------------------------------------------------------
void test_video_fullscreen_wait() {
    Boot b("[video file=\"logo.ogv\"]\n[stop]\n");
    oa::runtime::FrameInput idle;
    bool video_park = false;
    size_t frame = 0;
    std::string seen;
    for (; frame < 400 && !b.rt->exit_requested(); ++frame) {
        b.rt->tick(16, idle);
        const auto* w = b.rt->current_wait();
        if (seen != wait_str(w)) {
            seen = wait_str(w);
            std::printf("[R5] f=%zu wait=%s\n", frame, seen.c_str());
        }
        if (w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id == "video") {
            video_park = true;
        }
        if (video_park && w && w->kind == oa::runtime::WaitReason::Kind::Stop &&
            w->id.empty()) {
            break; // reached the trailing [stop]
        }
    }
    check(video_park, "R5 fullscreen [video] parked the script (Stop{video})");
    check(frame < 400 && !b.rt->exit_requested(),
          "R5 the video stop released without input (logical completion)");
    std::printf("[R5] final wait=%s frame=%zu\n", wait_str(b.rt->current_wait()).c_str(),
                frame);
}

// ---------------------------------------------------------------------------
// R6: [wait video=layer] releases when the layer video stopped
// ---------------------------------------------------------------------------
void test_video_layer_wait() {
    Boot b("[video id=\"mw\" file=\"mv.ogv\"]\n"
           "[wait video=\"mw\"]\n"
           "[stop]\n");
    oa::runtime::FrameInput idle;
    size_t frame = 0;
    bool reached = false;
    std::string seen;
    for (; frame < 400 && !b.rt->exit_requested(); ++frame) {
        b.rt->tick(16, idle);
        const auto* w = b.rt->current_wait();
        if (seen != wait_str(w)) {
            seen = wait_str(w);
            std::printf("[R6] f=%zu wait=%s\n", frame, seen.c_str());
        }
        if (w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id.empty()) {
            reached = true;
            break;
        }
    }
    check(reached, "R6 [wait video=layer] released after the layer video stopped");
    std::printf("[R6] final wait=%s frame=%zu\n", wait_str(b.rt->current_wait()).c_str(),
                frame);
}

// ---------------------------------------------------------------------------
// S5 (research/108): [video id=layer] 的绑定语义面 —— 内容来源状态 + 角色读取
// 域键(旧的 `__video_layer__:<id>` 保留命名空间已删除)。集成面:宿主上传键
// (VideoContent::frame_key)与场景读取键(角色 texture_key)同源。
// ---------------------------------------------------------------------------
void test_video_layer_content_binding() {
    // loop=1 + 不可解码文件:通道不产生 finish 事件 ⇒ 绑定态可稳定观察。
    Boot b("[lyc id=\"mw\" file=\"still.png\" path=\":fg/\"]\n"
           "[video id=\"mw\" file=\"mv.ogv\" loop=\"1\"]\n"
           "[stop]\n");
    oa::runtime::FrameInput idle;
    b.rt->tick(16, idle); // 事件派发帧:[lyc] 物化 + [video] 绑定
    const oa::render::Layer* mv = b.rt->scene().find("mw");
    check(mv != nullptr, "S5 video bind: carrier exists");
    if (mv) {
        check(mv->content == oa::render::LayerContent::VideoFrame,
              "S5 video bind: layer content = VideoFrame (explicit state)");
        check(mv->file.empty() && mv->path.empty(),
              "S5 video bind: no resource name/path left in the layer");
        check(!mv->has_color && mv->mask.empty(),
              "S5 video bind: solid/mask state dropped");
        check(oa::render::kind_of(*mv) == oa::render::LayerKind::Video,
              "S5 video bind: kind_of reads the content state");
        const oa::render::ContentRole& role = oa::render::content_role_of(*mv);
        check(role.host_pumped() && !role.decodable_asset(),
              "S5 video bind: role reads the host-uploaded frame domain");
        check(role.texture_key(*mv) == oa::render::VideoContent::frame_key("mw"),
              "S5 video bind: scene read key == host upload key");
        check(role.textured_content(*mv) && role.content_present(*mv),
              "S5 video bind: content present while bound");
    }
    // 视频收尾(EOF 等价路径)→ 解绑:层回到空态,不回滚旧静态图(旧拼写绑定
    // 语义逐位保持)。
    b.rt->video().queue_finish("mw");
    b.rt->tick(16, idle);
    const oa::render::Layer* after = b.rt->scene().find("mw");
    check(after != nullptr && after->content == oa::render::LayerContent::Unbound,
          "S5 video finish: content unbound");
    check(after != nullptr && after->file.empty(),
          "S5 video finish: carrier holds no resource name");
    check(after != nullptr &&
              oa::render::kind_of(*after) == oa::render::LayerKind::None,
          "S5 video finish: carrier back to the empty role");
}

// ---------------------------------------------------------------------------
// R7: s.bgmvol / s.sevol reach the audio engine ( volume bridge)
// ---------------------------------------------------------------------------
void test_system_volume_bridge() {
    Boot b("[var name=\"s.bgmvol\" data=\"500\"]\n"
           "[wait time=\"0\" input=\"0\"]\n");
    oa::runtime::FrameInput idle;
    for (int i = 0; i < 3 && !b.rt->exit_requested(); ++i) b.rt->tick(16, idle);
    check(approx(b.rt->audio().state().bgm_volume, 0.5f),
          "R7 s.bgmvol=500 reaches the bgm volume bus");
    // default se volume untouched
    check(b.rt->audio().state().se_volume == 1.0f, "R7 se volume untouched by default");
}

// ---------------------------------------------------------------------------
// R8: BGM A-B loop switch — the intro (foo_a) plays once, then the player
// opens the loop segment (foo_b) and keeps looping it (splay.md convention).
// ---------------------------------------------------------------------------
void test_ab_loop_switch() {
    Boot b("[splay file=\"bgm_a.ogg\" loop=\"1\"]\n"
           "[stop]\n");
    std::vector<std::string> requested;
    b.rt->set_media_loader([&](const std::string& name)
                               -> std::optional<std::vector<uint8_t>> {
        requested.push_back(name);
        if (name == "bgm_a.ogg") return oafix::tone_ogg(0.08, 1, 44100, 440.0, 0.3f);
        if (name == "bgm_b.ogg") return oafix::tone_ogg(1.0, 1, 44100, 660.0, 0.3f);
        return std::nullopt;
    });
    oa::runtime::FrameInput idle;
    bool saw_b = false;
    size_t frame = 0;
    bool channel_alive = false;
    for (; frame < 300 && !b.rt->exit_requested(); ++frame) {
        b.rt->tick(16, idle);
        if (b.rt->audio().is_bgm_playing()) channel_alive = true;
        for (const std::string& n : requested) {
            if (n == "bgm_b.ogg") saw_b = true;
        }
        if (saw_b && frame >= 30) break;
    }
    std::printf("[R8] frame=%zu saw_b=%s bgm_alive=%s requested=%zu\n", frame,
                saw_b ? "yes" : "no", channel_alive ? "yes" : "no", requested.size());
    check(saw_b, "R8 the BGM player switched to the A-B loop segment (bgm_b)");
    check(channel_alive, "R8 looped BGM stayed alive across the switch");
    check(b.rt->audio().state().bgm_channel &&
              !b.rt->audio().state().bgm_channel->fade.has_value(),
          "R8 no spurious fade on the looped BGM");
}

} // namespace

int main() {
    test_tag_parsing();
    test_se_wait_notify();
    test_se_wait_time();
    test_se_wait_eof();
    test_video_fullscreen_wait();
    test_video_layer_wait();
    test_video_layer_content_binding();
    test_system_volume_bridge();
    test_ab_loop_switch();
    if (failures) {
        std::fprintf(stderr, "media_runtime_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("media_runtime_test: all ok\n");
    return 0;
}
