// Asset-free regression for the timeline track sampler.
//
// A type-0 frame carries no value: it only marks the END of a segment, so a
// sample past it must walk back to the previous content key and hold it. The
// sampler used to report "no write" instead, which is harmless on an integer
// clock but loses whole keys on a fractional one: NekoMiko's 照れ timeline keeps
// arm_type=1 only in authored frames [3,4), and at 60 fps the samples landed at
// 0, 2.2, 4.3 ... so the shy pose kept the default arm_type=0 (hands down)
// while the t=0 face keys still applied — the "wrong pose, eyes and mouth off"
// frame reported against the official capture.
#include <cassert>
#include <cmath>
#include <cstdio>

#include "core/emote/emote_file.h"

using oa::emote::EmoteTimeline;
using oa::emote::EmoteFile;
using oa::emote::emote_sample_track;
using oa::emote::TimeVar;
using oa::emote::TimeVarFrame;

namespace {

TimeVarFrame key(double t, double v) {
    TimeVarFrame f;
    f.time = t;
    f.type = 2; // content key
    f.hasContent = true;
    f.value = v;
    return f;
}

TimeVarFrame tail(double t) {
    TimeVarFrame f;
    f.time = t;
    f.type = 0; // segment end, no value
    f.hasContent = false;
    return f;
}

double sample(const TimeVar& track, double t, bool* ok = nullptr) {
    double v = -12345.0;
    const bool hit = emote_sample_track(track, t, false, &v);
    if (ok) *ok = hit;
    assert(hit);
    return v;
}

} // namespace

int main() {
    // The shape that exposed the bug: value 0 held to t=1, a short content
    // window [3,4) carrying 1, then the tail.
    TimeVar arm;
    arm.label = "arm_type";
    arm.frames = {key(0, 0), tail(1), key(3, 1), tail(4)};

    assert(sample(arm, 0.0) == 0.0);
    assert(sample(arm, 0.5) == 0.0);
    // Past the first tail: hold the previous content key, not "no write".
    assert(sample(arm, 2.2) == 0.0);
    // Inside the short window and past its tail both keep the raised value.
    assert(sample(arm, 3.5) == 1.0);
    assert(sample(arm, 4.3) == 1.0);
    assert(sample(arm, 60.0) == 1.0);

    // Between two content keys the value interpolates (reference type-2 rule).
    TimeVar ramp;
    ramp.label = "face_eye_open";
    ramp.frames = {key(0, 0), key(10, 100), tail(20)};
    assert(std::fabs(sample(ramp, 5.0) - 50.0) < 1e-9);
    assert(std::fabs(sample(ramp, 13.0) - 100.0) < 1e-9); // tail holds

    // A track whose content starts later than the sample reports "no write"
    // (the domain keeps whatever it had).
    TimeVar late;
    late.label = "late";
    late.frames = {key(30, 7), tail(40)};
    double ignored = 0;
    bool ok = true;
    emote_sample_track(late, 10.0, false, &ignored);
    ok = emote_sample_track(late, 10.0, false, &ignored);
    assert(!ok);
    assert(sample(late, 35.0) == 7.0);

    // Empty track: never a write.
    TimeVar empty;
    empty.label = "empty";
    ok = emote_sample_track(empty, 5.0, false, &ignored);
    assert(!ok);

    std::printf("emote_track_sample_test: ok\n");
    return 0;
}
