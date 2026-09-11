// S3 G1/G2 direct regression (research/122; S1 census research/121 §5 S3).
//
// Drives EmotePlayer straight on the two new-generation archives whose
// gesture tracks exposed the P0 finding: 甜蜜女友3 (tg3) and きら☆かの (slny).
// Both mark their gesture/action timelines `diff=1` with the "no loop"
// sentinel loop=[-1,-1]; the pre-S3 engine approximated "diff==1 => loop"
// with a fallback period of lo+1, so those tracks sat at t = fmod(t, 1) == 0
// and the whole gesture class was FROZEN (441 sampled timelines in the S1
// census). The reference rule (E:emoterunner.cpp:1411-1417) wraps only when
// the authored loopEnd > 0, which makes those timelines ordinary one-shots.
//
// Assertions (per archive arm):
//   G1a broad classification scan over every timeline of the file:
//       -- gesture (diff==1 && loopEnd<=0 && >=2 content keys) => the
//          per-frame variable signature must NOT be constant;
//       -- static  (<=1 content key per track) => the signature MUST stay
//          constant (counter-guard: a 1-frame instant expression is not an
//          animation and must not be turned into one);
//   G1b named gesture timelines: the slot clock t advances by exactly one
//       authored frame per progress(1), is monotone, covers maxContentTime,
//       never runs past the authored tail, and the timeline auto-ends at the
//       tail LEAVING its last authored sample in the domain (no return to t0);
//   G1c loop boundary (待機 loop=[0,290]): the loopEnd frame itself is
//       reachable and the clock then wraps by period = loopEnd - loopBegin;
//   G2  ordered foreground play list: playTimeline(label, 1) appends,
//       playTimeline(label) replaces, compose is list-order (later wins),
//       every entry keeps its own clock, pass() ends the whole list,
//       stopTimeline(name) ends one entry, step() folds every entry's end
//       pose, the list is capped at 8;
//   REF/NEW same-process A-B (OA_EMOTE_LEGACY_SLOTS=1 = pre-S3 model):
//       gesture tracks flip constant -> moving, the differing variable set
//       stays inside the timeline's own tracks, and the loop-boundary frame
//       is only reachable in the NEW model.
//
// Env: OA_TEST_TG3_PFS (甜蜜女友3 root.pfs) / OA_TEST_SLNY_PFS (きら☆かの
// root.pfs) — arms run when set, exit 77 when neither is (the FPM/NekoMiko
// asset-gate convention).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/emote/emote_player.h"
#include "core/fs/physfs_fs.h"

namespace {

int failures = 0;
// Windows port (research/128): setenv/unsetenv are POSIX-only. MSVC spells the
// same two operations _putenv_s(name, value) and _putenv_s(name, "") (an empty
// value REMOVES the variable) — the REF/NEW switches below are read through
// std::getenv when the EmotePlayer is CONSTRUCTED. Same inline convention as
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
void checkf(bool cond, const char* what, double got) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s (got %.3f)\n", what, got);
        ++failures;
    }
}
void checku(bool cond, const char* what, unsigned long long got) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s (got %llu)\n", what, got);
        ++failures;
    }
}

constexpr double kEps = 1e-9;

// FNV-1a over the composed variable snapshot (values quantised to 1e-3, the
// pose-relevant precision used by the S1 freeze probe / research/121 §7.3).
std::string var_sig(const std::map<std::string, double>& v) {
    unsigned long long h = 1469598103934665603ULL;
    for (const auto& [k, val] : v) {
        const long long q = (long long)std::llround(val * 1000.0);
        for (char c : k) {
            h ^= (unsigned char)c;
            h *= 1099511628211ULL;
        }
        h ^= (unsigned long long)q;
        h *= 1099511628211ULL;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", h);
    return buf;
}

struct Shape {
    size_t tracks = 0, nonEmpty = 0, multi = 0, single = 0;
    double maxContent = 0, tail = 0;
    std::string widest; // track carrying maxContent
    double end = 0;     // authored end the player deactivates at
};
Shape shape_of(const oa::emote::EmoteTimeline& tl) {
    Shape s;
    s.tracks = tl.variables.size();
    for (const auto& tv : tl.variables) {
        if (tv.frames.empty()) continue;
        ++s.nonEmpty;
        double mt = 0;
        int nc = 0;
        for (const auto& f : tv.frames) {
            if (f.hasContent) {
                ++nc;
                if (f.time > mt) mt = f.time;
            }
        }
        if (nc > 1) ++s.multi;
        else if (nc == 1) ++s.single;
        if (mt > s.maxContent) {
            s.maxContent = mt;
            s.widest = tv.label;
        }
        if (tv.frames.back().time > s.tail) s.tail = tv.frames.back().time;
    }
    s.end = tl.lastTime > 0 ? double(tl.lastTime) : s.tail;
    return s;
}

const oa::emote::EmoteTimeline* find_tl(
    const oa::emote::EmoteFile& f, const std::string& label) {
    for (const auto& t : f.timelines)
        if (t.label == label) return &t;
    return nullptr;
}

const oa::emote::TimeVar* find_track(
    const oa::emote::EmoteTimeline& tl, const std::string& label) {
    for (const auto& tv : tl.variables)
        if (tv.label == label) return &tv;
    return nullptr;
}

std::set<std::string> labels_of(const oa::emote::EmoteTimeline& tl) {
    std::set<std::string> out;
    for (const auto& tv : tl.variables)
        if (!tv.frames.empty()) out.insert(tv.label);
    return out;
}

double first_content_value(const oa::emote::TimeVar& tv, double* timeOut = nullptr) {
    for (const auto& f : tv.frames) {
        if (!f.hasContent) continue;
        if (timeOut) *timeOut = f.time;
        return f.value;
    }
    return 0.0;
}

double last_content_value(const oa::emote::TimeVar& tv, double* timeOut = nullptr) {
    for (auto it = tv.frames.rbegin(); it != tv.frames.rend(); ++it) {
        if (!it->hasContent) continue;
        if (timeOut) *timeOut = it->time;
        return it->value;
    }
    return 0.0;
}

// One timeline playthrough: signature + clock trace + per-label distinct values.
struct Run {
    int frames = 0;
    size_t uniqueSigs = 0; // distinct per-frame signatures
    double firstT = -1;    // slot clock right after play (start phase)
    double maxT = -1;      // largest clock seen
    double endT = -1;      // clock at which the entry left the list
    bool ended = false;
    bool monotone = true;  // +1 exactly per progress(1), never decreasing
    int endFrame = -1;     // frame index at which the entry vanished
    std::string firstSig, endSig, lastSig;
    bool sigStableAfterEnd = true;
    std::set<std::string> moved;                      // labels that ever changed
    std::map<std::string, std::set<long long>> dist;  // per-label distinct values
    std::vector<double> ts;                           // active clock per frame
    std::vector<std::map<std::string, double>> snaps; // per-frame (opt-in)
};

Run run_timeline(oa::emote::EmotePlayer& p, const std::string& label,
                 int frames, int flags = 0, bool keepSnaps = false) {
    Run r;
    r.frames = frames;
    p.play_timeline(label, flags);
    {
        const auto slots = p.foreground_slots();
        if (!slots.empty()) r.firstT = slots.back().t;
    }
    std::set<std::string> sigs;
    double prevT = r.firstT >= 0 ? r.firstT : 0.0;
    for (int f = 0; f < frames; ++f) {
        p.progress(1.0);
        const auto v = p.variables();
        const std::string s = var_sig(v);
        sigs.insert(s);
        if (f == 0) r.firstSig = s;
        if (r.endFrame >= 0 && s != r.endSig) r.sigStableAfterEnd = false;
        r.lastSig = s;
        for (const auto& [k, val] : v) {
            const long long q = (long long)std::llround(val * 1000.0);
            auto& set = r.dist[k];
            const size_t before = set.size();
            set.insert(q);
            if (set.size() != before) r.moved.insert(k);
        }
        if (keepSnaps) r.snaps.push_back(v);
        const auto slots = p.foreground_slots();
        if (!slots.empty()) {
            const double t = slots.back().t;
            if (std::fabs(t - (prevT + 1.0)) > kEps) r.monotone = false;
            prevT = t;
            if (t > r.maxT) r.maxT = t;
            r.ts.push_back(t);
        } else if (!r.ended) {
            // progress() adds exactly one authored frame, so the clock at
            // which the entry left the list is the previous clock + 1
            r.endFrame = f;
            r.ended = true;
            r.endT = prevT + 1.0;
            prevT = r.endT;
            r.endSig = s;
        }
    }
    r.uniqueSigs = sigs.size();
    return r;
}

// ---------------------------------------------------------------------------
// Per-arm data: the timelines this test names explicitly (labels verified
// present by the S1 census and the S3 prep scan; a missing label fails).
// ---------------------------------------------------------------------------
struct Arm {
    const char* tag;
    const char* env;
    const char* entry;
    std::vector<std::string> gestures; // multi-content-key diff1 loopEnd<=0
    std::vector<std::string> statics;  // single-content-key expression tracks
};

void run_arm(const Arm& arm) {
    const char* pfs = std::getenv(arm.env);
    if (!pfs || !*pfs) {
        std::printf("[%s] %s unset; arm skipped\n", arm.tag, arm.env);
        return;
    }
    oa::fs::PhysFileSystem fs(pfs, false);
    auto bytes = fs.read(arm.entry);
    if (!bytes) {
        std::fprintf(stderr, "FAIL: [%s] read %s failed\n", arm.tag, arm.entry);
        ++failures;
        return;
    }
    oa::emote::EmotePlayer p;
    std::string err;
    if (!p.load(*bytes, 1920, 2048, &err)) {
        std::fprintf(stderr, "FAIL: [%s] load: %s\n", arm.tag, err.c_str());
        ++failures;
        return;
    }
    // external pose: this test asserts the variable domain / timeline clocks,
    // not pixels — no CPU raster work per frame.
    p.set_external_pose(true);
    const auto& file = p.file();
    std::printf("[%s] %s: timelines=%zu\n", arm.tag, arm.entry, file.timelines.size());

    // --- G1a: classification scan over every timeline ----------------------
    size_t gestures = 0, statics = 0, frozenGestures = 0, movedStatics = 0;
    std::vector<std::string> frozenList, movedStaticList;
    for (const auto& tl : file.timelines) {
        const Shape sh = shape_of(tl);
        if (tl.diff == 1 && tl.loopEnd <= 0 && sh.multi > 0) {
            ++gestures;
            const int frames = int(sh.end) + 40;
            const Run r = run_timeline(p, tl.label, frames);
            if (r.uniqueSigs < 2) {
                ++frozenGestures;
                frozenList.push_back(tl.label);
            }
        } else if (sh.multi == 0 && sh.single > 0) {
            ++statics;
            const int frames = std::min(60, int(sh.end) + 20);
            const Run r = run_timeline(p, tl.label, frames);
            if (r.uniqueSigs > 1) {
                ++movedStatics;
                movedStaticList.push_back(tl.label);
            }
        }
    }
    std::printf("[%s] G1a scan: gesture(diff1,loopEnd<=0,multi>0)=%zu frozen=%zu | "
                "static(singleContentKey)=%zu moved=%zu\n",
                arm.tag, gestures, frozenGestures, statics, movedStatics);
    if (!frozenList.empty()) {
        std::printf("[%s]   FROZEN gestures:", arm.tag);
        for (const auto& s : frozenList) std::printf(" %s", s.c_str());
        std::printf("\n");
    }
    if (!movedStaticList.empty()) {
        std::printf("[%s]   MOVED statics:", arm.tag);
        for (const auto& s : movedStaticList) std::printf(" %s", s.c_str());
        std::printf("\n");
    }
    checku(gestures >= 10, "G1a: enough gesture timelines to be non-vacuous",
           (unsigned long long)gestures);
    checku(statics >= 10, "G1a: enough static expression timelines (counter-guard)",
           (unsigned long long)statics);
    checku(frozenGestures == 0, "G1a: no gesture timeline is frozen at t==0",
           (unsigned long long)frozenGestures);
    checku(movedStatics == 0,
           "G1a counter-guard: 1-frame expression timelines stay constant",
           (unsigned long long)movedStatics);

    // --- G1a': named counter-guard (the S1 census 'correct constant' set) --
    for (const std::string& label : arm.statics) {
        const auto* tl = find_tl(file, label);
        if (!tl) {
            std::fprintf(stderr, "FAIL: [%s] static guard '%s' not in %s\n", arm.tag,
                         label.c_str(), arm.entry);
            ++failures;
            continue;
        }
        const Shape sh = shape_of(*tl);
        const Run r = run_timeline(p, label, std::min(80, int(sh.end) + 30));
        std::printf("[%s] guard '%s': diff=%d loop=[%d,%d] single=%zu multi=%zu "
                    "tail=%.0f | sigs=%zu first=%s\n",
                    arm.tag, label.c_str(), tl->diff, tl->loopBegin, tl->loopEnd,
                    sh.single, sh.multi, sh.tail, r.uniqueSigs, r.firstSig.c_str());
        checku(sh.multi == 0, "guard: the expression timeline is 1-key per track",
               (unsigned long long)sh.multi);
        checku(r.uniqueSigs == 1,
               "guard counter-example: the instant expression pose is constant",
               (unsigned long long)r.uniqueSigs);
    }

    // --- G1b: named gesture timelines, clock + tail + keep-end ------------
    for (const std::string& label : arm.gestures) {
        const auto* tl = find_tl(file, label);
        if (!tl) {
            std::fprintf(stderr, "FAIL: [%s] gesture '%s' not in %s\n", arm.tag,
                         label.c_str(), arm.entry);
            ++failures;
            continue;
        }
        const Shape sh = shape_of(*tl);
        const int frames = int(sh.end) + 30;
        const Run r = run_timeline(p, label, frames);
        std::printf("[%s] G1b '%s': diff=%d loop=[%d,%d] nonEmpty=%zu multi=%zu "
                    "maxContent=%.0f tail=%.0f | t0=%.0f tmax=%.0f endT=%.0f "
                    "ended=%d monotone=%d sigs=%zu (first=%s end=%s)\n",
                    arm.tag, label.c_str(), tl->diff, tl->loopBegin, tl->loopEnd,
                    sh.nonEmpty, sh.multi, sh.maxContent, sh.tail, r.firstT, r.maxT,
                    r.endT, r.ended ? 1 : 0, r.monotone ? 1 : 0, r.uniqueSigs,
                    r.firstSig.c_str(), r.endSig.c_str());
        checku(r.uniqueSigs >= 2, "G1b: gesture variable signature is not constant",
               (unsigned long long)r.uniqueSigs);
        check(r.monotone,
              "G1b: slot clock advances exactly one authored frame per progress(1)");
        check(r.maxT <= sh.end + kEps, "G1b: slot clock never runs past the authored tail");
        checkf(r.endT >= sh.maxContent, "G1b: playback covers maxContentTime", r.endT);
        check(std::fabs(r.endT - sh.end) < kEps,
              "G1b: one-shot ends at its authored tail (auto-end kept)");
        check(r.sigStableAfterEnd,
              "G1b: the ended timeline leaves a stable pose (no return to t0)");
        // The track that visibly moves is the one with the largest first->last
        // content-value excursion (the "widest" track by time may hold a flat
        // value while a shorter track does the gesture).
        const oa::emote::TimeVar* mover = nullptr;
        double moveDelta = 0;
        for (const auto& tv : tl->variables) {
            if (tv.frames.empty()) continue;
            int nc = 0;
            for (const auto& f : tv.frames)
                if (f.hasContent) ++nc;
            if (nc < 2) continue;
            const double d = std::fabs(last_content_value(tv) - first_content_value(tv));
            if (d > moveDelta) {
                moveDelta = d;
                mover = &tv;
            }
        }
        if (mover) {
            double tEnd = 0;
            const double lastV = last_content_value(*mover, &tEnd);
            const double firstV = first_content_value(*mover);
            const double got = p.get_variable(mover->label);
            std::printf("[%s]   keep-end '%s': first=%.3f last=%.3f(at t=%.0f) "
                        "after-end=%.3f\n",
                        arm.tag, mover->label.c_str(), firstV, lastV, tEnd, got);
            checkf(moveDelta > 1e-6,
                   "G1b: the gesture has a track with a real value excursion", moveDelta);
            checkf(std::fabs(got - lastV) < 1e-9,
                   "G1b: the domain keeps the last authored sample (no t0 reset)", got);
            const auto it = r.dist.find(mover->label);
            checku(it != r.dist.end() && it->second.size() > 1,
                   "G1b: that track's composed value really moved frame by frame",
                   it == r.dist.end() ? 0ULL : (unsigned long long)it->second.size());
        } else {
            check(false, "G1b: gesture timeline has a multi-content track");
        }
    }

    // --- G1c: loop boundary reachability (待機 loop=[0,290]) ---------------
    {
        const std::string label = "待機";
        const auto* tl = find_tl(file, label);
        if (!tl) {
            std::fprintf(stderr, "FAIL: [%s] '%s' missing\n", arm.tag, label.c_str());
            ++failures;
        } else {
            const int loopEnd = tl->loopEnd;
            const int frames = loopEnd + 20;
            const Run r = run_timeline(p, label, frames);
            bool hitEnd = false;
            int wraps = 0;
            for (size_t i = 0; i < r.ts.size(); ++i) {
                if (std::fabs(r.ts[i] - double(loopEnd)) < kEps) hitEnd = true;
                if (i > 0 && r.ts[i] < r.ts[i - 1]) ++wraps;
            }
            double maxT = 0;
            for (double t : r.ts) maxT = std::max(maxT, t);
            std::printf("[%s] G1c '%s' loop=[%d,%d]: frames=%d maxT=%.0f "
                        "loopEndReached=%d wraps=%d sigs=%zu\n",
                        arm.tag, label.c_str(), tl->loopBegin, loopEnd, frames, maxT,
                        hitEnd ? 1 : 0, wraps, r.uniqueSigs);
            check(hitEnd, "G1c: the authored loopEnd frame is reachable");
            check(maxT <= double(loopEnd) + kEps,
                  "G1c: the loop clock never exceeds loopEnd");
            check(wraps >= 1, "G1c: the loop wraps (period = loopEnd - loopBegin)");
            checku(r.uniqueSigs >= 2, "G1c: the idle loop keeps animating",
                   (unsigned long long)r.uniqueSigs);
        }
    }

    // --- G2: ordered foreground list --------------------------------------
    {
        // Pick the gesture pair (A, B) with the largest A-only label set, so
        // "A keeps writing while B plays in parallel" is observable (two
        // gestures may otherwise write the same variables, e.g. tg3
        // うんうん/はい both write head_UD+head_slant only).
        const oa::emote::EmoteTimeline* tlA = nullptr;
        const oa::emote::EmoteTimeline* tlB = nullptr;
        std::vector<std::string> aOnly, shared, bestAOnly;
        for (const auto& cand : file.timelines) {
            const Shape cs = shape_of(cand);
            if (!(cand.diff == 1 && cand.loopEnd <= 0 && cs.multi > 0)) continue;
            for (const auto& other : file.timelines) {
                if (&other == &cand) continue;
                const Shape os = shape_of(other);
                if (!(other.diff == 1 && other.loopEnd <= 0 && os.multi > 0)) continue;
                std::vector<std::string> only, both;
                for (const auto& tv : cand.variables) {
                    if (tv.frames.empty()) continue;
                    (labels_of(other).count(tv.label) ? both : only).push_back(tv.label);
                }
                if (only.size() > bestAOnly.size()) {
                    bestAOnly = only;
                    aOnly = only;
                    shared = both;
                    tlA = &cand;
                    tlB = &other;
                }
            }
        }
        if (!tlA || !tlB) {
            check(false, "G2: parallel-test timelines exist");
        } else {
            const std::string A = tlA->label, B = tlB->label;
            const std::string probe = aOnly.empty() ? std::string() : aOnly.front();
            std::printf("[%s] G2 A='%s'(%zu labels) B='%s'(%zu labels) "
                        "aOnly=%zu shared=%zu probe='%s'\n",
                        arm.tag, A.c_str(), labels_of(*tlA).size(), B.c_str(),
                        labels_of(*tlB).size(), aOnly.size(), shared.size(),
                        probe.c_str());
            check(!probe.empty(), "G2: A has a label B does not write");

            oa::emote::EmotePlayer pp;
            std::string e2;
            if (!pp.load(*bytes, 1920, 2048, &e2)) {
                check(false, "G2: player load");
            } else {
                pp.set_external_pose(true);
                // (1) PARALLEL push: list = [A, B], both clocks independent
                pp.play_timeline(A, 0);
                pp.play_timeline(B, 1);
                auto slots = pp.foreground_slots();
                checku(slots.size() == 2, "G2: PARALLEL playTimeline appends (list size)",
                       (unsigned long long)slots.size());
                if (slots.size() == 2) {
                    check(slots[0].label == A && slots[1].label == B,
                          "G2: list order = play order (compose order)");
                    check(slots[1].flags == 1, "G2: PARALLEL flag recorded on the entry");
                }
                pp.progress(3.0);
                slots = pp.foreground_slots();
                check(slots.size() == 2 && std::fabs(slots[0].t - 3.0) < kEps &&
                          std::fabs(slots[1].t - 3.0) < kEps,
                      "G2: every entry keeps its own clock (no shared reset)");
                // A keeps writing while B plays in parallel: its value has to
                // keep up with an A-only player at the same clock. Shared
                // labels must follow B (list order = compose order: the later
                // entry overwrites).
                if (!probe.empty()) {
                    oa::emote::EmotePlayer pa, pb;
                    std::string e3, e5;
                    if (pa.load(*bytes, 1920, 2048, &e3) &&
                        pb.load(*bytes, 1920, 2048, &e5)) {
                        pa.set_external_pose(true);
                        pb.set_external_pose(true);
                        pa.play_timeline(A, 0);
                        pa.progress(3.0);
                        pb.play_timeline(B, 0);
                        pb.progress(3.0);
                        const double aOnlyV = pa.get_variable(probe);
                        const double parV = pp.get_variable(probe);
                        std::printf("[%s] G2 parallel '%s': A-only=%.3f parallel=%.3f "
                                    "(t=3)\n",
                                    arm.tag, probe.c_str(), aOnlyV, parV);
                        checkf(std::fabs(aOnlyV - parV) < 1e-9,
                               "G2: a PARALLEL entry keeps writing (A's label tracks A-only)",
                               parV);
                        size_t sharedChecked = 0, sharedDiscriminating = 0,
                               sharedMismatch = 0;
                        for (const std::string& k : shared) {
                            const double va = pa.get_variable(k);
                            const double vb = pb.get_variable(k);
                            const double vp = pp.get_variable(k);
                            ++sharedChecked;
                            if (std::fabs(va - vb) > 1e-9) ++sharedDiscriminating;
                            if (std::fabs(vb - vp) > 1e-9) ++sharedMismatch;
                        }
                        std::printf("[%s] G2 later-wins: shared=%zu discriminating=%zu "
                                    "mismatch=%zu\n",
                                    arm.tag, sharedChecked, sharedDiscriminating,
                                    sharedMismatch);
                        checku(sharedMismatch == 0,
                               "G2: later list entry wins for shared labels (compose order)",
                               (unsigned long long)sharedMismatch);
                        checku(sharedDiscriminating > 0,
                               "G2: shared labels are actually discriminating",
                               (unsigned long long)sharedDiscriminating);
                    } else {
                        check(false, "G2: A-only/B-only reference players load");
                    }
                }
                // (2) a plain playTimeline replaces the whole list
                pp.play_timeline(A, 0);
                slots = pp.foreground_slots();
                check(slots.size() == 1 && slots[0].label == A,
                      "G2: a plain playTimeline replaces the list (single entry)");
                // (3) list cap
                for (int i = 0; i < 12; ++i) pp.play_timeline(B, 1);
                slots = pp.foreground_slots();
                checku(slots.size() <= 8, "G2: foreground list is capped at 8",
                       (unsigned long long)slots.size());
                // (4) pass() ends every entry (pose kept)
                const auto before = pp.variables();
                pp.pass();
                check(pp.foreground_slots().empty(), "G2: pass() clears the whole list");
                check(pp.foreground_timeline().empty(),
                      "G2: no foreground timeline after pass");
                const auto after = pp.variables();
                bool same = before.size() == after.size();
                for (const auto& kv : before)
                    if (!after.count(kv.first) ||
                        std::fabs(after.at(kv.first) - kv.second) > kEps)
                        same = false;
                check(same, "G2: pass() keeps the composed pose (no snap back)");
                // (5) stop_timeline(name) removes only that entry
                pp.play_timeline(A, 0);
                pp.play_timeline(B, 1);
                const bool stopped = pp.stop_timeline(B);
                slots = pp.foreground_slots();
                check(stopped && slots.size() == 1 && slots[0].label == A,
                      "G2: stop_timeline(B) removes only B (A keeps playing)");
                check(pp.is_timeline_playing(A) && !pp.is_timeline_playing(B),
                      "G2: isTimelinePlaying is per-entry");
                // (6) step() folds EVERY entry's end pose into the domain
                pp.play_timeline(B, 1); // list = [A, B]
                pp.step();
                check(pp.foreground_slots().empty(), "G2: step() clears the list");
                if (!probe.empty()) {
                    if (const auto* tvA = find_track(*tlA, probe)) {
                        const double endA = last_content_value(*tvA);
                        const double gotA = pp.get_variable(probe);
                        std::printf("[%s] G2 step-fold '%s': A end=%.3f got=%.3f\n",
                                    arm.tag, probe.c_str(), endA, gotA);
                        checkf(std::fabs(gotA - endA) < 1e-9,
                               "G2: step() folds every entry's end pose (A's label)",
                               gotA);
                    }
                }
            }

            // (7) OA_EMOTE_PARALLEL=0 restores single-slot replacement: A's
            // labels stop being written once B is pushed (frozen at the push)
            if (!probe.empty()) {
                set_env("OA_EMOTE_PARALLEL", "0");
                oa::emote::EmotePlayer pseq;
                unset_env("OA_EMOTE_PARALLEL");
                std::string e4;
                if (pseq.load(*bytes, 1920, 2048, &e4)) {
                    pseq.set_external_pose(true);
                    pseq.play_timeline(A, 0);
                    pseq.progress(3.0);
                    const double frozenAt = pseq.get_variable(probe);
                    pseq.play_timeline(B, 1);
                    const auto slots = pseq.foreground_slots();
                    checku(slots.size() == 1,
                           "G2 A-B: OA_EMOTE_PARALLEL=0 restores replacement",
                           (unsigned long long)slots.size());
                    pseq.progress(20.0);
                    const double later = pseq.get_variable(probe);
                    std::printf("[%s] G2 A-B parallel=0 '%s': at-push=%.3f after-20f=%.3f\n",
                                arm.tag, probe.c_str(), frozenAt, later);
                    checkf(std::fabs(later - frozenAt) < 1e-9,
                           "G2 A-B: with PARALLEL=0 the replaced track freezes (old behaviour)",
                           later);
                } else {
                    check(false, "G2 A-B: player load");
                }
            }
        }
    }

    // --- REF/NEW same-process A-B (OA_EMOTE_LEGACY_SLOTS=1) ---------------
    // Both players are FRESH for every probed timeline: the persistent
    // variable domain would otherwise carry the residue of the previous run
    // (and the two arms play it out differently), which shows up as differing
    // labels the probed timeline never writes. The loop-boundary A-B runs
    // last on its own fresh pair.
    {
        // The switches are latched in the CONSTRUCTOR (same convention as
        // OA_EMOTE_FPS/OA_EMOTE_HALFRES), so the A-B players are built here.
        auto make_pair = [&](std::unique_ptr<oa::emote::EmotePlayer>* outRef,
                             std::unique_ptr<oa::emote::EmotePlayer>* outNew) {
            set_env("OA_EMOTE_LEGACY_SLOTS", "1");
            auto ref = std::make_unique<oa::emote::EmotePlayer>();
            std::string e1;
            const bool ok1 = ref->load(*bytes, 1920, 2048, &e1);
            unset_env("OA_EMOTE_LEGACY_SLOTS");
            auto neu = std::make_unique<oa::emote::EmotePlayer>();
            std::string e2;
            const bool ok2 = neu->load(*bytes, 1920, 2048, &e2);
            if (!ok1 || !ok2) check(false, "REF/NEW: A-B player load");
            if (ok1) ref->set_external_pose(true);
            if (ok2) neu->set_external_pose(true);
            *outRef = std::move(ref);
            *outNew = std::move(neu);
            return ok1 && ok2;
        };
        const size_t probeCount = std::min<size_t>(2, arm.gestures.size());
        for (size_t gi = 0; gi < probeCount; ++gi) {
            const std::string label = arm.gestures[gi];
            const auto* tl = find_tl(file, label);
            if (!tl) continue;
            std::unique_ptr<oa::emote::EmotePlayer> ref, neu;
            if (!make_pair(&ref, &neu)) continue;
            const Shape sh = shape_of(*tl);
            const int frames = int(sh.end) + 30;
            const Run rr = run_timeline(*ref, label, frames, 0, true);
            const Run rn = run_timeline(*neu, label, frames, 0, true);
            std::set<std::string> diffLabels;
            int firstDiffFrame = -1;
            const size_t n = std::min(rr.snaps.size(), rn.snaps.size());
            for (size_t f = 0; f < n; ++f) {
                for (const auto& [k, v] : rn.snaps[f]) {
                    const auto it = rr.snaps[f].find(k);
                    const double rv = it == rr.snaps[f].end() ? 0.0 : it->second;
                    if (std::fabs(rv - v) > 1e-9) {
                        diffLabels.insert(k);
                        if (firstDiffFrame < 0) firstDiffFrame = int(f);
                    }
                }
            }
            const std::set<std::string> tlLabels = labels_of(*tl);
            // The selector-control derivation rewrites every option label of a
            // selector whose own value (or option label) a track writes, so
            // that closure is part of "the timeline's own effect".
            std::set<std::string> own = tlLabels;
            for (const auto& sel : file.selectors) {
                bool touched = own.count(sel.label) > 0;
                for (const auto& opt : sel.options)
                    if (tlLabels.count(opt.label)) touched = true;
                if (touched)
                    for (const auto& opt : sel.options) own.insert(opt.label);
            }
            std::set<std::string> outside;
            for (const auto& k : diffLabels)
                if (!own.count(k)) outside.insert(k);
            std::printf("[%s] REF/NEW '%s': legacy sigs=%zu new sigs=%zu "
                        "diffLabels=%zu subset=%d firstDiffFrame=%d\n",
                        arm.tag, label.c_str(), rr.uniqueSigs, rn.uniqueSigs,
                        diffLabels.size(), outside.empty() ? 1 : 0, firstDiffFrame);
            if (!outside.empty()) {
                std::printf("[%s]   outside:", arm.tag);
                for (const auto& k : outside) std::printf(" %s", k.c_str());
                std::printf("\n");
            }
            checku(rr.uniqueSigs == 1,
                   "REF/NEW: the pre-S3 model leaves the gesture frozen",
                   (unsigned long long)rr.uniqueSigs);
            checku(rn.uniqueSigs >= 2, "REF/NEW: the S3 model moves it",
                   (unsigned long long)rn.uniqueSigs);
            check(outside.empty(),
                  "REF/NEW: differences stay inside the timeline's own tracks");
            check(!diffLabels.empty(), "REF/NEW: the gesture timeline actually differs");
        }
        // loop-boundary REF/NEW (待機): the loopEnd frame is NEW-only
        const std::string label = "待機";
        const auto* tl = find_tl(file, label);
        if (tl) {
            std::unique_ptr<oa::emote::EmotePlayer> ref, neu;
            const bool ok = make_pair(&ref, &neu);
            const int loopEnd = tl->loopEnd;
            const int frames = loopEnd + 20;
            const Run rr = ok ? run_timeline(*ref, label, frames) : Run{};
            const Run rn = ok ? run_timeline(*neu, label, frames) : Run{};
            auto scan = [&](const Run& r) {
                bool hit = false;
                for (double t : r.ts)
                    if (std::fabs(t - double(loopEnd)) < kEps) hit = true;
                return hit;
            };
            const bool refHit = scan(rr), newHit = scan(rn);
            std::printf("[%s] REF/NEW '%s' loop=[%d,%d]: legacy maxT=%.0f hit=%d | "
                        "new maxT=%.0f hit=%d\n",
                        arm.tag, label.c_str(), tl->loopBegin, loopEnd, rr.maxT,
                        refHit ? 1 : 0, rn.maxT, newHit ? 1 : 0);
            check(ok && !refHit && newHit,
                  "REF/NEW: only the S3 wrap rule reaches the loopEnd frame");
            checkf(std::fabs(rn.maxT - double(loopEnd)) < kEps,
                   "REF/NEW: the new model stops at loopEnd (not beyond)", rn.maxT);
            checkf(std::fabs(rr.maxT - double(loopEnd - 1)) < kEps,
                   "REF/NEW: the legacy model stops at loopEnd-1", rr.maxT);
        }

        // --- S2 G5 (research/124 §2.3): +-180 angle-interpolation wrap -------
        // The tg3/slny rig encodes the neck/body axes as 348/350 degrees for
        // the NEGATIVE authored endpoints (t=0) against 0 degrees at t=30, so
        // any axis value strictly inside (-30..0) samples an intermediate tick
        // between two keys that straddle the +-180 boundary. The pre-S2 engine
        // lerped the raw values (348 -> 0 at the midpoint = 174 degrees, "the
        // long way round"); the reference folds the pair to the short way
        // (E:emoterunner.cpp:604-620). The assertions pin all three facts:
        // both arms agree EXACTLY at the authored keys, they differ hugely in
        // between, and the wrapped arm is continuous where the old one jumps.
        {
            oa::emote::EmoteFile s2file;
            if (s2file.load(*bytes, &err)) {
                oa::emote::StaticRenderOptions opt;
                opt.fitToCanvas = false;
                opt.scale = 0.6;
                opt.dx = 960.0;
                opt.dy = 1024.0;
                auto collect = [&](const std::map<std::string, double>& vars,
                                   std::vector<oa::emote::EmoteDrawPart>* out) {
                    out->clear();
                    return oa::emote::emote_collect_parts(s2file, vars, 1920, 2048,
                                                                 opt, out, &err);
                };
                auto max_delta = [](const std::vector<oa::emote::EmoteDrawPart>& a,
                                    const std::vector<oa::emote::EmoteDrawPart>& b) {
                    if (a.size() != b.size()) return -1.0;
                    double m = 0;
                    for (size_t i = 0; i < a.size(); ++i) {
                        if (a[i].verts.size() != b[i].verts.size()) return -1.0;
                        for (size_t k = 0; k < a[i].verts.size(); ++k)
                            m = std::max(m, std::hypot(a[i].verts[k].x - b[i].verts[k].x,
                                                       a[i].verts[k].y - b[i].verts[k].y));
                    }
                    return m;
                };
                for (const char* axis : {"head_slant", "body_slant"}) {
                    double maxArm = 0, keyDelta = 0, maxStepOn = 0, maxStepOff = 0;
                    std::vector<oa::emote::EmoteDrawPart> prevOn, prevOff;
                    for (int i = 0; i <= 10; ++i) {
                        const double val = -30.0 + 3.0 * i; // tick 0 .. 30 exactly
                        std::map<std::string, double> vars{{axis, val}};
                        std::vector<oa::emote::EmoteDrawPart> on, off;
                        oa::emote::emote_set_s2_policy(1, -1, -1, -1);
                        const bool okOn = collect(vars, &on);
                        oa::emote::emote_set_s2_policy(0, -1, -1, -1);
                        const bool okOff = collect(vars, &off);
                        oa::emote::emote_set_s2_policy(-1, -1, -1, -1);
                        if (!okOn || !okOff) { check(false, "S2 G5: collect failed"); break; }
                        const double d = max_delta(on, off);
                        maxArm = std::max(maxArm, d);
                        // authored keys: the axis endpoints sit exactly on the
                        // authored ticks, so the wrap cannot matter there
                        if (i == 0 || i == 10) keyDelta = std::max(keyDelta, d);
                        if (i > 0) {
                            maxStepOn = std::max(maxStepOn, max_delta(prevOn, on));
                            maxStepOff = std::max(maxStepOff, max_delta(prevOff, off));
                        }
                        prevOn = on;
                        prevOff = off;
                    }
                    std::printf("[%s] S2 G5 '%s' sweep -30..0: maxArmDelta=%.2f "
                                "deltaAtAuthoredKeys=%.3f maxStep on=%.2f off=%.2f\n",
                                arm.tag, axis, maxArm, keyDelta, maxStepOn, maxStepOff);
                    check(keyDelta < 1e-9,
                          "S2 G5: both arms are identical at the authored axis keys");
                    checkf(maxArm > 100.0,
                           "S2 G5: the wrap changes the interpolated pose (data-live)",
                           (unsigned long long)maxArm);
                    checkf(maxStepOff > maxStepOn * 4.0,
                           "S2 G5: the wrapped interpolation is continuous where the raw "
                           "lerp jumps (the +-180 detour)",
                           (unsigned long long)(maxStepOff / std::max(1.0, maxStepOn)));
                }
                // default policy: the wrap is ON (env OA_EMOTE_ANGLEWRAP=0 off)
                check(oa::emote::emote_s2_policy().angleWrap,
                      "S2 G5: the default policy has the angle wrap enabled");

                // --- S2 G8b (research/124 §2.4): content-less tail fallback ---
                // Default OFF and inert for every in-range axis value; the
                // mechanism is reachable when a parameter tick is driven past
                // the authored tail.
                auto hash_of = [&](const std::map<std::string, double>& vars) {
                    std::vector<oa::emote::EmoteDrawPart> parts;
                    if (!oa::emote::emote_collect_parts(s2file, vars, 1920, 2048, opt,
                                                               &parts, &err))
                        return std::string("ERR");
                    unsigned long long h = 1469598103934665603ULL;
                    for (const auto& pt : parts) {
                        h ^= (unsigned long long)pt.source;
                        h *= 1099511628211ULL;
                        for (const auto& v : pt.verts) {
                            h ^= (unsigned long long)std::llround(v.x * 4096.0);
                            h *= 1099511628211ULL;
                            h ^= (unsigned long long)std::llround(v.y * 4096.0);
                            h *= 1099511628211ULL;
                        }
                    }
                    char b[32];
                    std::snprintf(b, sizeof(b), "%016llx/%zu", h, parts.size());
                    return std::string(b);
                };
                size_t inRange = 0, live = 0;
                for (const auto& vars : std::vector<std::map<std::string, double>>{
                         {}, {{"head_slant", 20}}, {{"body_slant", -20}},
                         {{"head_UD", 12}}, {{"body_UD", -8}}}) {
                    oa::emote::emote_set_s2_policy(-1, 0, -1, -1);
                    const std::string off = hash_of(vars);
                    oa::emote::emote_set_s2_policy(-1, 1, -1, -1);
                    const std::string on = hash_of(vars);
                    oa::emote::emote_set_s2_policy(-1, -1, -1, -1);
                    if (off != on) ++inRange;
                }
                for (const char* axis : {"head_UD", "body_UD", "head_slant", "body_slant",
                                         "face_talk", "head_LR"}) {
                    std::map<std::string, double> vars{{axis, 500.0}};
                    oa::emote::emote_set_s2_policy(-1, 0, -1, -1);
                    const std::string off = hash_of(vars);
                    oa::emote::emote_set_s2_policy(-1, 1, -1, -1);
                    const std::string on = hash_of(vars);
                    oa::emote::emote_set_s2_policy(-1, -1, -1, -1);
                    if (off != on) ++live;
                }
                std::printf("[%s] S2 G8b: in-range differing=%zu/5 out-of-range firing=%zu/6\n",
                            arm.tag, inRange, live);
                checku(inRange == 0,
                       "S2 G8b: the tail fallback is inert for in-range axis values",
                       (unsigned long long)inRange);
                checkf(live > 0,
                       "S2 G8b: the fallback fires when a tick passes the authored tail",
                       (unsigned long long)live);
            } else {
                check(false, "S2: emote file load for the angle/tail arms");
            }
        }
    }
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* tg3 = std::getenv("OA_TEST_TG3_PFS");
    const char* slny = std::getenv("OA_TEST_SLNY_PFS");
    if ((!tg3 || !*tg3) && (!slny || !*slny)) {
        std::printf("neither OA_TEST_TG3_PFS nor OA_TEST_SLNY_PFS set; skipping\n");
        return 77;
    }
    Arm tg;
    tg.tag = "tg3";
    tg.env = "OA_TEST_TG3_PFS";
    tg.entry = "image\\fg\\say_6.psb";
    tg.gestures = {"うんうん", "思考", "首かしげ", "びっくり"};
    tg.statics = {"普通Ａ", "笑顔表Ａ", "ジト目Ａ"};
    Arm sl;
    sl.tag = "slny";
    sl.env = "OA_TEST_SLNY_PFS";
    sl.entry = "image\\fg\\kir_2.psb";
    sl.gestures = {"うんうん", "耳ぴく", "首かしげ", "考える"};
    sl.statics = {"デフォ顔", "平常", "怒り"};
    run_arm(tg);
    run_arm(sl);

    std::printf("%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
