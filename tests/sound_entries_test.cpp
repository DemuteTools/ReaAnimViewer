// SPDX-License-Identifier: MIT
//
// Host test of src/sound_entries.h (Epic 13 story 13-3): detection per mode, the two slice readers,
// silence split, frame clamping and hand edits, normalize. Pure: any C++17 compiler.

#include "sound_entries.h"
#include "sound_record.h"

#include "sound_draw.h"  // ActiveVariationsFor

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
    std::vector<PartSpan> back = PlacedSpans(d, 0, kBack);
    CHECK(back.size() == 2 && Near(back[0].start, 1.0) && Near(back[0].length, 0.05) && Near(back[1].start, 1.08) &&
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

int main()
{
    TestSyncSliced();
    TestOnsetsAndCuts();
    TestOwnershipAndSpans();
    TestEmptyPart();
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
