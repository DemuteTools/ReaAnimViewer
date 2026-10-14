// SPDX-License-Identifier: MIT
//
// Host test of src/sound_entries.h (Epic 13 story 13-3): detection per mode, the two slice readers,
// silence split, frame clamping and hand edits, normalize; 13-62 per-part settings and comping. Pure:
// any C++17 compiler.

#include "sound_entries.h"
#include "sound_record.h"

#include "sound_audition_plan.h"  // 13-62g review: MergeHandFrames (a re-detection)
#include "sound_draw.h"  // ActiveVariationsFor

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

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
    CHECK(fr.size() == 1 && Near(fr[0].start, 2) && Near(fr[0].length, 1) && Near(fr[0].db, 0));
    {  // the item's own volume is every detected variation's gain (an item at +18.5 dB)
        EntryFacts loud = e;
        loud.item_db = 18.5;
        const std::vector<SoundFrame> lf = DetectFrames(loud, DetectMode::Whole, SilenceParams{}, nullptr);
        CHECK(lf.size() == 1 && Near(lf[0].db, 18.5));
        // Takes: each variation at its own take's gain (the item x that take), not the active take's.
        EntryFacts tk = loud;
        SoundFrame t0, t1;
        t0.db = 18.5;
        t1.db = -3.0;
        t1.start = 0.5;
        tk.takes = {t0, t1};
        const std::vector<SoundFrame> tf = DetectFrames(tk, DetectMode::Takes, SilenceParams{}, nullptr);
        CHECK(tf.size() == 2 && Near(tf[0].db, 18.5) && Near(tf[1].db, -3.0));
    }
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

// 13-24 review: the item's length is read in source time (playrate), so a File Sound slices it right.
void TestPlayrate()
{
    EntryFacts e;
    e.source_len = 60;
    e.start_offs = 12;
    e.item_len = 0.8;
    e.playrate = 2;
    std::vector<SoundFrame> fr = DetectFrames(e, DetectMode::Whole, SilenceParams{}, nullptr);
    CHECK(fr.size() == 1 && Near(fr[0].start, 12) && Near(fr[0].length, 1.6));
    e.start_offs = 59;  // clamped to the source end
    fr = DetectFrames(e, DetectMode::Whole, SilenceParams{}, nullptr);
    CHECK(fr.size() == 1 && Near(fr[0].length, 1));
    e.start_offs = 0;
    e.takes = {F(0, 0), F(3, 0.4)};  // 0 = the item's length in source time; a set length kept
    fr = DetectFrames(e, DetectMode::Takes, SilenceParams{}, nullptr);
    CHECK(fr.size() == 2 && Near(fr[0].length, 1.6) && Near(fr[1].length, 0.4));
    e.playrate = 0;  // unreadable: 1
    fr = DetectFrames(e, DetectMode::Takes, SilenceParams{}, nullptr);
    CHECK(fr.size() == 2 && Near(fr[0].length, 0.8));
    e.playrate = 0.5;
    e.takes.clear();
    fr = DetectFrames(e, DetectMode::Whole, SilenceParams{}, nullptr);
    CHECK(fr.size() == 1 && Near(fr[0].length, 0.4));
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

// Peak Normalize as the gesture runs it (13-28): regions, cached-peak adjustments, ApplyNormalize.
NormalizeOutcome PeakNormalize(std::vector<SoundFrame>& fr, const PeakData& p, double target, NormScope scope,
                               const std::vector<int>& sel)
{
    SoundData d;
    d.frames = fr;
    const std::vector<NormRegion> regs = NormalizeRegions(d, scope, sel);
    std::vector<double>           adj;
    for (const NormRegion& r : regs) adj.push_back(RegionPeakAdjDb(p, r, target));
    const NormalizeOutcome o = ApplyNormalize(fr, scope, regs, adj);
    return o;
}

void TestNormalize()
{
    // Ported from NormalizeFrames: the Peak cases behave as before.
    PeakData p;
    p.rate = 10;
    p.max_abs = {0.5f, 0.5f, 0.25f, 0.25f, 0.0f, 0.0f, 1.0f, 1.0f};
    std::vector<SoundFrame> fr = {F(0, 0.2), F(0.2, 0.2), F(0.4, 0.2), F(0.6, 0.2)};
    fr[3].on = false;
    fr[3].db = 7;
    PeakNormalize(fr, p, -1, NormScope::Variation, {});
    CHECK(Near(FramePeakDb(p, fr[0]) + fr[0].db, -1, 0.1));
    CHECK(Near(FramePeakDb(p, fr[1]) + fr[1].db, -1, 0.1));
    CHECK(Near(fr[2].db, 0) && std::isinf(FramePeakDb(p, fr[2])));
    CHECK(Near(fr[3].db, 7));
    PeakNormalize(fr, p, -1, NormScope::Whole, {});
    CHECK(Near(FramePeakDb(p, fr[0]) + fr[0].db, -1, 0.1));
    CHECK(Near(fr[1].db, fr[0].db));
    CHECK(Near(fr[3].db, 7));
    CHECK(Near(fr[2].db, 0) && !fr[2].hand);  // -inf peak: untouched by the whole pass too
    CHECK(fr[0].hand && !fr[3].hand);
    PeakNormalize(fr, p, -6, NormScope::Variation, {1, 3});
    CHECK(Near(FramePeakDb(p, fr[1]) + fr[1].db, -6, 0.1) && Near(fr[3].db, 7));
    CHECK(std::isinf(FramePeakDb(p, F(5, 1))));
}

// 13-28: the modes table, the applier per scope, silent regions, per-part gains.
void TestNormalizeModes()
{
    // REAPER's normalizeTo, in the mockup's order.
    const int want[kNormModeCount] = {2, 3, 1, 0, 4, 5};
    for (int i = 0; i < kNormModeCount; ++i) CHECK(NormModeReaperIndex(static_cast<NormMode>(i)) == want[i]);

    // Matrix "Per variation, Peak -1": peaks -6 / -3 / -9 / -12 -> +5 / +2 / +8 / +11.
    std::vector<SoundFrame> fr = {F(0, 1), F(1, 1), F(2, 1), F(3, 1)};
    std::vector<NormRegion> regs(4);
    for (int i = 0; i < 4; ++i) regs[static_cast<size_t>(i)].frame = i;
    NormalizeOutcome o = ApplyNormalize(fr, NormScope::Variation, regs, {-1 + 6.0, -1 + 3.0, -1 + 9.0, -1 + 12.0});
    CHECK(o.done == 4 && o.skipped == 0 && o.changed);
    CHECK(Near(fr[0].db, 5) && Near(fr[1].db, 2) && Near(fr[2].db, 8) && Near(fr[3].db, 11) && fr[0].hand);
    // The same again: nothing changed.
    CHECK(!ApplyNormalize(fr, NormScope::Variation, regs, {5.0, 2.0, 8.0, 11.0}).changed);

    // Matrix "Whole, LUFS-I -14": adjustments +3 / +1 / +6 -> all +1.
    fr = {F(0, 1), F(1, 1), F(2, 1)};
    regs.resize(3);
    o = ApplyNormalize(fr, NormScope::Whole, regs, {3.0, 1.0, 6.0});
    CHECK(o.done == 3 && Near(fr[0].db, 1) && Near(fr[1].db, 1) && Near(fr[2].db, 1));

    // Matrix "Silent region": factor <= 0, NaN, or > +60 dB -> skipped, counted out.
    CHECK(std::isnan(NormAdjustmentDb(0.0)) && std::isnan(NormAdjustmentDb(-1.0)) &&
          std::isnan(NormAdjustmentDb(std::nan(""))) && std::isnan(NormAdjustmentDb(std::pow(10.0, 61.0 / 20.0))));
    CHECK(Near(NormAdjustmentDb(2.0), 6.0206, 1e-3));
    fr = {F(0, 1), F(1, 1), F(2, 1)};
    o = ApplyNormalize(fr, NormScope::Variation, regs, {NormAdjustmentDb(0.0), 61.0, 4.0});
    CHECK(o.done == 1 && o.skipped == 2 && Near(fr[0].db, 0) && !fr[0].hand && Near(fr[1].db, 0) && Near(fr[2].db, 4));
    o = ApplyNormalize(fr, NormScope::Whole, regs, {std::nan(""), 2.0, 4.0});
    CHECK(o.done == 2 && o.skipped == 1 && Near(fr[1].db, 2) && Near(fr[2].db, 2) && Near(fr[0].db, 0));

    // An off frame is never changed (even when a region names it).
    fr = {F(0, 1), F(1, 1)};
    fr[1].on = false;
    regs.resize(2);
    o = ApplyNormalize(fr, NormScope::Variation, regs, {3.0, 3.0});
    CHECK(o.done == 1 && Near(fr[1].db, 0) && !fr[1].hand);

    // Combining spans: Peak = the loudest span (smallest adjustment); LUFS-I = energy over the summed duration.
    CHECK(Near(CombineSpanAdjustments(NormMode::Peak, -1, {5.0, 2.0}, {0.1, 0.1}), 2.0));
    CHECK(Near(CombineSpanAdjustments(NormMode::LufsSMax, -18, {5.0, std::nan(""), 7.0}, {}), 5.0));
    CHECK(Near(CombineSpanAdjustments(NormMode::LufsI, -14, {3.0, 3.0}, {0.2, 0.5}), 3.0));
    // Two equal-length spans 10 dB apart: the energy mean is ~7.4 dB under the loud one.
    CHECK(Near(CombineSpanAdjustments(NormMode::LufsI, -14, {0.0, 10.0}, {1.0, 1.0}), 10.0 * std::log10(2.0 / 1.1), 1e-6));
    CHECK(std::isnan(CombineSpanAdjustments(NormMode::RmsI, -14, {std::nan("")}, {1.0})));
}

void TestNormalizeParts()
{
    const std::string back = "vb", front = "vf";
    SoundData d;
    d.divide_group = "g";
    d.divide_values = {back, front};
    d.frames = {F(0, 1), F(1, 1), F(2, 1)};
    for (size_t i = 0; i < 3; ++i) d.frames[i].cuts = {d.frames[i].start + 0.4};
    CHECK(NormPartsAvailable(d));
    // Matrix "Per part": divided Back / Front, 3 variations -> 6 part dBs, frame dB = the smallest of them.
    std::vector<NormRegion> regs = NormalizeRegions(d, NormScope::Part, {});
    CHECK(regs.size() == 6 && regs[0].frame == 0 && regs[0].part == back && regs[1].part == front);
    CHECK(regs[0].spans.size() == 1 && Near(regs[0].spans[0].length, 0.4) && Near(regs[1].spans[0].start, 0.4));
    d.frames[0].db = -2;
    std::vector<double> adj = {1, 2, 3, 4, 5, 6};
    NormalizeOutcome    o = ApplyNormalize(d.frames, NormScope::Part, regs, adj);
    CHECK(o.done == 6 && o.changed);
    CHECK(Near(d.frames[0].db, 1) && d.frames[0].part_db.size() == 2 && Near(d.frames[0].part_db[back], 1) &&
          Near(d.frames[2].part_db[front], 6) && Near(d.frames[2].db, 5));
    // FrameGainDb: the part's own, else the frame's; "" = the frame's (the whole variation, never an older gain).
    CHECK(Near(FrameGainDb(d.frames[0], front), 2) && Near(FrameGainDb(d.frames[0], ""), 1) &&
          Near(FrameGainDb(d.frames[0], "other"), 1));
    CHECK(Near(FrameLabelDb(d, 0), 2) && Near(FrameLabelDb(d, 2), 6));
    // A hand dB drag moves the parts' own gains by the same step.
    {
        SoundData h = d;
        SetFrameDb(h, 0, 4.0);  // +1 -> +4: +3
        CHECK(Near(h.frames[0].db, 4) && Near(h.frames[0].part_db[back], 4) && Near(h.frames[0].part_db[front], 5));
    }
    // Matrix "Per variation after Per part": the acted frames' part dBs cleared, frame dB written.
    regs = NormalizeRegions(d, NormScope::Variation, {1});
    CHECK(regs.size() == 1 && regs[0].frame == 1 && regs[0].part.empty());
    ApplyNormalize(d.frames, NormScope::Variation, regs, {7.0});
    CHECK(d.frames[1].part_db.empty() && Near(d.frames[1].db, 7) && d.frames[0].part_db.size() == 2);
    // An excluded zone inside Back of frame 0: Back measured over its 2 spans; a frame with nothing left gives none.
    d.zones.push_back({0.1, 0.1, "", {}});
    d.zones.push_back({2.0, 1.0, "", {}});  // frame 2 fully excluded
    regs = NormalizeRegions(d, NormScope::Part, {});
    CHECK(regs.size() == 4 && regs[0].part == back && regs[0].spans.size() == 2);
    for (const NormRegion& r : regs) CHECK(r.frame != 2);
    // Per variation / Whole: frame 0's spans skip the zone; the fully excluded frame 2 gives no region.
    regs = NormalizeRegions(d, NormScope::Variation, {});
    CHECK(regs.size() == 2 && regs[0].frame == 0 && regs[1].frame == 1);
    if (!regs.empty())
        CHECK(regs[0].spans.size() == 2 && Near(regs[0].spans[0].start, 0.0) && Near(regs[0].spans[0].length, 0.1) &&
              Near(regs[0].spans[1].start, 0.2) && Near(regs[0].spans[1].length, 0.8));
    regs = NormalizeRegions(d, NormScope::Whole, {});
    CHECK(regs.size() == 2);
    // Whole on a divided frame with part gains clears them.
    CHECK(!d.frames[0].part_db.empty());
    ApplyNormalize(d.frames, NormScope::Whole, regs, {3.0, 5.0});
    CHECK(d.frames[0].part_db.empty() && Near(d.frames[0].db, 3) && Near(d.frames[1].db, 3));
    // Selection: only the selected on frames; {-1} = none; an off frame none.
    d.frames[1].on = false;
    CHECK(NormalizeRegions(d, NormScope::Variation, {-1}).empty());
    CHECK(NormalizeRegions(d, NormScope::Variation, {1}).empty());
    CHECK(NormalizeRegions(d, NormScope::Variation, {0, 1}).size() == 1);
    // Matrix "Part preset, undivided": Per part acts per variation.
    SoundData u;
    u.frames = {F(0, 1), F(1, 1)};
    CHECK(!NormPartsAvailable(u));
    regs = NormalizeRegions(u, NormScope::Part, {});
    CHECK(regs.size() == 2 && regs[0].part.empty());
    // Peak from cached peaks over the spans: the loudest span.
    PeakData p;
    p.rate = 10;
    p.max_abs.assign(10, 0.25f);
    p.max_abs[7] = 0.5f;
    NormRegion r;
    r.spans = {{"", 0.0, 0.3}, {"", 0.6, 0.3}};
    CHECK(Near(RegionPeakAdjDb(p, r, -1), -1 - 20 * std::log10(0.5), 1e-4));
    r.spans = {{"", 0.0, 0.3}};
    CHECK(Near(RegionPeakAdjDb(p, r, -1), -1 - 20 * std::log10(0.25), 1e-4));
    p.max_abs.assign(10, 0.0f);
    CHECK(std::isnan(RegionPeakAdjDb(p, r, -1)));
    // FrameClips: peak + the gain it plays at over 0 dBFS, part by part (the red dB label).
    SoundData c;
    c.divide_group = "g";
    c.divide_values = {back, front};
    c.frames = {F(0, 1)};
    c.frames[0].cuts = {0.5};
    p.rate = 10;
    p.max_abs.assign(10, 0.5f);  // -6 dBFS
    CHECK(!FrameClips(c, 0, p));
    c.frames[0].part_db[front] = 7.0;
    CHECK(FrameClips(c, 0, p));
    c.frames[0].part_db[front] = 5.0;
    c.frames[0].db = 7.0;  // Back plays at the frame's dB
    CHECK(FrameClips(c, 0, p));
    c.frames[0].db = 0.0;
    CHECK(!FrameClips(c, 0, p) && Near(FrameLabelDb(c, 0), 5.0));
    c.frames[0].on = false;
    c.frames[0].db = 20.0;
    CHECK(!FrameClips(c, 0, p));
    // Undivided: the frame's dB; Takes: the take whole at the frame's dB, a part gain neither shown nor used.
    SoundData w;
    w.frames = {F(0, 1)};
    w.frames[0].db = 5.0;
    CHECK(!FrameClips(w, 0, p));
    w.frames[0].db = 7.0;
    CHECK(FrameClips(w, 0, p));
    c.frames[0].on = true;
    c.frames[0].db = 0.0;
    c.frames[0].part_db[front] = 7.0;
    c.mode = DetectMode::Takes;
    CHECK(!FrameClips(c, 0, p) && Near(FrameLabelDb(c, 0), 0.0));
    c.frames[0].db = 7.0;
    CHECK(FrameClips(c, 0, p) && Near(FrameLabelDb(c, 0), 7.0));
    // SameNormMeasured: what the regions depend on cancels the job; a gain change does not.
    {
        SoundData a = d, x = d;
        CHECK(SameNormMeasured(a, x));
        x.frames[0].db = 9.0;
        x.frames[0].part_db[back] = 4.0;
        CHECK(SameNormMeasured(a, x));
        x = d; x.frames[0].start += 0.01; CHECK(!SameNormMeasured(a, x));
        x = d; x.frames[0].cuts = {0.3}; CHECK(!SameNormMeasured(a, x));
        x = d; x.frames[0].on = !x.frames[0].on; CHECK(!SameNormMeasured(a, x));
        x = d; x.zones[0].start += 0.01; CHECK(!SameNormMeasured(a, x));
        x = d; x.zones[0].to = front; CHECK(!SameNormMeasured(a, x));
        x = d; x.zones.pop_back(); CHECK(!SameNormMeasured(a, x));
        x = d; x.divide_values = {front, back}; CHECK(!SameNormMeasured(a, x));
        x = d; x.divide_group = "h"; CHECK(!SameNormMeasured(a, x));
        x = d; x.role_map.push_back({"toe", front}); CHECK(!SameNormMeasured(a, x));
        x = d; x.mode = DetectMode::Takes; CHECK(!SameNormMeasured(a, x));
    }
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

// ---- 13-20 / 13-23 / 13-25: parts, zones, sync points (spec 13-20-23-25 D5 / D6) -----------------

// 1 kHz peaks of `seconds`, silent at -100 dB.
PeakData Quiet(double seconds)
{
    PeakData p;
    p.rate = 1000.0;
    p.max_abs.assign(static_cast<size_t>(seconds * 1000.0), 1e-5f);
    return p;
}

// A transient at `t` (s) peaking at `db`, decaying 1 dB per ms (kept where louder than what is there).
void Hit(PeakData& p, double t, double db)
{
    const long long i0 = std::llround(t * p.rate);
    for (long long i = i0; i < static_cast<long long>(p.max_abs.size()) && i < i0 + 200; ++i) {
        const float v = static_cast<float>(std::pow(10.0, (db - static_cast<double>(i - i0)) / 20.0));
        if (v > p.max_abs[static_cast<size_t>(i)]) p.max_abs[static_cast<size_t>(i)] = v;
    }
}

// A footstep Sound: one frame 1.000-1.400 s, heel at 1.020 (-4 dB), toe at 1.150 (-7 dB).
SoundData Step(PeakData* peaks)
{
    *peaks = Quiet(3.0);
    Hit(*peaks, 1.020, -4.0);
    Hit(*peaks, 1.150, -7.0);
    SoundData d;
    d.mode = DetectMode::Silence;
    d.frames = {F(1.0, 0.4)};
    return d;
}

const char kBack[] = "v0000000000000b1";
const char kFront[] = "v0000000000000f1";

void TestSyncSliced()
{
    PeakData  pk;
    SoundData d = Step(&pk);
    RefreshParts(d, &pk);
    // Matrix row 1: the attack at 1.020, -2 ms: the item starts at 1.000 with snap 0.018.
    CHECK(d.frames[0].syncs.size() == 1 && Near(d.frames[0].syncs[0], 1.018));
    CHECK(Near(SyncFor(d, 0, ""), 1.018) && !SyncsStale(d, 0));
    CHECK(d.frames[0].cuts.empty());
    // Peak: the loudest bin (the heel's top), + the offset.
    d.sync_mode = SyncMode::Peak;
    d.sync_offset_ms = 0.0;
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[0], 1.020));
    // Slice start + 3 ms; a negative offset is clamped into the frame.
    d.sync_mode = SyncMode::SliceStart;
    d.sync_offset_ms = 3.0;
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[0], 1.003));
    d.sync_offset_ms = -5.0;
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[0], 1.0));
    // Attack + a large offset is clamped to the frame's end.
    d.sync_mode = SyncMode::Attack;
    d.sync_offset_ms = 1000.0;
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[0], 1.4));
    // Attack over a part quieter than the threshold: 20 dB under its peak still finds it.
    d.sync_offset_ms = 0.0;
    d.silence.threshold_db = 10.0;
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[0], 1.020));
    // Without peaks: Slice start is measured, Attack / Peak keep a stored sync still inside, else wait
    // for placement (syncs empty: stale).
    SoundData n = d;
    RefreshParts(n, nullptr);
    CHECK(n.frames[0].syncs.size() == 1 && Near(n.frames[0].syncs[0], 1.020));
    n.frames[0].syncs.clear();
    RefreshParts(n, nullptr);
    CHECK(n.frames[0].syncs.empty() && SyncsStale(n, 0) && SyncFor(n, 0, "") < 0.0);
    n.sync_mode = SyncMode::SliceStart;
    RefreshParts(n, nullptr);
    CHECK(n.frames[0].syncs.size() == 1 && Near(n.frames[0].syncs[0], 1.0));
    // Review: changing the sync settings drops the measured syncs, so without peaks the old Attack sync
    // is not kept under Peak: placement measures it.
    SoundData m = d;
    CHECK(SetSyncSettings(m, SyncMode::Peak, 0.0) && !SetSyncSettings(m, SyncMode::Peak, 0.0));
    CHECK(m.frames[0].syncs.empty());
    RefreshParts(m, nullptr);
    CHECK(SyncsStale(m, 0));
    CHECK(!SetSyncSettings(m, SyncMode::Attack, std::nan("")));
    // Takes mode: no cuts, no syncs (A8).
    SoundData t = d;
    t.mode = DetectMode::Takes;
    RefreshParts(t, &pk);
    CHECK(t.frames[0].syncs.empty() && t.frames[0].cuts.empty() && !SyncsStale(t, 0));
}

// Review: a transient rising over 30 ms (-60 to -4 dB): Attack and Peak differ, and the threshold
// counts only when it is above the peak - 20 dB.
void TestAttackVsPeak()
{
    PeakData pk = Quiet(2.0);
    for (int k = 0; k <= 30; ++k)
        pk.max_abs[static_cast<size_t>(1000 + k)] = static_cast<float>(std::pow(10.0, (-60.0 + 56.0 * k / 30.0) / 20.0));
    Hit(pk, 1.030, -4.0);
    SoundData d;
    d.mode = DetectMode::Silence;
    d.frames = {F(0.9, 0.4)};
    d.sync_offset_ms = 0.0;
    d.silence.threshold_db = -50.0;
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[0], 1.020));  // first bin >= -24 dB (peak - 20)
    d.silence.threshold_db = -10.0;
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[0], 1.027));  // first bin >= the threshold
    SetSyncSettings(d, SyncMode::Peak, 0.0);
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[0], 1.030));
}

void TestOnsetsAndCuts()
{
    PeakData  pk;
    SoundData d = Step(&pk);
    std::vector<Onset> on = FindOnsets(pk, 1.0, 0.4);
    CHECK(on.size() == 2);
    if (on.size() == 2) {
        CHECK(Near(on[0].t, 1.020) && Near(on[0].db, -4.0, 1e-3) && Near(on[1].t, 1.150) && Near(on[1].db, -7.0, 1e-3));
        CHECK(Near(on[1].rise, 1.150));  // it rises from the heel's tail right at 1.150
    }
    // A rise under 6 dB is no onset; two maxima closer than 20 ms keep the louder.
    PeakData small = Quiet(1.0);
    Hit(small, 0.1, -10.0);
    Hit(small, 0.112, -4.0);  // 12 ms later, louder
    Hit(small, 0.3, -10.0);
    Hit(small, 0.330, -36.0);  // a bump in its tail rising 3 dB over the previous 15 ms: no onset
    std::vector<Onset> s2 = FindOnsets(small, 0.0, 1.0);
    CHECK(s2.size() == 2 && Near(s2[0].t, 0.112) && Near(s2[1].t, 0.3));
    // A transient on the source's first bin is an onset; a quiet floor there is not.
    PeakData first = Quiet(0.5);
    CHECK(FindOnsets(first, 0.0, 0.5).empty());
    Hit(first, 0.0, -6.0);
    std::vector<Onset> f0 = FindOnsets(first, 0.0, 0.5);
    CHECK(f0.size() == 1 && Near(f0[0].t, 0.0) && Near(f0[0].rise, 0.0));

    // Divided in two: the cut sits 5 ms before the toe's rise; sure (one candidate).
    bool unsure = true;
    std::vector<double> cuts = AutoCuts(&pk, d.frames[0], 2, &unsure);
    CHECK(cuts.size() == 1 && Near(cuts[0], 1.145) && !unsure);
    // Two candidates within 3 dB after the first: unsure; the louder one is cut.
    PeakData q = pk;
    Hit(q, 1.250, -8.5);
    cuts = AutoCuts(&q, d.frames[0], 2, &unsure);
    CHECK(cuts.size() == 1 && Near(cuts[0], 1.145) && unsure);
    // Three parts: both, in time order.
    cuts = AutoCuts(&q, d.frames[0], 3, &unsure);
    CHECK(cuts.size() == 2 && Near(cuts[0], 1.145) && Near(cuts[1], 1.245) && !unsure);
    // Too few onsets: an even split.
    cuts = AutoCuts(&pk, d.frames[0], 3, &unsure);
    CHECK(cuts.size() == 2 && Near(cuts[0], 1.0 + 0.4 / 3) && Near(cuts[1], 1.0 + 0.8 / 3) && !unsure);
    cuts = AutoCuts(nullptr, d.frames[0], 2, &unsure);
    CHECK(cuts.size() == 1 && Near(cuts[0], 1.2));
    CHECK(AutoCuts(&pk, d.frames[0], 1, &unsure).empty());
}

void TestOwnershipAndSpans()
{
    PeakData  pk;
    SoundData d = Step(&pk);
    CHECK(!IsDivided(d) && PartCount(d) == 1);
    // Combined: one span, owner "".
    std::vector<PartSpan> sp = PartSpansOf(d, 0);
    CHECK(sp.size() == 1 && sp[0].value_uid.empty() && Near(sp[0].start, 1.0) && Near(sp[0].length, 0.4));
    CHECK(SetDivide(d, "g", {kBack, kFront}) && !SetDivide(d, "g", {kBack, kFront}));
    CHECK(IsDivided(d) && PartCount(d) == 2 && PartIndex(d, kFront) == 1 && PartIndex(d, "x") == -1);
    RefreshParts(d, &pk);
    CHECK(d.frames[0].cuts.size() == 1 && Near(d.frames[0].cuts[0], 1.145));
    // Each part's sync on its own attack, -2 ms.
    CHECK(d.frames[0].syncs.size() == 2 && Near(d.frames[0].syncs[0], 1.018) && Near(d.frames[0].syncs[1], 1.148));
    CHECK(Near(SyncFor(d, 0, kFront), 1.148) && Near(SyncFor(d, 0, ""), 1.018));
    std::string v;
    CHECK(PartAt(d, 0, 1.1, &v) && v == kBack && PartAt(d, 0, 1.2, &v) && v == kFront);
    CHECK(!PartAt(d, 0, 0.5, &v) && !PartAt(d, 3, 1.1, &v));
    sp = PartSpansOf(d, 0);
    CHECK(sp.size() == 2 && sp[0].value_uid == kBack && Near(sp[0].length, 0.145) && sp[1].value_uid == kFront);
    // An excluded zone inside Back: 2 Back spans, the gap empty; the whole placement keeps the hole.
    CHECK(AddZone(d, 1.05, 0.03, ""));
    CHECK(!PartAt(d, 0, 1.06, &v));
    // 13-62g: Back's piece (from 5 ms before its sound to the cut) minus the excluded stretch.
    std::vector<PartSpan> back = PlacedSpans(d, 0, kBack);
    CHECK(back.size() == 2 && Near(back[0].start, 1.015) && Near(back[0].length, 0.035) && Near(back[1].start, 1.08) &&
          Near(back[1].length, 0.065));
    std::vector<PartSpan> whole = PlacedSpans(d, 0, "");
    CHECK(whole.size() == 2 && whole[0].value_uid.empty() && Near(whole[1].start, 1.08) && Near(whole[1].length, 0.32));
    // A zone given to Back inside Front: Back owns it (later zones win), Front gets two spans.
    CHECK(AddZone(d, 1.3, 0.02, kBack));
    CHECK(PartAt(d, 0, 1.31, &v) && v == kBack);
    CHECK(PlacedSpans(d, 0, kFront).size() == 2 && PlacedSpans(d, 0, kBack).size() == 3);
    // A later exclusion over it wins; Include again removes the exclusion only.
    CHECK(AddZone(d, 1.29, 0.04, ""));  // covers the Back zone fully: it goes
    CHECK(d.zones.size() == 2 && !PartAt(d, 0, 1.31, &v));
    CHECK(IncludeAgain(d, 1.31) && d.zones.size() == 1 && PartAt(d, 0, 1.31, &v) && v == kFront);
    CHECK(!IncludeAgain(d, 1.31) && !AddZone(d, 1.2, 0.0005, ""));
    // A zone's `to` that is not a divide value: the part by cuts.
    SoundData o = d;
    o.zones.push_back({1.2, 0.05, "elsewhere", {}});
    CHECK(PartAt(o, 0, 1.22, &v) && v == kFront);
    // Combined: a `to` zone owns nothing special.
    SoundData c = d;
    CHECK(SetDivide(c, "", {}));
    CHECK(c.divide_values.empty() && PartSpansOf(c, 0).size() == 2);
    // An off frame has no span.
    SoundData off = d;
    off.frames[0].on = false;
    CHECK(PartSpansOf(off, 0).empty() && PlacedSpans(off, 0, "").empty());
}

void TestEmptyPart()
{
    PeakData  pk;
    SoundData d = Step(&pk);
    SetDivide(d, "g", {kBack, kFront});
    d.frames.push_back(F(2.0, 0.4));
    Hit(pk, 2.020, -4.0);
    Hit(pk, 2.150, -7.0);
    RefreshParts(d, &pk);
    // Every Front part excluded in frame 0: it is skipped for Front, kept for Back and the whole.
    CHECK(AddZone(d, 1.145, 0.255, ""));
    RefreshParts(d, &pk);
    CHECK(PlacedSpans(d, 0, kFront).empty() && d.frames[0].syncs.size() == 2 && d.frames[0].syncs[1] < 0.0);
    CHECK(!SyncsStale(d, 0) && SyncFor(d, 0, kFront) < 0.0);
    CHECK(ActiveVariationsFor(d, kFront) == std::vector<int>{1});
    CHECK((ActiveVariationsFor(d, kBack) == std::vector<int>{0, 1}) && (ActiveVariationsFor(d, "") == std::vector<int>{0, 1}));
    CHECK(AddZone(d, 2.145, 0.255, ""));
    CHECK(ActiveVariationsFor(d, kFront).empty());
    d.frames[1].on = false;
    CHECK(ActiveVariationsFor(d, kBack) == std::vector<int>{0});
}

// 13-54 (Antho 2026-10-14, "each part is a section inside the section"): a divided Silence Sound's
// pre-roll and tail apply around each part's own sound. Step(): heel loud 1.020-1.061, toe loud
// 1.150-1.188 (-45 dB threshold), frame 1.000-1.400, the cut at 1.145.
std::vector<PartSpan> Margins(SoundData d, const PeakData& pk, double pre_ms, double tail_ms)
{
    d.silence.preroll_ms = pre_ms;
    d.silence.tail_ms = tail_ms;
    RefreshParts(d, &pk);
    return PartPieces(d, 0);
}

void TestPartMargins()
{
    PeakData  pk;
    SoundData d = Step(&pk);
    // Combined: exactly as before (the whole frame, no trims stored).
    CHECK(!PartMarginsApply(d));
    RefreshParts(d, &pk);
    CHECK(d.frames[0].part_trims.empty());
    std::vector<PartSpan> c = PartPieces(d, 0);
    CHECK(c.size() == 1 && Near(c[0].start, 1.0) && Near(c[0].length, 0.4));
    CHECK(PlacedSpans(d, 0, "").size() == 1 && Near(PlacedSpans(d, 0, "")[0].length, 0.4));
    SetDivide(d, "g", {kBack, kFront});
    CHECK(PartMarginsApply(d));
    // Tail 0: Back ends with its sound, Front's pre-roll 5 ms before it. 13-62g: the margins apply at the
    // variation's own edges too (Back starts 5 ms before its sound at 1.015, Front ends with its sound at 1.188):
    // a detected frame carries the largest margin, each part takes its own (DetectionSilence).
    std::vector<PartSpan> m = Margins(d, pk, 5.0, 0.0);
    CHECK(m.size() == 2 && m[0].value_uid == kBack && m[1].value_uid == kFront);
    CHECK(Near(m[0].start, 1.015) && Near(m[0].start + m[0].length, 1.061));
    CHECK(Near(m[1].start, 1.145) && Near(m[1].start + m[1].length, 1.188));
    // Tail 300 ms: Back's tail stops at the cut, Front's at the variation's end.
    m = Margins(d, pk, 5.0, 300.0);
    CHECK(m.size() == 2 && Near(m[0].start + m[0].length, 1.145) && Near(m[1].start + m[1].length, 1.4));
    // Tail 50 ms: Back ends 50 ms after its sound.
    m = Margins(d, pk, 5.0, 50.0);
    CHECK(m.size() == 2 && Near(m[0].start + m[0].length, 1.111) && Near(m[1].start, 1.145));
    // A variation stretched by hand (0.9 .. 1.5, its edge handles): its outer pieces' edges are pinned there by
    // hand (13-62g, REAPER's item edge), whatever the margins.
    {
        SoundData h = d;
        h.frames[0].cuts = {1.145};
        h.frames[0].cuts_hand = true;
        TrimFrame(h, 0, 0.9, 0.6, 3.0);
        CHECK(h.frames[0].edge_in.size() == 1 && Near(h.frames[0].edge_in[0], 0.9) && h.frames[0].edge_out.size() == 1 &&
              Near(h.frames[0].edge_out[0], 1.5));
        m = Margins(h, pk, 5.0, 0.0);
        CHECK(m.size() == 2 && Near(m[0].start, 0.9) && Near(m[0].start + m[0].length, 1.061) && Near(m[1].start, 1.145) &&
              Near(m[1].start + m[1].length, 1.5));
    }
    // 13-62g (Antho 2026-10-14, "if there is a cut, it is a section of its own"; replaces 13-58's crossing): a
    // margin never crosses a cut, either way. 13-62g review (a part's Pre-roll must act): RAV's cut sits the later
    // part's Pre-roll before its onset (rise 1.150), never before the earlier part's sound end (1.061) plus its Tail;
    // when both do not fit, they share the silence (the cut at its middle, 1.1055).
    m = Margins(d, pk, 50.0, 300.0);
    CHECK(m.size() == 2 && Near(m[0].start, 1.0) && Near(m[0].start + m[0].length, 1.1055) && Near(m[1].start, 1.1055));
    m = Margins(d, pk, 300.0, 300.0);
    CHECK(m.size() == 2 && Near(m[0].start, 1.0) && Near(m[0].start + m[0].length, 1.1055) && Near(m[1].start, 1.1055) &&
          Near(m[1].start + m[1].length, 1.4));
    m = Margins(d, pk, 50.0, 50.0);  // Back's tail fits: the cut right after it, Front's pre-roll up to it
    CHECK(m.size() == 2 && Near(m[0].start + m[0].length, 1.111) && Near(m[1].start, 1.111));
    m = Margins(d, pk, 50.0, 0.0);  // room for both: Front's full 50 ms
    CHECK(m.size() == 2 && Near(m[0].start + m[0].length, 1.061) && Near(m[1].start, 1.10));
    m = Margins(d, pk, 300.0, 0.0);
    CHECK(m.size() == 2 && Near(m[0].start + m[0].length, 1.061) && Near(m[1].start, 1.061));
    m = Margins(d, pk, 0.0, 300.0);  // a pre-roll under 5 ms keeps the cut 5 ms before the rise
    CHECK(m.size() == 2 && Near(m[0].start + m[0].length, 1.145) && Near(m[1].start, 1.15));
    {   // A hand cut stays where it is (only RAV's cuts follow the pre-roll).
        SoundData hc = d;
        hc.frames[0].cuts = {1.145};
        hc.frames[0].cuts_hand = true;
        m = Margins(hc, pk, 50.0, 300.0);
        CHECK(m.size() == 2 && Near(m[0].start + m[0].length, 1.145) && Near(m[1].start, 1.145));
    }
    // Stored with the frame: Front's sync on its attack, inside its piece; the placement plays the piece. (The cut
    // held by hand at 1.145 from here on: these checks are about the syncs and the trims around a fixed cut.)
    SoundData s = d;
    s.silence.preroll_ms = 50.0;
    s.silence.tail_ms = 0.0;
    s.frames[0].cuts = {1.145};
    s.frames[0].cuts_hand = true;
    RefreshParts(s, &pk);
    CHECK(s.frames[0].part_trims.size() == 4 && !SyncsStale(s, 0));
    const std::vector<PartSpan> front = PlacedSpans(s, 0, kFront);
    CHECK(front.size() == 1 && Near(front[0].start, 1.145) && Near(front[0].start + front[0].length, 1.188));
    CHECK(Near(SyncFor(s, 0, kFront), 1.148));
    // A whole (combined event) placement keeps the whole variation.
    CHECK(PlacedSpans(s, 0, "").size() == 1 && Near(PlacedSpans(s, 0, "")[0].length, 0.4));
    // Stale trims (lost, or no longer fitting the spans) read as the raw spans and ask for a measure.
    SoundData st = s;
    st.frames[0].part_trims.clear();
    CHECK(SyncsStale(st, 0) && Near(PlacedSpans(st, 0, kFront)[0].start, 1.145));
    CHECK(MeasureSyncs(st, 0, &pk) && st.frames[0].part_trims == s.frames[0].part_trims && !SyncsStale(st, 0));
    st.frames[0].part_trims = {1.0, 1.3, 1.2, 1.4};  // overlapping
    CHECK(SyncsStale(st, 0) && Near(PlacedSpans(st, 0, kBack)[0].length, 0.145));
    // A moved cut drops them (measured again by RefreshOne).
    st = s;
    CHECK(MoveCut(st, 0, 0, 1.13) && st.frames[0].part_trims.empty());
    // Without peaks the stored ones stay while they fit.
    st = s;
    RefreshParts(st, nullptr);
    CHECK(st.frames[0].part_trims == s.frames[0].part_trims);
    // Review: stored with the key they were measured with. Another tail, threshold or pre-roll, or no key
    // (an older build): stale, the raw spans until measured again; without peaks they are dropped.
    CHECK(!s.frames[0].part_trim_key.empty() && !PartTrimsStale(s, 0));
    st = s;
    st.silence.tail_ms = 300.0;
    CHECK(PartTrimsStale(st, 0) && SyncsStale(st, 0) && Near(PlacedSpans(st, 0, kFront)[0].start, 1.145));
    RefreshParts(st, nullptr);
    CHECK(st.frames[0].part_trims.empty());
    st = s;
    st.silence.threshold_db = -30.0;
    CHECK(PartTrimsStale(st, 0));
    st = s;
    st.frames[0].part_trim_key.clear();
    CHECK(PartTrimsStale(st, 0) && SyncsStale(st, 0));
    // 13-58 / 13-62g: a key from an older rule (same settings, no "r3," version): stale, measured again.
    st = s;
    CHECK(st.frames[0].part_trim_key.compare(0, 3, "r3,") == 0);
    st.frames[0].part_trim_key.erase(0, 3);
    CHECK(PartTrimsStale(st, 0) && SyncsStale(st, 0));
    CHECK(MeasureSyncs(st, 0, &pk) && !PartTrimsStale(st, 0) && st.frames[0].part_trim_key == s.frames[0].part_trim_key);
    // A cut written elsewhere (an older RAV keeps ptrim as read) changes the spans: stale.
    st = s;
    st.frames[0].cuts = {1.146};
    CHECK(PartTrimsStale(st, 0));
    // The file they were measured on: the same version is fresh, another one (re-rendered) is stale; an
    // unknown version (no file to compare with) is not.
    {
        PeakData pv = pk;
        pv.file.time = 100;
        pv.file.size = 2000;
        SoundData sv = s;
        RefreshParts(sv, &pv);
        FileVersion same, other;
        same.time = 100;
        same.size = 2000;
        other.time = 101;
        other.size = 2000;
        CHECK(!PartTrimsStale(sv, 0, &same) && PartTrimsStale(sv, 0, &other) && !PartTrimsStale(sv, 0) &&
              !PartTrimsStale(sv, 0, nullptr) && !PartTrimsStale(sv, 0, &pk.file));
        CHECK(MeasureSyncs(sv, 0, &pv) && !PartTrimsStale(sv, 0, &same));
    }
    // Review: a hand sync belongs to the part owning its spot by the cut (pre-roll 50 ms, tail 0: Back
    // plays to 1.061, Front from the cut at 1.145). Back's hand sync at 1.12 (in Front's pre-roll) stays Back's, clamped to
    // the end of Back's piece; Front keeps its own attack.
    {
        SoundData hs = s;  // tail 0 (13-58: with a long tail Back's zone would reach the cut, Front's pre-roll 0)
        hs.frames[0].hand_syncs = {1.12};
        RefreshParts(hs, &pk);
        CHECK(hs.frames[0].syncs.size() == 2 && Near(hs.frames[0].syncs[0], 1.061, 1e-5) && hs.frames[0].syncs[0] < 1.061 &&
              Near(hs.frames[0].syncs[1], 1.148) && !SyncsStale(hs, 0));
        // Front's sync dragged into its pre-roll (Back's spot by the cut): kept inside Front's own spans,
        // Back's hand sync not taken away.
        CHECK(MoveSync(hs, 0, 1, 1.11) && hs.frames[0].hand_syncs.size() == 2 && Near(hs.frames[0].syncs[1], 1.145, 1e-5) &&
              hs.frames[0].syncs[1] > 1.145);
        RefreshParts(hs, &pk);
        CHECK(Near(hs.frames[0].syncs[0], 1.061, 1e-5) && Near(hs.frames[0].syncs[1], 1.145, 1e-5));
        // Back's sync moved: only Back's hand sync is replaced.
        CHECK(MoveSync(hs, 0, 0, 1.05) && hs.frames[0].hand_syncs.size() == 2 && Near(hs.frames[0].syncs[0], 1.05));
        RefreshParts(hs, &pk);
        CHECK(Near(hs.frames[0].syncs[0], 1.05) && Near(hs.frames[0].syncs[1], 1.145, 1e-5));
        // The cut moved past Back's hand sync: that spot is Front's now, Front keeps its own (first) one.
        CHECK(MoveCut(hs, 0, 0, 1.03));
        RefreshParts(hs, &pk);
        CHECK(hs.frames[0].syncs.size() == 2 && Near(hs.frames[0].syncs[1], 1.145, 1e-5) && hs.frames[0].syncs[0] < 1.03);
    }
    // A part at the file's end: its tail stops there (the frame ends with the file).
    {
        PeakData  e = Quiet(3.0);
        SoundData d2;
        d2.mode = DetectMode::Silence;
        d2.frames = {F(2.6, 0.4)};
        Hit(e, 2.620, -4.0);
        Hit(e, 2.850, -7.0);
        SetDivide(d2, "g", {kBack, kFront});
        const std::vector<PartSpan> me = Margins(d2, e, 5.0, 300.0);
        CHECK(me.size() == 2 && Near(me[1].start + me[1].length, 3.0) && Near(me[0].start + me[0].length, 2.845));
    }
    // An excluded zone inside Back's tail (13-62g): it takes its own stretch away and nothing more; Back's tail
    // goes on after it up to the cut, Front's pre-roll stops at the cut.
    {
        SoundData z = d;
        z.frames[0].cuts = {1.145};  // held by hand (RAV's would follow Front's 50 ms pre-roll)
        z.frames[0].cuts_hand = true;
        CHECK(AddZone(z, 1.08, 0.04, ""));
        const std::vector<PartSpan> mz = Margins(z, pk, 50.0, 300.0);
        CHECK(mz.size() == 3 && mz[0].value_uid == kBack && Near(mz[0].start, 1.0) && Near(mz[0].start + mz[0].length, 1.08));
        CHECK(mz[1].value_uid == kBack && Near(mz[1].start, 1.12) && Near(mz[1].start + mz[1].length, 1.145));
        CHECK(mz[2].value_uid == kFront && Near(mz[2].start, 1.145) && Near(mz[2].start + mz[2].length, 1.4));
        SoundData zz = z;
        zz.silence.preroll_ms = 50.0;
        zz.silence.tail_ms = 300.0;
        RefreshParts(zz, &pk);
        CHECK(PlacedSpans(zz, 0, kBack).size() == 2 && !SyncsStale(zz, 0));
    }
    // A part quiet all along keeps its spans as they are (still placed), its neighbour stops at it.
    {
        PeakData  q = Quiet(3.0);
        Hit(q, 1.020, -4.0);
        SoundData d3;
        d3.mode = DetectMode::Silence;
        d3.frames = {F(1.0, 0.4)};
        SetDivide(d3, "g", {kBack, kFront});
        d3.frames[0].cuts = {1.2};
        d3.frames[0].cuts_hand = true;
        const std::vector<PartSpan> mq = Margins(d3, q, 5.0, 300.0);
        CHECK(mq.size() == 2 && Near(mq[0].start + mq[0].length, 1.2) && Near(mq[1].start, 1.2) &&
              Near(mq[1].start + mq[1].length, 1.4));
    }
    // Not Silence detection (take markers): the parts as cut, untouched.
    {
        SoundData tm = d;
        tm.mode = DetectMode::TakeMarkers;
        RefreshParts(tm, &pk);
        CHECK(!PartMarginsApply(tm) && tm.frames[0].part_trims.empty() && Near(PlacedSpans(tm, 0, kBack)[0].length, 0.145));
    }
}

void TestHandMarks()
{
    PeakData  pk;
    SoundData d = Step(&pk);
    SetDivide(d, "g", {kBack, kFront});
    RefreshParts(d, &pk);
    // Move the cut by hand: clamped inside the frame, kept by a refresh.
    CHECK(MoveCut(d, 0, 0, 1.6) && Near(d.frames[0].cuts[0], 1.399) && d.frames[0].cuts_hand);
    CHECK(MoveCut(d, 0, 0, 1.17) && !MoveCut(d, 0, 0, 1.17) && !MoveCut(d, 0, 1, 1.2) && !MoveCut(d, 2, 0, 1.2));
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].cuts[0], 1.17) && d.frames[0].cuts_hand && !d.frames[0].cut_unsure);
    // Move Front's sync by hand: clamped into Front, wins over the attack, kept by a refresh.
    CHECK(MoveSync(d, 0, 1, 1.1) && Near(d.frames[0].hand_syncs.back(), 1.17, 1e-5) && d.frames[0].hand_syncs.back() > 1.17);
    CHECK(MoveSync(d, 0, 1, 1.2) && d.frames[0].hand_syncs.size() == 1 && Near(d.frames[0].syncs[1], 1.2));
    CHECK(!MoveSync(d, 0, 2, 1.2) && !MoveSync(d, 1, 0, 1.2));
    // Review: a sync dragged onto the cut belongs to one part only; Back's does not take or erase Front's.
    {
        SoundData c = d;
        CHECK(MoveSync(c, 0, 1, 1.1) && MoveSync(c, 0, 0, 1.3) && c.frames[0].hand_syncs.size() == 2);
        RefreshParts(c, &pk);
        CHECK(Near(c.frames[0].syncs[0], 1.17, 1e-5) && c.frames[0].syncs[0] < 1.17);
        CHECK(Near(c.frames[0].syncs[1], 1.17, 1e-5) && c.frames[0].syncs[1] > 1.17);
    }
    d.sync_offset_ms = -4.0;
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[1], 1.2) && Near(d.frames[0].syncs[0], 1.016));

    // Threshold change (matrix): a new detection splits the frame in two; the hand sync goes to the frame
    // now holding 1.2, the hand cut stays when it falls inside one new frame.
    std::vector<SoundFrame> now = {F(1.0, 0.1), F(1.12, 0.28)};
    CarryHandMarks(now, d.frames);
    CHECK(now[0].hand_syncs.empty() && now[1].hand_syncs == std::vector<double>{1.2});
    CHECK(now[1].cuts_hand && now[1].cuts == std::vector<double>{1.17} && !now[0].cuts_hand);
    // A hand cut whose cuts fall in two frames is not carried.
    SoundData three = d;
    SetDivide(three, "g", {kBack, kFront, "v0000000000000c1"});
    three.frames[0].cuts = {1.05, 1.3};
    three.frames[0].cuts_hand = true;
    std::vector<SoundFrame> split = {F(1.0, 0.1), F(1.12, 0.28)};
    CarryHandMarks(split, three.frames);
    CHECK(!split[0].cuts_hand && !split[1].cuts_hand);
    // Marks `now` held are replaced (a hand frame copied by MergeHandFrames does not double them).
    std::vector<SoundFrame> same = d.frames;
    CarryHandMarks(same, d.frames);
    CHECK(same[0].hand_syncs.size() == 1 && same[0].cuts_hand);

    // Reset to detected (A7): the hand sync stays, the excluded zone stays, the hand cut and the
    // "-> value" zone go.
    CHECK(AddZone(d, 1.3, 0.02, ""));
    CHECK(AddZone(d, 1.05, 0.02, kFront));
    d.edited = true;
    ResetToDetected(d, {F(1.0, 0.4)});
    CHECK(!d.edited && d.frames.size() == 1 && d.frames[0].hand_syncs == std::vector<double>{1.2});
    CHECK(!d.frames[0].cuts_hand && d.zones.size() == 1 && d.zones[0].to.empty());
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].cuts[0], 1.145) && Near(d.frames[0].syncs[1], 1.2));
    // Reset sync point / cuts of one variation.
    CHECK(ResetSyncs(d, 0) && !ResetSyncs(d, 0) && !ResetCuts(d, 0));
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[1], 1.146));  // the toe's attack, -4 ms
    CHECK(MoveCut(d, 0, 0, 1.16) && ResetCuts(d, 0) && d.frames[0].cuts.empty());
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].cuts[0], 1.145) && !d.frames[0].cuts_hand);
    // A stale sync (outside its part after a hand cut) is reported.
    d.frames[0].cuts[0] = 1.3;
    CHECK(SyncsStale(d, 0));
}

void TestSetDivideAndRoles()
{
    SoundStore  st;
    SwitchGroup g;
    g.uid = "g";
    g.name = "Contact";
    g.values = {{kBack, "Back", {}, {"heel"}}, {kFront, "Front", {}, {"toe", "toe_end"}}, {"v0000000000000z1", "Ball", {}, {"ball"}}};
    st.groups.push_back(g);
    SoundData d;
    d.frames = {F(0, 1)};
    CHECK(PartOfRole(st, d, "heel").empty());  // combined: the whole variation
    d.zones = {{0.1, 0.1, "", {}}, {0.3, 0.1, "v0000000000000z1", {}}, {0.5, 0.1, kFront, {}}};
    d.role_map = {{"hand", kFront}, {"heel", "v0000000000000z1"}};
    d.frames[0].cuts = {0.5};
    d.frames[0].syncs = {0.1, 0.6};
    d.frames[0].hand_syncs = {0.62};
    CHECK(SetDivide(d, "g", {kBack, kFront}));
    // The "-> Ball" zone and the role entry on Ball go; the excluded zone, the Front zone and the hand
    // sync stay; cuts and syncs are measured again.
    CHECK(d.zones.size() == 2 && d.zones[0].to.empty() && d.zones[1].to == kFront);
    CHECK(d.role_map.size() == 1 && d.role_map[0].role == "hand");
    CHECK(d.frames[0].cuts.empty() && d.frames[0].syncs.empty() && d.frames[0].hand_syncs.size() == 1);
    // The Sound's own map wins whole: hand -> Front, heel (not listed) -> the whole variation.
    CHECK(PartOfRole(st, d, "hand") == kFront && PartOfRole(st, d, "heel").empty() && PartOfRole(st, d, "").empty());
    // The group's map when the Sound has none.
    d.role_map.clear();
    CHECK(PartOfRole(st, d, "heel") == kBack && PartOfRole(st, d, "toe_end") == kFront && PartOfRole(st, d, "hand").empty());
    // A value the Sound is not divided by (Ball): the whole variation.
    CHECK(PartOfRole(st, d, "ball").empty());
    // An override of heel -> Front for this Sound only.
    d.role_map = {{"heel", kFront}};
    CHECK(PartOfRole(st, d, "heel") == kFront);
    // Back to combined: every part setting goes.
    CHECK(SetDivide(d, "", {kBack}) && d.divide_values.empty() && d.role_map.empty() && d.zones.size() == 1);
}

// Part C: the Edit window's counts and its role gestures.
void TestEditCounts()
{
    SoundData d;
    d.frames = {F(0, 1), F(2, 1), F(4, 1)};
    d.frames[2].on = false;
    CHECK(ExcludedZoneCount(d) == 0 && ExcludedStretchCount(d) == 1 && VariationsWithPart(d, "") == 2);
    d.zones = {{0.2, 0.1, "", {}}, {0.9, 0.5, "", {}}, {2.5, 0.1, kFront, {}}};
    // Frame 0 loses [0.2, 0.3] and [0.9, 1]: two stretches; frame 1 only has a "-> Front" zone; frame 2 is off.
    CHECK(ExcludedZoneCount(d) == 2 && ExcludedStretchCount(d) == 3);
    CHECK(SetDivide(d, "g", {kBack, kFront}));
    d.frames[0].cuts = {0.5};
    d.frames[1].cuts = {2.7};
    d.frames[1].cut_unsure = true;
    d.frames[0].cuts_hand = true;
    d.frames[2].cut_unsure = true;  // off: not counted
    d.frames[2].cuts = {4.5};
    d.frames[2].cuts_hand = true;   // off: not counted
    CHECK(HandCutCount(d) == 1 && UnsureCutCount(d) == 1);
    CHECK(VariationsWithPart(d, kBack) == 2 && VariationsWithPart(d, kFront) == 2);
    // The whole Back part of frame 1 excluded: Back is in frame 0 only.
    d.zones.push_back({2.0, 0.7, "", {}});
    CHECK(VariationsWithPart(d, kBack) == 1);
    // Sync points moved by hand: inside the part's spans only.
    d.frames[0].hand_syncs = {0.6};
    d.frames[1].hand_syncs = {2.1, 2.8};
    CHECK(HandSyncCount(d) == 3);
    d.frames[1].hand_syncs.clear();
    CHECK(HandSyncCount(d) == 1 && SyncIsHand(d, 0, 1) && !SyncIsHand(d, 0, 0) && !SyncIsHand(d, 1, 1));
    CHECK(!SyncIsHand(d, 7, 0) && !SyncIsHand(d, 0, 2));
}

void TestRoleToggles()
{
    SoundStore  st;
    SwitchGroup g;
    g.uid = "g";
    g.name = "Contact";
    g.values = {{kBack, "Back", {}, {"heel"}}, {kFront, "Front", {}, {"toe", "toe_end"}}};
    st.groups.push_back(g);
    // The group's map: a role belongs to one value; turning it on elsewhere moves it.
    std::string from;
    CHECK(ToggleGroupRole(st, "g", kBack, "toe", &from) && from == kFront);
    CHECK(st.groups[0].values[0].roles == std::vector<std::string>({"heel", "toe"}) &&
          st.groups[0].values[1].roles == std::vector<std::string>({"toe_end"}));
    CHECK(ToggleGroupRole(st, "g", kBack, "toe", &from) && from.empty() && st.groups[0].values[0].roles.size() == 1);
    CHECK(ToggleGroupRole(st, "g", kFront, "hand", &from) && from.empty() && st.groups[0].values[1].roles.back() == "hand");
    CHECK(!ToggleGroupRole(st, "g", "nope", "hand") && !ToggleGroupRole(st, "x", kBack, "hand") &&
          !ToggleGroupRole(st, "g", kBack, ""));

    SoundData d;
    d.frames = {F(0, 1)};
    CHECK(!ToggleSoundRole(st, d, kBack, "heel"));  // combined
    CHECK(SetDivide(d, "g", {kBack, kFront}));
    bool own = true;
    std::vector<std::vector<std::string>> by = RolesByValue(st, d, &own);
    CHECK(!own && by.size() == 2 && by[0] == std::vector<std::string>({"heel"}) && by[1].size() == 2);
    CHECK(!DivideStale(st, d));
    // This sound: heel -> Front, seeded from the group's (Back: heel; Front: toe end, hand) so the others
    // keep their part.
    CHECK(ToggleSoundRole(st, d, kFront, "heel"));
    by = RolesByValue(st, d, &own);
    CHECK(own && by[0].empty() && by[1].size() == 3);
    CHECK(PartOfRole(st, d, "heel") == kFront && PartOfRole(st, d, "toe_end") == kFront && PartOfRole(st, d, "hand") == kFront &&
          PartOfRole(st, d, "toe").empty());
    CHECK(st.groups[0].values[0].roles == std::vector<std::string>({"heel"}));  // the group is untouched
    // Every role taken off: the Sound keeps its own (empty) map, never falls back on the group's.
    for (const char* r : {"heel", "toe_end", "hand"}) CHECK(ToggleSoundRole(st, d, kFront, r));
    by = RolesByValue(st, d, &own);
    CHECK(own && by[0].empty() && by[1].empty() && PartOfRole(st, d, "heel").empty());
    // The mark survives the record.
    {
        SoundStore    s2 = st;
        SoundLibrary  lib;
        lib.uid = "b000000000000001";
        lib.name = "L";
        lib.root = "c000000000000001";
        SoundNode n;
        n.uid = "c000000000000001";
        n.type = SoundNodeType::Sound;
        n.name = "S";
        n.sound = d;
        lib.nodes.push_back(n);
        s2.libraries.push_back(lib);
        SoundStore back;
        CHECK(ParseSoundStore(SerializeSoundStore(s2), &back) && back.libraries.size() == 1 &&
              back.libraries[0].nodes.size() == 1);
        if (back.libraries.size() == 1 && back.libraries[0].nodes.size() == 1) {
            const SoundData& b = back.libraries[0].nodes[0].sound;
            bool o2 = false;
            RolesByValue(back, b, &o2);
            CHECK(o2 && b.role_map.size() == 1 && PartOfRole(back, b, "heel").empty());
        }
    }
    CHECK(ToggleSoundRole(st, d, kBack, "heel") && d.role_map.size() == 1 && PartOfRole(st, d, "heel") == kBack);
    // A value not divided by, and the mark itself, are refused.
    CHECK(!ToggleSoundRole(st, d, "v0000000000000z1", "heel") && !ToggleSoundRole(st, d, kBack, kNoRolesMark));
    // The group's values changed after dividing: the amber note.
    st.groups[0].values.push_back({"v0000000000000z1", "Ball", {}, {}});
    CHECK(DivideStale(st, d));
    st.groups.clear();
    CHECK(DivideStale(st, d));
}

}  // namespace

// ---- 13-62: per-part settings and comping (spec 13-62 I/O matrix, Lot A rows) ---------------------------

// A divided footstep (Back | Front, the cut at 1.145) measured on the peaks.
SoundData Divided(PeakData* pk)
{
    SoundData d = Step(pk);
    SetDivide(d, "g", {kBack, kFront});
    RefreshParts(d, pk);
    return d;
}

PartSettings Patch()
{
    return PartSettings();
}

void TestPartSettings()
{
    PeakData  pk;
    SoundData d = Divided(&pk);
    // No settings of its own: every part reads the Sound's.
    CHECK(d.parts.empty() && PartSettingsOf(d, kFront) == nullptr && OwnSettingCount(d, kFront) == 0);
    CHECK(PartGainDb(d, kFront) == 0.0 && PartTailMs(d, kFront) == d.silence.tail_ms && PartMono(d, kFront) == d.mono);
    CHECK(PartSyncMode(d, kFront) == d.sync_mode && PartSyncOffsetMs(d, kFront) == d.sync_offset_ms);
    // Matrix "Part gain": Toe -3 dB on top of what the part plays at (FrameGainDb: its per-part Normalize
    // gain, else the variation dB; 13-28 keeps part_db absolute): variation +2 -> -1; part_db +1 -> -2.
    PartSettings g = Patch();
    g.gain_db = -3.0;
    SetPartSetting(d, kFront, g);
    d.frames[0].db = 2.0;
    CHECK(PartGainDb(d, kFront) == -3.0 && PartGainDb(d, kBack) == 0.0 && PartGainDb(d, "") == 0.0);
    CHECK(Near(FrameGainDb(d.frames[0], kFront) + PartGainDb(d, kFront), -1.0));
    d.frames[0].part_db[kFront] = 1.0;
    CHECK(Near(FrameGainDb(d.frames[0], kFront) + PartGainDb(d, kFront), -2.0));
    CHECK(OwnSettingCount(d, kFront) == 1 && d.parts.size() == 1 && d.parts[0].value == kFront);
    // Not a divide value, or the Sound itself: nothing written.
    SetPartSetting(d, "elsewhere", g);
    SetPartSetting(d, "", g);
    CHECK(d.parts.size() == 1);
    // Matrix "Part follows Sound": Heel with no Tail of its own follows the Sound's; Toe keeps its own.
    d.silence.tail_ms = 100.0;
    PartSettings t = Patch();
    t.tail_ms = 40.0;
    SetPartSetting(d, kFront, t);
    CHECK(PartTailMs(d, kBack) == 100.0 && PartTailMs(d, kFront) == 40.0);
    d.silence.tail_ms = 60.0;
    CHECK(PartTailMs(d, kBack) == 60.0 && PartTailMs(d, kFront) == 40.0 && OwnSettingCount(d, kFront) == 2);
    // One field back to the Sound's; "All" back to the Sound's (matrix "Revert": the entry goes).
    ResetPartSetting(d, kFront, PartField::Gain);
    CHECK(PartGainDb(d, kFront) == 0.0 && PartTailMs(d, kFront) == 40.0 && OwnSettingCount(d, kFront) == 1);
    SetPartSetting(d, kFront, g);
    CHECK(OwnSettingCount(d, kFront) == 2);
    ResetPartAll(d, kFront);
    CHECK(d.parts.empty() && PartGainDb(d, kFront) == 0.0 && PartTailMs(d, kFront) == 60.0);
    ResetPartSetting(d, kFront, PartField::Tail);  // nothing to reset: no entry made
    CHECK(d.parts.empty());
    // Matrix "Part fades own": Toe's own 4 ms fade in, the other side the Sound's default; Heel has none.
    PartSettings f = Patch();
    f.fade_in = 0.004;
    SetPartSetting(d, kFront, f);
    double in = -1.0, out = -1.0;
    CHECK(PartFades(d, kFront, &in, &out) && in == 0.004 && out == d.fade_out);
    CHECK(!PartFades(d, kBack, &in, &out) && in == d.fade_in && out == d.fade_out);
    // Mono, Normalize mode / target (kept in range), non-finite values ignored.
    PartSettings m = Patch();
    m.mono = MonoMode::Left;
    m.norm_mode = static_cast<int>(NormMode::LufsI);
    m.norm_target = -100.0;
    m.preroll_ms = std::nan("");
    SetPartSetting(d, kFront, m);
    const PartSettings* fp = PartSettingsOf(d, kFront);
    CHECK(fp && PartMono(d, kFront) == MonoMode::Left && PartMono(d, kBack) == MonoMode::Off);
    CHECK(fp && fp->norm_mode == static_cast<int>(NormMode::LufsI) && fp->norm_target == kNormTargetMin && !fp->preroll_ms);
    m = Patch();
    m.norm_mode = kNormModeCount;
    SetPartSetting(d, kFront, m);
    CHECK(PartSettingsOf(d, kFront)->norm_mode == static_cast<int>(NormMode::LufsI) && OwnSettingCount(d, kFront) == 4);
    // Matrix "Divide by changed": every part setting and owner cleared (one gesture).
    CHECK(AssignRange(d, 0, 1.3, 1.4, kBack) && !d.frames[0].owners.empty());
    CHECK(SetDivide(d, "g2", {kBack, kFront}) && d.parts.empty() && d.frames[0].owners.empty());
}

void TestPartSyncAndMargins()
{
    PeakData  pk;
    SoundData d = Divided(&pk);
    CHECK(Near(d.frames[0].syncs[0], 1.018) && Near(d.frames[0].syncs[1], 1.148));
    // Toe's own sync offset (0 ms): its sync moves, Heel's stays; the measured syncs go until measured again.
    PartSettings s;
    s.sync_offset_ms = 0.0;
    SetPartSetting(d, kFront, s);
    CHECK(d.frames[0].syncs.empty());
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[0], 1.018) && Near(d.frames[0].syncs[1], 1.150));
    // Toe's own Peak mode: AutoSync reads the spans' part.
    PartSettings pm;
    pm.sync_mode = SyncMode::SliceStart;
    SetPartSetting(d, kFront, pm);
    CHECK(PartSyncMode(d, kFront) == SyncMode::SliceStart && PartSyncMode(d, kBack) == SyncMode::Attack);
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[0], 1.018) && Near(d.frames[0].syncs[1], PlacedSpans(d, 0, kFront)[0].start));
    ResetPartAll(d, kFront);
    RefreshParts(d, &pk);
    CHECK(Near(d.frames[0].syncs[1], 1.148));
    // Per-part margins (each section with its own part's pre-roll / tail): tail 0, Toe's own 50 ms pre-roll
    // (13-62g review: RAV's cut moves 50 ms before Toe's rise, 1.100) while Heel's ends with its sound; the Sound's
    // pre-roll is 5 ms.
    d.silence.preroll_ms = 5.0;
    d.silence.tail_ms = 0.0;
    RefreshParts(d, &pk);
    const std::string key0 = PartTrimKey(d, 0, FileVersion{});
    CHECK(Near(PartPieces(d, 0)[1].start, 1.145));
    PartSettings gain;
    gain.gain_db = 1.0;  // not a margin: the key does not change
    SetPartSetting(d, kBack, gain);
    CHECK(PartTrimKey(d, 0, FileVersion{}) == key0 && !d.frames[0].part_trims.empty());
    PartSettings pre;
    pre.preroll_ms = 50.0;
    SetPartSetting(d, kFront, pre);
    CHECK(d.frames[0].part_trims.empty() && PartTrimKey(d, 0, FileVersion{}) != key0);
    RefreshParts(d, &pk);
    std::vector<PartSpan> m = PartPieces(d, 0);
    CHECK(m.size() == 2 && Near(m[0].start + m[0].length, 1.061) && Near(m[1].start, 1.10) && Near(d.frames[0].cuts[0], 1.10) &&
          !PartTrimsStale(d, 0));
    // Heel's own tail 50 ms: Heel ends at 1.111, the cut after it (13-62g: neither crosses the cut; Heel's Tail first,
    // Toe's pre-roll gets what is left: 39 ms).
    PartSettings tail;
    tail.tail_ms = 50.0;
    SetPartSetting(d, kBack, tail);
    RefreshParts(d, &pk);
    m = PartPieces(d, 0);
    CHECK(m.size() == 2 && Near(m[0].start + m[0].length, 1.111) && Near(m[1].start, 1.111));
    // A stored key without the parts' margins is stale.
    SoundData stale = d;
    stale.parts.clear();
    CHECK(PartTrimsStale(stale, 0));
    // The red dB label counts the part's own gain: Toe at -7 dBFS + 8 dB clips.
    SoundData clip = Divided(&pk);
    CHECK(!FrameClips(clip, 0, pk));
    PartSettings loud;
    loud.gain_db = 8.0;
    SetPartSetting(clip, kFront, loud);
    CHECK(FrameClips(clip, 0, pk));
}

// 13-62 review (lot E1): the swipe snap over every variation, the "nothing changes" check, the comp click's
// decision, the owners exposed for the first cut drag.
void TestCompGestures()
{
    PeakData  pk;
    SoundData d = Divided(&pk);
    d.mode = DetectMode::TakeMarkers;
    RefreshParts(d, &pk);
    // SnapSpanEdge: a cut, a frame edge, a zone edge, else t unchanged.
    CHECK(Near(SnapSpanEdge(d, 1.143, 0.005), 1.145) && Near(SnapSpanEdge(d, 0.998, 0.005), 1.0));
    CHECK(Near(SnapSpanEdge(d, 1.25, 0.005), 1.25) && Near(SnapSpanEdge(d, 1.25, 0.0), 1.25));
    {
        SoundData z = d;
        CHECK(ExcludeRange(z, 0, 1.2, 1.25));
        CHECK(Near(SnapSpanEdge(z, 1.248, 0.005), 1.25) && Near(SnapSpanEdge(z, 1.203, 0.005), 1.2));
    }
    // SameEditPieces: a write that changes only hidden state (owners filled as shown) is "the same".
    {
        SoundData o = d;
        CHECK(SameEditPieces(o, d));
        o.frames[0].owners = LegacyOwners(o, o.frames[0]);
        CHECK(!OwnersSane(d, d.frames[0]) && OwnersSane(o, o.frames[0]) && SameEditPieces(o, d));
        CHECK(AssignRange(o, 0, 1.3, 1.4, kBack) && !SameEditPieces(o, d));
        SoundData x = d;
        CHECK(ExcludeRange(x, 0, 1.2, 1.25) && !SameEditPieces(x, d));
    }
    // PlanCompClick. A Split lane on another part's range: select the lane, take the range.
    CompClickPlan c = PlanCompClick(d, 0, 1.2, kBack, false, false);
    CHECK(c.on_piece && c.piece.index == 1 && c.set_part && c.part == kBack && c.take);
    // Its own range: selected, not taken (it plays); with Ctrl: never taken.
    c = PlanCompClick(d, 0, 1.2, kFront, false, false);
    CHECK(c.on_piece && c.set_part && c.part == kFront && !c.take);
    CHECK(!PlanCompClick(d, 0, 1.2, kBack, true, false).take);
    // An excluded range on a lane: taken (included again).
    {
        SoundData x = d;
        CHECK(ExcludeRange(x, 0, 1.2, 1.25));
        c = PlanCompClick(x, 0, 1.22, kFront, false, false);
        CHECK(c.on_piece && c.piece.excluded && c.take && c.part == kFront);
        // Combined / the strip on it: the part kept (nothing to select).
        c = PlanCompClick(x, 0, 1.22, "", false, false);
        CHECK(c.on_piece && !c.set_part && !c.take);
    }
    // Combined / the strip: the part under the click; outside every variation: the Sound.
    c = PlanCompClick(d, 0, 1.05, "", false, false);
    CHECK(c.on_piece && c.set_part && c.part == kBack && !c.take);
    c = PlanCompClick(d, -1, 5.0, "", false, true);
    CHECK(!c.on_piece && c.set_part && c.part.empty() && !c.take);
    // A lane click outside the variations: the lane selected, nothing taken.
    c = PlanCompClick(d, -1, 5.0, kBack, false, true);
    CHECK(!c.on_piece && c.set_part && c.part == kBack && !c.take);
}

void TestComping()
{
    PeakData  pk;
    SoundData d = Divided(&pk);
    d.mode = DetectMode::TakeMarkers;  // no 13-54 margins: the pieces as cut
    RefreshParts(d, &pk);
    // Legacy (no owners): the pieces between cuts, unmerged, in time order.
    std::vector<EditPiece> p = EditPiecesOf(d, 0);
    CHECK(p.size() == 2 && p[0].owner == kBack && p[1].owner == kFront && !p[0].excluded && p[1].index == 1);
    CHECK(Near(p[1].start, 1.145) && Near(p[1].length, 0.255) && d.frames[0].owners.empty());
    CHECK(EditPiecesOf(d, 3).empty());
    // Under 4 ms, not a divide value, Takes mode: nothing.
    CHECK(!AssignRange(d, 0, 1.2, 1.203, kBack) && !AssignRange(d, 0, 1.2, 1.3, "elsewhere") && !AssignRange(d, 0, 1.2, 1.3, ""));
    {
        SoundData tk = d;
        tk.mode = DetectMode::Takes;
        CHECK(!AssignRange(tk, 0, 1.2, 1.3, kBack));
    }
    // A swipe over the lane's own part changes nothing: no cut, no owners, no undo point.
    CHECK(!AssignRange(d, 0, 1.2, 1.3, kFront) && d.frames[0].cuts.size() == 1 && d.frames[0].owners.empty());
    // Matrix "Two ranges one part": Heel, Toe, Heel. The Heel event places 2 items from Heel's sync point.
    CHECK(AssignRange(d, 0, 1.3, 1.5, kBack));  // clipped to the frame's end: one new cut
    CHECK(d.frames[0].cuts.size() == 2 && Near(d.frames[0].cuts[1], 1.3) && d.frames[0].cuts_hand);
    CHECK((d.frames[0].owners == std::vector<std::string>{kBack, kFront, kBack}));
    RefreshParts(d, &pk);  // a comped variation keeps its cut count
    CHECK(d.frames[0].cuts.size() == 2 && d.frames[0].owners.size() == 3);
    std::vector<PartSpan> heel = PlacedSpans(d, 0, kBack);
    CHECK(heel.size() == 2 && Near(heel[0].start, 1.0) && Near(heel[1].start, 1.3) && Near(heel[1].length, 0.1));
    CHECK(Near(SyncFor(d, 0, kBack), 1.018) && Near(SyncFor(d, 0, kFront), 1.148));
    std::string v;
    CHECK(PartAt(d, 0, 1.35, &v) && v == kBack && PartAt(d, 0, 1.2, &v) && v == kFront);
    // Matrix "Comp click": Toe's lane over Heel's last range (between the cut and the frame end): that range
    // goes to Toe, the cuts unchanged.
    {
        SoundData c = d;
        CHECK(AssignRange(c, 0, 1.3, 1.4, kFront) && c.frames[0].cuts == d.frames[0].cuts);
        CHECK((c.frames[0].owners == std::vector<std::string>{kBack, kFront, kFront}));
        // Decision 10, no merge: two Toe pieces in Edit, one placed stretch.
        CHECK(EditPiecesOf(c, 0).size() == 3 && PlacedSpans(c, 0, kFront).size() == 1 &&
              Near(PlacedSpans(c, 0, kFront)[0].length, 0.255));
    }
    // Matrix "No merge": Heel | Toe | Heel, the Toe range given to Heel: three Heel pieces in Edit (both cuts
    // kept, each piece on its own), Populate's spans one Heel stretch.
    {
        SoundData n = d;
        CHECK(AssignRange(n, 0, 1.145, 1.3, kBack) && n.frames[0].cuts.size() == 2);
        p = EditPiecesOf(n, 0);
        CHECK(p.size() == 3 && p[0].owner == kBack && p[1].owner == kBack && p[2].owner == kBack);
        CHECK(PlacedSpans(n, 0, kBack).size() == 1 && Near(PlacedSpans(n, 0, kBack)[0].length, 0.4));
        CHECK(PlacedSpans(n, 0, kFront).empty());
        RefreshParts(n, &pk);
        CHECK(n.frames[0].cuts.size() == 2 && n.frames[0].syncs.size() == 2 && n.frames[0].syncs[1] < 0.0);
    }
    // Matrix "Swipe": across three ranges, the near edge snapped to the cut (6 px = 5 ms here): the span
    // goes to Toe, its edges become cuts.
    {
        SoundData w = d;
        CHECK(Near(SnapToCut(w, 0, 1.297, 0.005), 1.3) && Near(SnapToCut(w, 0, 1.25, 0.005), 1.25));
        CHECK(Near(SnapToCut(w, 0, 1.003, 0.005), 1.0) && Near(SnapToCut(w, 0, 1.399, 0.005), 1.4) &&
              Near(SnapToCut(w, 9, 1.3, 0.005), 1.3));
        CHECK(AssignRange(w, 0, 1.05, SnapToCut(w, 0, 1.352, 0.005), kFront));
        CHECK((w.frames[0].cuts == std::vector<double>{1.05, 1.145, 1.3, 1.352}));
        CHECK((w.frames[0].owners == std::vector<std::string>{kBack, kFront, kFront, kFront, kBack}));
        // An edge within 4 ms of a cut lands on it (no sliver piece).
        SoundData l = d;
        CHECK(AssignRange(l, 0, 1.147, 1.302, kBack) && l.frames[0].cuts.size() == 2);
    }
    // Matrix "Hatched click" and "Alt+drag": an excluded span (hatched on every lane) given back to a lane.
    {
        SoundData h = d;
        CHECK(ExcludeRange(h, 0, 1.25, 1.2) && h.zones.size() == 1 && Near(h.zones[0].start, 1.2));
        CHECK(!PartAt(h, 0, 1.22, &v));
        p = EditPiecesOf(h, 0);
        CHECK(p.size() == 5 && p[2].excluded && p[2].owner.empty() && Near(p[2].start, 1.2) && p[4].index == 4);
        CHECK(AssignRange(h, 0, 1.2, 1.25, kBack) && h.zones.empty() && PartAt(h, 0, 1.22, &v) && v == kBack);
        // Clipped to the frame; a part of the exclusion outside [a, b] stays.
        CHECK(ExcludeRange(h, 0, 1.35, 2.0) && Near(h.zones.back().start + h.zones.back().length, 1.4));
        CHECK(AssignRange(h, 0, 1.37, 1.4, kFront) && h.zones.size() == 1 && Near(h.zones[0].length, 0.02));
        CHECK(!ExcludeRange(h, 0, 1.5, 2.0) && !ExcludeRange(h, 7, 1.0, 1.1));
        // Review: a range already excluded whole writes nothing (no undo point); one partly played still does.
        {
            SoundData x = h;
            CHECK(!ExcludeRange(x, 0, 1.352, 1.368) && x.zones.size() == h.zones.size());
            CHECK(ExcludeRange(x, 0, 1.36, 1.4));
            SoundData off = d;
            off.frames[0].on = false;
            CHECK(!ExcludeRange(off, 0, 1.2, 1.3) && off.zones.empty());
        }
        // A legacy "-> value" zone inside the range no longer overrides the new owner.
        SoundData z = d;
        CHECK(AddZone(z, 1.2, 0.02, kBack) && AssignRange(z, 0, 1.15, 1.25, kFront) && z.zones.empty());
    }
    // An off variation is not comped.
    {
        SoundData off = d;
        off.frames[0].on = false;
        CHECK(!AssignRange(off, 0, 1.2, 1.3, kBack) && EditPiecesOf(off, 0).empty());
    }
    // C4: a cut dragged onto its neighbour (a piece under 4 ms) removes that piece and the cut.
    {
        SoundData c = d;
        CHECK(MoveCut(c, 0, 1, 1.146));  // the Toe piece squashed: Heel takes the room
        CHECK((c.frames[0].cuts == std::vector<double>{1.145}) && (c.frames[0].owners == std::vector<std::string>{kBack, kBack}));
        SoundData e = d;
        CHECK(MoveCut(e, 0, 0, 0.5));  // onto the frame's start: the first Heel piece goes
        CHECK((e.frames[0].cuts == std::vector<double>{1.3}) && (e.frames[0].owners == std::vector<std::string>{kFront, kBack}));
        SoundData r = d;
        CHECK(MoveCut(r, 0, 1, 1.398));  // the last Heel piece squashed against the frame's end
        CHECK((r.frames[0].owners == std::vector<std::string>{kBack, kFront}) && r.frames[0].cuts.size() == 1);
        SoundData ok = d;
        CHECK(MoveCut(ok, 0, 1, 1.25) && ok.frames[0].cuts.size() == 2 && Near(ok.frames[0].cuts[1], 1.25));
        // Every cut gone: the whole variation is one part's.
        CHECK(MoveCut(c, 0, 0, 1.0) && c.frames[0].cuts.empty() && (c.frames[0].owners == std::vector<std::string>{kBack}));
        RefreshParts(c, &pk);
        CHECK(c.frames[0].cuts.empty() && PlacedSpans(c, 0, kBack).size() == 1 && PlacedSpans(c, 0, kFront).empty());
    }
    // Split: each half keeps the cuts and owners on its side.
    {
        SoundData s = d;
        CHECK(SplitFrame(s, 0, 1.2) && s.frames.size() == 2);
        CHECK((s.frames[0].cuts == std::vector<double>{1.145}) && (s.frames[0].owners == std::vector<std::string>{kBack, kFront}));
        CHECK((s.frames[1].cuts == std::vector<double>{1.3}) && (s.frames[1].owners == std::vector<std::string>{kFront, kBack}));
        RefreshParts(s, &pk);
        CHECK(PartAt(s, 1, 1.35, &v) && v == kBack && PartAt(s, 0, 1.1, &v) && v == kBack);
        // On a cut: the cut goes, each half its own side.
        SoundData c = d;
        CHECK(SplitFrame(c, 0, 1.3) && c.frames[0].owners.size() == 2 && c.frames[1].cuts.empty() &&
              (c.frames[1].owners == std::vector<std::string>{kBack}));
    }
    // Carried with the hand marks (cuts + owners together); a one-part frame by its middle.
    {
        std::vector<SoundFrame> now = {F(0.9, 0.6)};
        CarryHandMarks(now, d.frames);
        CHECK(now[0].cuts == d.frames[0].cuts && now[0].owners == d.frames[0].owners && now[0].cuts_hand);
        SoundFrame one = F(1.0, 0.4);
        one.owners = {kFront};
        one.cuts_hand = true;
        std::vector<SoundFrame> now2 = {F(0.9, 0.6)};
        CarryHandMarks(now2, {one});
        CHECK(now2[0].cuts.empty() && (now2[0].owners == std::vector<std::string>{kFront}));
        std::vector<SoundFrame> reset = d.frames;  // a copy holding them: replaced, not doubled
        CarryHandMarks(reset, d.frames);
        CHECK(reset[0].owners.size() == 3);
    }
    // SameNormMeasured sees the owners; Reset its cuts / Reset to detected go back to the legacy rule.
    {
        SoundData legacy = d;
        legacy.frames[0].owners.clear();
        CHECK(!SameNormMeasured(d, legacy) && SameNormMeasured(d, d));
        SoundData c = d;
        c.frames[0].cuts_hand = false;
        CHECK(ResetCuts(c, 0) && c.frames[0].owners.empty() && c.frames[0].cuts.empty());
        RefreshParts(c, &pk);
        CHECK(c.frames[0].cuts.size() == 1 && Near(c.frames[0].cuts[0], 1.145));
        SoundData r = d;
        ResetToDetected(r, {r.frames[0]});
        CHECK(r.frames[0].owners.empty());
    }
    // Owners that do not fit (another cut count, an unknown value) read the legacy rule and are dropped by
    // a refresh.
    {
        SoundData bad = d;
        bad.frames[0].owners = {kBack, "elsewhere", kBack};
        CHECK(PartAt(bad, 0, 1.35, &v) && v == kFront);
        RefreshParts(bad, &pk);
        CHECK(bad.frames[0].owners.empty() && bad.frames[0].cuts.size() == 1);
    }
}

// 13-62 review: a trim past a cut, zone edges in the comp gestures, several comped frames merged.
void TestCompingReview()
{
    PeakData  pk;
    SoundData d = Divided(&pk);
    d.mode = DetectMode::TakeMarkers;
    RefreshParts(d, &pk);
    CHECK(AssignRange(d, 0, 1.3, 1.5, kBack));  // [1, 1.4]: Back | 1.145 Front | 1.3 Back
    RefreshParts(d, &pk);
    CHECK((d.frames[0].owners == std::vector<std::string>{kBack, kFront, kBack}));
    // Finding 1: a trim that moves an edge onto / past a cut drops the cut and the sliver piece it bounded
    // (no 1 ms piece placed).
    {
        SoundData t = d;
        TrimFrame(t, 0, 1.0, 0.25, 3.0);  // end 1.25: the cut at 1.3 is past it
        RefreshParts(t, &pk);
        CHECK(t.frames[0].cuts.size() == 1 && Near(t.frames[0].cuts[0], 1.145));
        CHECK((t.frames[0].owners == std::vector<std::string>{kBack, kFront}));
        const std::vector<PartSpan> back = PlacedSpans(t, 0, kBack);
        CHECK(back.size() == 1 && Near(back[0].start, 1.0) && Near(back[0].length, 0.145));
        SoundData u = d;
        TrimFrame(u, 0, 1.0, 0.302, 3.0);  // end 2 ms after the cut at 1.3: under 4 ms, the piece goes
        RefreshParts(u, &pk);
        CHECK((u.frames[0].owners == std::vector<std::string>{kBack, kFront}) && u.frames[0].cuts.size() == 1);
        SoundData w = d;
        TrimFrame(w, 0, 1.2, 0.2, 3.0);  // start 1.2, past the cut at 1.145: the frame starts Front
        RefreshParts(w, &pk);
        CHECK((w.frames[0].owners == std::vector<std::string>{kFront, kBack}) && w.frames[0].cuts.size() == 1 &&
              Near(w.frames[0].cuts[0], 1.3));
        SoundData x = d;
        TrimFrame(x, 0, 1.0, 0.1, 3.0);  // end 1.1: both cuts out, the first piece's owner alone
        RefreshParts(x, &pk);
        CHECK(x.frames[0].cuts.empty() && (x.frames[0].owners == std::vector<std::string>{kBack}));
        std::string v;
        CHECK(PartAt(x, 0, 1.05, &v) && v == kBack && PlacedSpans(x, 0, kFront).empty());
    }
    // Finding 2: an excluded zone's edges are marks for the swipe's snap and AssignRange's landing.
    {
        SoundData t = d;
        CHECK(ExcludeRange(t, 0, 1.2, 1.25));
        CHECK(Near(SnapToCut(t, 0, 1.2012, 0.005), 1.2) && Near(SnapToCut(t, 0, 1.2488, 0.005), 1.25));
        CHECK(Near(SnapToCut(t, 0, 1.22, 0.005), 1.22));
        CHECK(AssignRange(t, 0, 1.2012, 1.29, kBack));  // lands on the zone's start: no 1.2 ms sliver
        for (const EditPiece& p : EditPiecesOf(t, 0)) CHECK(p.length >= 0.004 - 1e-9);
        CHECK(t.frames[0].cuts.size() == 4 && Near(t.frames[0].cuts[1], 1.2));
        // A zone remnant under 4 ms goes: the range's start lands on the cut at 1.145 (nearer than the
        // zone's start at 1.142), which would leave [1.142, 1.145] excluded.
        SoundData r = d;
        CHECK(ExcludeRange(r, 0, 1.142, 1.25));
        CHECK(AssignRange(r, 0, 1.1455, 1.29, kBack));
        CHECK(ExcludedZoneCount(r) == 0);
        for (const EditPiece& p : EditPiecesOf(r, 0)) CHECK(p.length >= 0.004 - 1e-9 && !p.excluded);
    }
    // Finding 4: two comped frames merged into one keep both pieces lists, with a cut at the boundary.
    {
        SoundData t = d;
        CHECK(SplitFrame(t, 0, 1.2));
        RefreshParts(t, &pk);
        std::vector<SoundFrame> now = {F(1.0, 0.4)};
        CarryHandMarks(now, t.frames);
        CHECK(now[0].cuts.size() == 3 && Near(now[0].cuts[0], 1.145) && Near(now[0].cuts[1], 1.2) && Near(now[0].cuts[2], 1.3));
        CHECK((now[0].owners == std::vector<std::string>{kBack, kFront, kFront, kBack}) && now[0].cuts_hand);
        SoundData m = t;
        m.frames = now;
        RefreshParts(m, &pk);
        std::string v;
        CHECK(m.frames[0].owners.size() == 4 && PartAt(m, 0, 1.25, &v) && v == kFront && PartAt(m, 0, 1.35, &v) && v == kBack);
        // One of them with hand cuts but no owners: the first one's cuts alone (as before).
        std::vector<SoundFrame> old = t.frames;
        old[1].owners.clear();
        std::vector<SoundFrame> now2 = {F(1.0, 0.4)};
        CarryHandMarks(now2, old);
        CHECK(now2[0].cuts.size() == 1 && (now2[0].owners == std::vector<std::string>{kBack, kFront}));
    }
}

// 13-62 Lot C: Normalize per part, each part to its own mode and target (C1), or one part only.
void TestPartNormalize()
{
    const std::string heel = "vh", toe = "vt";
    SoundData d;
    d.divide_group = "g";
    d.divide_values = {heel, toe};
    d.frames = {F(0, 1), F(1, 1), F(2, 1)};
    for (size_t i = 0; i < 3; ++i) d.frames[i].cuts = {d.frames[i].start + 0.4};
    // No own setting: the Sound-level ones (the LEVEL prefs); "" = those too.
    CHECK(PartNormMode(d, toe, NormMode::Peak) == NormMode::Peak && Near(PartNormTarget(d, toe, -1.0), -1.0));
    PartSettings own;
    own.norm_mode = static_cast<int>(NormMode::LufsSMax);
    own.norm_target = -18.0;
    SetPartSetting(d, toe, own);
    CHECK(PartNormMode(d, toe, NormMode::Peak) == NormMode::LufsSMax && Near(PartNormTarget(d, toe, -1.0), -18.0));
    CHECK(PartNormMode(d, heel, NormMode::RmsI) == NormMode::RmsI && Near(PartNormTarget(d, "", -3.0), -3.0));
    // "" = every part, each with its own mode / target; a value = that part only.
    std::vector<NormRegion> all = PartNormRegions(d, "", {}, NormMode::Peak, -1.0);
    CHECK(all.size() == 6);
    for (const NormRegion& r : all) {
        CHECK(!r.part.empty());
        if (r.part == toe) CHECK(r.mode == NormMode::LufsSMax && Near(r.target, -18.0));
        if (r.part == heel) CHECK(r.mode == NormMode::Peak && Near(r.target, -1.0));
    }
    std::vector<NormRegion> one = PartNormRegions(d, toe, {1}, NormMode::Peak, -1.0);  // the selection only
    CHECK(one.size() == 1 && one[0].frame == 1 && one[0].part == toe && one[0].mode == NormMode::LufsSMax);
    CHECK(PartNormRegions(d, "nope", {}, NormMode::Peak, -1.0).empty());
    // Toe alone on frames at +2 dB: Toe's part_db written, Heel keeps +2 (pinned), frame dB = the smallest.
    for (SoundFrame& f : d.frames) f.db = 2.0;
    std::vector<NormRegion> toes = PartNormRegions(d, toe, {}, NormMode::Peak, -1.0);
    std::vector<SoundFrame> fr = d.frames;
    CHECK(ApplyNormalize(fr, NormScope::Part, toes, {5.0, -4.0, 6.0}).changed);
    KeepOtherPartGains(d, fr, toes, {5.0, -4.0, 6.0});
    CHECK(Near(fr[0].part_db[toe], 5.0) && Near(fr[0].part_db[heel], 2.0) && Near(fr[0].db, 2.0));
    CHECK(Near(fr[1].part_db[toe], -4.0) && Near(fr[1].part_db[heel], 2.0) && Near(fr[1].db, -4.0));
    CHECK(Near(FrameGainDb(fr[2], heel), 2.0));  // Heel plays as before
    // A frame not acted on is left alone (a silent region: no part_db added).
    fr = d.frames;
    ApplyNormalize(fr, NormScope::Part, toes, {5.0, std::nan(""), 6.0});
    KeepOtherPartGains(d, fr, toes, {5.0, std::nan(""), 6.0});
    CHECK(fr[1].part_db.empty() && Near(fr[1].db, 2.0) && fr[0].part_db.size() == 2);
    // Full Per part pass, one part silent: the silent part keeps its gain, frame.db = the non-silent part's
    // dB (a silent Heel at +2 does not hold the variation below Toe's +5).
    {
        SoundData s = d;
        std::vector<NormRegion> both = PartNormRegions(s, "", {0}, NormMode::Peak, -1.0);
        CHECK(both.size() == 2);
        std::vector<double> adj;
        for (const NormRegion& r : both) adj.push_back(r.part == toe ? 5.0 : std::nan(""));
        std::vector<SoundFrame> g = s.frames;
        CHECK(ApplyNormalize(g, NormScope::Part, both, adj).changed);
        KeepOtherPartGains(s, g, both, adj);
        CHECK(Near(g[0].part_db[toe], 5.0) && Near(g[0].part_db[heel], 2.0) && Near(g[0].db, 5.0));
        CHECK(Near(FrameGainDb(g[0], heel), 2.0) && Near(FrameGainDb(g[0], toe), 5.0));
        // A silent part with an own gain from before: kept, and still out of the minimum.
        s.frames[0].part_db[heel] = -10.0;
        g = s.frames;
        ApplyNormalize(g, NormScope::Part, both, adj);
        KeepOtherPartGains(s, g, both, adj);
        CHECK(Near(g[0].part_db[heel], -10.0) && Near(g[0].db, 5.0));
    }
    // Not available: undivided or Takes.
    SoundData u;
    u.frames = {F(0, 1)};
    CHECK(PartNormRegions(u, "", {}, NormMode::Peak, -1.0).empty());
}

// 13-62 (E2): the part rows' pure helpers.
void TestPartRowHelpers()
{
    PeakData  pk;
    SoundData d = Divided(&pk);
    // SamePartSettings: none == none; a field set differs; the same fields again are the same (a gesture
    // that changed nothing writes nothing).
    CHECK(SamePartSettings(nullptr, nullptr));
    PartSettings a = Patch();
    a.gain_db = 3.0;
    CHECK(!SamePartSettings(&a, nullptr) && !SamePartSettings(nullptr, &a));
    PartSettings b = a;
    CHECK(SamePartSettings(&a, &b));
    b.mono = MonoMode::Sum;
    CHECK(!SamePartSettings(&a, &b));
    b = a;
    b.value = "other";  // its value is not a setting
    CHECK(SamePartSettings(&a, &b));
    // OwnPartNumber / PartNumberPatch round-trip in the row's unit (fades in ms, stored in s).
    for (PartField f : {PartField::Preroll, PartField::Tail, PartField::SyncOffset, PartField::Gain, PartField::NormTarget,
                        PartField::FadeIn, PartField::FadeOut}) {
        const PartSettings p = PartNumberPatch(f, 12.5);
        CHECK(OwnPartNumber(&p, f) && Near(*OwnPartNumber(&p, f), 12.5));
        CHECK(!OwnPartNumber(nullptr, f));
    }
    const PartSettings fi = PartNumberPatch(PartField::FadeIn, 4.0);
    CHECK(fi.fade_in && Near(*fi.fade_in, 0.004));
    CHECK(!OwnPartNumber(&fi, PartField::Gain) && !OwnPartNumber(&fi, PartField::SyncMode));
    // A patch then read back through the store's own setter.
    SetPartSetting(d, kFront, PartNumberPatch(PartField::Tail, 40.0));
    CHECK(OwnPartNumber(PartSettingsOf(d, kFront), PartField::Tail) == 40.0 && !OwnPartNumber(PartSettingsOf(d, kBack), PartField::Tail));
    // Remeasuring fields: sync and margins only.
    CHECK(PartFieldRemeasures(PartField::Preroll) && PartFieldRemeasures(PartField::Tail) &&
          PartFieldRemeasures(PartField::SyncMode) && PartFieldRemeasures(PartField::SyncOffset));
    CHECK(!PartFieldRemeasures(PartField::Gain) && !PartFieldRemeasures(PartField::Mono) && !PartFieldRemeasures(PartField::FadeIn));
    // The mockup's sgn: "+3", "0" (never "+0" nor "-0"), "-2.5"; without sign no "+".
    CHECK(RoundedNumberText(3.0, true) == "+3" && RoundedNumberText(0.0, true) == "0" && RoundedNumberText(-0.04, true) == "0");
    CHECK(RoundedNumberText(-2.5, true) == "-2.5" && RoundedNumberText(2.54, true) == "+2.5" && RoundedNumberText(3.0, false) == "3");
}

// ---- 13-62g: sections (Antho 2026-10-14: "the same control as cutting an item into several items in REAPER") ----

// The placed part at source time t of variation v ("" = nothing placed there).
std::string PlacedAt(const SoundData& d, int v, double t)
{
    for (const PartSpan& s : PartPieces(d, v))
        if (t >= s.start && t < s.start + s.length) return s.value_uid;
    return std::string();
}

// Antho's screenshot: a variation comped Heel | Toe | Heel (the Toe section 1.590 .. 1.840), Toe's sound at 1.600.
SoundData Comped(PeakData* pk)
{
    *pk = Quiet(3.0);
    Hit(*pk, 1.45, -6.0);
    Hit(*pk, 1.60, -15.0);  // loud (>= -45 dB) up to 1.631
    Hit(*pk, 1.845, -6.0);
    SoundData d;
    d.mode = DetectMode::Silence;
    d.frames = {F(1.40, 0.70)};
    SetDivide(d, "g", {kBack, kFront});
    // As stored by the build of the screenshot (13-62g review: a comp gesture now pins the given range's edges, so
    // the fixture is written as the record held it: cuts and owners, no hand edge).
    d.frames[0].cuts = {1.59, 1.84};
    d.frames[0].owners = {kBack, kFront, kBack};
    d.frames[0].cuts_hand = true;
    RefreshParts(d, pk);
    return d;
}

double PartEnd(const SoundData& d, const std::string& v)
{
    const std::vector<PartSpan> p = PlacedSpans(d, 0, v);
    return p.empty() ? -1.0 : p.back().start + p.back().length;
}

void TestTailAndGaps()
{
    // Root cause 1: a part's own Tail did nothing on the variation's last part (Toe after Heel): the 13-54 review
    // kept the variation's own end whatever the part's margins. Now Toe's Tail sets Toe's end (up to the
    // variation's end), lowered or raised, and the Sound's Tail too while Toe follows it.
    {
        PeakData  pk;
        SoundData d = Step(&pk);
        SetDivide(d, "g", {kBack, kFront});
        RefreshParts(d, &pk);
        CHECK(Near(PartEnd(d, kFront), 1.288));  // the Sound's 100 ms after Toe's sound (1.188), not the frame's 1.4
        PartSettings t;
        t.tail_ms = 40.0;
        SetPartSetting(d, kFront, t);
        RefreshParts(d, &pk);
        CHECK(Near(PartEnd(d, kFront), 1.228) && Near(PartEnd(d, kBack), 1.145));
        t.tail_ms = 150.0;
        SetPartSetting(d, kFront, t);
        RefreshParts(d, &pk);
        CHECK(Near(PartEnd(d, kFront), 1.338));
        t.tail_ms = 5000.0;  // never past the variation's own end
        SetPartSetting(d, kFront, t);
        RefreshParts(d, &pk);
        CHECK(Near(PartEnd(d, kFront), 1.4));
        // The variation is detected with the largest margin of all (so the part's own fits in it).
        CHECK(Near(DetectionSilence(d).tail_ms, 5000.0) && Near(DetectionSilence(d).preroll_ms, 5.0));
        ResetPartAll(d, kFront);
        CHECK(Near(DetectionSilence(d).tail_ms, 100.0));
        d.silence.tail_ms = 60.0;  // the Sound's Tail, Toe following it
        RefreshParts(d, &pk);
        CHECK(Near(PartEnd(d, kFront), 1.248) && Near(PartEnd(d, kBack), 1.121));
        // Combined: the Sound's own (no part margins).
        SoundData c = Step(&pk);
        CHECK(Near(DetectionSilence(c).tail_ms, c.silence.tail_ms));
    }
    // The screenshot's Toe (a comped section between two Heel ones): its tail fills its section up to the cut.
    PeakData  pk;
    SoundData d = Comped(&pk);
    CHECK(PartsProblem(d).empty());
    CHECK(Near(PartEnd(d, kFront), 1.730) && Near(PlacedSpans(d, 0, kFront)[0].start, 1.595));
    {
        SoundData r = d;
        PartSettings t;
        t.tail_ms = 200.0;
        SetPartSetting(r, kFront, t);
        RefreshParts(r, &pk);
        CHECK(Near(PartEnd(r, kFront), 1.830));
        t.tail_ms = 400.0;  // up to the cut, never across it
        SetPartSetting(r, kFront, t);
        RefreshParts(r, &pk);
        CHECK(Near(PartEnd(r, kFront), 1.84) && PlacedAt(r, 0, 1.839) == kFront && PlacedAt(r, 0, 1.841) == kBack);
    }
    // The stretch after Toe's tail (1.730 .. 1.840) is a gap of Toe's section: why, and what it holds.
    GapInfo g = GapAt(d, 0, 1.79);
    CHECK(g.kind == GapKind::AfterSound && g.value == kFront && Near(g.start, 1.730) && Near(g.end, 1.84) && Near(g.margin_ms, 100.0));
    CHECK(GapAt(d, 0, 1.70).kind == GapKind::None && GapAt(d, 0, 1.592).kind == GapKind::BeforeSound);
    CHECK(GapAt(d, 0, 5.0).kind == GapKind::None);
    // Root cause 2: a gesture there acted on the whole Toe section (EditPiecesOf: 1.590 .. 1.840): right-click >
    // Exclude excluded the whole Toe. Its range is now the gap only.
    EditPiece r;
    CHECK(EditRangeAt(d, 0, 1.79, &r) && !r.excluded && r.owner == kFront && Near(r.start, 1.730) && Near(r.start + r.length, 1.84));
    CHECK(EditRangeAt(d, 0, 1.70, &r) && Near(r.start, 1.59) && Near(r.start + r.length, 1.84));  // on the piece: all of it
    {
        SoundData x = d;
        CHECK(EditRangeAt(x, 0, 1.79, &r) && ExcludeRange(x, 0, r.start, r.start + r.length));
        RefreshParts(x, &pk);
        CHECK(Near(PlacedSpans(x, 0, kFront)[0].start, 1.595) && Near(PartEnd(x, kFront), 1.730) && PartsProblem(x).empty());
        CHECK(GapAt(x, 0, 1.79).kind == GapKind::Excluded);
        // Excluding a stretch inside Toe's piece takes that stretch only: its tail goes on after it.
        SoundData y = d;
        CHECK(ExcludeRange(y, 0, 1.65, 1.68));
        RefreshParts(y, &pk);
        const std::vector<PartSpan> toe = PlacedSpans(y, 0, kFront);
        CHECK(toe.size() == 2 && Near(toe[0].start, 1.595) && Near(toe[0].start + toe[0].length, 1.65) && Near(toe[1].start, 1.68) &&
              Near(toe[1].start + toe[1].length, 1.730));
        // Excluding Toe's own sound keeps its edges (the sound is measured with the exclusion ignored).
        SoundData z = d;
        CHECK(ExcludeRange(z, 0, 1.595, 1.64));
        RefreshParts(z, &pk);
        CHECK(PlacedSpans(z, 0, kFront).size() == 1 && Near(PlacedSpans(z, 0, kFront)[0].start, 1.64) && Near(PartEnd(z, kFront), 1.730));
    }
    // A click on the Heel lane in the gap takes the gap, never Toe's sound.
    CompClickPlan c = PlanCompClick(d, 0, 1.79, kBack, false, false);
    CHECK(c.take && Near(c.piece.start, 1.730) && Near(c.piece.start + c.piece.length, 1.84));
    {
        SoundData x = d;
        CHECK(AssignRange(x, 0, c.piece.start, c.piece.start + c.piece.length, kBack));
        RefreshParts(x, &pk);
        CHECK(Near(PlacedSpans(x, 0, kFront)[0].start, 1.595) && Near(PartEnd(x, kFront), 1.730) && PartsProblem(x).empty());
    }
    // Piece edges moved by hand (REAPER: an item's edge trimmed in place): Toe's end dragged into its gap.
    PieceEdgeHit h;
    CHECK(PieceEdgeAt(d, 0, kFront, 1.7302, 0.002, &h) && h.side == 1 && Near(h.edge, 1.730) && !h.hand && Near(h.hi, 1.84));
    CHECK(!PieceEdgeAt(d, 0, kBack, 1.7302, 0.002, &h));  // another part's lane
    CHECK(SetPieceEdge(d, 0, h.at, 1, 1.80) && !SetPieceEdge(d, 0, h.at, 1, 1.80));
    RefreshParts(d, &pk);
    CHECK(Near(PartEnd(d, kFront), 1.80) && GapAt(d, 0, 1.82).kind == GapKind::HandEdge && PartsProblem(d).empty());
    CHECK(SetPieceEdge(d, 0, h.at, 1, 1.95) && Near(PartEnd(d, kFront), 1.84));  // never across the cut
    CHECK(SetPieceEdge(d, 0, h.at, 1, 1.80));
    // Kept across a margin or threshold change.
    PartSettings t0;
    t0.tail_ms = 0.0;
    SetPartSetting(d, kFront, t0);
    d.silence.threshold_db = -40.0;
    RefreshParts(d, &pk);
    CHECK(Near(PartEnd(d, kFront), 1.80));
    // The start too, 4 ms from the other edge at least.
    CHECK(SetPieceEdge(d, 0, h.at, -1, 1.799) && Near(PlacedSpans(d, 0, kFront)[0].start, 1.796));
    CHECK(ResetPieceEdge(d, 0, h.at, -1) && !ResetPieceEdge(d, 0, h.at, -1));
    // Through a re-detection (CarryHandMarks), and its record.
    {
        std::vector<SoundFrame> now = {F(1.38, 0.75)};
        CarryHandMarks(now, d.frames);
        CHECK(now[0].edge_out.size() == 1 && Near(now[0].edge_out[0], 1.80) && now[0].cuts.size() == 2);
        std::vector<SoundFrame> reset = {F(1.38, 0.75)};
        CarryHandMarks(reset, d.frames, false);  // Reset to detected: gone
        CHECK(reset[0].edge_out.empty() && reset[0].edge_in.empty());
        SoundStore   st;
        SoundLibrary lib;
        lib.uid = "b000000000000002";
        lib.name = "L";
        lib.root = "c000000000000002";
        SoundNode n;
        n.uid = "c000000000000002";
        n.type = SoundNodeType::Sound;
        n.name = "S";
        n.sound = d;
        lib.nodes.push_back(n);
        st.libraries.push_back(lib);
        SoundStore back;
        CHECK(ParseSoundStore(SerializeSoundStore(st), &back) && back.libraries.size() == 1 && back.libraries[0].nodes.size() == 1);
        if (back.libraries.size() == 1 && back.libraries[0].nodes.size() == 1)
            CHECK(back.libraries[0].nodes[0].sound.frames[0].edge_out == d.frames[0].edge_out);
    }
    CHECK(ResetPieceEdge(d, 0, h.at, 1));
    RefreshParts(d, &pk);
    CHECK(Near(PartEnd(d, kFront), 1.625));  // tail 0: its sound's end (threshold -40 dB)
    // A variation's edge dragged by hand pins its outer piece there (and a split pins both halves at the split).
    {
        SoundData x = Comped(&pk);
        TrimFrame(x, 0, 1.40, 0.80, 3.0);  // end 2.10 -> 2.20
        RefreshParts(x, &pk);
        CHECK(Near(PartEnd(x, kBack), 2.2) && PartsProblem(x).empty());
        SoundData s = Comped(&pk);
        CHECK(SplitFrame(s, 0, 1.70));
        RefreshParts(s, &pk);
        CHECK(Near(PartEnd(s, kFront), 1.70) && PartsProblem(s).empty());
        CHECK(Near(PlacedSpans(s, 1, kFront)[0].start, 1.70));
    }
}

// 13-62g review: a re-detection as the Edit window runs it (Redetect): the frames detected with the parts' largest
// margin, the hand frames merged (MergeHandFrames), their marks carried, the parts measured again.
void Redetected(SoundData& d, const PeakData& pk, double source_len)
{
    const SoundData before = d;
    d.frames = MergeHandFrames(SplitOnSilence(pk, DetectionSilence(d), source_len), before.frames);
    CarryHandMarks(d.frames, before.frames);
    RefreshParts(d, &pk);
    KeepCarriedHandEdges(before, d, &pk);
    RefreshParts(d, &pk);
}

// 13-62g review: a hand edge belongs to its section (part + side); a gesture that changes a section's bounds or owner
// never hands it to another part, and no part's sound is lost by it (the reviewer's probe scenarios).
void TestHandEdgesFollowSections()
{
    PeakData  pk;
    SoundData d = Comped(&pk);  // Heel | Toe 1.59..1.84 | Heel, Heel's sound at 1.845
    PieceEdgeHit h;
    CHECK(PieceEdgeAt(d, 0, kFront, 1.7302, 0.002, &h) && SetPieceEdge(d, 0, h.at, 1, 1.80));
    RefreshParts(d, &pk);
    CHECK(Near(PartEnd(d, kFront), 1.80) && d.frames[0].edge_out_part.size() == 1 && d.frames[0].edge_out_part[0] == kFront &&
          PartsProblem(d, &pk).empty());
    // The cut after Toe moved before its hand end (1.84 -> 1.75): the hand end would sit in Heel's section and cut
    // Heel's sound at 1.845 away. It goes; Heel's sound stays placed.
    {
        SoundData m = d;
        CHECK(MoveCut(m, 0, 1, 1.75));
        RefreshParts(m, &pk);
        CHECK(m.frames[0].edge_out.empty() && PlacedAt(m, 0, 1.85) == kBack && Near(PartEnd(m, kFront), 1.73) &&
              PartsProblem(m, &pk).empty());
    }
    // Moved inward but not past it (1.84 -> 1.82): Toe's hand end stays.
    {
        SoundData m = d;
        CHECK(MoveCut(m, 0, 1, 1.82));
        RefreshParts(m, &pk);
        CHECK(m.frames[0].edge_out.size() == 1 && Near(PartEnd(m, kFront), 1.80) && PartsProblem(m, &pk).empty());
    }
    // Moved outward over Heel's sound (1.84 -> 1.90): what comes into Toe's section is placed (the automatic edge),
    // never hidden by the old hand end.
    {
        SoundData m = d;
        CHECK(MoveCut(m, 0, 1, 1.90));
        RefreshParts(m, &pk);
        CHECK(m.frames[0].edge_out.empty() && PlacedAt(m, 0, 1.85) == kFront && PartsProblem(m, &pk).empty());
    }
    // 1.76 .. 1.84 given to Heel (over Toe's hand end): Heel's own sound and the given stretch are placed.
    {
        SoundData a = d;
        CHECK(AssignRange(a, 0, 1.76, 1.84, kBack));
        RefreshParts(a, &pk);
        CHECK(PlacedAt(a, 0, 1.79) == kBack && PlacedAt(a, 0, 1.85) == kBack && PlacedAt(a, 0, 1.70) == kFront &&
              PartsProblem(a, &pk).empty());
    }
    // Reset its cuts (back to RAV's legacy parts): the hand edges inside go, nobody's sound is lost.
    {
        SoundData r = d;
        CHECK(ResetCuts(r, 0));
        RefreshParts(r, &pk);
        CHECK(r.frames[0].edge_out.empty() && r.frames[0].edge_in.empty() && PartsProblem(r, &pk).empty());
    }
    // Divided again: the hand edges inside go too (other parts).
    {
        SoundData r = d;
        CHECK(SetDivide(r, "g2", {kFront, kBack}));
        RefreshParts(r, &pk);
        CHECK(r.frames[0].edge_out.empty() && PartsProblem(r, &pk).empty());
    }
    // A hand edge read from an older record (no part known): it takes its section's part on the next refresh.
    {
        SoundData o = d;
        o.frames[0].edge_out_part.clear();
        RefreshParts(o, &pk);
        CHECK(o.frames[0].edge_out_part.size() == 1 && o.frames[0].edge_out_part[0] == kFront);
    }
    // The record keeps the parts.
    {
        SoundStore   st;
        SoundLibrary lib;
        lib.uid = "b000000000000003";
        lib.name = "L";
        lib.root = "c000000000000003";
        SoundNode n;
        n.uid = "c000000000000003";
        n.type = SoundNodeType::Sound;
        n.name = "S";
        n.sound = d;
        n.sound.frames[0].trimmed = true;
        lib.nodes.push_back(n);
        st.libraries.push_back(lib);
        SoundStore back;
        CHECK(ParseSoundStore(SerializeSoundStore(st), &back) && back.libraries.size() == 1 && back.libraries[0].nodes.size() == 1);
        if (back.libraries.size() == 1 && back.libraries[0].nodes.size() == 1) {
            const SoundFrame& f = back.libraries[0].nodes[0].sound.frames[0];
            CHECK(f.edge_out_part == d.frames[0].edge_out_part && f.trimmed);
        }
    }
    // Finding 2: a split pin survives a re-detection on its own side (a start goes to the frame starting there, an
    // end to the one ending there).
    {
        SoundData s = Comped(&pk);
        CHECK(SplitFrame(s, 0, 1.55));
        RefreshParts(s, &pk);
        std::vector<SoundFrame> now = s.frames;
        for (SoundFrame& f : now) {
            f.edge_in.clear();
            f.edge_out.clear();
        }
        CarryHandMarks(now, s.frames);
        CHECK(now.size() == 2 && now[0].edge_out.size() == 1 && Near(now[0].edge_out[0], 1.55) && now[0].edge_in.empty() &&
              now[1].edge_in.size() == 1 && Near(now[1].edge_in[0], 1.55) && now[1].edge_out.empty());
        SoundData s2 = s;
        s2.frames = now;
        RefreshParts(s2, &pk);
        CHECK(Near(PlacedSpans(s2, 1, kBack)[0].start, 1.55) && PartsProblem(s2, &pk).empty());
    }
    // Finding 5: a gap given to a part is placed (its piece's edge set by hand over it, REAPER: the item covers it).
    {
        SoundData g = Comped(&pk);
        CHECK(GapAt(g, 0, 1.79).kind == GapKind::AfterSound);
        CHECK(AssignRange(g, 0, 1.73, 1.84, kBack));
        RefreshParts(g, &pk);
        CHECK(PlacedAt(g, 0, 1.79) == kBack && PlacedAt(g, 0, 1.731) == kBack && GapAt(g, 0, 1.79).kind == GapKind::None &&
              Near(PartEnd(g, kFront), 1.73) && PartsProblem(g, &pk).empty());
        // Heel's end stays automatic there (its own sound follows): its Tail still acts.
        PartSettings t;
        t.tail_ms = 20.0;
        SetPartSetting(g, kBack, t);
        RefreshParts(g, &pk);
        CHECK(PlacedAt(g, 0, 1.79) == kBack && PlacedAt(g, 0, 2.0) == std::string());
        // An excluded stretch given to a part on a lane: included again and placed.
        SoundData x = Comped(&pk);
        CHECK(ExcludeRange(x, 0, 1.75, 1.80) && AssignRange(x, 0, 1.75, 1.80, kFront));
        RefreshParts(x, &pk);
        CHECK(PlacedAt(x, 0, 1.77) == kFront && PartsProblem(x, &pk).empty());
    }
    // Finding 7: an automatic edge "moved" by less than 1 ms stays automatic (no undo point).
    {
        SoundData e = Comped(&pk);
        CHECK(PieceEdgeAt(e, 0, kFront, 1.7302, 0.002, &h) && !h.hand && !SetPieceEdge(e, 0, h.at, 1, h.edge + 0.0004) &&
              e.frames[0].edge_out.empty() && SetPieceEdge(e, 0, h.at, 1, h.edge + 0.002));
    }
    // Finding 8: a section whose hand edges crossed (nothing placed) is a hand-edge gap, not a "no sound" one.
    {
        SoundData x = Comped(&pk);
        x.frames[0].edge_in = {1.70};
        x.frames[0].edge_out = {1.65};
        CHECK(GapAt(x, 0, 1.60).kind == GapKind::HandEdge && GapAt(x, 0, 1.75).kind == GapKind::HandEdge);
    }
    // Finding 3: a non-first part's own Pre-roll acts (RAV's cut moves before its onset).
    {
        PeakData  sp;
        SoundData s = Step(&sp);
        SetDivide(s, "g", {kBack, kFront});
        PartSettings tail0;
        tail0.tail_ms = 0.0;
        SetPartSetting(s, kBack, tail0);
        RefreshParts(s, &sp);
        CHECK(Near(PlacedSpans(s, 0, kFront)[0].start, 1.145));
        PartSettings pre;
        pre.preroll_ms = 30.0;
        SetPartSetting(s, kFront, pre);
        RefreshParts(s, &sp);
        CHECK(Near(PlacedSpans(s, 0, kFront)[0].start, 1.12) && Near(PartEnd(s, kBack), 1.061) && PartsProblem(s, &sp).empty());
    }
}

// 13-62g review (finding 6): a variation edited by hand but never trimmed (gain, fade, on / off, Normalize) follows
// a raised Tail: the re-detection gives it the new range and keeps its edits; a trimmed one keeps its range.
void TestRedetectUntrimmed()
{
    PeakData  pk;
    SoundData d = Step(&pk);
    d.frames = SplitOnSilence(pk, DetectionSilence(d), 3.0);
    SetDivide(d, "g", {kBack, kFront});
    RefreshParts(d, &pk);
    CHECK(d.frames.size() == 1 && Near(PartEnd(d, kFront), 1.288));
    // A Normalize per part (the frame's gains written, `hand` set by ApplyNormalize).
    d.frames[0].part_db[kFront] = 4.0;
    d.frames[0].db = -1.0;
    d.frames[0].hand = true;
    d.frames[0].fade_out = 0.01;
    PartSettings t;
    t.tail_ms = 250.0;
    SetPartSetting(d, kFront, t);
    Redetected(d, pk, 3.0);
    CHECK(d.frames.size() == 1 && Near(PartEnd(d, kFront), 1.438) && Near(d.frames[0].part_db[kFront], 4.0) &&
          Near(d.frames[0].db, -1.0) && Near(d.frames[0].fade_out, 0.01) && d.frames[0].hand && PartsProblem(d, &pk).empty());
    // Trimmed by hand: its range stays (Toe stops at its end).
    TrimFrame(d, 0, d.frames[0].start, 0.3, 3.0);
    const double end = d.frames[0].start + d.frames[0].length;
    t.tail_ms = 400.0;
    SetPartSetting(d, kFront, t);
    Redetected(d, pk, 3.0);
    CHECK(d.frames.size() == 1 && d.frames[0].trimmed && Near(d.frames[0].start + d.frames[0].length, end) &&
          Near(PartEnd(d, kFront), end));
}

// The parts' invariant through random gesture sequences (seeded, reproducible): every stretch of a divided variation
// is a piece that is excluded or owned, what is placed stays in its own part's stretches without overlapping, an
// exclusion takes its own stretch away and nothing more, and a part's tail raised high fills each of its sections
// up to its end. 13-62g review: also re-detections (a threshold or margin change: hand frames merged, marks
// carried), a new division, Reset to detected and piece edges; no sound lost (PartsProblem with the peaks), each hand
// edge in a section of its own part, and a hand edge that hides sound only when it was there before (or just set).
void TestPartsFuzz()
{
    PeakData pk = Quiet(4.0);
    for (double t : {0.21, 0.36, 0.52, 0.80, 1.02, 1.15, 1.31, 1.60, 1.845, 2.30, 2.47, 2.90, 3.20})
        Hit(pk, t, -3.0 - std::fmod(t * 37.0, 20.0));
    const std::string vals[3] = {kBack, kFront, "v0000000000000c1"};
    int               broken = 0;
    // The hand edges that leave a part's sound out: (side, microseconds, part) -> the loud times they hide.
    auto hiding = [&](const SoundData& x) {
        std::map<std::tuple<int, long long, std::string>, std::vector<double>> out;
        for (size_t v = 0; v < x.frames.size(); ++v)
            for (const PartSection& s : PartSectionsOf(x, static_cast<int>(v)))
                for (int side : {-1, 1}) {
                    if (!(side < 0 ? s.hand_start : s.hand_end)) continue;
                    const double a = side < 0 ? s.start : s.placed_end, b = side < 0 ? s.placed_start : s.end;
                    std::vector<double> loud;
                    for (long long k = static_cast<long long>(std::ceil(a * pk.rate - 1e-6)); k < static_cast<long long>(std::ceil(b * pk.rate - 1e-6)); ++k)
                        if (k >= 0 && k < static_cast<long long>(pk.max_abs.size()) &&
                            20.0 * std::log10(pk.max_abs[static_cast<size_t>(k)]) >= x.silence.threshold_db &&
                            PartAt(x, static_cast<int>(v), static_cast<double>(k) / pk.rate, nullptr))
                            loud.push_back(static_cast<double>(k) / pk.rate);
                    if (loud.empty()) continue;
                    // Keyed by the hand edge itself (crossed edges place nothing: the placed edge is not it).
                    const SoundFrame&          f = x.frames[v];
                    const std::vector<double>& es = side < 0 ? f.edge_in : f.edge_out;
                    double                     h = side < 0 ? s.placed_start : s.placed_end;
                    for (double e : es)
                        if (side < 0 ? (e >= s.start - 1e-9 && e < s.end - 1e-9) : (e > s.start + 1e-9 && e <= s.end + 1e-9)) {
                            h = e;
                            break;
                        }
                    out[{side, std::llround(h * 1e6), s.value_uid}] = loud;
                }
        return out;
    };
    // The section (start, end) of `x` holding a hand edge of that side, position and part; false when none.
    auto held = [&](const SoundData& x, int side, double h, const std::string& part, double* s0, double* s1) {
        for (size_t v = 0; v < x.frames.size(); ++v) {
            const SoundFrame& f = x.frames[v];
            const std::vector<double>& es = side < 0 ? f.edge_in : f.edge_out;
            bool has = false;
            for (double e : es) has = has || std::fabs(e - h) <= 1e-6;
            if (!has) continue;
            for (const PartSection& s : PartSectionsOf(x, static_cast<int>(v)))
                if (s.value_uid == part && (side < 0 ? (h >= s.start - 1e-6 && h < s.end) : (h > s.start && h <= s.end + 1e-6))) {
                    *s0 = s.start;
                    *s1 = s.end;
                    return true;
                }
        }
        return false;
    };
    const unsigned seeds = 120;
    for (unsigned seed = 1; seed <= seeds && broken == 0; ++seed) {
        unsigned state = seed * 2654435761u;
        auto     rnd = [&]() {
            state = state * 1664525u + 1013904223u;
            return static_cast<double>(state >> 8) / 16777216.0;
        };
        auto pick = [&](int n) { return std::min(n - 1, static_cast<int>(rnd() * n)); };
        SoundData d;
        d.mode = DetectMode::Silence;
        d.frames = {F(0.15, 0.95), F(1.25, 0.9), F(2.25, 1.2)};
        SetDivide(d, "g", {vals[0], vals[1], seed % 2 ? vals[2] : vals[1]});
        if (seed % 2 == 0) SetDivide(d, "g", {vals[0], vals[1]});
        RefreshParts(d, &pk);
        for (int step = 0; step < 60 && broken == 0; ++step) {
            if (d.frames.empty()) break;
            const int    v = pick(static_cast<int>(d.frames.size()));
            const double fs = d.frames[static_cast<size_t>(v)].start, fl = d.frames[static_cast<size_t>(v)].length;
            const double a = fs - 0.05 + rnd() * (fl + 0.1), b = a + rnd() * 0.4;
            const std::string val = d.divide_values[static_cast<size_t>(pick(static_cast<int>(d.divide_values.size())))];
            const int    op = pick(17);
            SoundData    before = d;
            RefreshParts(before, &pk);
            switch (op) {
            case 0: AssignRange(d, v, a, b, val); break;
            case 1: ExcludeRange(d, v, a, b); break;
            case 2:
                if (!d.frames[static_cast<size_t>(v)].cuts.empty())
                    MoveCut(d, v, pick(static_cast<int>(d.frames[static_cast<size_t>(v)].cuts.size())), a);
                break;
            case 3: SplitFrame(d, v, fs + rnd() * fl); break;
            case 4: TrimFrame(d, v, fs + (rnd() - 0.5) * 0.1, fl + (rnd() - 0.5) * 0.2, 4.0); break;
            case 5: ResetCuts(d, v); break;
            case 6: IncludeAgain(d, a); break;
            case 7: {
                PieceEdgeHit h;
                for (const PartSection& s : PartSectionsOf(d, v))
                    if (s.placed_end > s.placed_start && PieceEdgeAt(d, v, s.value_uid, rnd() < 0.5 ? s.placed_start : s.placed_end, 1e-6, &h))
                        SetPieceEdge(d, v, h.at, h.side, h.edge + (rnd() - 0.5) * 0.2);
                break;
            }
            case 8: ResetPieceEdge(d, v, a, rnd() < 0.5 ? -1 : 1); break;
            case 9: d.silence.threshold_db = -55.0 + rnd() * 25.0; break;
            case 10: {
                PartSettings p;
                if (rnd() < 0.5) p.tail_ms = rnd() * 300.0;
                else p.preroll_ms = rnd() * 80.0;
                SetPartSetting(d, val, p);
                break;
            }
            case 12:  // a threshold change re-detects (Redetect)
                d.silence.threshold_db = -55.0 + rnd() * 25.0;
                Redetected(d, pk, 4.0);
                break;
            case 13: {  // a part's margin raised or lowered, re-detected
                PartSettings p;
                if (rnd() < 0.6) p.tail_ms = rnd() * 400.0;
                else p.preroll_ms = rnd() * 60.0;
                SetPartSetting(d, val, p);
                Redetected(d, pk, 4.0);
                break;
            }
            case 14:  // divided again (2 or 3 parts)
                SetDivide(d, "g", rnd() < 0.5 ? std::vector<std::string>{vals[0], vals[1]}
                                             : std::vector<std::string>{vals[0], vals[1], vals[2]});
                break;
            case 15:  // Reset to detected
                ResetToDetected(d, SplitOnSilence(pk, DetectionSilence(d), 4.0));
                break;
            case 16:  // a gain written (Normalize, dB): a hand frame never trimmed
                SetFrameDb(d, v, -3.0 + rnd() * 6.0);
                break;
            default: SetFrameOn(d, v, rnd() < 0.85); break;
            }
            RefreshParts(d, rnd() < 0.9 ? &pk : nullptr);
            std::string why = PartsProblem(d, &pk);
            // A hand edge hiding sound was there before, and hides only what its old section held (a split, a trim
            // and a piece edge set by hand are the user's).
            if (why.empty() && op != 3 && op != 4 && op != 7) {
                RefreshParts(d, &pk);
                for (const auto& e : hiding(d)) {
                    const int    side = std::get<0>(e.first);
                    const double h = static_cast<double>(std::get<1>(e.first)) / 1e6;
                    double       s0 = 0.0, s1 = 0.0;
                    bool         ok = held(before, side, h, std::get<2>(e.first), &s0, &s1);
                    for (double t : e.second) ok = ok && t >= s0 - 1e-6 && t < s1;
                    if (!ok) {
                        char m[160];
                        std::snprintf(m, sizeof(m), "a hand edge at %.6f (side %d) now hides sound at %.6f", h, side, e.second.front());
                        why = m;
                        break;
                    }
                }
            }
            if (!why.empty()) {
                std::printf("seed %u step %d op %d: %s\n", seed, step, op, why.c_str());
                for (const SoundData* x : {&before, &d})
                    for (const SoundFrame& f : x->frames) {
                        std::printf("  [%.4f %.4f] on%d hand%d ch%d cuts:", f.start, f.start + f.length, f.on, f.hand, f.cuts_hand);
                        for (double c : f.cuts) std::printf(" %.4f", c);
                        std::printf(" own %d\n", static_cast<int>(f.owners.size()));
                    }
                ++broken;
            }
            // An exclusion (same frames) changes what is placed inside [a, b] only.
            if (op == 1 && d.frames.size() == before.frames.size()) {
                RefreshParts(before, &pk);
                RefreshParts(d, &pk);
                auto at = [](const std::vector<PartSpan>& ps, double t) {
                    for (const PartSpan& s : ps)
                        if (t >= s.start && t < s.start + s.length) return s.value_uid;
                    return std::string();
                };
                for (size_t i = 0; i < d.frames.size() && broken == 0; ++i) {
                    const std::vector<PartSpan> p0 = PartPieces(before, static_cast<int>(i)), p1 = PartPieces(d, static_cast<int>(i));
                    for (double t = d.frames[i].start; t < d.frames[i].start + d.frames[i].length; t += 0.0007)
                        if ((t < a - 1e-6 || t > b + 1e-6) && at(p0, t) != at(p1, t)) {
                            std::printf("seed %u step %d: excluding %.4f..%.4f changed %.4f\n", seed, step, a, b, t);
                            ++broken;
                            break;
                        }
                }
            }
        }
        // A part's tail raised high fills each of its sounding sections up to its end (unless moved by hand).
        if (broken == 0) {
            PartSettings p;
            p.tail_ms = 5000.0;
            SetPartSetting(d, vals[1], p);
            RefreshParts(d, &pk);
            for (size_t i = 0; i < d.frames.size(); ++i)
                for (const PartSection& s : PartSectionsOf(d, static_cast<int>(i)))
                    if (s.value_uid == vals[1] && !s.hand_end && s.placed_end > s.placed_start && s.measured &&
                        std::fabs(s.placed_end - s.end) > 1e-9) {
                        std::printf("seed %u: a raised tail left %.4f..%.4f out\n", seed, s.placed_end, s.end);
                        ++broken;
                    }
        }
    }
    CHECK(broken == 0);
}

int main()
{
    TestTailAndGaps();  // 13-62g
    TestHandEdgesFollowSections();  // 13-62g review
    TestRedetectUntrimmed();
    TestPartsFuzz();
    TestPartSettings();  // 13-62
    TestPartRowHelpers();
    TestPartNormalize();
    TestPartSyncAndMargins();
    TestComping();
    TestCompGestures();
    TestCompingReview();
    TestSyncSliced();
    TestOnsetsAndCuts();
    TestOwnershipAndSpans();
    TestEmptyPart();
    TestPartMargins();
    TestAttackVsPeak();
    TestHandMarks();
    TestSetDivideAndRoles();
    TestEditCounts();
    TestRoleToggles();
    TestTakes();
    TestWholeClamp();
    TestPlayrate();
    TestTakeMarkers();
    TestRpp();
    TestSilence();
    TestSilenceMerge();
    TestClampAndEdits();
    TestNormalize();
    TestNormalizeModes();
    TestNormalizeParts();
    TestFades();
    if (g_fails) {
        std::printf("sound_entries_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("sound_entries_test: all passed\n");
    return 0;
}
