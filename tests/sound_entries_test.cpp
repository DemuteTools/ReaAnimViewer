// SPDX-License-Identifier: MIT
//
// Host test of src/sound_entries.h (Epic 13 story 13-3): detection per mode, the two slice readers,
// silence split, frame clamping and hand edits, normalize; 13-62 per-part settings (13-62h: the divided Sound's
// comping, cuts, owners, margins and hand marks are zones now: tests/sound_zones_test.cpp). Pure:
// any C++17 compiler.

#include "sound_entries.h"
#include "sound_record.h"
#include "sound_zones.h"  // 13-62i: every Sound's zones

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

    // An item playing one whole slice keeps the marker-to-marker slices.
    e.slices = s;
    e.start_offs = 1;
    e.item_len = 1;
    fr = DetectFrames(e, DetectMode::TakeMarkers, SilenceParams{}, nullptr);
    CHECK(fr.size() == 4 && Near(fr[1].start, 1) && Near(fr[1].length, 1));

    // 13-68, Antho's NVK items (sample\ReaAnimViewer - Export video test.rpp, 2026-10-18): the item plays a short
    // window around take marker 1 (the sync point); each variation is that window around its own marker.
    EntryFacts boots;  // FEETHmn_Boots Concrete Jump 01: SOFFS 0.047, LENGTH 0.208, TKM 0.105 / 2.555 / 4.833
    boots.source_len = 7;
    boots.start_offs = 0.047;
    boots.item_len = 0.208;
    boots.slices = SlicesFromTakeMarkers({{0.105, "1"}, {2.555, "2"}, {4.833, "3"}}, 7);
    fr = DetectFrames(boots, DetectMode::TakeMarkers, SilenceParams{}, nullptr);
    CHECK(fr.size() == 3);
    if (fr.size() == 3) {
        CHECK(Near(fr[0].start, 0.047) && Near(fr[1].start, 2.497) && Near(fr[2].start, 4.775));
        for (const SoundFrame& f : fr) CHECK(Near(f.length, 0.208));
    }
    EntryFacts dress;  // FEETHmn_Dress Boots Concrete Grit Jump 01: playrate 1.3348, TKM 0.166 / 1.959 / 3.769
    dress.source_len = 6;
    dress.start_offs = 0.09925800729146;
    dress.item_len = 0.16666666666669;
    dress.playrate = 1.33483985417003;
    dress.slices = SlicesFromTakeMarkers({{0.166, "1"}, {1.959, "2"}, {3.769, "3"}}, 6);
    fr = DetectFrames(dress, DetectMode::TakeMarkers, SilenceParams{}, nullptr);
    CHECK(fr.size() == 3);
    if (fr.size() == 3) {
        const double pre = 0.166 - 0.09925800729146;
        CHECK(Near(fr[1].start, 1.959 - pre) && Near(fr[2].start, 3.769 - pre));
        CHECK(Near(fr[0].length, 0.16666666666669 * 1.33483985417003));
    }
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
    // 13-17b: the defaults Antho picked (200 ms gap, 100 ms tail; 13-67b: -60 dB); the checks below pin
    // their own values so they test the split, not the defaults.
    CHECK(sp.threshold_db == -60.0 && sp.min_gap_ms == 200.0 && sp.preroll_ms == 5.0 && sp.tail_ms == 100.0);
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
    // 13-62i: the variation trims, splits, adds and dB drags are zone gestures now (sound_zones_test). On / off and
    // Reset to detected stay: a variation turned on gets its zones back (proposed when it has none).
    SoundData s;
    s.mode = DetectMode::Whole;
    s.frames = {F(0, 2)};
    s.frames[0].on = false;
    SetFrameOn(s, 0, true);
    CHECK(s.frames[0].on && s.frames[0].hand && s.edited && s.frames[0].zones.size() == 1 && s.frames[0].zones[0].part == kWholePart);
    ResetToDetected(s, {F(0, 2)});
    CHECK(s.frames.size() == 1 && !s.edited && !s.frames[0].hand && s.frames[0].zones.empty());
}

// 13-28: the modes table, the applier per scope, silent regions, per-part gains.
void TestNormalizeModes()
{
    // REAPER's normalizeTo, in the mockup's order.
    const int want[kNormModeCount] = {2, 3, 1, 0, 4, 5};
    for (int i = 0; i < kNormModeCount; ++i) CHECK(NormModeReaperIndex(static_cast<NormMode>(i)) == want[i]);

    // Matrix "Silent region": factor <= 0, NaN, or > +60 dB -> skipped, counted out.
    CHECK(std::isnan(NormAdjustmentDb(0.0)) && std::isnan(NormAdjustmentDb(-1.0)) &&
          std::isnan(NormAdjustmentDb(std::nan(""))) && std::isnan(NormAdjustmentDb(std::pow(10.0, 61.0 / 20.0))));
    CHECK(Near(NormAdjustmentDb(2.0), 6.0206, 1e-3));
    // Combining spans: Peak = the loudest span (smallest adjustment); LUFS-I = energy over the summed duration.
    CHECK(Near(CombineSpanAdjustments(NormMode::Peak, -1, {5.0, 2.0}, {0.1, 0.1}), 2.0));
    CHECK(Near(CombineSpanAdjustments(NormMode::LufsSMax, -18, {5.0, std::nan(""), 7.0}, {}), 5.0));
    CHECK(Near(CombineSpanAdjustments(NormMode::LufsI, -14, {3.0, 3.0}, {0.2, 0.5}), 3.0));
    // Two equal-length spans 10 dB apart: the energy mean is ~7.4 dB under the loud one.
    CHECK(Near(CombineSpanAdjustments(NormMode::LufsI, -14, {0.0, 10.0}, {1.0, 1.0}), 10.0 * std::log10(2.0 / 1.1), 1e-6));
    CHECK(std::isnan(CombineSpanAdjustments(NormMode::RmsI, -14, {std::nan("")}, {1.0})));
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
    // The Sound default: never negative.
    SetDefaultFades(d, -2.0, 0.01);
    CHECK(d.fade_in == 0.0 && Near(d.fade_out, 0.01));
    // Zero default: no fade on a frame without its own.
    f = EffectiveFades(d, d.frames[1]);
    CHECK(Near(f.in, 0.0) && Near(f.out, 0.004));
    // 13-62i: the implicit part's fades are the Sound's (SetPartSetting on kWholePart writes them).
    PartSettings pf;
    pf.fade_in = 0.003;
    pf.tail_ms = 70.0;
    pf.gain_db = 5.0;  // no Sound-level home: ignored
    SetPartSetting(d, kWholePart, pf);
    CHECK(Near(d.fade_in, 0.003) && Near(d.silence.tail_ms, 70.0) && d.parts.empty() && PartSettingsOf(d, kWholePart) == nullptr);
    CHECK(PartIndex(d, kWholePart) == 0 && PartKeys(d).size() == 1 && PartKeys(d)[0] == kWholePart && Near(PartTailMs(d, kWholePart), 70.0));
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

// 13-62i: an undivided Sound's sync point is its zone's snap (Settings of: Sync point, Offset), measured on the
// peaks and stored (auto_snap) by the layout; Silence mode: the zone's automatic start follows Pre-roll (5 ms).
void TestSyncSliced()
{
    PeakData  pk;
    SoundData d = Step(&pk);
    RefreshParts(d, &pk);
    CHECK(d.frames[0].zones.size() == 1 && d.frames[0].zones[0].part == kWholePart && d.frames[0].syncs.empty());
    const ZoneRef z{0, 0};
    // Matrix row 1: the attack at 1.020, -2 ms.
    CHECK(Near(ZoneOf(d, z)->a, 1.015) && Near(ZoneSnap(d, z, nullptr).t, 1.018) && ZoneSnapStored(d, z));
    // Peak: the loudest bin (the heel's top), + the offset.
    CHECK(SetSyncSettings(d, SyncMode::Peak, 0.0));
    RefreshParts(d, &pk);
    CHECK(Near(ZoneSnap(d, z, nullptr).t, 1.020));
    // Slice start (the zone's start) + 3 ms; a negative offset is clamped into the zone.
    SetSyncSettings(d, SyncMode::SliceStart, 3.0);
    RefreshParts(d, &pk);
    CHECK(Near(ZoneSnap(d, z, nullptr).t, 1.018));
    SetSyncSettings(d, SyncMode::SliceStart, -5.0);
    CHECK(Near(ZoneSnap(d, z, nullptr).t, ZoneOf(d, z)->a));
    // Attack + a large offset is clamped to the zone's end.
    SetSyncSettings(d, SyncMode::Attack, 1000.0);
    RefreshParts(d, &pk);
    CHECK(Near(ZoneSnap(d, z, nullptr).t, ZoneOf(d, z)->b));
    // Without peaks after a settings change: the stored measure no longer holds (the zone's onset + offset then).
    SetSyncSettings(d, SyncMode::Attack, 0.0);
    CHECK(!ZoneSnapStored(d, z));
    CHECK(!SetSyncSettings(d, SyncMode::Attack, std::nan("")));
    // Takes mode: the take's zone gets its Sync point too (P5, no longer the item's snap).
    SoundData t = Step(&pk);
    t.mode = DetectMode::Takes;
    RefreshParts(t, &pk, 0);
    CHECK(t.frames[0].zones.size() == 1 && t.frames[0].zones[0].a_hand && Near(ZoneSnap(t, z, &pk).t, 1.018));
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
    d.sync_mode = SyncMode::Attack;  // 13-67b: a new Sound's default is Peak
    d.mode = DetectMode::TakeMarkers;  // 13-62i: the variation's edges kept (P7), so the measure is over all of it
    d.frames = {F(0.9, 0.4)};
    d.sync_offset_ms = 0.0;
    d.silence.threshold_db = -50.0;
    RefreshParts(d, &pk);
    const ZoneRef z{0, 0};
    CHECK(Near(ZoneSnap(d, z, nullptr).t, 1.020));  // first bin >= -24 dB (peak - 20)
    d.silence.threshold_db = -10.0;
    RefreshParts(d, &pk);
    CHECK(Near(ZoneSnap(d, z, nullptr).t, 1.027));  // first bin >= the threshold
    SetSyncSettings(d, SyncMode::Peak, 0.0);
    RefreshParts(d, &pk);
    CHECK(Near(ZoneSnap(d, z, nullptr).t, 1.030));
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
    CHECK(ExcludedZoneCount(d) == 0);
    d.zones = {{0.2, 0.1, "", {}}, {0.9, 0.5, "", {}}, {2.5, 0.1, kFront, {}}};
    CHECK(ExcludedZoneCount(d) == 2);
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
    // 13-62h review: "All" keeps the lane fader (what it applied to the zones), only the settings go.
    SetPartSetting(d, kFront, g);
    CHECK(d.parts.size() == 1);
    d.parts[0].fader_db = -4.5;
    ResetPartAll(d, kFront);
    CHECK(d.parts.size() == 1 && OwnSettingCount(d, kFront) == 0 && d.parts[0].fader_db == -4.5);
    d.parts[0].fader_db.reset();
    ResetPartAll(d, kFront);  // nothing left: the entry goes
    CHECK(d.parts.empty());
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
    // Matrix "Divide by changed": every part setting and zone cleared (one gesture; 13-62h: zones, no owners).
    CHECK(!d.frames[0].zones.empty() && d.frames[0].owners.empty());
    CHECK(SetDivide(d, "g2", {kBack, kFront}) && d.parts.empty() && d.frames[0].zones.empty());
}

// 13-62 Lot C: each part's own Normalize mode and target (C1), else the Sound-level ones (13-62i: the regions are the
// zones', NormGroupRegions in sound_zones_test).
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

// 13-67b: a mode switch parks the variations left; back to that mode they come back (the 13-62h Takes bug).
void TestSwitchDetectMode()
{
    SoundData d;
    d.mode = DetectMode::Silence;
    d.frames = {F(0.1, 0.2), F(0.5, 0.2)};
    d.frames[0].hand = true;
    d.edited = true;
    CHECK(!SwitchDetectMode(d, DetectMode::Silence, {}));
    CHECK(SwitchDetectMode(d, DetectMode::Takes, {F(0.0, 1.0), F(0.0, 0.8)}));
    CHECK(d.mode == DetectMode::Takes && d.frames.size() == 2 && Near(d.frames[0].length, 1.0) && !d.edited);
    CHECK(d.parked_mode == DetectMode::Silence && d.parked.size() == 2 && d.parked_edited);
    CHECK(SwitchDetectMode(d, DetectMode::Silence, {F(0.0, 1.0)}));  // back: the parked ones, not the fresh detection
    CHECK(d.frames.size() == 2 && d.frames[0].hand && Near(d.frames[1].start, 0.5) && d.edited);
    CHECK(d.parked_mode == DetectMode::Takes && d.parked.size() == 2 && !d.parked_edited);
    // Another mode: its fresh detection; the one left is parked.
    CHECK(SwitchDetectMode(d, DetectMode::Whole, {F(0.0, 1.0)}) && d.frames.size() == 1 && !d.edited);
    CHECK(d.parked_mode == DetectMode::Silence && d.parked.size() == 2);
    // Divide by changed: the parked zones belonged to the old division.
    CHECK(SetDivide(d, "g", {"a", "b"}) && d.parked.empty());
}

// 13-73: Left / Right per variation: the Pattern, Swap, sides by hand, the count, a re-detection that keeps every side by
// time (new variations continue the alternation of their neighbours, nothing re-alternated), the record round trip.
static SoundFrame SideFrame(double start, double len)
{
    SoundFrame f;
    f.start = start;
    f.length = len;
    return f;
}
static std::string SidesOf(const SoundData& d)
{
    std::string s;
    for (char c : FrameSides(d)) s += c ? c : '0';
    return s;
}
// 13-73 review: a Sound made with Left / Right on (New sounds pref): its first detection writes the Pattern, and a zone
// edit (LockSides first) deleting a variation never shifts the sides of the ones after it.
static void TestSidesFirstDetection()
{
    std::vector<SoundFrame> now;
    for (int i = 0; i < 4; ++i) now.push_back(SideFrame(i * 1.0, 0.4));
    CarrySides(now, {}, SidePattern::AltLR, true);
    CHECK(now[0].side == 'L' && now[1].side == 'R' && now[2].side == 'L' && now[3].side == 'R');
    std::vector<SoundFrame> off = now;
    CarrySides(off, {}, SidePattern::AltLR, false);
    CHECK(off[0].side == 0 && off[3].side == 0);  // Left / Right off, nothing known: nothing written
    SoundData d;
    d.sides = true;
    for (int i = 0; i < 4; ++i) d.frames.push_back(SideFrame(i * 1.0, 0.4));  // never written (sides 0)
    CHECK(LockSides(d) && SidesOf(d) == "LRLR");
    d.frames.erase(d.frames.begin() + 1);  // V2 deleted
    CHECK(SidesOf(d) == "LLR");            // V3 stays Left, V4 Right
}

static void TestSides()
{
    SoundData d;
    CHECK(!d.sides);  // Off by default (New sounds pref Off)
    for (int i = 0; i < 6; ++i) d.frames.push_back(SideFrame(i * 1.0, 0.4));
    CHECK(SidesOf(d) == "LRLRLR");  // Alternate L R before anything is written
    CHECK(SetSidesOn(d, true) && d.sides && d.frames[3].side == 'R');  // On: written (locked)
    CHECK(!SetSidesOn(d, true));
    CHECK(SetSidePattern(d, SidePattern::AltRL) && SidesOf(d) == "RLRLRL");
    CHECK(SetSidePattern(d, SidePattern::AllLeft) && SidesOf(d) == "LLLLLL");
    CHECK(SetSidePattern(d, SidePattern::AltLR) && SidesOf(d) == "LRLRLR");
    CHECK(!SetSidePattern(d, SidePattern::AltLR));
    // By hand: V2 (R) and V3 (L) set Left: V2 by hand, V3 already Left (not by hand).
    CHECK(SetFrameSides(d, {1, 2}, 'L') && SidesOf(d) == "LLLRLR");
    std::vector<char> rule = FrameRuleSides(d);
    CHECK(FrameSideByHand(d, rule, 1) && !FrameSideByHand(d, rule, 2));
    SideCounts c = CountSides(d);
    CHECK(c.left == 4 && c.right == 2 && c.hand == 1);
    CHECK(!SetFrameSides(d, {2}, 'L'));
    // A new Pattern keeps the hand sides (one it now gives anyway is no longer by hand).
    CHECK(SetSidePattern(d, SidePattern::AllRight) && SidesOf(d) == "RLRRRR" && CountSides(d).hand == 1);
    CHECK(SetSidePattern(d, SidePattern::AltLR) && SidesOf(d) == "LLLRLR");
    // Swap: the Pattern and the hand sides flipped, in one gesture.
    CHECK(SwapSides(d) && d.side_pattern == SidePattern::AltRL && SidesOf(d) == "RRRLRL" && CountSides(d).hand == 1);
    CHECK(SwapSides(d) && d.side_pattern == SidePattern::AltLR && SidesOf(d) == "LLLRLR");
    // Follow rule, Reset hand sides.
    CHECK(SetFrameSides(d, {1}, 0) && SidesOf(d) == "LRLRLR" && CountSides(d).hand == 0);
    CHECK(!ResetHandSides(d));
    CHECK(SetFrameSides(d, {4, 5}, 'R') && SidesOf(d) == "LRLRRR");
    CHECK(ResetHandSides(d) && SidesOf(d) == "LRLRLR");

    // Re-detection: V3 (2.0) is lost and comes back; a new one appears at 6.5 and one before V1 at -1 (none before: the
    // first variation's side by parity). Every known variation keeps its side, by time.
    CHECK(SetFrameSides(d, {4}, 'R'));  // V5 Right by hand
    const std::vector<SoundFrame> old = d.frames;
    std::vector<SoundFrame>       now;
    for (int i : {0, 1, 3, 4, 5}) now.push_back(SideFrame(i * 1.0 + 0.05, 0.3));  // V3 gone, the others moved a little
    CarrySides(now, old, d.side_pattern, d.sides);
    SoundData r = d;
    r.frames = now;
    CHECK(SidesOf(r) == "LRRRR");  // L R (V3 gone) R R(hand) R: nothing re-alternated
    CHECK(r.frames[3].side_hand == 'R' && r.frames[3].side == 'L');
    // V3 back, plus a new one after the last: each continues the alternation of its neighbours.
    std::vector<SoundFrame> back;
    for (int i : {0, 1, 2, 3, 4, 5, 6}) back.push_back(SideFrame(i * 1.0, 0.4));
    CarrySides(back, r.frames, r.side_pattern, r.sides);
    SoundData b = r;
    b.frames = back;
    CHECK(SidesOf(b) == "LRLRRRL");  // V3 new: after V2 (R) -> L; V7 new: after V6 (R) -> L; V5 still Right by hand
    CHECK(b.frames[2].side == 'L' && b.frames[6].side == 'L');  // written: the next re-detection keeps them too
    // A Sound that never had a side written (Left / Right off, none set): nothing written by a re-detection.
    SoundData plain;
    for (int i = 0; i < 3; ++i) plain.frames.push_back(SideFrame(i * 1.0, 0.4));
    std::vector<SoundFrame> pn = plain.frames;
    CarrySides(pn, plain.frames, plain.side_pattern, plain.sides);
    CHECK(pn[0].side == 0 && pn[1].side == 0 && pn[2].side == 0);

    // Saved with the Sound: sides, Pattern, each variation's side and hand side.
    SoundStore st;
    SoundLibrary lib;
    lib.uid = "0123456789abcdef";
    lib.name = "Steps";
    SoundNode n;
    n.uid = "1123456789abcdef";
    n.type = SoundNodeType::Sound;
    n.name = "S";
    n.sound = d;
    n.sound.side_pattern = SidePattern::AltRL;
    lib.root = n.uid;
    lib.nodes.push_back(n);
    st.libraries.push_back(lib);
    SoundStore back_st;
    CHECK(ParseSoundStore(SerializeSoundStore(st), &back_st) && back_st.libraries.size() == 1 && back_st.libraries[0].nodes.size() == 1);
    if (!back_st.libraries.empty() && !back_st.libraries[0].nodes.empty()) {
        const SoundData& x = back_st.libraries[0].nodes[0].sound;
        CHECK(x.sides && x.side_pattern == SidePattern::AltRL && x.frames.size() == 6);
        for (size_t i = 0; i < x.frames.size() && i < d.frames.size(); ++i)
            CHECK(x.frames[i].side == d.frames[i].side && x.frames[i].side_hand == d.frames[i].side_hand);
    }
    st.libraries[0].nodes[0].sound = SoundData();  // Off, nothing set: nothing written
    st.libraries[0].nodes[0].sound.frames.push_back(SideFrame(0.0, 1.0));
    const std::string plain_text = SerializeSoundStore(st);
    CHECK(plain_text.find("sides") == std::string::npos && plain_text.find("side=") == std::string::npos);
}

int main()
{
    TestSides();  // 13-73
    TestSidesFirstDetection();  // 13-73 review
    TestSwitchDetectMode();  // 13-67b
    TestPartSettings();  // 13-62
    TestPartRowHelpers();
    TestPartNormalize();
    TestSyncSliced();
    TestOnsetsAndCuts();
    TestAttackVsPeak();
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
    TestNormalizeModes();
    TestFades();
    if (g_fails) {
        std::printf("sound_entries_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("sound_entries_test: all passed\n");
    return 0;
}
