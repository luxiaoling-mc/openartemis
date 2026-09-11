// P5a media engine tests: the sound/video LOGIC state machines mirror
// P5a media engine tests: the sound/video LOGIC state machines
// (audio/video state backends, ab_loop naming). No decode, no device:
// these tests verify pure state semantics (channel replacement, fades,
// volumes, skipping gates, finish events/handlers, A-B naming).
#include <cmath>
#include <cstdio>
#include <string>

#include "core/media/audio.h"
#include "core/media/video.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool approx(float a, float b, float eps = 0.02f) {
    const float d = a - b;
    return d > -eps && d < eps;
}

using namespace oa::media;

// ---------------------------------------------------------------- audio ----
void test_bgm_play_stop() {
    AudioEngine a;
    check(!a.is_bgm_playing(), "A1 no BGM initially");
    a.play_bgm("bgm01.ogg", BgmConfig{}); // loop default on
    check(a.is_bgm_playing(), "A1 play_bgm starts the channel");
    check(a.state().bgm_channel && a.state().bgm_channel->file == "bgm01.ogg",
          "A1 channel keeps the file");
    check(a.stop_bgm(0), "A1 stop_bgm returns true when a BGM ran");
    check(!a.is_bgm_playing(), "A1 stop clears the channel");
}

void test_bgm_fade_stop_finish_event() {
    AudioEngine a;
    a.play_bgm("bgm01.ogg", BgmConfig{});
    a.stop_bgm(500);
    check(a.is_bgm_playing(), "A2 still playing during the fade-out");
    a.update(600);
    check(!a.is_bgm_playing(), "A2 channel removed after the fade");
    const auto evs = a.poll_finish_events();
    check(evs.size() == 1 && evs[0].category == SoundCategory::Bgm,
          "A2 fade-out stop emits one BGM finish event");
}

void test_se_play_stop() {
    AudioEngine a;
    a.play_se("se01", "click.wav", SeConfig{});
    check(a.is_se_playing("se01"), "A3 seplay starts the id channel");
    check(a.stop_se("se01", 0), "A3 sestop returns true");
    check(!a.is_se_playing("se01"), "A3 sestop clears it");
    // stop of a missing id returns false
    check(!a.stop_se("nope", 0), "A3 stopping an absent id returns false");
}

void test_se_controls_target_voice_ids() {
    // : sefade/sepan/sestop apply to the shared se+voice id space.
    AudioEngine a;
    a.play_voice("voice01", "line.ogg", SeConfig{});
    a.fade_se_gain("voice01", 500, 100);
    a.pan_se("voice01", -1000, 100);
    const auto* v = a.state().voice_channels.count("voice01")
                        ? &a.state().voice_channels.at("voice01")
                        : nullptr;
    check(v != nullptr && v->raw_gain == 500, "A4 sefade retargets a voice id");
    check(v && v->raw_pan == -1000, "A4 sepan retargets a voice id");
    check(a.stop_se("voice01", 0), "A4 sestop stops a voice id");
    check(!a.state().voice_channels.count("voice01"), "A4 sestop removes the voice");
}

void test_duplicate_se_replaces() {
    AudioEngine a;
    a.play_se("se01", "click.wav", SeConfig{});
    a.play_se("se01", "boom.wav", SeConfig{});
    check(a.state().se_channels.at("se01").file == "boom.wav",
          "A5 same-id seplay replaces the old file");
}

void test_skippable_se_gated_by_skip() {
    AudioEngine a;
    a.set_skipping(true);
    SeConfig c;
    c.skippable = true;
    a.play_se("se01", "click.wav", c);
    check(!a.is_se_playing("se01"), "A6 skippable SE does not play while skipping");
    // non-skippable SE still plays
    a.play_se("se02", "click.wav", SeConfig{});
    check(a.is_se_playing("se02"), "A6 plain SE plays while skipping");
}

void test_bgm_gain_fade_state() {
    AudioEngine a;
    a.play_bgm("bgm.ogg", BgmConfig{});
    a.fade_bgm_gain(500, 1000);
    const auto& ch = *a.state().bgm_channel;
    check(ch.raw_gain == 500, "A7 fade records the raw target gain");
    check(ch.fade.has_value() && ch.fade->duration_ms == 1000, "A7 fade timeline set");
}

void test_advance_noop() {
    AudioEngine a;
    a.update(1000);
    check(a.state().clock_ms == 1000, "A8 advance moves the audio clock");
    check(a.poll_finish_events().empty(), "A8 no events without channels");
}

void test_crossfade() {
    AudioEngine a;
    a.play_bgm("old.ogg", BgmConfig{});
    BgmConfig cfg;
    cfg.fade_in_ms = 500;
    a.crossfade_bgm("new.ogg", cfg);
    check(a.state().bgm_channel && a.state().bgm_channel->file == "new.ogg",
          "A9 crossfade starts the new BGM");
    check(a.state().bgm_channel->fade.has_value(), "A9 new BGM fades in from 0");
    check(a.state().bgm_channel->current_gain == 0.0f, "A9 new BGM starts silent");
}

void test_volume_clamps() {
    AudioEngine a;
    a.set_master_volume(1.5f);
    check(a.state().master_volume == 1.0f, "A10 master volume clamps at 1");
    a.set_master_volume(-0.5f);
    check(a.state().master_volume == 0.0f, "A10 master volume clamps at 0");
}

void test_finish_handler_registry() {
    AudioEngine a;
    SoundFinishHandler h;
    h.file = "script.iet";
    h.label = "on_bgm_end";
    h.call = false;
    a.set_sound_finish_handler("", h); // id-less = BGM slot
    check(a.state().bgm_finish_handler.has_value(), "A11 id-less handler = BGM slot");
    a.remove_sound_finish_handler("");
    check(!a.state().bgm_finish_handler.has_value(), "A11 removed again");
    // per-id slot does not pollute the BGM slot
    a.set_sound_finish_handler("s1", h);
    check(!a.state().bgm_finish_handler.has_value(),
          "A11 per-id handler does not touch the BGM slot");
    check(a.state().se_finish_handlers.count("s1") == 1, "A11 per-id handler stored");
}

void test_stop_all_clears() {
    AudioEngine a;
    a.play_bgm("bgm.ogg", BgmConfig{});
    a.play_se("se01", "click.wav", SeConfig{});
    a.play_voice("v01", "line01.ogg", SeConfig{});
    a.stop_all_sounds();
    check(!a.is_bgm_playing(), "A12 stop-all clears the BGM");
    check(!a.is_se_playing("se01"), "A12 stop-all clears the SE");
    check(a.state().voice_channels.empty(), "A12 stop-all clears voices");
}

void test_gain_fade_linear() {
    AudioEngine a;
    BgmConfig cfg;
    cfg.gain = 0;
    a.play_bgm("bgm.ogg", cfg);
    a.fade_bgm_gain(1000, 1000); // 0 -> 1000 over 1 s
    a.update(500);
    check(approx(a.state().bgm_channel->current_gain, 0.5f),
          "A13 gain halfway through the fade is 0.5");
    a.update(600);
    check(approx(a.state().bgm_channel->current_gain, 1.0f),
          "A13 gain completes at 1.0");
    check(!a.state().bgm_channel->fade.has_value(), "A13 fade cleared when done");
}

void test_instant_pan_keeps_fade_in() {
    // research/115 (snll recap-montage BGM inaudible): the game's media/bgm.lua
    // bgm_play() emits [splay gain=1000 time=2000] IMMEDIATELY followed by
    // [span pan=0 time=0] (its epan() helper always fires while scr.bgm.pan is
    // still nil, i.e. after any bgm_stop). The instant pan must not cancel the
    // fade-in: doing so left current_gain pinned at the fade-in's 0.0 start
    // while raw_gain stayed 1000 — a fully decoded but inaudible BGM channel.
    AudioEngine a;
    BgmConfig cfg;
    cfg.gain = 1000;
    cfg.fade_in_ms = 2000;
    a.play_bgm(":bgm/bgm34_a.ogg", cfg);
    check(approx(a.state().bgm_channel->current_gain, 0.0f), "A16 fade-in starts at 0");
    a.pan_bgm(0, 0); // the game's epan() -> [span pan=0 time=0]
    check(a.state().bgm_channel->fade.has_value(),
          "A16 instant pan keeps the fade-in alive");
    a.update(1000);
    check(approx(a.state().bgm_channel->current_gain, 0.5f),
          "A16 gain halfway through the preserved fade-in is 0.5");
    a.update(1100);
    check(approx(a.state().bgm_channel->current_gain, 1.0f),
          "A16 preserved fade-in completes at full gain");
    check(a.state().bgm_channel->raw_gain == 1000, "A16 raw gain stays as requested");
    check(!a.state().bgm_channel->fade.has_value(), "A16 fade cleared when done");
}

void test_instant_gain_keeps_pan_fade() {
    // The symmetric direction: an instant gain change must not cancel a
    // running pan fade (only the pan axis keeps moving).
    AudioEngine a;
    a.play_bgm("bgm.ogg", BgmConfig{});
    a.pan_bgm(1000, 1000); // pan 0 -> 1.0 over 1 s
    a.update(500);
    check(approx(a.state().bgm_channel->current_pan, 0.5f), "A17 pan halfway is 0.5");
    a.fade_bgm_gain(400, 0); // instant gain change
    check(a.state().bgm_channel->fade.has_value(), "A17 instant gain keeps the pan fade");
    check(approx(a.state().bgm_channel->current_gain, 0.4f), "A17 gain applied instantly");
    a.update(600);
    check(approx(a.state().bgm_channel->current_pan, 1.0f), "A17 pan fade still completes");
    check(approx(a.state().bgm_channel->current_gain, 0.4f), "A17 gain stays at the target");
}

void test_instant_fades_still_cancel_stop() {
    // Historical behavior kept: an instant gain change during a stop fade
    // (stop_on_complete) still cancels that stop.
    AudioEngine a;
    a.play_bgm("bgm.ogg", BgmConfig{});
    a.stop_bgm(1000);
    check(a.state().bgm_channel->fade.has_value() &&
              a.state().bgm_channel->fade->stop_on_complete,
          "A18 stop fade is a stop fade");
    a.fade_bgm_gain(1000, 0);
    check(!a.state().bgm_channel->fade.has_value(), "A18 instant gain cancels the stop fade");
    a.update(2000);
    check(a.is_bgm_playing(), "A18 the channel survives (stop was cancelled)");
}

void test_gain_pan_scales() {
    check(approx(SoundChannel::gain_to_linear(1000), 1.0f), "A14 gain 1000 -> 1.0");
    check(approx(SoundChannel::gain_to_linear(500), 0.5f), "A14 gain 500 -> 0.5");
    check(approx(SoundChannel::gain_to_linear(-200), 0.0f), "A14 negative gain -> 0");
    check(approx(SoundChannel::pan_to_linear(-1000), -1.0f), "A14 pan -1000 -> -1");
    check(approx(SoundChannel::pan_to_linear(1000), 1.0f), "A14 pan 1000 -> 1");
    check(approx(SoundChannel::pan_to_linear(2500), 1.0f), "A14 pan clamps at 1");
}

void test_ab_loop_file() {
    //  runtime/ ab_loop_naming_convention test.
    check(ab_loop_file("foo_a.ogg") == std::optional<std::string>("foo_b.ogg"),
          "A15 foo_a.ogg -> foo_b.ogg");
    check(ab_loop_file("bgm/theme_a.ogg") == std::optional<std::string>("bgm/theme_b.ogg"),
          "A15 dirs are preserved");
    check(ab_loop_file(":bgm/foo_a") == std::optional<std::string>(":bgm/foo_b"),
          "A15 magic/no-ext names map too");
    check(!ab_loop_file("foo.ogg").has_value(), "A15 plain file -> none");
    check(!ab_loop_file("foo_b.ogg").has_value(), "A15 _b file -> none");
    check(ab_loop_file("dir.v2/foo_a.ogg") ==
              std::optional<std::string>("dir.v2/foo_b.ogg"),
          "A15 dots in directories are not extensions");
}

void test_se_finish_event_rides_handler() {
    AudioEngine a;
    SoundFinishHandler h;
    h.handler = "calllua";
    a.set_sound_finish_handler("s1", h);
    a.play_se("s1", "click.wav", SeConfig{});
    a.stop_se("s1", 300); // fade-out stop
    check(a.is_se_playing("s1"), "A16 still playing mid fade");
    a.update(400);
    check(!a.is_se_playing("s1"), "A16 channel ended after fade");
    const auto evs = a.poll_finish_events();
    check(evs.size() == 1 && evs[0].id == "s1" && evs[0].handler.has_value(),
          "A16 finish event carries the per-id handler");
}

// ---------------------------------------------------------------- video ----
void test_video_fullscreen_instant() {
    VideoEngine v;
    VideoConfig cfg;
    cfg.file = "test.mpg";
    cfg.skippable = true;
    cfg.loop_play = false;
    v.play_overlay(cfg);
    check(v.is_overlay_playing(), "V1 overlay video playing");
    const auto evs = v.poll_finish_events();
    check(evs.size() == 1 && evs[0].id.empty(),
          "V1 logical non-loop fullscreen completes immediately (id-less)");
    v.update(16);
    check(!v.is_overlay_playing(), "V1 advance clears the playing flag");
}

void test_video_loop_no_finish() {
    VideoEngine v;
    VideoConfig cfg;
    cfg.file = "test.mpg";
    cfg.loop_play = true;
    v.play_overlay(cfg);
    check(v.poll_finish_events().empty(), "V2 looped video emits no finish event");
    v.update(16);
    check(v.is_overlay_playing(), "V2 looped video stays playing");
}

void test_video_layer_instant() {
    VideoEngine v;
    VideoConfig cfg;
    cfg.file = "test.ogv";
    v.play_layer("1", cfg);
    check(v.is_layer_playing("1"), "V3 layer video playing");
    const auto evs = v.poll_finish_events();
    check(evs.size() == 1 && evs[0].id == "1", "V3 layer video completes instantly");
    v.update(16);
    check(!v.is_layer_playing("1"), "V3 advance clears the layer");
}

void test_video_finish_handlers() {
    VideoEngine v;
    VideoFinishHandler h;
    h.file = "script.asb";
    h.label = "@finish";
    v.set_finish_handler("", h);
    check(v.state().finish_handler.has_value(), "V4 global handler stored");
    v.remove_finish_handler("");
    check(!v.state().finish_handler.has_value(), "V4 global handler removed");
    // per-layer registration does not pollute the global slot
    v.set_finish_handler("mw.movie", h);
    check(!v.state().finish_handler.has_value(), "V4 layer handler is separate");
    check(v.state().layer_finish_handlers.count("mw.movie") == 1, "V4 layer handler stored");
    v.remove_finish_handler("mw.movie");
    check(v.state().layer_finish_handlers.empty(), "V4 layer handler removed");
}

void test_video_layer_uses_per_id_handler() {
    VideoEngine v;
    VideoFinishHandler h;
    h.file = "mv.asb";
    v.set_finish_handler("1", h);
    VideoConfig cfg;
    cfg.file = "test.ogv";
    v.play_layer("1", cfg);
    const auto evs = v.poll_finish_events();
    check(evs.size() == 1 && evs[0].handler.has_value() &&
              evs[0].handler->file == "mv.asb",
          "V5 layer finish carries the per-layer handler");
}

void test_video_layer_falls_back_to_global() {
    VideoEngine v;
    VideoFinishHandler h;
    h.file = "global.asb";
    v.set_finish_handler("", h);
    VideoConfig cfg;
    cfg.file = "test.ogv";
    v.play_layer("2", cfg);
    const auto evs = v.poll_finish_events();
    check(evs.size() == 1 && evs[0].handler.has_value() &&
              evs[0].handler->file == "global.asb",
          "V6 layer finish falls back to the global handler");
}

void test_video_layer_subtree_stop() {
    // research/101: a deleted scene subtree releases the layer-video
    // channels inside it (cgdel / ui-group teardown); siblings and the
    // overlay channel stay untouched.
    VideoEngine v;
    VideoConfig cfg;
    cfg.file = "snow.ogv";
    cfg.loop_play = true; // no loader: loop channels just stay listed
    const std::string story = "1.0.bx.by.bs.610.bg.3.t.y.x.p.m.a.a.b";
    v.play_layer(story, cfg);
    v.play_layer("500.z.mv", cfg);
    v.play_layer("ui.clip", cfg);
    check(v.state().video_layers.size() == 3, "V7 three layer channels");
    // deleting the story bg group releases only the channel under it
    v.stop_layer_subtree("1.0.bx.by.bs.610.bg.3");
    check(!v.is_layer_playing(story), "V7 subtree channel stopped");
    check(v.is_layer_playing("500.z.mv") && v.is_layer_playing("ui.clip"),
          "V7 sibling channels untouched");
    check(v.state().video_layers.size() == 2, "V7 two channels remain");
    // exact-id delete
    v.stop_layer_subtree("500.z.mv");
    check(!v.is_layer_playing("500.z.mv"), "V7 exact-id channel stopped");
    // overlay is not a scene layer: survives every subtree stop
    v.play_overlay(cfg);
    v.stop_layer_subtree("ui");
    check(!v.is_layer_playing("ui.clip"), "V7 ui subtree channel stopped");
    check(v.is_overlay_playing(), "V7 overlay survives subtree stops");
    // deleting an ancestor group id (not the leaf) also releases it
    v.play_layer(story, cfg);
    v.stop_layer_subtree("1.0.bx.by.bs.610");
    check(!v.is_layer_playing(story), "V7 ancestor-group delete releases");
    // missing id: no-op
    const size_t before = v.state().video_layers.size();
    v.stop_layer_subtree("nope.nothing");
    check(v.state().video_layers.size() == before, "V7 missing prefix no-op");
}

} // namespace

int main() {
    test_bgm_play_stop();
    test_bgm_fade_stop_finish_event();
    test_se_play_stop();
    test_se_controls_target_voice_ids();
    test_duplicate_se_replaces();
    test_skippable_se_gated_by_skip();
    test_bgm_gain_fade_state();
    test_advance_noop();
    test_crossfade();
    test_volume_clamps();
    test_finish_handler_registry();
    test_stop_all_clears();
    test_gain_fade_linear();
    test_instant_pan_keeps_fade_in();
    test_instant_gain_keeps_pan_fade();
    test_instant_fades_still_cancel_stop();
    test_gain_pan_scales();
    test_ab_loop_file();
    test_se_finish_event_rides_handler();
    test_video_fullscreen_instant();
    test_video_loop_no_finish();
    test_video_layer_instant();
    test_video_finish_handlers();
    test_video_layer_uses_per_id_handler();
    test_video_layer_falls_back_to_global();
    test_video_layer_subtree_stop();
    if (failures) {
        std::fprintf(stderr, "media_state_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("media_state_test: all ok\n");
    return 0;
}
