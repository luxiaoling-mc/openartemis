// P1 chain test (research/42): the REAL NekoMiko e-table surface, driven
// from the game Lua VM, must produce a bound static emote layer.
//
// Boots the real NekoMiko root.pfs (qureate Artemis game) headless, then —
// from the *engine-side Lua state* — calls the exact entry the game's own
// emote.lua wrapper uses:
//     e:createEmoteLayer{ id=..., files={'image\fhd\fg\aya\tay_0.psb'}, ... }
// The e-table implementation enqueues lyc2 + the native "emotestatic" tag;
// the runtime decodes the PSB, renders the static pose at the requested
// canvas size, stores the RGBA frame and binds the carrier layer to that
// emote canvas (layer-model S5: LayerContent::EmoteCanvas;读取域键 =
// EmoteContent::canvas_key(id),旧的 __emote_layer__ 保留命名空间已删除).
// Gates:
//   E-1 the runtime processed an emotestatic event (emote_layer_events > 0)
//   E-2 a scene layer with the dotted emote id exists and is bound to the
//       emote canvas (content == LayerContent::EmoteCanvas)
//   E-3 the stored frame has plausible content: opaque pixels in a central
//       band, coloured (non-black) pixels present, vertical span within the
//       canvas (a failed decode / exploding layout would fail these)
// Also writes /tmp/nekomiko_emote_static.png (headless PNG evidence of the
// P1 static 立绘).
// Env: OA_TEST_NEKOMIKO_PFS (skip 77 unset).
#include <chrono>
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
#include "core/media/image.h"
#include <array>

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
} // namespace

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
        fsf::temp_directory_path(ec) / ("oa_emote_chain_" + std::to_string(::getpid()));
    fsf::remove_all(store_dir, ec);
    auto store = std::make_shared<oa::runtime::DirSaveStore>(store_dir.string());
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);

    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();

    // let the boot Lua run until it parks somewhere
    oa::runtime::FrameInput idle;
    size_t f = 0;
    for (; f < 9000 && !rt.current_wait(); ++f) rt.tick(16, idle);
    std::printf("[emote] parked at f=%zu wait='%s'\n", f,
                rt.current_wait() ? "" : "(none)");

    // Drive the real e-table entry point (game emote.lua wrapper shape):
    // id dotted (carrier chain materializes), single psb file of あやめ.
    // Lua short strings: '\\' -> one backslash; the path must not rely on
    // '\f' (form feed in Lua).
    const std::string code = R"code(
e:createEmoteLayer{ id='1.0.1.fg.aya.p.m.a.b.0',
 files={'image\\fhd\\fg\\aya\\tay_0.psb'},
 width=1920, height=1620 }
)code";
    try {
        rt.interpreter().lua_bridge().run_code(code, "emote_chain_test");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "FAIL: run_code: %s\n", ex.what());
        return 1;
    }
    // Drain: the lyc2 + emotestatic instructions process over the next ticks.
    const size_t start_events = rt.emote_layer_events();
    for (f = 0; f < 6000 && rt.emote_layer_events() == start_events; ++f)
        rt.tick(16, idle);
    std::printf("[emote] after %zu ticks events=%zu layers=%zu wait='%s' layer=%s\n", f,
                rt.emote_layer_events(), rt.emote_layers().size(),
                rt.current_wait() ? "set" : "none",
                rt.scene().find("1.0.1.fg.aya.p.m.a.b.0") ? "exists" : "missing");

    check(rt.emote_layer_events() > start_events, "E-1: emotestatic processed");
    if (rt.emote_layers().empty()) {
        check(false, "E-2: emote state stored");
    } else {
        const auto& kv = *rt.emote_layers().begin();
        const std::string& id = kv.first;
        const oa::runtime::GameRuntime::EmoteLayerState& st = kv.second;
        std::printf("[emote] layer id='%s' file='%s' %dx%d rev=%llu\n", id.c_str(),
                    st.file.c_str(), st.width, st.height,
                    (unsigned long long)st.revision);
        check(id == "1.0.1.fg.aya.p.m.a.b.0", "E-2: emote layer id");
        check(st.width > 0 && st.height > 0 && st.width <= 1920 && st.height <= 1620,
              "E-2: canvas size sane");
        check(st.player != nullptr, "E-2: player exists");
        if (!st.player) {
            check(false, "E-3: frame unavailable without player");
            std::printf("%s\n", failures ? "FAILED" : "PASS");
            return failures ? 1 : 0;
        }
        check(st.width == st.player->width() && st.height == st.player->height() &&
                  st.player->rgba().size() ==
                      size_t(st.width) * size_t(st.height) * 4,
              "E-2: state dims match the render buffer");
        const std::vector<uint8_t>& frgba = st.player->rgba();
        const oa::render::Layer* l = rt.scene().find(id);
        check(l != nullptr, "E-2: scene layer exists");
        check(l && l->content == oa::render::LayerContent::EmoteCanvas,
              "E-2: layer bound to the emote canvas (LayerContent::EmoteCanvas)");

        // E-3: content plausibility (design px are *fit* into the canvas)
        size_t opaque = 0, colored = 0, midColored = 0;
        int minX = st.width, minY = st.height, maxX = 0, maxY = 0;
        for (int y = 0; y < st.height; ++y) {
            for (int x = 0; x < st.width; ++x) {
                const uint8_t* p = &frgba[(size_t(y) * st.width + x) * 4];
                if (p[3] > 64) {
                    ++opaque;
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                    if (p[0] > 12 || p[1] > 12 || p[2] > 12) {
                        ++colored;
                        if (y > st.height / 6 && y < st.height * 5 / 6) ++midColored;
                    }
                }
            }
        }
        const double cov = 100.0 * opaque / (double(st.width) * st.height);
        std::printf("[emote] frame: opaque %.1f%% bbox=(%d,%d)-(%d,%d) colored=%zu "
                    "midColored=%zu\n",
                    cov, minX, minY, maxX, maxY, colored, midColored);
        check(cov > 3.0 && cov < 70.0, "E-3: opaque coverage band");
        check(colored > 4000, "E-3: coloured pixels present");
        check(midColored > 2000, "E-3: central-band coloured pixels");
        check(maxY - minY > st.height / 2, "E-3: figure spans most of the canvas");

        // headless PNG evidence (P1 acceptance artifact)
        const std::vector<uint8_t> png =
            oa::media::encode_png(uint32_t(st.width), uint32_t(st.height), frgba);
        FILE* po = std::fopen("/tmp/nekomiko_emote_static.png", "wb");
        if (po) {
            std::fwrite(png.data(), 1, png.size(), po);
            std::fclose(po);
            std::printf("[emote] evidence PNG written: /tmp/nekomiko_emote_static.png\n");
        }

        // P4 (research/45): the view model is ground-anchored and scale
        // driven — setScale(0.8) must grow the figure upward from the same
        // ground row instead of re-fitting/centering the canvas.
        auto bbox = [&](const std::vector<uint8_t>& buf) {
            int mnX = st.width, mnY = st.height, mxX = 0, mxY = 0;
            for (int y = 0; y < st.height; ++y) {
                for (int x = 0; x < st.width; ++x) {
                    if (buf[(size_t(y) * st.width + x) * 4 + 3] > 64) {
                        if (x < mnX) mnX = x;
                        if (x > mxX) mxX = x;
                        if (y < mnY) mnY = y;
                        if (y > mxY) mxY = y;
                    }
                }
            }
            return std::array<int, 4>{mnX, mnY, mxX, mxY};
        };
        // P4U3 (research/48): the view model follows the E-Mote surface
        // semantics — setScale and setCoord work together (emote.lua:
        // top = pos.height*scale - surfaceH/2 + 100), which anchors the
        // head marker ~110 px below the surface top at every zoom size.
        // Apply the game's pair at size "no" (0.6) and at 0.8 and check the
        // figure zooms head-anchored (top stays ~110..113, width grows,
        // legs run below the surface and crop at its bottom).
        auto apply_view = [&](double sc) {
            const std::string code =
                "local l = e:getEmoteLayer{ id='1.0.1.fg.aya.p.m.a.b.0' }\n"
                "l:setScale(" + std::to_string(sc) + ", 0, 0)\n" +
                "l:setCoord(0, 3695*" + std::to_string(sc) + " - 810 + 100, 0, 0)";
            rt.interpreter().lua_bridge().run_code(code, "emote_chain_test_scale");
            const auto& stv = rt.emote_layers().begin()->second;
            return bbox(stv.player->rgba());
        };
        std::array<int, 4> bb1{};
        std::array<int, 4> bb2{};
        try {
            bb1 = apply_view(0.6);
            bb2 = apply_view(0.8);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "FAIL: setScale lua: %s\n", ex.what());
            return 1;
        }
        // dispatch is synchronous (the player exists): no ticks needed —
        // extra ticks would continue the boot flow and wipe the scene
        // (transition clears emote layers).
        if (rt.emote_layers().empty()) {
            check(false, "E-4: layer vanished before setScale");
        } else {
            check(rt.emote_layers().begin()->second.player->view_scale() > 0.79,
                  "E-4: setScale took effect");
        }
        const int span1 = bb1[3] - bb1[1];
        const int span2 = bb2[3] - bb2[1];
        std::printf("[emote] view0.6 bb=(%d,%d)-(%d,%d) view0.8 bb=(%d,%d)-(%d,%d)\n",
                    bb1[0], bb1[1], bb1[2], bb1[3], bb2[0], bb2[1], bb2[2], bb2[3]);
        // head-anchored zoom: top-of-hair stays in the same ~110 px band at
        // both zooms (formula residual 16px*scale), feet go deeper below
        // the surface (cropped at its bottom in both cases)
        check(bb1[1] > 60 && bb1[1] < 170, "E-4: head row band at 0.6");
        check(std::fabs(double(bb2[1]) - bb1[1]) <= 12.0,
              "E-4: head row anchored on zoom");
        check(bb1[3] >= st.height - 2 && bb2[3] >= st.height - 2,
              "E-4: figure bottom crops at the surface bottom (legs run below)");
        check(span2 >= span1 - 8, "E-4: figure vertical span holds on zoom "
              "(head-anchored: residual 16px*scale)");
        const auto& st2b = rt.emote_layers().begin()->second;
        check(st2b.player->view_scale() > 0.79, "E-4: setScale took effect");
        check(bb2[2] - bb2[0] > bb1[2] - bb1[0] + 20, "E-4: figure widens when zoomed");
    }

    // S3 (research/122): the Lua bridge -> runtime -> EmotePlayer flags path.
    // The official SDK PlayTimeline(label, flags) carries the second argument
    // (TIMELINE_PLAY_PARALLEL=1) — 甜蜜女友3's gesture path calls
    // em:playTimeline(nm, 1). Drive the real Lua bridge call and assert the
    // ordered foreground list received the flag (runtime_lua parses arg 3 as
    // the string-first method family's first number; runtime_media forwards
    // it; the player appends instead of replacing).
    if (!rt.emote_layers().empty()) {
        const std::string code2 =
            "local l = e:getEmoteLayer{ id='1.0.1.fg.aya.p.m.a.b.0' }\n"
            "l:playTimeline('通常待機', 0)\n"
            "l:playTimeline('笑顔_ボイス再生用', 1)\n";
        try {
            rt.interpreter().lua_bridge().run_code(code2, "emote_chain_test_flags");
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "FAIL: playTimeline lua: %s\n", ex.what());
            return 1;
        }
        const auto& stf = rt.emote_layers().begin()->second;
        const auto slots = stf.player->foreground_slots();
        std::printf("[emote] S3 flags: fg list=%zu", slots.size());
        for (const auto& s : slots)
            std::printf(" [%s t=%.0f flags=%d]", s.label.c_str(), s.t, s.flags);
        std::printf("\n");
        check(slots.size() == 2,
              "S3: Lua playTimeline(label, 1) appends to the foreground list");
        if (slots.size() == 2) {
            check(slots[0].label == "通常待機" && slots[1].label == "笑顔_ボイス再生用",
                  "S3: Lua flags path keeps the play order");
            check(slots[1].flags == 1, "S3: the PARALLEL flag survived the bridge");
        }
        check(stf.player->is_timeline_playing("通常待機") &&
                  stf.player->is_timeline_playing("笑顔_ボイス再生用"),
              "S3: isTimelinePlaying reports both list entries");
        check(stf.player->stop_timeline("通常待機"),
              "S3: stopTimeline(name) finds the non-top list entry");
        check(stf.player->is_timeline_playing("笑顔_ボイス再生用") &&
                  !stf.player->is_timeline_playing("通常待機"),
              "S3: stopTimeline(name) ended only that entry");
    }

    std::printf("%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
