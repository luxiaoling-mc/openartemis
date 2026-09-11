// P2 playback test (research/43): EmotePlayer timeline/variable engine.
// Loads the real tay_0.psb from OA_TEST_NEKOMIKO_PFS and drives the layer
// API the game uses (playTimeline/fadeInTimeline/setVariable/pass/step/skip):
//   * idle fade-in (通常待機) runs the breathing loop — revision grows and
//     the composed body_UD variable moves inside [-30..30];
//   * a foreground expression (笑顔_ボイス再生用) changes the *visible*
//     pose — eye icons switch on the face_eye_open axis, so the frame
//     differs from the idle frame by a measurable pixel count (mesh-free
//     acceptance; mesh warp itself is still P3);
//   * face_talk (lip) > 0 flips the mouth icon (0003 -> 0009) — a second,
//     independent pixel diff;
//   * pass()/step()/skip() semantics and timeline auto-end are asserted on
//     the state machine.
// Env: OA_TEST_NEKOMIKO_PFS (skip 77 unset).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/emote/emote_player.h"

namespace {
int failures = 0;
// Windows port (research/128): setenv/unsetenv are POSIX-only. MSVC spells the
// same two operations _putenv_s(name, value) and _putenv_s(name, "") (an empty
// value REMOVES the variable), which is what the emote switches below observe
// through std::getenv at player-construction time. Same inline convention as
// tests/save_thumb_readback_test.cpp:78.
void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1); // overwrite = 1
#endif
}
void unset_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
void checkf(bool cond, const char* what, unsigned long long got) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s (got %llu)\n", what, got);
        ++failures;
    }
}
void checkv(bool cond, const char* what, double got) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s (got %.1f)\n", what, got);
        ++failures;
    }
}
void checku(bool cond, const char* what, unsigned long long got) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s (got %llu)\n", what, got);
        ++failures;
    }
}
uint64_t diff_px(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const size_t px = std::min(a.size(), b.size()) / 4;
    uint64_t d = 0;
    for (size_t i = 0; i < px; ++i) {
        if (std::abs(int(a[i * 4]) - int(b[i * 4])) > 24 ||
            std::abs(int(a[i * 4 + 1]) - int(b[i * 4 + 1])) > 24 ||
            std::abs(int(a[i * 4 + 2]) - int(b[i * 4 + 2])) > 24)
            ++d;
    }
    return d;
}
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pfs_path = std::getenv("OA_TEST_NEKOMIKO_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_NEKOMIKO_PFS unset; skipping\n");
        return 77;
    }
    std::string err;
    oa::fs::PhysFileSystem fs_phys(pfs_path, false);    oa::fs::IFileSystem& fs = fs_phys;
    auto bytes = fs.read("image\\fhd\\fg\\aya\\tay_0.psb");
    if (!bytes) {
        std::fprintf(stderr, "FAIL: read tay_0.psb failed\n");
        return 1;
    }

    oa::emote::EmotePlayer p;
    if (!p.load(*bytes, 960, 810, &err)) {
        std::fprintf(stderr, "FAIL: player load: %s\n", err.c_str());
        return 1;
    }
    // P4U3 (research/48): the view model follows the E-Mote surface
    // semantics — the figure is placed by the game's setScale/setCoord pair
    // (emote.lua: top = pos.height*scale - surfaceH/2 + 100 anchors the head
    // ~110 px below the surface top at every zoom). Mirror the game call
    // with this canvas (surface height 810) so the face/torso region the
    // tests measure is on-canvas.
    p.set_scale(0.6, 0, 0);
    p.set_coord(0, 3695.0 * 0.6 - 810.0 / 2.0 + 100.0);
    check(p.idle_timeline().empty(), "start: no idle timeline");
    check(p.foreground_timeline().empty(), "start: no foreground timeline");

    // --- idle breathing loop -------------------------------------------------
    p.fade_in_timeline("通常待機");
    check(p.idle_timeline() == "通常待機", "idle timeline after fadeIn");
    const uint64_t rev0 = p.revision();
    double body_ud_seen = 0;
    for (int i = 0; i < 100; ++i) { // 100 * 100 ms = 10 s (well past 5 s loop)
        p.advance_ms(100);
        const double v = p.get_variable("body_UD");
        if (std::fabs(v) > std::fabs(body_ud_seen)) body_ud_seen = v;
    }
    std::printf("idle: rev %llu -> %llu maxBodyUD=%.1f\n", (unsigned long long)rev0,
                (unsigned long long)p.revision(), body_ud_seen);
    check(p.revision() > rev0 + 3, "idle loop re-renders (revision grows)");
    checkv(std::fabs(body_ud_seen) > 8.0, "idle breathing moves body_UD", body_ud_seen);
    // P3 (research/44): breathing is bezier-mesh motion — two idle poses a
    // second apart must differ in the pixel domain once the mesh warp lands.
    p.advance_ms(1200);
    const std::vector<uint8_t> breath_a = p.rgba();
    const double ud_a = p.get_variable("body_UD");
    p.advance_ms(1500);
    const std::vector<uint8_t> breath_b = p.rgba();
    const double ud_b = p.get_variable("body_UD");
    const uint64_t d_breath = diff_px(breath_a, breath_b);
    std::printf("breath diff over 1.5s: %llu px (body_UD %.1f -> %.1f)\n",
                (unsigned long long)d_breath, ud_a, ud_b);
    checkf(d_breath > 10000, "breathing (bezier warp) changes visible pixels",
           (unsigned long long)d_breath);
    const std::vector<uint8_t> idle_frame = breath_b;

    // --- expression foreground (icon switches on face_eye_open) -------------
    p.play_timeline("笑顔_ボイス再生用");
    check(p.foreground_timeline() == "笑顔_ボイス再生用", "foreground active after play");
    for (int i = 0; i < 5; ++i) p.advance_ms(100); // ~30 frames in: eye open ~23
    const double eye_v = p.get_variable("face_eye_open");
    const std::vector<uint8_t> smile_frame = p.rgba();
    for (int i = 0; i < 35; ++i) p.advance_ms(100); // past the one-shot end
    check(p.foreground_timeline().empty(), "one-shot foreground ends by itself");
    const uint64_t d_eye = diff_px(idle_frame, smile_frame);
    std::printf("expression diff vs idle: %llu px (face_eye_open=%.1f)\n",
                (unsigned long long)d_eye, eye_v);
    checkf(d_eye > 40000, "expression switch changes visible pixels", (unsigned long long)d_eye);

    // --- lip sync variable (mouth icon 0003 -> 0009) ------------------------
    p.play_timeline("通常_ボイス再生用");
    for (int i = 0; i < 10; ++i) p.advance_ms(100);
    p.advance_ms(300); // let the throttled pose settle
    const std::vector<uint8_t> mouth_closed = p.rgba();
    p.set_variable("face_talk", 3);
    p.advance_ms(300);
    const std::vector<uint8_t> mouth_open = p.rgba();
    const uint64_t d_mouth = diff_px(mouth_closed, mouth_open);
    std::printf("mouth diff (face_talk 0->3): %llu px\n", (unsigned long long)d_mouth);
    checkf(d_mouth > 1500, "face_talk flips the mouth icon", (unsigned long long)d_mouth);
    p.set_variable("face_talk", 0);
    check(p.get_variable("face_talk") == 0.0, "getVariable returns the set value");

    // --- pass/step/skip state semantics -------------------------------------
    p.play_timeline("笑顔_ボイス再生用");
    p.pass();
    check(p.foreground_timeline().empty(), "pass clears the foreground");
    p.play_timeline("笑顔_ボイス再生用");
    p.step();
    check(p.foreground_timeline().empty(), "step clears the foreground");
    p.play_timeline("通常_ボイス再生用");
    p.skip();
    check(p.foreground_timeline().empty(), "skip clears the foreground");
    check(p.idle_timeline().empty(), "skip releases the idle loop");
    p.fade_in_timeline("通常待機");
    p.stop();
    check(p.idle_timeline().empty() && p.foreground_timeline().empty(),
          "stop drops both slots");

    // --- P4U9 Pass display semantics (research/54) --------------------------
    // Official SDK (api_skip.html): Pass() skips the INTERNAL playback state
    // but the displayed picture must not jump — it transitions gently toward
    // the next timeline play. Regression: pass() must keep the composed
    // variable snapshot bit-identical (expression frozen, not released to
    // the neutral base), keep the idle loop breathing on top, and let the
    // next playTimeline replace the freeze wholesale.
    {
        oa::emote::EmotePlayer pp;
        if (pp.load(*bytes, 960, 810, &err)) {
            pp.set_scale(0.6, 0, 0);
            pp.set_coord(0, 3695.0 * 0.6 - 810.0 / 2.0 + 100.0);
            pp.fade_in_timeline("通常待機");
            pp.play_timeline("怒り_ボイス再生用");
            pp.advance_ms(400); // ~24 frames in: mid-expression sample
            const auto vBefore = pp.variables();
            pp.pass();
            check(pp.foreground_timeline().empty(), "pass ends the fg internally");
            // frozen: identical snapshot before/after pass (no display jump)
            const auto vAfter = pp.variables();
            bool same = vBefore.size() == vAfter.size();
            if (same) {
                for (const auto& kv : vBefore) {
                    const auto it = vAfter.find(kv.first);
                    if (it == vAfter.end() || std::fabs(it->second - kv.second) > 1e-9) {
                        same = false;
                        break;
                    }
                }
            }
            std::printf("pass freeze: varsBefore=%zu sameAfterPass=%d\n",
                        vBefore.size(), same ? 1 : 0);
            checkf(same, "pass() keeps the composed snapshot (display does not jump)",
                   vBefore.size());
            // idle keeps breathing under the frozen expression: body_UD moves
            // while a face-only axis (face_eyebrow, not written by the idle
            // diff loop) stays frozen at the pass-time value
            const auto eyebrowFrozen = pp.get_variable("face_eyebrow");
            pp.advance_ms(500);
            const bool bodyMoved =
                std::fabs(pp.get_variable("body_UD") - vAfter.at("body_UD")) > 0.5;
            const bool faceFrozen =
                pp.get_variable("face_eyebrow") == eyebrowFrozen;
            std::printf("pass freeze: body_UD %.2f -> %.2f face_eyebrow stays %.2f\n",
                        vAfter.at("body_UD"), pp.get_variable("body_UD"), eyebrowFrozen);
            checkf(bodyMoved && faceFrozen,
                   "idle breathing continues under the pass-frozen expression",
                   eyebrowFrozen);
            // the next play replaces the frozen/domain pose wholesale (fg t0)
            pp.play_timeline("笑顔_ボイス再生用");
            check(pp.get_variable("face_eyebrow") != eyebrowFrozen ||
                      pp.get_variable("face_eye_open") != vAfter.at("face_eye_open"),
                  "next playTimeline replaces the pass pose");
            pp.pass(); // keep B's pose in the domain
            const double browB = pp.get_variable("face_eyebrow");
            check(browB != eyebrowFrozen, "pass after the new play holds B's pose");
            pp.stop(); // stop ends both slots; the domain keeps the pose
            check(pp.idle_timeline().empty() && pp.foreground_timeline().empty(),
                  "stop drops both slots");
            check(pp.get_variable("face_eyebrow") == browB,
                  "stop() keeps the domain pose (no snap to defaults)");
        } else {
            check(false, "pass-freeze player load");
        }
    }

    // --- research/54: natural end keeps the domain (expression persists) ---
    // Official variable model: a timeline that ends stops WRITING; the
    // variables keep their last values (krkr domain behaviour) — the old
    // per-frame rebuild released them to defaults, snapping the face back
    // to neutral the moment a one-shot voice timeline ended mid-wait. Lock:
    // the face values captured mid-expression are unchanged after the
    // timeline auto-ends and on further frames.
    {
        oa::emote::EmotePlayer ne;
        if (ne.load(*bytes, 960, 810, &err)) {
            ne.set_scale(0.6, 0, 0);
            ne.set_coord(0, 3695.0 * 0.6 - 810.0 / 2.0 + 100.0);
            ne.fade_in_timeline("通常待機");
            ne.play_timeline("怒り_ボイス再生用");
            ne.advance_ms(400); // mid-expression (~24 frames in)
            const double browMid = ne.get_variable("face_eyebrow");
            const double brow0 = ne.get_variable("face_eye_open");
            ne.advance_ms(1600); // well past the one-shot tail
            const bool ended = ne.foreground_timeline().empty();
            const bool kept =
                ne.get_variable("face_eyebrow") == browMid &&
                ne.get_variable("face_eye_open") == brow0;
            ne.advance_ms(1000); // still held on further frames
            const bool held = ne.get_variable("face_eyebrow") == browMid &&
                              ne.get_variable("face_eye_open") == brow0;
            std::printf("natural end: fg ended=%d browMid=%.1f kept=%d held=%d\n",
                        ended ? 1 : 0, browMid, kept ? 1 : 0, held ? 1 : 0);
            checkf(browMid != 0.0,
                   "mid-expression face is non-neutral (assert meaningful)",
                   browMid);
            checkf(ended && kept && held,
                   "one-shot natural end keeps the domain pose (no release snap)",
                   browMid);
        } else {
            check(false, "natural-end player load");
        }
    }

    // --- E14 fold fix (research/52, D1): step/skip fold the natural END ---
    // One-shot expression tracks (arm_type, face_* ...) end with their last
    // authored content key followed by a type-0 tail; a line that plays out
    // naturally leaves every track's final content value in the persistent
    // domain (the authored end pose: arm_type -> 驚き's B/C arm group, not
    // the t0 neutral A group). step()/skip() must fold that same end pose —
    // the old fold sampled t = lastTime>0 ? lastTime : 0, and since every
    // NekoMiko one-shot carries lastTime = -1 it re-sampled t = 0: a
    // stepped/skipped (or load-restored) expression reverted to its START
    // pose (arm_type snapped back to 0) and could mix in stale pre-play
    // domain values. Lock: after step()/skip(), immediate or mid-expression,
    // variables() equals the natural-end snapshot and arm_type equals the
    // natural-end value (tay 驚き_ボイス再生用 ends 1, tka ends 2).
    auto fold_check = [&](const char* file, const char* tag, double armEnd) {
        oa::emote::EmotePlayer f;
        std::string ferr;
        auto fb = fs.read(file);
        if (!fb || !f.load(*fb, 960, 810, &ferr)) {
            std::fprintf(stderr, "FAIL: %s fold load\n", tag);
            return;
        }
        auto arm = [&]() { return f.get_variable("arm_type"); };
        auto same_vars = [](const std::map<std::string, double>& a,
                            const std::map<std::string, double>& b) {
            if (a.size() != b.size()) return false;
            for (const auto& kv : a) {
                const auto it = b.find(kv.first);
                if (it == b.end() || std::fabs(it->second - kv.second) > 1e-9)
                    return false;
            }
            return true;
        };
        // reference: the same line played out to its natural end (16 ms ticks
        // sample every track's final content window before the slot stops)
        f.play_timeline("驚き_ボイス再生用");
        while (!f.foreground_timeline().empty()) f.advance_ms(16);
        const double armNat = arm();
        const std::map<std::string, double> varsNat = f.variables();
        // step()/skip() right after play: the fold must land that end pose
        f.play_timeline("驚き_ボイス再生用");
        f.step();
        const double armStep = arm();
        const bool stepSame = same_vars(f.variables(), varsNat);
        f.play_timeline("驚き_ボイス再生用");
        f.skip();
        const double armSkip = arm();
        const bool skipSame = same_vars(f.variables(), varsNat);
        // mid-expression step (the game's skip/auto path): the fold still
        // lands the end pose — not the start pose, not the mid-curve mix
        f.play_timeline("驚き_ボイス再生用");
        f.advance_ms(250); // ~15 frames in: arm window crossed, face mid-curve
        f.step();
        const double armMid = arm();
        const bool midSame = same_vars(f.variables(), varsNat);
        std::printf("%s fold: arm natural=%.1f step=%.1f skip=%.1f midstep=%.1f "
                    "varsSame step=%d skip=%d mid=%d\n",
                    tag, armNat, armStep, armSkip, armMid, stepSame ? 1 : 0,
                    skipSame ? 1 : 0, midSame ? 1 : 0);
        checkv(std::fabs(armNat - armEnd) < 0.01,
               "natural end leaves the authored arm_type tail value", armNat);
        checkv(std::fabs(armStep - armEnd) < 0.01 &&
                   std::fabs(armSkip - armEnd) < 0.01 &&
                   std::fabs(armMid - armEnd) < 0.01,
               "step/skip fold the natural-end arm_type (D1)", armStep);
        checkf(stepSame && skipSame && midSame,
               "step/skip fold == natural-end variable snapshot (D1)",
               varsNat.size());
    };
    fold_check("image\\fhd\\fg\\aya\\tay_0.psb", "tay", 1.0);
    fold_check("image\\fhd\\fg\\kae\\tka_0.psb", "tka", 2.0);

    // --- S3 (research/122): wrap rule / phase / REF-NEW / foreground list ----
    // The S3 slot model: a timeline wraps iff its authored loopEnd > 0 (period
    // loopEnd - loopBegin, the loopEnd frame itself reachable), everything else
    // is a one-shot that ends at its tail; the foreground is an ordered play
    // list. NekoMiko's idle (通常待機) is an authored loop with loopBegin 0, so
    // the S1 census classified it as never having exposed the P0 freeze — this
    // block pins the loop-boundary phase (G7) and the same-process REF/NEW.
    {
        const oa::emote::EmoteTimeline* idle = nullptr;
        for (const auto& tl : p.file().timelines)
            if (tl.label == "通常待機") idle = &tl;
        check(idle && idle->loopEnd > 0, "S3: 通常待機 is an authored loop");
        if (idle) {
            const int loopEnd = idle->loopEnd;
            const int loopBegin = idle->loopBegin;
            oa::emote::EmotePlayer sp;
            if (!sp.load(*bytes, 960, 810, &err)) {
                check(false, "S3 wrap player load");
            } else {
                sp.set_external_pose(true);
                sp.fade_in_timeline("通常待機");
                checkv(std::fabs(sp.idle_time() - double(loopBegin)) < 1e-9,
                       "S3: start phase = loopBegin", sp.idle_time());
                bool hitEnd = false, monotone = true;
                int wraps = 0;
                double maxT = 0, prevT = sp.idle_time(), firstWrapTo = -1;
                for (int f = 0; f < loopEnd + 5; ++f) {
                    sp.progress(1.0);
                    const double t = sp.idle_time();
                    if (std::fabs(t - double(loopEnd)) < 1e-9) hitEnd = true;
                    if (t > maxT) maxT = t;
                    if (t < prevT) {
                        ++wraps;
                        if (firstWrapTo < 0) firstWrapTo = t;
                    } else if (std::fabs(t - (prevT + 1.0)) > 1e-9) {
                        monotone = false;
                    }
                    prevT = t;
                }
                std::printf("S3 wrap: 通常待機 loop=[%d,%d] maxT=%.0f hitLoopEnd=%d "
                            "wraps=%d wrapTo=%.0f monotone=%d\n",
                            loopBegin, loopEnd, maxT, hitEnd ? 1 : 0, wraps,
                            firstWrapTo, monotone ? 1 : 0);
                check(monotone, "S3: the loop clock advances one frame per progress(1)");
                check(hitEnd, "S3: the authored loopEnd frame is reachable (G7)");
                check(maxT <= double(loopEnd) + 1e-9,
                      "S3: the loop clock never exceeds loopEnd");
                check(wraps == 1 && std::fabs(firstWrapTo - 1.0) < 1e-9,
                      "S3: the clock wraps by period = loopEnd - loopBegin");
            }
            // same-process REF/NEW: the pre-S3 model (OA_EMOTE_LEGACY_SLOTS=1
            // latched at construction, like OA_EMOTE_FPS) vs the S3 model. The
            // data is lazy for NekoMiko inside the loop, so the frame-by-frame
            // composed snapshots must be identical until the loop boundary —
            // and any difference has to sit exactly on the loopEnd frame.
            auto snapshots = [&](bool legacy, int frames, std::vector<double>* ts) {
                std::vector<std::map<std::string, double>> out;
                if (legacy) set_env("OA_EMOTE_LEGACY_SLOTS", "1");
                oa::emote::EmotePlayer lp;
                const bool ok = lp.load(*bytes, 960, 810, &err);
                if (legacy) unset_env("OA_EMOTE_LEGACY_SLOTS");
                if (!ok) return out;
                lp.set_external_pose(true);
                lp.fade_in_timeline("通常待機");
                for (int f = 0; f < frames; ++f) {
                    lp.progress(1.0);
                    out.push_back(lp.variables());
                    if (ts) ts->push_back(lp.idle_time());
                }
                return out;
            };
            std::vector<double> tsRef, tsNew;
            const auto refSnaps = snapshots(true, 120, nullptr);
            const auto newSnaps = snapshots(false, 120, nullptr);
            size_t windowDiff = 0;
            for (size_t f = 0; f < std::min(refSnaps.size(), newSnaps.size()); ++f) {
                bool diff = refSnaps[f].size() != newSnaps[f].size();
                for (const auto& kv : newSnaps[f]) {
                    const auto it = refSnaps[f].find(kv.first);
                    if (it == refSnaps[f].end() || std::fabs(it->second - kv.second) > 1e-9)
                        diff = true;
                }
                if (diff) ++windowDiff;
            }
            std::printf("S3 REF/NEW window(120f): differingFrames=%zu\n", windowDiff);
            check(refSnaps.size() == 120 && newSnaps.size() == 120,
                  "S3 REF/NEW: both models produced 120 frames");
            checku(windowDiff == 0,
                   "S3 REF/NEW: inside the loop both models are frame-identical",
                   (unsigned long long)windowDiff);
            const int longFrames = loopEnd * 2 + 20;
            const auto refLong = snapshots(true, longFrames, &tsRef);
            const auto newLong = snapshots(false, longFrames, &tsNew);
            size_t longDiff = 0, offBoundary = 0;
            size_t boundaryHits = 0;
            for (size_t f = 0; f < std::min(refLong.size(), newLong.size()); ++f) {
                bool diff = refLong[f].size() != newLong[f].size();
                for (const auto& kv : newLong[f]) {
                    const auto it = refLong[f].find(kv.first);
                    if (it == refLong[f].end() || std::fabs(it->second - kv.second) > 1e-9)
                        diff = true;
                }
                if (std::fabs(tsNew[f] - double(loopEnd)) < 1e-9) ++boundaryHits;
                if (diff) {
                    ++longDiff;
                    if (std::fabs(tsNew[f] - double(loopEnd)) > 1e-9) ++offBoundary;
                }
            }
            std::printf("S3 REF/NEW window(%df): boundaryFrames=%zu differing=%zu "
                        "offBoundary=%zu (new t at wrap=%.0f)\n",
                        longFrames, boundaryHits, longDiff, offBoundary,
                        tsNew.empty() ? -1 : tsNew.back());
            checku(newLong.size() == size_t(longFrames),
                   "S3 REF/NEW: long window produced frames",
                   (unsigned long long)newLong.size());
            checku(boundaryHits == 2, "S3 REF/NEW: the window crosses the loop boundary twice",
                   (unsigned long long)boundaryHits);
            checku(offBoundary == 0,
                   "S3 REF/NEW: every NekoMiko difference sits on the loopEnd frame",
                   (unsigned long long)offBoundary);
        }
    }

    // --- S3 foreground play list on the NekoMiko gate -----------------------
    {
        oa::emote::EmotePlayer lp;
        if (!lp.load(*bytes, 960, 810, &err)) {
            check(false, "S3 list player load");
        } else {
            lp.set_external_pose(true);
            lp.play_timeline("通常待機", 0);
            lp.play_timeline("笑顔_ボイス再生用", 1); // PARALLEL
            auto slots = lp.foreground_slots();
            checku(slots.size() == 2, "S3 list: PARALLEL playTimeline appends",
                   (unsigned long long)slots.size());
            check(lp.is_timeline_playing("通常待機") &&
                      lp.is_timeline_playing("笑顔_ボイス再生用"),
                  "S3 list: both entries report as playing");
            lp.progress(4.0);
            slots = lp.foreground_slots();
            check(slots.size() == 2 && std::fabs(slots[0].t - 4.0) < 1e-9 &&
                      std::fabs(slots[1].t - 4.0) < 1e-9,
                  "S3 list: entries keep independent clocks");
            lp.play_timeline("悲しみ_ボイス再生用", 0); // replace
            slots = lp.foreground_slots();
            check(slots.size() == 1 && slots[0].label == "悲しみ_ボイス再生用",
                  "S3 list: a plain playTimeline replaces the list");
            lp.pass();
            check(lp.foreground_slots().empty(), "S3 list: pass() clears the list");
        }
    }

    // --- P4U4 dense timeline interplay regression (research/49 §A) --------
    // The idle loop (通常待機) is a diff (delta) timeline: its samples ADD to
    // the foreground's absolute pose (参照实现-emote runtime semantics). With
    // the old overwrite model breathing froze whenever an expression played
    // and body axes popped by up to half their range at line boundaries —
    // the intermittent head/body separation the user reported.
    p.fade_in_timeline("通常待機");
    p.play_timeline("笑顔_ボイス再生用");
    for (int i = 0; i < 8; ++i) p.advance_ms(16); // past the 4-frame entrance
    check(p.foreground_timeline() == "笑顔_ボイス再生用", "fg still active");
    double blo = 1e9, bhi = -1e9, hlo = 1e9, hhi = -1e9;
    for (int i = 0; i < 34; ++i) { // sample frames 8..42 (fg ends at 47)
        p.advance_ms(16);
        check(p.foreground_timeline() == "笑顔_ボイス再生用", "fg active mid-window");
        const double b = p.get_variable("body_UD");
        const double h = p.get_variable("head_UD");
        blo = std::min(blo, b); bhi = std::max(bhi, b);
        hlo = std::min(hlo, h); hhi = std::max(hhi, h);
    }
    std::printf("under-fg breathing body_UD [%.1f..%.1f] head_UD [%.1f..%.1f]\n",
                blo, bhi, hlo, hhi);
    checkf(bhi - blo > 8.0,
           "breathing keeps oscillating under an active foreground (body_UD)",
           (unsigned long long)((bhi - blo) * 10));
    checkf(hhi - hlo > 3.0,
           "head sway keeps oscillating under an active foreground (head_UD)",
           (unsigned long long)((hhi - hlo) * 10));
    // foreground auto-end must not pop the body axes: the expression's tail
    // pose is 0 on every axis, so the composed value stays continuous when
    // the slot releases (old model jumped from 0 to the idle phase value,
    // up to half the body_UD range in one frame).
    p.stop();
    p.fade_in_timeline("通常待機");
    p.play_timeline("通常_ボイス再生用"); // 47-frame one-shot, neutral tail
    double prevB = p.get_variable("body_UD");
    double maxPop = 0;
    bool saw_end = false;
    for (int i = 0; i < 90; ++i) { // ~1.5 s @16 ms frames
        p.advance_ms(16);
        const double b = p.get_variable("body_UD");
        if (!p.foreground_timeline().empty()) prevB = b;
        else if (!saw_end) {
            saw_end = true;
            maxPop = std::fabs(b - prevB);
        }
    }
    std::printf("fg auto-end body_UD pop=%.2f\n", maxPop);
    check(maxPop <= 2.0, "fg auto-end does not pop body axes");

    // --- P4U7 variable-driven pose regression (research/52) ------------------
    // Semantics re-check (krkr variable system + raw PSB classification):
    // timelineControl tracks are TIME tracks that write the variable domain;
    // the POSE is a function of those variables through parameterized motion
    // chains (variable -> transToTick -> node frame axis). Directly setting a
    // variable must move the pose with no timeline active — head_slant drives
    // the head-region morph chain.
    {
        oa::emote::EmotePlayer vp;
        if (vp.load(*bytes, 960, 810, &err)) {
            vp.set_scale(0.6, 0, 0);
            vp.set_coord(0, 3695.0 * 0.6 - 810.0 / 2.0 + 100.0);
            const std::vector<uint8_t> pose0 = vp.rgba();
            auto band_diff_rows = [](const std::vector<uint8_t>& a,
                                     const std::vector<uint8_t>& b, size_t y0,
                                     size_t y1, size_t w) {
                uint64_t d = 0;
                for (size_t y = y0; y < y1 && y < b.size() / (w * 4); ++y)
                    for (size_t x = 0; x < w; ++x) {
                        const uint8_t* pa = &a[(y * w + x) * 4];
                        const uint8_t* pb = &b[(y * w + x) * 4];
                        if (std::abs(int(pa[0]) - int(pb[0])) > 24 ||
                            std::abs(int(pa[1]) - int(pb[1])) > 24 ||
                            std::abs(int(pa[2]) - int(pb[2])) > 24)
                            ++d;
                    }
                return d;
            };
            // head band rows 60..420 (960x810 canvas, game view)
            vp.set_variable("face_eye_open", 23.0);
            vp.advance_ms(200); // let the throttled renderer settle
            const std::vector<uint8_t> poseEye = vp.rgba();
            vp.set_variable("face_eye_open", 0.0);
            vp.set_variable("face_talk", 3.0);
            vp.advance_ms(200);
            const std::vector<uint8_t> poseTalk = vp.rgba();
            // Eye-open change spans the face rows (eyes ~rows 120..420);
            // the mouth icon switch is a small feature on this canvas and
            // lands in rows ~420..540 (measured 269 px at delta>24 on the
            // static scene; the old 48k "mouth" number was breathing drift,
            // which this no-timeline scene intentionally excludes).
            const uint64_t dHeadEye = band_diff_rows(pose0, poseEye, 60, 420, 960);
            const uint64_t dBodyEye = band_diff_rows(pose0, poseEye, 560, 800, 960);
            const uint64_t dMouthTalk = band_diff_rows(pose0, poseTalk, 420, 540, 960);
            std::printf("var-driven pose: dHead(eye23)=%llu dBody(eye23)=%llu "
                        "dMouth(talk3)=%llu\n",
                        (unsigned long long)dHeadEye, (unsigned long long)dBodyEye,
                        (unsigned long long)dMouthTalk);
            checkf(dHeadEye > 500 && dMouthTalk > 150,
                   "variables move the face region (variable->pose)",
                   (unsigned long long)dHeadEye);
            checkf(dHeadEye > dBodyEye,
                   "face_eye_open moves the head more than the body",
                   (unsigned long long)dHeadEye);
        } else {
            check(false, "variable-pose player load");
        }
    }

    // --- P4U8 variable-hub invariant (research/53) ----------------------------
    // v4 semantics (user adjudication): timelines are TIME-driven writers of
    // the shared VARIABLE domain; part/mesh/icon state is a pure function of
    // the composed variable snapshot (variable value -> parameter-axis tick ->
    // node frames). The pose evaluator receives no clock at all (base-motion
    // tick is constant 0; only parameterized axes move with the variables), so
    // the structural invariant "same variable snapshot => same pose, whatever
    // the slot clocks" must hold. Force it: capture a running-idle pose with
    // its exact composed snapshot, then replay that snapshot on a second
    // player whose idle slot sits at a DIFFERENT clock position, and require
    // byte-identical canvases.
    {
        oa::emote::EmotePlayer hubA;
        std::string hubErr;
        if (hubA.load(*bytes, 960, 810, &hubErr)) {
            hubA.set_scale(0.6, 0, 0);
            hubA.set_coord(0, 3695.0 * 0.6 - 810.0 / 2.0 + 100.0);
            hubA.fade_in_timeline("通常待機");
            hubA.advance_ms(1500); // idle clock at ~t90 (body_UD -11.4 ...)
            hubA.render_now();     // force the render matching the snapshot
            const std::map<std::string, double> snap = hubA.variables();
            const std::vector<uint8_t> frameA = hubA.rgba();

            oa::emote::EmotePlayer hubB;
            if (!hubB.load(*bytes, 960, 810, &hubErr) ||
                !(hubB.set_scale(0.6, 0, 0),
                  hubB.set_coord(0, 3695.0 * 0.6 - 810.0 / 2.0 + 100.0),
                  hubB.fade_in_timeline("通常待機"),
                  hubB.advance_ms(3500), // idle clock at a DIFFERENT position
                  true)) {
                check(false, "varhub player B setup");
            } else {
                hubB.stop(); // slots off: next compose is defaults+explicit
                const std::vector<uint8_t> neutral = hubB.rgba();
                for (const auto& kv : snap) hubB.set_variable(kv.first, kv.second);
                hubB.render_now();
                const std::vector<uint8_t> frameB = hubB.rgba();
                const bool same =
                    frameA.size() == frameB.size() && !frameA.empty() &&
                    std::memcmp(frameA.data(), frameB.data(), frameA.size()) == 0;
                std::printf("varhub invariant: idle clocks tA=90f tB=210f "
                            "snapVars=%zu pxSame=%d\n",
                            snap.size(), same ? 1 : 0);
                checkf(same, "same variable snapshot => same pose (clock-free parts)",
                       snap.size());
                checkf(frameA != neutral,
                       "captured snapshot is not the neutral pose",
                       (unsigned long long)std::count_if(
                           frameA.begin(), frameA.end(),
                           [](uint8_t v) { return v != 0; }));
            }
        } else {
            check(false, "varhub player load");
        }
    }

    // --- P4U6/E9-r entry-state regression (ruling correction, research/50) --
    // The user's standing-idle report ("head offset large, body small, then
    // return; left/right models differ") was the idle diff loop driving the
    // pose-rotation axes through 通常待機's authored curves — head_slant
    // (0,12)(1,-5)(119,0)(204,12) / body_slant (0,6)... (tay) and -14.4 /
    // 8.19 (tka) — so every 5 s loop (and the fade-in entry) tilts head/body
    // sideways. This was first adjudicated as an engine defect (E9, da9172f:
    // idle must not clock-write rotation axes; slant frozen at 0), then the
    // user RULED THAT ADJUDICATION WRONG: the perceived 5 s tilt drift was
    // the pre-f228cce geometry/interpolation bug (stair-step full-extent
    // slant holds, wrong pivot); with the render fixed, the idle diff layer
    // must drive head_slant/body_slant again (the authored curves are the
    // natural idle head/body motion — the engine only reproduces the data).
    // Lock the corrected semantics:
    //  * during a plain standing idle (fade-in 通常待機, no expression) the
    //    slant axes follow the authored idle curves from the entry seam
    //    pose (values below are timeline samples, the same keyframes our
    //    parser reads; 16 ms ticks advance 0.96 frames);
    //  * the idle delta is still NOT folded into the domain: once the idle
    //    slot ends (stop), the slant axes return to the domain value (0
    //    without an expression) — no accumulation;
    //  * the breathing axes keep oscillating (body_UD/head_UD);
    //  * direct setVariable still moves the slant (variable->pose chain,
    //    asserted in P4U7 above).
    auto entry_check = [&](const char* file, const char* tag, double hs0, double bs0,
                           double hs30, double bs30, double hs110, double bs110) {
        oa::emote::EmotePlayer en;
        std::string enerr;
        auto bytes2 = fs.read(file);
        if (!bytes2 || !en.load(*bytes2, 1920, 1620, &enerr)) {
            std::fprintf(stderr, "FAIL: %s entry load\n", tag);
            return;
        }
        en.set_scale(0.6, 0, 0);
        en.set_coord(0, 3695.0 * 0.6 - 810.0 + 100.0);
        en.fade_in_timeline("通常待機");
        auto V = [&](const char* k) { return en.get_variable(k); };
        // t0 (seam pose): idle loopBegin sample, no engine transient
        const double hs0g = V("head_slant"), bs0g = V("body_slant");
        checkv(std::fabs(hs0g - hs0) < 0.01, tag, hs0g);
        checkv(std::fabs(bs0g - bs0) < 0.01, tag, bs0g);
        double hs_30 = 0, bs_30 = 0, hs_110 = 0, bs_110 = 0;
        double maxHs = 0, maxBs = 0, maxBu = 0, maxHu = 0;
        for (int i = 0; i <= 110; ++i) { // 110 ticks * 16 ms = 105.6 authored frames
            en.advance_ms(16);
            maxHs = std::max(maxHs, std::fabs(V("head_slant")));
            maxBs = std::max(maxBs, std::fabs(V("body_slant")));
            maxBu = std::max(maxBu, std::fabs(V("body_UD")));
            maxHu = std::max(maxHu, std::fabs(V("head_UD")));
            if (i == 30) { hs_30 = V("head_slant"); bs_30 = V("body_slant"); }
            if (i == 110) { hs_110 = V("head_slant"); bs_110 = V("body_slant"); }
        }
        checkv(std::fabs(hs_30 - hs30) < 0.05, tag, hs_30);
        checkv(std::fabs(bs_30 - bs30) < 0.05, tag, bs_30);
        checkv(std::fabs(hs_110 - hs110) < 0.05, tag, hs_110);
        checkv(std::fabs(bs_110 - bs110) < 0.05, tag, bs_110);
        checkv(maxBu > 8.0 && maxHu > 2.0,
               "standing idle keeps breathing (body_UD/head_UD oscillate)", maxBu);
        std::printf("%s entry: slant follows idle curves (hs0=%.2f bs0=%.2f hs30=%.2f "
                    "bs30=%.2f hs110=%.2f bs110=%.2f; max %.1f/%.1f) "
                    "breathing body_UD max %.1f head_UD max %.1f\n",
                    tag, hs0g, bs0g, hs_30, bs_30, hs_110, bs_110, maxHs, maxBs,
                    maxBu, maxHu);
        // idle delta never folds into the domain: ending the idle slot
        // (nothing else ever wrote head_slant/body_slant here) releases the
        // slant axes back to their domain value 0 — no accumulation
        en.stop();
        checkv(std::fabs(V("head_slant")) < 0.01 && std::fabs(V("body_slant")) < 0.01,
               "idle delta never folds into the domain (slant back to 0 after stop)",
               V("head_slant"));
        // the pose-rotation axes still respond to direct variable writes
        en.set_variable("head_slant", 12.0);
        en.advance_ms(50);
        checkv(std::fabs(V("head_slant") - 12.0) < 0.01,
               "direct head_slant write reaches the domain", V("head_slant"));
    };
    // data (参照实现 timeline sample; same keyframes our parser reads):
    // tay: head_slant (0,12)(1,-5)(119,0)(204,12); body_slant (0,6)(1,-6)(142,6)
    // tka: head_slant (0,-14.4)(111,5.4)(234,0); body_slant (0,-1.4)(37,4.2)(111,8.19)(223,0)
    // Checkpoint i lands on the (i+1)-th 16 ms tick (each advances 0.96
    // authored frames): i=30 -> t=29.76 f, i=110 -> t=106.56 f; values below
    // are the type2-linear interp of the curve at those exact times.
    entry_check("image\\fhd\\fg\\aya\\tay_0.psb", "tay entry", 12.0, 6.0,
                -3.78, -3.55, -0.53, 2.98);
    entry_check("image\\fhd\\fg\\kae\\tka_0.psb", "tka entry", -14.4, -1.4,
                -9.09, 3.10, 4.60, 7.95);

    // --- P4U5 dense-alternation long run (research/50) -----------------------
    // krkr's runner evaluates the whole emote every display frame and every
    // part reads the same per-frame variable domain; pose = f(vars) is
    // coherent everywhere (research/49 + per-axis var sweeps), so a true
    // head/body decoupling between consecutive presented poses would betray
    // a playback-management defect (non-atomic state). Track two stable
    // single landmarks — the face contour icon and the chest icon (looked up
    // by icon name at the neutral pose) — over minutes of dense dialog
    // alternation (geometry via emote_collect_parts; external-pose mode so
    // the GPU pose cadence applies). Assert no opposite-sign step and a
    // cadence-bounded single-pose step (aliasing fast authored segments is
    // what made single frames look like abrupt head/body pops).
    auto run_dense = [&](const char* tag) -> std::string {
        oa::emote::EmotePlayer pl;
        if (!pl.load(*bytes, 1920, 1620, &err)) return std::string("loadfail");
        pl.set_external_pose(true);
        pl.set_scale(0.6, 0, 0);
        pl.set_coord(0, 3695.0 * 0.6 - 810.0 + 100.0);
        pl.fade_in_timeline("通常待機");
        auto bboxOf = [](const oa::emote::EmoteDrawPart& pr) {
            double mnX = 1e300, mxX = -1e300, mnY = 1e300, mxY = -1e300;
            for (const auto& v : pr.verts) {
                mnX = std::min(mnX, v.x); mxX = std::max(mxX, v.x);
                mnY = std::min(mnY, v.y); mxY = std::max(mxY, v.y);
            }
            return std::array<double, 4>{mnX, mnY, mxX, mxY};
        };
        auto iconName = [&](int src, int ic) -> std::string {
            if (src < 0 || src >= int(pl.file().sources.size())) return "";
            const auto& s0 = *pl.file().sources[size_t(src)];
            if (ic < 0 || ic >= int(s0.icons.size())) return "";
            return s0.icons[size_t(ic)].name;
        };
        std::vector<oa::emote::EmoteDrawPart> parts0;
        pl.collect_pose_parts(&parts0, &err);
        // landmark parts: face contour (icon "0024") and chest (icon "0027")
        int headKey[2] = {-1, -1};
        int chestKey[2] = {-1, -1};
        for (const auto& pr : parts0) {
            const std::string nm = iconName(pr.source, pr.icon);
            if (nm == "0024") { headKey[0] = pr.source; headKey[1] = pr.icon; }
            if (nm == "0027") { chestKey[0] = pr.source; chestKey[1] = pr.icon; }
        }
        auto landmark = [&](const std::vector<oa::emote::EmoteDrawPart>& parts,
                            const int key[2]) {
            for (const auto& pr : parts)
                if (pr.source == key[0] && pr.icon == key[1]) return bboxOf(pr);
            return std::array<double, 4>{0, 0, 0, 0};
        };
        const auto h0 = landmark(parts0, headKey);
        const auto c0 = landmark(parts0, chestKey);
        if (headKey[0] < 0 || chestKey[0] < 0 || h0[3] <= 0 || c0[3] <= 0)
            return std::string("landmark-not-found");
        unsigned seed = 424242u;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return double(seed % 1000) / 1000.0; };
        static const char* const faces[] = {"笑顔_ボイス再生用", "通常_ボイス再生用",
                                            "怒り_ボイス再生用", "悲しみ_ボイス再生用",
                                            "照れ_ボイス再生用"};
        uint64_t opposite = 0;
        double maxFaceStep = 0, maxChestStep = 0;
        uint64_t totalPoses = 0;
        uint64_t lastRev = pl.revision();
        double prevFace = (h0[1] + h0[3]) / 2;
        double prevChest = (c0[1] + c0[3]) / 2;
        int snapUntil = 0;
        for (int frame = 0; frame < 24 * 60; ++frame) { // ~24 s virtual
            if (frame % (240 + int(rnd() * 300)) == 0) {
                pl.pass();
                pl.play_timeline(faces[size_t(rnd() * 5) % 5]);
                pl.fade_in_timeline("通常待機");
                // expression lines snap to their authored t0 pose and carry
                // authored entrance spikes over the first ~15-20 frames
                // (also in the reference implementations) — skip that
                // window when measuring steady-state coherence
                snapUntil = frame + 24;
            }
            if (frame % 60 == 0 && rnd() < 0.35) {
                pl.set_variable("face_talk", double(int(rnd() * 4)));
                snapUntil = frame + 24;
            }
            if (frame % 240 == 0 && rnd() < 0.2) {
                if (rnd() < 0.5) pl.step(); else pl.skip();
                snapUntil = frame + 24;
            }
            pl.advance_ms(16);
            if (pl.revision() != lastRev) {
                lastRev = pl.revision();
                std::vector<oa::emote::EmoteDrawPart> parts;
                pl.collect_pose_parts(&parts, &err);
                const auto hb = landmark(parts, headKey);
                const auto cb = landmark(parts, chestKey);
                if (hb[3] <= 0 || cb[3] <= 0) continue;
                const double face = (hb[1] + hb[3]) / 2;
                const double chest = (cb[1] + cb[3]) / 2;
                const double dFace = face - prevFace;
                const double dChest = chest - prevChest;
                prevFace = face;
                prevChest = chest;
                if (frame < snapUntil) continue;
                ++totalPoses;
                maxFaceStep = std::max(maxFaceStep, std::fabs(dFace));
                maxChestStep = std::max(maxChestStep, std::fabs(dChest));
                if (dFace > 2.0 && dChest < -2.0) {
                    ++opposite;
                    std::printf("  [%s] opposite frame=%d dFace=%.2f dChest=%.2f "
                                "fg=%s idle=%s\n", tag, frame, dFace, dChest,
                                pl.foreground_timeline().c_str(), pl.idle_timeline().c_str());
                }
                if (dFace < -2.0 && dChest > 2.0) {
                    ++opposite;
                    std::printf("  [%s] opposite frame=%d dFace=%.2f dChest=%.2f "
                                "fg=%s idle=%s\n", tag, frame, dFace, dChest,
                                pl.foreground_timeline().c_str(), pl.idle_timeline().c_str());
                }
            }
        }
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "%s: poses=%llu opposite=%llu maxFaceStep=%.2f maxChestStep=%.2f",
                      tag, (unsigned long long)totalPoses, (unsigned long long)opposite,
                      maxFaceStep, maxChestStep);
        return std::string(buf);
    };
    unset_env("OA_EMOTE_FPS");
    const std::string r30 = run_dense("30fps");
    set_env("OA_EMOTE_FPS", "8");
    const std::string r8 = run_dense("8fps");
    unset_env("OA_EMOTE_FPS");
    std::printf("dense long-run: %s\n", r30.c_str());
    std::printf("dense long-run: %s\n", r8.c_str());
    // opposite-sign face/chest steps also occur in idle sway (authored: the
    // head and chest curves cross in opposite directions) — pose=f(vars) is
    // reference-identical, so no decoupling is possible; what the cadence
    // controls is the single-pose step size (aliasing of authored fast
    // segments). E9 (research/55): with the slant axes correctly interpolated
    // and pivoted (stair-step full-extent holds removed), single-pose steps
    // are small at BOTH cadences — the old ordering contrast (30fps < 8fps)
    // only existed because 8 fps poses landed on hard-held slant extremes
    // (60 px steps); those extremes are gone (measured 6.5 px @30fps /
    // 2.7 px @8fps), so the ordering is no longer a discriminating property.
    // Assert the absolute single-pose bound at BOTH cadences (8 fps was
    // previously unbounded) plus the no-opposite-sign result.
    auto parseNum = [](const std::string& r, const char* key) {
        const auto p = r.find(key);
        if (p == std::string::npos) return 1e9;
        return std::atof(r.c_str() + p + std::strlen(key));
    };
    const double m30 = parseNum(r30, "maxFaceStep=");
    const double m8 = parseNum(r8, "maxFaceStep=");
    check(m30 <= 30.0, "30 fps dense run: single-pose face steps bounded");
    check(parseNum(r30, "maxChestStep=") <= 30.0,
          "30 fps dense run: single-pose chest steps bounded");
    check(m8 <= 30.0, "8 fps dense run: single-pose face steps bounded (E9)");
    check(parseNum(r8, "maxChestStep=") <= 30.0,
          "8 fps dense run: single-pose chest steps bounded (E9)");
    check(r30.find("opposite=0") != std::string::npos &&
              r8.find("opposite=0") != std::string::npos,
          "no opposite-sign head/chest steps at either cadence");
    check(r30.find("poses=") != std::string::npos && m30 < 1e9,
          "dense long-run produced poses");

    // --- P4U3 sharpness regression (research/48) ----------------------------
    // Full-resolution renders must carry visibly more detail than the
    // half-size fallback at the same pose/geometry (the layer quad stretches
    // a half buffer 2x, which blurred the user-visible figure). Both players
    // request the same 1920x1620 surface; the second one renders at half
    // resolution via the OA_EMOTE_HALFRES fallback.
    p.set_scale(0.6, 0, 0);
    p.set_coord(0, 3695.0 * 0.6 - 810.0 + 100.0);
    p.set_variable("face_talk", 0);
    p.set_variable("face_eye_open", 23.0);
    p.advance_ms(500);
    const std::vector<uint8_t> full = p.rgba();
    set_env("OA_EMOTE_HALFRES", "1");
    oa::emote::EmotePlayer ph;
    if (ph.load(*bytes, 1920, 1620, &err)) {
        ph.set_scale(0.6, 0, 0);
        ph.set_coord(0, 3695.0 * 0.6 - 810.0 + 100.0);
        ph.set_variable("face_talk", 0);
        ph.set_variable("face_eye_open", 23.0);
        ph.advance_ms(500);
        const std::vector<uint8_t> half = ph.rgba();
        // resolution difference metric: count pixels whose colour differs by
        // >24 between the full render and the nearest-neighbour-upsampled
        // half render inside the figure band (rows 60..600 = head/face/torso)
        uint64_t sharp = 0;
        uint64_t edgeE = 0, edgeH = 0;
        for (int y = 60; y < 600 && y < int(half.size()) / (1920 * 4); ++y) {
            for (int x = 250; x < 1600; ++x) {
                const uint8_t* pf = &full[(size_t(y) * 1920 + x) * 4];
                const uint8_t* phh = &half[(size_t(y / 2) * 960 + x / 2) * 4];
                if (std::abs(int(pf[0]) - int(phh[0])) > 24 ||
                    std::abs(int(pf[1]) - int(phh[1])) > 24 ||
                    std::abs(int(pf[2]) - int(phh[2])) > 24)
                    ++sharp;
                if (pf[3] > 64) {
                    if (x + 1 < 1920) {
                        const uint8_t* pn = &full[(size_t(y) * 1920 + x + 1) * 4];
                        edgeE += uint64_t(std::abs(int(pf[0]) - int(pn[0])) +
                                          std::abs(int(pf[1]) - int(pn[1])) +
                                          std::abs(int(pf[2]) - int(pn[2])));
                    }
                    const uint8_t* phc = &half[(size_t(y / 2) * 960 + x / 2) * 4];
                    if (x / 2 + 1 < 960) {
                        const uint8_t* phn = &half[(size_t(y / 2) * 960 + x / 2 + 1) * 4];
                        edgeH += uint64_t(std::abs(int(phc[0]) - int(phn[0])) +
                                          std::abs(int(phc[1]) - int(phn[1])) +
                                          std::abs(int(phc[2]) - int(phn[2])));
                    }
                }
            }
        }
        std::printf("sharpness: full-vs-half differ on %llu px; edge energy "
                    "full=%llu half=%llu\n",
                    (unsigned long long)sharp, (unsigned long long)edgeE,
                    (unsigned long long)edgeH);
        checkf(sharp > 20000, "full-res pose differs from half-res pose",
               (unsigned long long)sharp);
        checkf(edgeE > edgeH * 12 / 10,
               "full-res figure edges are sharper than half-res",
               (unsigned long long)edgeE);
    } else {
        check(false, "half-res player load");
    }
    unset_env("OA_EMOTE_HALFRES");

    // --- S2 (research/124 §2): write surface + numeric robustness -----------
    // Same-process REF/NEW: the REF player latches the pre-S2 write surface
    // (OA_EMOTE_S2_LEGACY=1) at construction, the NEW player the aligned one.
    // Every NekoMiko expression is driven through both and the composed
    // variable snapshot is compared frame by frame — the S2 census says the
    // difference must be ZERO (G4: 0 predicate-changing key pairs in all six
    // scanned files; G6: the 5 selector-option tracks each NekoMiko timeline
    // carries are re-derived by the selector control every frame either way).
    {
        std::printf("S2 policy defaults: angleWrap=%d tailFallback=%d paramOobSkip=%d "
                    "nonfiniteGuard=%d\n",
                    int(oa::emote::emote_s2_policy().angleWrap),
                    int(oa::emote::emote_s2_policy().tailFallback),
                    int(oa::emote::emote_s2_policy().paramOobSkip),
                    int(oa::emote::emote_s2_policy().nonfiniteGuard));
        check(oa::emote::emote_s2_policy().angleWrap,
              "S2 default: G5 angle wrap is ON (reference rule)");
        check(!oa::emote::emote_s2_policy().tailFallback,
              "S2 default: G8b tail fallback is OFF (OA_EMOTE_TAILFALLBACK=1)");
        check(!oa::emote::emote_s2_policy().paramOobSkip,
              "S2 default: G8a param-OOB skip is OFF (OA_EMOTE_PARAMOOB=1)");
        check(oa::emote::emote_s2_policy().nonfiniteGuard,
              "S2 default: G8c non-finite guard is ON");
        check(oa::emote::emote_mesh_div_policy().mode ==
                  oa::emote::kEmoteMeshDivNodeAdaptive,
              "S5 default: mesh subdivision is the node-adaptive hybrid");

        oa::emote::EmoteFile s2file;
        const bool s2fileOk = s2file.load(*bytes, &err);
        check(s2fileOk, "S2: emote file loads");

        set_env("OA_EMOTE_S2_LEGACY", "1");
        oa::emote::EmotePlayer ref;
        const bool refOk = ref.load(*bytes, 960, 810, &err);
        unset_env("OA_EMOTE_S2_LEGACY");
        oa::emote::EmotePlayer nw;
        const bool newOk = nw.load(*bytes, 960, 810, &err);
        check(refOk && newOk, "S2 REF/NEW players load");
        if (refOk && newOk) {
            const std::set<std::string> known = nw.known_labels();
            checkf(known.size() > 40, "S2 G6: known-label universe is populated",
                   (unsigned long long)known.size());
            auto setup = [](oa::emote::EmotePlayer& pl) {
                pl.set_scale(0.6, 0, 0);
                pl.set_coord(0, 3695.0 * 0.6 - 810.0 / 2.0 + 100.0);
                pl.set_external_pose(true); // pose path only: no CPU raster cost
            };
            setup(ref);
            setup(nw);
            static const char* const kTls[] = {
                "通常待機", "通常",          "笑顔",     "照れ",           "悲しみ",
                "怒り",     "呆れ",          "驚き",     "笑顔_ボイス再生用",
                "照れ_ボイス再生用"};
            size_t diffFrames = 0, unknownLabels = 0, frames = 0;
            std::string firstDiffTl, firstDiffLabel;
            for (const char* tl : kTls) {
                ref.play_timeline(tl, 0);
                nw.play_timeline(tl, 0);
                for (int f = 0; f < 90; ++f) {
                    ref.progress(1.0);
                    nw.progress(1.0);
                    const auto a = ref.variables();
                    const auto b = nw.variables();
                    ++frames;
                    bool frameDiff = a.size() != b.size();
                    for (const auto& kv : b) {
                        const auto it = a.find(kv.first);
                        if (it == a.end() || std::fabs(it->second - kv.second) > 1e-9) {
                            frameDiff = true;
                            if (firstDiffLabel.empty()) firstDiffLabel = kv.first;
                        }
                        if (known.find(kv.first) == known.end()) {
                            ++unknownLabels;
                            if (firstDiffLabel.empty())
                                firstDiffLabel = "unknown:" + kv.first;
                        }
                    }
                    if (frameDiff) {
                        ++diffFrames;
                        if (firstDiffTl.empty()) firstDiffTl = tl;
                    }
                }
            }
            std::printf("S2 REF/NEW (NekoMiko): timelines=%zu frames=%zu differingFrames=%zu "
                        "unknownLabels=%zu firstDiff=%s/%s\n",
                        sizeof(kTls) / sizeof(kTls[0]), frames, diffFrames, unknownLabels,
                        firstDiffTl.c_str(), firstDiffLabel.c_str());
            checku(diffFrames == 0,
                   "S2 REF/NEW: pre-S2 and aligned write surfaces compose identically "
                   "(G4 lazy, G6 observationally equivalent)",
                   (unsigned long long)diffFrames);
            checku(unknownLabels == 0,
                   "S2 G6: no composed label falls outside known_labels()",
                   (unsigned long long)unknownLabels);
            const auto rs = ref.s2_stats();
            const auto ns = nw.s2_stats();
            std::printf("S2 G6 counters: ref selSkipped=%zu unknown=%zu | new selSkipped=%zu "
                        "unknown=%zu\n", rs.selectorTracksSkipped, rs.unknownLabelWrites,
                        ns.selectorTracksSkipped, ns.unknownLabelWrites);
            checku(rs.selectorTracksSkipped == 0,
                   "S2 G6: the legacy arm keeps writing selector option-name tracks",
                   (unsigned long long)rs.selectorTracksSkipped);
            checkf(ns.selectorTracksSkipped > 0,
                   "S2 G6: the aligned arm skips real selector option-name tracks",
                   (unsigned long long)ns.selectorTracksSkipped);
            checku(ns.unknownLabelWrites == 0,
                   "S2 G6: no real track writes an unknown label (data-lazy)",
                   (unsigned long long)ns.unknownLabelWrites);
        }

        // --- G8 numeric robustness: inert in-range, reachable out-of-range ---
        oa::emote::StaticRenderOptions opt;
        opt.fitToCanvas = false;
        opt.scale = 0.6;
        opt.dx = 480.0;
        opt.dy = 405.0;
        auto collect_hash = [&](const std::map<std::string, double>& vars) {
            if (!s2fileOk) return std::string("ERR");
            std::vector<oa::emote::EmoteDrawPart> parts;
            if (!oa::emote::emote_collect_parts(s2file, vars, 960, 810, opt, &parts,
                                                       &err))
                return std::string("ERR");
            unsigned long long h = 1469598103934665603ULL;
            for (const auto& pt : parts) {
                h ^= (unsigned long long)pt.source * 1315423911u +
                     (unsigned long long)pt.icon;
                h *= 1099511628211ULL;
                for (const auto& v : pt.verts) {
                    h ^= (unsigned long long)std::llround(v.x * 4096.0);
                    h *= 1099511628211ULL;
                    h ^= (unsigned long long)std::llround(v.y * 4096.0);
                    h *= 1099511628211ULL;
                }
            }
            char b[40];
            std::snprintf(b, sizeof(b), "%016llx/%zu", h, parts.size());
            return std::string(b);
        };
        const std::vector<std::map<std::string, double>> inrange = {
            {},          {{"head_slant", 20}}, {{"body_UD", -20}}, {{"face_talk", 3}},
            {{"head_UD", -20}}, {{"body_LR", 20}}, {{"arm_type", 1}}};
        size_t tailDiff = 0, oobDiff = 0, nfDiff = 0;
        for (const auto& vars : inrange) {
            oa::emote::emote_set_s2_policy(-1, 0, -1, -1);
            const std::string tailOff = collect_hash(vars);
            oa::emote::emote_set_s2_policy(-1, 1, -1, -1);
            const std::string tailOn = collect_hash(vars);
            if (tailOff != tailOn) ++tailDiff;
            oa::emote::emote_set_s2_policy(-1, -1, 0, -1);
            const std::string oob0 = collect_hash(vars);
            oa::emote::emote_set_s2_policy(-1, -1, 1, -1);
            const std::string oob1 = collect_hash(vars);
            if (oob0 != oob1) ++oobDiff;
            oa::emote::emote_set_s2_policy(-1, -1, -1, 1);
            const std::string nf1 = collect_hash(vars);
            oa::emote::emote_set_s2_policy(-1, -1, -1, 0);
            const std::string nf0 = collect_hash(vars);
            if (nf1 != nf0) ++nfDiff;
            oa::emote::emote_set_s2_policy(-1, -1, -1, -1);
        }
        std::printf("S2 G8 inertness over %zu in-range poses: tailFallback=%zu paramOob=%zu "
                    "nonfinite=%zu differing\n", inrange.size(), tailDiff, oobDiff, nfDiff);
        checku(tailDiff == 0,
               "S2 G8b: the tail fallback changes nothing in range (data-lazy)",
               (unsigned long long)tailDiff);
        checku(oobDiff == 0, "S2 G8a: the param-OOB skip changes nothing (0/1592 nodes)",
               (unsigned long long)oobDiff);
        checku(nfDiff == 0, "S2 G8c: the non-finite guard is inert (0 non-finite frames)",
               (unsigned long long)nfDiff);
        // The mechanism itself must stay reachable: an axis value far outside
        // the authored range drives the parameter tick past the content-less
        // tail frames the fallback exists for.
        size_t liveAxes = 0;
        for (const char* axis : {"head_UD", "body_UD", "head_slant", "body_slant",
                                 "face_talk", "head_LR"}) {
            std::map<std::string, double> vars{{axis, 500.0}};
            oa::emote::emote_set_s2_policy(-1, 0, -1, -1);
            const std::string off = collect_hash(vars);
            oa::emote::emote_set_s2_policy(-1, 1, -1, -1);
            const std::string on = collect_hash(vars);
            oa::emote::emote_set_s2_policy(-1, -1, -1, -1);
            if (off != on) ++liveAxes;
        }
        std::printf("S2 G8b mechanism: %zu/6 out-of-range axes change geometry\n", liveAxes);
        checkf(liveAxes > 0, "S2 G8b: the tail fallback fires on out-of-range ticks",
               (unsigned long long)liveAxes);

        // G5 counter-guard on NekoMiko: its data has no >180 angle key, so the
        // wrap must be a no-op here (the tg3/slny arm lives in emote_timeline).
        size_t wrapDiff = 0;
        for (const char* axis : {"head_slant", "body_slant", "head_UD", "body_UD"}) {
            for (double v : {-30.0, -20.0, -15.0, -7.0, 0.0, 7.0, 15.0, 20.0, 30.0}) {
                std::map<std::string, double> vars{{axis, v}};
                oa::emote::emote_set_s2_policy(1, -1, -1, -1);
                const std::string on = collect_hash(vars);
                oa::emote::emote_set_s2_policy(0, -1, -1, -1);
                const std::string off = collect_hash(vars);
                oa::emote::emote_set_s2_policy(-1, -1, -1, -1);
                if (on != off) ++wrapDiff;
            }
        }
        std::printf("S2 G5 counter-guard (NekoMiko, no >180 key): %zu differing poses\n",
                    wrapDiff);
        checku(wrapDiff == 0,
               "S2 G5: the angle wrap is a no-op on NekoMiko (no >180 angle keys)",
               (unsigned long long)wrapDiff);
    }

    std::printf("%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
