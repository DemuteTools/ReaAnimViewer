// SPDX-License-Identifier: MIT
//
// Host test of src/sound_entries.h (Epic 13 story 13-3): detection per mode, the two slice readers,
// silence split, frame clamping and hand edits, normalize. Pure: any C++17 compiler.

#include "sound_entries.h"

#include <cmath>
#include <cstdio>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++g_fails;                                       \
        }                                                    \
    } while (0)

bool Near(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) <= eps;
}

SoundFrame F(double s, double l)
{
    SoundFrame f;
    f.start = s;
    f.length = l;
    return f;
}

void TestTakes()
{
    EntryFacts e;
    e.source_len = 5;
    e.item_len = 1;
    e.takes = {F(0, 0), F(2, 1), F(1, 1)};
    std::vector<SoundFrame> fr = DetectFrames(e, DetectMode::Takes, SilenceParams{}, nullptr);
    CHECK(fr.size() == 3);
    if (fr.size() == 3) {
        CHECK(Near(fr[0].start, 0) && Near(fr[0].length, 1));  // 0 length -> item_len
        CHECK(Near(fr[1].start, 2) && Near(fr[2].start, 1));   // take order, not sorted
    }
    CHECK(DefaultDetectMode(e, false) == DetectMode::Takes);
    EntryFacts none;
    none.source_len = 5;
    none.item_len = 1;
    none.start_offs = 0.5;
    fr = DetectFrames(none, DetectMode::Takes, SilenceParams{}, nullptr);
    CHECK(fr.size() == 1 && Near(fr[0].start, 0.5) && Near(fr[0].length, 1));
    CHECK(DefaultDetectMode(none, false) == DetectMode::Whole);
    CHECK(DefaultDetectMode(none, true) == DetectMode::Silence);
}

void TestWholeClamp()
{
    EntryFacts e;
    e.source_len = 3;
    e.start_offs = 2;
    e.item_len = 5;
    std::vector<SoundFrame> fr = DetectFrames(e, DetectMode::Whole, SilenceParams{}, nullptr);
    CHECK(fr.size() == 1 && Near(fr[0].start, 2) && Near(fr[0].length, 1));
    e.start_offs = -1;  // the item starts 1 s before its source
    e.item_len = 2;
    fr = DetectFrames(e, DetectMode::Whole, SilenceParams{}, nullptr);
    CHECK(fr.size() == 1 && Near(fr[0].start, 0) && Near(fr[0].length, 1));
    e.start_offs = 3;  // past the source end
    CHECK(DetectFrames(e, DetectMode::Whole, SilenceParams{}, nullptr).empty());
    e.start_offs = 0;
    e.source_len = 0;  // unknown source length
    CHECK(DetectFrames(e, DetectMode::Whole, SilenceParams{}, nullptr).empty());
}

void TestTakeMarkers()
{
    std::vector<TakeMarkerPos> m = {{0, "V1"}, {0, "R1"}, {1, "V2"}, {2, "V3"}, {3, "V4"}};
    std::vector<Slice> s = SlicesFromTakeMarkers(m, 4);
    CHECK(s.size() == 4);
    for (size_t i = 0; i < s.size(); ++i) CHECK(Near(s[i].start, double(i)) && Near(s[i].length, 1));
    CHECK(SlicesFromTakeMarkers({{0, "V1"}}, 4).empty());

    EntryFacts e;
    e.source_len = 4;
    e.item_len = 4;
    e.slices = s;
    CHECK(DefaultDetectMode(e, true) == DetectMode::TakeMarkers);
    std::vector<SoundFrame> fr = DetectFrames(e, DetectMode::TakeMarkers, SilenceParams{}, nullptr);
    CHECK(fr.size() == 4);

    // TakeMarkers with fewer than 2 slices falls back to Whole.
    e.start_offs = 0.5;
    e.item_len = 2;
    e.slices = {{0, 4, "V1"}};
    fr = DetectFrames(e, DetectMode::TakeMarkers, SilenceParams{}, nullptr);
    CHECK(fr.size() == 1 && Near(fr[0].start, 0.5) && Near(fr[0].length, 2));
    e.slices.clear();
    fr = DetectFrames(e, DetectMode::TakeMarkers, SilenceParams{}, nullptr);
    CHECK(fr.size() == 1 && Near(fr[0].start, 0.5) && Near(fr[0].length, 2));
}

void TestRpp()
{
    const char* rpp =
        "<REAPER_PROJECT 0.1 \"7.0\"\n"
        "  MARKER 9 8 V0 0 0 1\n"
        "  MARKER 1 10 =START 0 0 1\n"
        "  MARKER 2 10 V1 0 0 1 B {A}\n"
        "  MARKER 3 11 \"V 2\" 0 0 1\n"
        "  MARKER 4 12 `V'3` 0\n"
        "  MARKER 5 13 'V4' 0\n"
        "  MARKER 6 10 R1 1 0 1\n"
        "  MARKER 6 14 \"\" 1\n"
        "  MARKER 7 14 =END 0\n"
        "  MARKER 8 15 After 0\n"
        ">\n";
    std::vector<Slice> s = SlicesFromRppText(rpp);
    CHECK(s.size() == 4);
    if (s.size() == 4) {
        for (size_t i = 0; i < 4; ++i) CHECK(Near(s[i].start, double(i)) && Near(s[i].length, 1));
        CHECK(s[0].name == "V1" && s[1].name == "V 2" && s[2].name == "V'3" && s[3].name == "V4");
    }
    CHECK(SlicesFromRppText("<REAPER_PROJECT\n>\n").empty());

    s = SlicesFromRppText("MARKER 1 0 V1 0\nMARKER 2 1 V2 0\n");
    CHECK(s.size() == 2);
    if (s.size() == 2) {
        CHECK(Near(s[0].start, 0) && Near(s[0].length, 1));
        CHECK(Near(s[1].start, 1) && Near(s[1].length, -1));
        EntryFacts e;
        e.source_len = 3;
        e.item_len = 3;
        e.slices = s;
        std::vector<SoundFrame> fr = DetectFrames(e, DetectMode::TakeMarkers, SilenceParams{}, nullptr);
        CHECK(fr.size() == 2 && Near(fr[1].length, 2));
    }

    // Out-of-order MARKER lines are sorted; two at one position leave one slice.
    s = SlicesFromRppText("MARKER 1 2 V3 0\nMARKER 2 0 V1 0\nMARKER 3 1 V2 0\nMARKER 4 1 V2b 0\nMARKER 5 3 =END 0\n");
    CHECK(s.size() == 3);
    if (s.size() == 3) {
        for (size_t i = 0; i < 3; ++i) CHECK(Near(s[i].start, double(i)) && Near(s[i].length, 1));
        CHECK(s[0].name == "V1" && s[2].name == "V3");
    }
}

PeakData Bursts()
{
    PeakData p;
    p.rate = 1000;
    p.max_abs.assign(10000, 0.0f);
    for (int b = 0; b < 10; ++b) {
        const float amp = (b == 3 || b == 7) ? 0.0316f : 1.0f;  // two at -30 dB
        for (int i = 0; i < 200; ++i) p.max_abs[size_t(b * 1000 + 200 + i)] = amp;
    }
    return p;
}

void TestSilence()
{
    PeakData p = Bursts();
    SilenceParams sp;
    // 13-17b: the defaults Antho picked (-45 dB, 200 ms gap, 100 ms tail); the checks below pin
    // their own values so they test the split, not the defaults.
    CHECK(sp.threshold_db == -45.0 && sp.min_gap_ms == 200.0 && sp.preroll_ms == 5.0 && sp.tail_ms == 100.0);
    sp.threshold_db = -40;
    sp.min_gap_ms = 50;
    sp.tail_ms = 50;
    std::vector<SoundFrame> fr = SplitOnSilence(p, sp, 10);
    CHECK(fr.size() == 10);
    if (!fr.empty()) CHECK(Near(fr[0].start, 0.195) && Near(fr[0].length, 0.255));
    sp.threshold_db = -25;
    CHECK(SplitOnSilence(p, sp, 10).size() == 8);
    for (size_t i = 0; i + 1 < fr.size(); ++i) CHECK(fr[i].start + fr[i].length <= fr[i + 1].start + 1e-9);

    EntryFacts e;
    e.source_len = 10;
    e.item_len = 10;
    fr = DetectFrames(e, DetectMode::Silence, sp, nullptr);
    CHECK(fr.size() == 1 && Near(fr[0].length, 10));  // null peaks -> Whole
    CHECK(SplitOnSilence(PeakData{}, sp, 10).empty());
    PeakData quiet;
    quiet.rate = 100;
    quiet.max_abs.assign(1000, 0.0f);
    fr = DetectFrames(e, DetectMode::Silence, sp, &quiet);
    CHECK(fr.size() == 1 && Near(fr[0].length, 10));  // all silent -> Whole

    PeakData nan = Bursts();
    nan.max_abs[5] = std::nanf("");
    CHECK(SplitOnSilence(nan, SilenceParams{}, 10).size() == 10);  // NaN = silence, no hang
}

void TestSilenceMerge()
{
    PeakData p;
    p.rate = 1000;
    p.max_abs.assign(1000, 0.0f);
    for (int i = 100; i < 200; ++i) p.max_abs[size_t(i)] = 1.0f;
    for (int i = 220; i < 300; ++i) p.max_abs[size_t(i)] = 1.0f;  // 20 ms gap < min_gap 50 ms
    SilenceParams m;
    m.min_gap_ms = 50;
    m.tail_ms = 50;
    std::vector<SoundFrame> fr = SplitOnSilence(p, m, 1);
    CHECK(fr.size() == 1);
    if (fr.size() == 1) CHECK(Near(fr[0].start, 0.095) && Near(fr[0].length, 0.255));

    PeakData q;
    q.rate = 1000;
    q.max_abs.assign(1000, 0.0f);
    for (int i = 100; i < 200; ++i) q.max_abs[size_t(i)] = 1.0f;
    for (int i = 300; i < 400; ++i) q.max_abs[size_t(i)] = 1.0f;
    SilenceParams sp;
    sp.min_gap_ms = 50;   // the 100 ms gap splits
    sp.preroll_ms = 150;  // reaches back past the previous run's end
    sp.tail_ms = 200;     // reaches past the next frame's start
    fr = SplitOnSilence(q, sp, 1);
    CHECK(fr.size() == 2);
    if (fr.size() == 2) {
        CHECK(Near(fr[0].start, 0) && Near(fr[0].length, 0.2));  // tail cut at the next start
        CHECK(Near(fr[1].start, 0.2) && Near(fr[1].length, 0.4));  // pre-roll stops at the previous end
    }
}

void TestClampAndEdits()
{
    std::vector<SoundFrame> fr = {F(2, 2), F(-1, 2), F(1.5, 0.0005), F(3, 5)};
    ClampFrames(fr, 6);
    CHECK(fr.size() == 3);
    if (fr.size() == 3) {
        CHECK(Near(fr[0].start, 0) && Near(fr[0].length, 1));
        CHECK(Near(fr[1].start, 2) && Near(fr[1].length, 1));
        CHECK(Near(fr[2].start, 3) && Near(fr[2].length, 3));
    }

    SoundData d;
    d.frames = {F(0, 1), F(2, 1), F(3, 1)};
    TrimFrame(d, 1, 0.5, 1.0, 4);
    CHECK(Near(d.frames[1].start, 1) && Near(d.frames[1].length, 0.5));
    CHECK(d.frames[1].hand && d.edited);
    TrimFrame(d, 1, 1.5, 9, 4);
    CHECK(Near(d.frames[1].length, 1.5));  // stops at next start
    TrimFrame(d, 1, 1.0, 0.0, 4);
    CHECK(Near(d.frames[1].start, 1.5) && Near(d.frames[1].length, 1.5));  // empty trim: unchanged

    SoundData a;
    a.frames = {F(0, 1), F(2, 1)};
    SoundData full;
    full.frames = {F(0, 1), F(1, 1)};
    CHECK(AddFrame(full, 0.5, 1, 2) == -1);  // no gap at all
    CHECK(!full.edited);
    int k = AddFrame(a, 1.2, 5, 3);
    CHECK(k == 1 && Near(a.frames[1].start, 1.2) && Near(a.frames[1].length, 0.8) && a.frames[1].hand);
    CHECK(AddFrame(a, 1.5, 0.1, 3) == -1);     // inside the new frame
    CHECK(AddFrame(a, 1.0, 0.0005, 3) == -1);  // gap too small
    SoundData last;
    last.frames = {F(0, 1)};
    CHECK(AddFrame(last, 0.5, 1, 3) == -1);  // inside the only frame, room after it
    CHECK(last.frames.size() == 1);
    CHECK(AddFrame(last, 1.5, 1, 3) == 1 && Near(last.frames[1].start, 1.5));

    SoundData s;
    s.frames = {F(0, 2)};
    s.frames[0].on = false;
    s.frames[0].db = -3;
    CHECK(!SplitFrame(s, 0, 2.5));
    CHECK(SplitFrame(s, 0, 0.5));
    CHECK(s.frames.size() == 2 && Near(s.frames[1].start, 0.5) && Near(s.frames[1].length, 1.5));
    CHECK(!s.frames[1].on && Near(s.frames[1].db, -3) && s.edited);
    CHECK(s.frames[0].hand && s.frames[1].hand);

    SetFrameOn(s, 0, true);
    SetFrameDb(s, 1, 2);
    CHECK(s.frames[0].on && Near(s.frames[1].db, 2));

    ResetToDetected(s, {F(0, 2)});
    CHECK(s.frames.size() == 1 && !s.edited && !s.frames[0].hand);
}

void TestNormalize()
{
    PeakData p;
    p.rate = 10;
    p.max_abs = {0.5f, 0.5f, 0.25f, 0.25f, 0.0f, 0.0f, 1.0f, 1.0f};
    std::vector<SoundFrame> fr = {F(0, 0.2), F(0.2, 0.2), F(0.4, 0.2), F(0.6, 0.2)};
    fr[3].on = false;
    fr[3].db = 7;
    NormalizeFrames(fr, p, -1, false, {});
    CHECK(Near(FramePeakDb(p, fr[0]) + fr[0].db, -1, 0.1));
    CHECK(Near(FramePeakDb(p, fr[1]) + fr[1].db, -1, 0.1));
    CHECK(Near(fr[2].db, 0) && std::isinf(FramePeakDb(p, fr[2])));
    CHECK(Near(fr[3].db, 7));
    NormalizeFrames(fr, p, -1, true, {});
    CHECK(Near(FramePeakDb(p, fr[0]) + fr[0].db, -1, 0.1));
    CHECK(Near(fr[1].db, fr[0].db));
    CHECK(Near(fr[3].db, 7));
    CHECK(Near(fr[2].db, 0) && !fr[2].hand);  // -inf peak: untouched by the whole pass too
    CHECK(fr[0].hand && !fr[3].hand);
    NormalizeFrames(fr, p, -6, false, {1, 3});
    CHECK(Near(FramePeakDb(p, fr[1]) + fr[1].db, -6, 0.1) && Near(fr[3].db, 7));
    CHECK(std::isinf(FramePeakDb(p, F(5, 1))));
}

// Antho 2026-10-12: fades on each variation.
void TestFades()
{
    SoundData d;
    d.frames = {F(0, 1), F(2, 0.004)};
    // The Sound's default (1 / 5 ms) on a frame with none of its own.
    FrameFades f = EffectiveFades(d, d.frames[0]);
    CHECK(Near(f.in, 0.001) && Near(f.out, 0.005) && !f.own_in && !f.own_out && f.in_shape == 0);
    // A 4 ms frame: 1 + 5 ms scaled down together to 4 ms.
    f = EffectiveFades(d, d.frames[1]);
    CHECK(Near(f.in + f.out, 0.004) && Near(f.in, 0.004 / 6.0) && Near(f.out, 0.004 * 5.0 / 6.0));
    // ClampFadePair: never negative, never past the length.
    double a = -1.0, b = 2.0;
    ClampFadePair(1.0, &a, &b);
    CHECK(Near(a, 0.0) && Near(b, 1.0));
    a = 0.5, b = 0.5;
    ClampFadePair(0.5, &a, &b);
    CHECK(Near(a, 0.25) && Near(b, 0.25));
    a = 1.0, b = 1.0;
    ClampFadePair(-3.0, &a, &b);
    CHECK(Near(a, 0.0) && Near(b, 0.0));
    // A hand-set fade: kept, marks hand / edited; the other side stays the default.
    SetFrameFades(d, 0, 0.02, -1.0);
    CHECK(Near(d.frames[0].fade_in, 0.02) && d.frames[0].fade_out < 0.0 && d.frames[0].hand && d.edited);
    f = EffectiveFades(d, d.frames[0]);
    CHECK(f.own_in && !f.own_out && Near(f.in, 0.02) && Near(f.out, 0.005));
    // The dragged side gives way to the other: fade out 0.9 then fade in 0.5 on a 1 s frame.
    SetFrameFades(d, 0, 0.02, 0.9);
    CHECK(Near(d.frames[0].fade_out, 0.9) && Near(d.frames[0].fade_in, 0.02));
    SetFrameFades(d, 0, 0.5, 0.9);
    CHECK(Near(d.frames[0].fade_in, 0.1) && Near(d.frames[0].fade_out, 0.9));
    SetFrameFades(d, 0, 0.1, 2.0);  // out dragged past: it gives way to in
    CHECK(Near(d.frames[0].fade_in, 0.1) && Near(d.frames[0].fade_out, 0.9));
    // Back to the default (Alt-click): < 0, shape cleared.
    d.frames[0].fade_in_shape = 3;
    SetFrameFades(d, 0, -1.0, 0.9);
    CHECK(d.frames[0].fade_in < 0.0 && d.frames[0].fade_in_shape == 0 && Near(d.frames[0].fade_out, 0.9));
    SetFrameFades(d, 7, 0.1, 0.1);  // bad index: nothing
    // Trim keeps the own fades inside the new length.
    SoundData t;
    t.frames = {F(0, 1)};
    SetFrameFades(t, 0, 0.3, 0.6);
    TrimFrame(t, 0, 0.0, 0.5, 10.0);
    CHECK(Near(t.frames[0].length, 0.5) && Near(t.frames[0].fade_in + t.frames[0].fade_out, 0.5) &&
          Near(t.frames[0].fade_in, 0.5 / 3.0));
    // Split: the left piece keeps its fade in, the right its fade out; the cut gets the default.
    SoundData s;
    s.frames = {F(0, 1)};
    SetFrameFades(s, 0, 0.05, 0.2);
    CHECK(SplitFrame(s, 0, 0.5));
    CHECK(s.frames.size() == 2 && Near(s.frames[0].fade_in, 0.05) && s.frames[0].fade_out < 0.0 &&
          s.frames[1].fade_in < 0.0 && Near(s.frames[1].fade_out, 0.2));
    // Split near the start: an own fade in longer than the left piece is clamped.
    SoundData s2;
    s2.frames = {F(0, 1)};
    SetFrameFades(s2, 0, 0.4, -1.0);
    CHECK(SplitFrame(s2, 0, 0.1));
    CHECK(Near(s2.frames[0].fade_in, 0.1));
    // The Sound default: never negative.
    SetDefaultFades(d, -2.0, 0.01);
    CHECK(d.fade_in == 0.0 && Near(d.fade_out, 0.01));
    // Zero default: no fade on a frame without its own.
    f = EffectiveFades(d, d.frames[1]);
    CHECK(Near(f.in, 0.0) && Near(f.out, 0.004));
}

}  // namespace

int main()
{
    TestTakes();
    TestWholeClamp();
    TestTakeMarkers();
    TestRpp();
    TestSilence();
    TestSilenceMerge();
    TestClampAndEdits();
    TestNormalize();
    TestFades();
    if (g_fails) {
        std::printf("sound_entries_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("sound_entries_test: all passed\n");
    return 0;
}
