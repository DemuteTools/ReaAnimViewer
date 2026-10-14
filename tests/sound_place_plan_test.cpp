// SPDX-License-Identifier: MIT
//
// Host test of src/sound_place_plan.h (13-6): position, gain, slice, folder rule, NVK chunk,
// copy chunk, pile member offset.

#include "sound_place_plan.h"
#include "sound_entries.h"
#include "sound_zones.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)
static bool Near(double a, double b, double e = 1e-9) { return std::fabs(a - b) <= e; }

// 13-20 / 13-23 / 13-25 (spec 13-20-23-25 D7, matrix row 1): a sliced variation placed from its sync
// point; stale syncs measured at placement (MeasureSyncs); File pieces; Whole-item pieces; Takes none.
static void TestPieces()
{
    // Row 1: silence frame 1.000-1.400, the attack at 1.020: D_STARTOFFS 1.000, D_SNAPOFFSET 0.018.
    SoundData d;
    d.mode = DetectMode::Silence;
    SoundFrame f;
    f.start = 1.0;
    f.length = 0.4;
    d.frames.push_back(f);
    CHECK(SyncsStale(d, 0));  // never measured: Populate measures it
    PeakData pk;
    pk.rate = 1000.0;
    pk.max_abs.assign(2000, 0.0f);
    for (int i = 1020; i < 1300; ++i) pk.max_abs[static_cast<size_t>(i)] = 0.5f;
    CHECK(MeasureSyncs(d, 0, &pk));
    CHECK(d.frames[0].syncs.size() == 1 && Near(d.frames[0].syncs[0], 1.018, 1e-9) && !SyncsStale(d, 0));
    std::vector<PieceApply> pa = PieceApplyFor(d, 0, "");
    CHECK(pa.size() == 1 && pa[0].anchor && Near(pa[0].slice.start_offs, 1.0) && Near(pa[0].slice.length, 0.4));
    CHECK(Near(pa[0].slice.snap_offset, 0.018, 1e-9));
    CHECK(Near(PlacementPosition(5.0, pa[0].slice.snap_offset, 0.0, 0.0), 4.982, 1e-9));  // the sync (attack - 2 ms) on the event
    CHECK(pa[0].fades.set_in && pa[0].fades.set_out && Near(pa[0].fades.in, kDefaultFadeIn));  // the frame's fades
    // Without peaks Attack cannot be measured: nothing written, the placement falls back to the
    // first span's start + the offset, clamped into the frame (the start).
    SoundData nd = d;
    nd.frames[0].syncs.clear();
    CHECK(!MeasureSyncs(nd, 0, nullptr) && nd.frames[0].syncs.empty());
    pa = PieceApplyFor(nd, 0, "");
    CHECK(pa.size() == 1 && Near(pa[0].slice.snap_offset, 0.0));
    // Slice start needs no peaks: the start + 3 ms.
    SetSyncSettings(nd, SyncMode::SliceStart, 3.0);
    CHECK(MeasureSyncs(nd, 0, nullptr) && Near(nd.frames[0].syncs[0], 1.003, 1e-9));
    // A hand sync inside the frame wins over the measure; a stored sync outside the spans is not used.
    SoundData hd = d;
    hd.frames[0].syncs.clear();
    hd.frames[0].hand_syncs = {1.25};
    CHECK(MeasureSyncs(hd, 0, &pk) && Near(hd.frames[0].syncs[0], 1.25));
    hd.frames[0].syncs = {3.0};
    CHECK(Near(PieceApplyFor(hd, 0, "")[0].slice.snap_offset, 0.0));  // stale: start + offset, clamped
    // Takes: no pieces (the take keeps its item's snap); a bad index or an off frame: none.
    SoundData tk = d;
    tk.mode = DetectMode::Takes;
    CHECK(PieceApplyFor(tk, 0, "").empty() && !MeasureSyncs(tk, 0, &pk));
    CHECK(PieceApplyFor(d, 3, "").empty() && PieceApplyFor(d, -1, "").empty());
    SoundData off = d;
    off.frames[0].on = false;
    CHECK(PieceApplyFor(off, 0, "").empty());
    // File: the frame's own fade (its shape) on both sides (FileFadeApplyFor); undivided: one piece, no zone gain.
    SoundData fd = d;
    fd.frames[0].fade_in = 0.004;
    fd.frames[0].fade_in_shape = 2;
    pa = PieceApplyFor(fd, 0, "", true);
    CHECK(pa.size() == 1 && Near(pa[0].fades.in, 0.004) && pa[0].fades.in_shape == 2 && pa[0].fades.set_out &&
          Near(pa[0].db, 0.0) && Near(pa[0].from_anchor, 0.0));
    const FileItemFields ff = FileItemFieldsFor(10.0, pa[0].slice, pa[0].fades, 0.0, 5.0, 0.0, 0.0);
    CHECK(ff.ok && Near(ff.snap_offset, 0.018) && Near(ff.position, 4.982) && Near(ff.start_offs, 1.0));
    // Whole item, undivided: its own fades only where the frame set them (the copy keeps its item's).
    SoundData wd = fd;
    wd.mode = DetectMode::Whole;
    wd.frames[0].fade_in = -1.0;
    pa = PieceApplyFor(wd, 0, "");
    CHECK(pa.size() == 1 && !pa[0].fades.set_in && !pa[0].fades.set_out);
    // Undivided Sound-level zones still cut the variation as before (13-20).
    SoundData zd = d;
    AddZone(zd, 1.25, 0.05, "");
    std::vector<PieceApply> za = PieceApplyFor(zd, 0, "");
    CHECK(za.size() == 2 && Near(za[0].slice.length, 0.25) && Near(za[1].slice.start_offs, 1.3) &&
          Near(za[1].from_anchor, 0.3) && Near(za[0].fades.out, kDefaultCutFade));
    // A negative or bad cut_fade reads as none.
    zd.cut_fade = -1.0;
    CHECK(Near(PieceApplyFor(zd, 0, "")[1].fades.in, 0.0));
    // ApplyMeasuredSyncs: the measured syncs written back into the store's Sound.
    {
        SoundStore   store;
        SoundLibrary lib;
        lib.uid = "L";
        SoundNode n;
        n.uid = "s";
        n.type = SoundNodeType::Sound;
        n.sound = d;  // frame [1, 1.4), syncs {1.018}
        n.sound.frames.push_back(n.sound.frames[0]);
        n.sound.frames[1].start = 2.0;
        n.sound.frames[0].syncs.clear();
        n.sound.frames[1].syncs.clear();
        lib.nodes.push_back(n);
        store.libraries.push_back(lib);
        SoundData m = n.sound;
        m.frames[0].syncs = {1.018};
        m.frames[1].syncs = {2.018};
        m.frames[1].start = 2.5;  // frame 1 moved meanwhile: left alone
        CHECK(ApplyMeasuredSyncs(store, "L", "s", m, {0, 1, 7}));
        const SoundData& w = store.libraries[0].nodes[0].sound;
        CHECK(w.frames[0].syncs.size() == 1 && Near(w.frames[0].syncs[0], 1.018) && w.frames[1].syncs.empty());
        CHECK(!ApplyMeasuredSyncs(store, "L", "s", m, {0}));  // equal syncs: no change
        CHECK(!ApplyMeasuredSyncs(store, "X", "s", m, {0}) && !ApplyMeasuredSyncs(store, "L", "nope", m, {0}));
    }
    // PiecePosition clamps at 0 and reads a bad rate as 1.
    CHECK(Near(PiecePosition(0.05, -0.1, 1.0), 0.0) && Near(PiecePosition(1.0, 0.5, 0.0), 1.5));
}

// 13-41: the take channel mode of a Sound forced to mono (REAPER's I_CHANMODE).
static void TestChanMode()
{
    CHECK(ChanModeFor(MonoMode::Off) == 0 && ChanModeFor(MonoMode::Sum) == 2 && ChanModeFor(MonoMode::Left) == 3 &&
          ChanModeFor(MonoMode::Right) == 4);
}

// 13-49 review: mono read from a take's channel mode, the playrate fold, the chunk stripping, the drops line.
static void TestAsAuditioned()
{
    CHECK(MonoForChanMode(2) == MonoMode::Sum && MonoForChanMode(3) == MonoMode::Left &&
          MonoForChanMode(4) == MonoMode::Right && MonoForChanMode(0) == MonoMode::Off &&
          MonoForChanMode(1) == MonoMode::Off && MonoForChanMode(67) == MonoMode::Off);
    for (MonoMode m : {MonoMode::Sum, MonoMode::Left, MonoMode::Right}) CHECK(MonoForChanMode(ChanModeFor(m)) == m);

    // A 2 s item at rate 1.5 plays 3 s of its source; its snap 0.4 s (item time) is 0.6 s into the source.
    PlayrateFold f = FoldPlayrate(1.5, 2.0, 0.4);
    CHECK(Near(f.rate, 1.5) && Near(f.length, 3.0) && Near(f.snap, 0.6));
    f = FoldPlayrate(0.5, 2.0, 1.0);
    CHECK(Near(f.length, 1.0) && Near(f.snap, 0.5));
    for (double bad : {0.0, -2.0, std::nan(""), static_cast<double>(INFINITY)}) {
        f = FoldPlayrate(bad, 2.0, 0.4);
        CHECK(Near(f.rate, 1.0) && Near(f.length, 2.0) && Near(f.snap, 0.4));
    }

    const std::string chunk =
        "<ITEM\r\nPOSITION 1\r\nIGUID {A}\r\nNAME a\r\nTAKE_FX_HAVE 1\r\n<SOURCE WAVE\r\nFILE \"a.wav\"\r\n>\r\n"
        "<TAKEFX\r\nWNDRECT 0 0 0 0\r\n<VST \"VST: ReaEQ\" reaeq.dll 0 \"\" 1919247729\r\nabc=\r\n>\r\n"
        "FXID {F}\r\n<JS foo \"\"\r\n<NESTED\r\nx\r\n>\r\n>\r\n>\r\n"
        "SM 0 0 + 1.5 1.2\r\nTAKE\r\nNAME b\r\n<SOURCE WAVE\r\nFILE \"b.wav\"\r\n>\r\n<TAKEFX\r\n>\r\n"
        "<VOLENV\r\nPT 0 1 0\r\n>\r\n>\r\n";
    const std::string want =
        "<ITEM\r\nPOSITION 1\r\nIGUID {A}\r\nNAME a\r\n<SOURCE WAVE\r\nFILE \"a.wav\"\r\n>\r\n"
        "TAKE\r\nNAME b\r\n<SOURCE WAVE\r\nFILE \"b.wav\"\r\n>\r\n"
        "<VOLENV\r\nPT 0 1 0\r\n>\r\n>\r\n";
    CHECK(StripTakeFxAndStretch(chunk) == want);
    CHECK(StripTakeFxAndStretch(want) == want);  // nothing to drop: byte-identical
    // SMOOTH / SMTH-like words and a nested SM are kept; only the item-level SM word goes.
    const std::string other = "<ITEM\nSMOOTH 1\n<SOURCE X\nSM 1\n>\nSM 0 0\n>";
    CHECK(StripTakeFxAndStretch(other) == "<ITEM\nSMOOTH 1\n<SOURCE X\nSM 1\n>\n>");

    CHECK(PlacementDropsText(false, false, false, false).empty());
    CHECK(PlacementDropsText(true, true, true, false) ==
          "Placed without the source item's take FX, pitch and rate (RAV plays the file itself)");
    CHECK(PlacementDropsText(false, false, false, true) == "Placed without the source item's pan (RAV plays the file itself)");
}

// 13-62h: a divided Sound (heel / toe) whose variation 0 is the given zones (Silence mode, edited as zones).
static EditZone Zn(double a, double b, const std::string& part, double xl = 0.0, double xr = 0.0)
{
    EditZone z;
    z.a = z.slot_a = a;
    z.b = z.slot_b = b;
    z.part = part;
    z.xl = xl;
    z.xr = xr;
    return z;
}
static SoundData Zoned(const std::vector<EditZone>& zones)
{
    SoundData d;
    d.mode = DetectMode::Silence;
    d.divide_group = "g";
    d.divide_values = {"heel", "toe"};
    SoundFrame f;
    f.zones = zones;
    f.start = zones.front().a;
    f.length = zones.back().b - zones.front().a;
    d.frames.push_back(f);
    return d;
}

// 13-62h final review: the event lands by the first placed zone with a snap offset of its own (a hand sync point,
// also one migrated from the old model), else by the first zone.
static void TestZoneAnchorOwnSnap()
{
    SoundData d = Zoned({Zn(1.0, 1.2, "heel"), Zn(1.3, 1.6, "heel")});
    d.frames[0].zones[1].snap = 1.4;
    std::vector<PieceApply> p = PieceApplyFor(d, 0, "heel");
    CHECK(p.size() == 2 && !p[0].anchor && p[1].anchor);
    if (p.size() == 2) {
        CHECK(Near(p[1].slice.snap_offset, 0.1) && Near(p[1].from_anchor, 0.0) && Near(p[0].from_anchor, -0.3));
    }
    d.frames[0].zones[1].snap.reset();
    p = PieceApplyFor(d, 0, "heel");
    CHECK(p.size() == 2 && p[0].anchor && !p[1].anchor && Near(p[1].from_anchor, 0.3));
}

// 13-62 (decision 6) on zones (13-62h): a part owning a fade gets it on each of its zones' edges (a zone's own
// fade wins; at a cut the At cuts fade comes before the part's own, mockup fadeOf); a part without: the Sound's.
static void TestPartFadesOnPieces()
{
    SoundData d = Zoned({Zn(1.0, 1.4, "heel"), Zn(1.4, 2.0, "toe")});
    d.fade_in = 0.03;
    d.fade_out = 0.05;
    CHECK(ZonesProblem(d, 0.0).empty());
    PartSettings own;
    own.fade_out = 0.004;
    SetPartSetting(d, "toe", own);
    std::vector<PieceApply> toe = PieceApplyFor(d, 0, "toe");
    CHECK(toe.size() == 1 && Near(toe[0].slice.start_offs, 1.4) && Near(toe[0].slice.length, 0.6));
    if (toe.size() == 1) {
        CHECK(toe[0].fades.set_in && Near(toe[0].fades.in, kDefaultCutFade) && toe[0].fades.in_shape == 0);  // at the cut
        CHECK(toe[0].fades.set_out && Near(toe[0].fades.out, 0.004) && toe[0].fades.out_shape == 0);       // the part's
    }
    // Heel owns nothing: the Sound's fade in at its free edge, At cuts at the cut.
    std::vector<PieceApply> heel = PieceApplyFor(d, 0, "heel");
    CHECK(heel.size() == 1 && Near(heel[0].fades.in, 0.03) && Near(heel[0].fades.out, kDefaultCutFade));
    // A zone's own fade wins over everything but a Gap.
    d.frames[0].zones[1].fade_in = 0.012;
    toe = PieceApplyFor(d, 0, "toe");
    CHECK(toe.size() == 1 && Near(toe[0].fades.in, 0.012) && Near(toe[0].fades.out, 0.004));
    // Per-part sync (decision 7): Slice start + its own offset, from the part's own zone.
    own = PartSettings{};
    own.sync_mode = SyncMode::SliceStart;
    own.sync_offset_ms = 10.0;
    SetPartSetting(d, "toe", own);
    CHECK(!PieceSnapNeedsPeaks(d, 0, "toe") && PieceSnapNeedsPeaks(d, 0, "heel"));
    toe = PieceApplyFor(d, 0, "toe");
    CHECK(toe.size() == 1 && Near(toe[0].slice.snap_offset, 0.01, 1e-9));
}

// 13-62F (F1) on zones (13-62h): the Lane view's fades per piece, as placed; own = the zone's or its part's.
static void TestPartPieceFades()
{
    SoundData d = Zoned({Zn(1.0, 1.4, "heel"), Zn(1.4, 2.0, "toe")});
    d.fade_in = 0.03;
    d.fade_out = 0.05;
    std::vector<PieceFade> heel = PartPieceFades(d, 0, "heel"), toe = PartPieceFades(d, 0, "toe");
    CHECK(heel.size() == 1 && Near(heel[0].start, 1.0) && Near(heel[0].length, 0.4) && Near(heel[0].in, 0.03) &&
          Near(heel[0].out, kDefaultCutFade) && !heel[0].own_in && !heel[0].own_out);
    CHECK(toe.size() == 1 && Near(toe[0].in, kDefaultCutFade) && Near(toe[0].out, 0.05) && !toe[0].own_out);
    PartSettings own;
    own.fade_out = 0.004;
    SetPartSetting(d, "toe", own);
    toe = PartPieceFades(d, 0, "toe");
    CHECK(toe.size() == 1 && Near(toe[0].out, 0.004) && toe[0].own_out && !toe[0].own_in);
    d.frames[0].zones[0].fade_in = 0.01;
    heel = PartPieceFades(d, 0, "heel");
    CHECK(heel.size() == 1 && Near(heel[0].in, 0.01) && heel[0].own_in);
    CHECK(PartPieceFades(d, 5, "heel").empty());
}

// 13-62h (spec Lot B, I/O matrix rows "Placement" and "Empty space", C2, C3): a divided Sound placed from its
// zones only.
static void TestZonePlacement()
{
    // Row "Placement": Heel [0.10, 0.40] · Gap [0.40, 0.45] xl = xr = 5 ms · Heel [0.45, 0.70]: a Heel event places
    // 2 items overlapping by 10 ms, the left one ends at 0.405, the right one starts at 0.445, 0.05 s earlier.
    {
        SoundData d = Zoned({Zn(0.10, 0.40, "heel"), Zn(0.40, 0.45, "", 0.005, 0.005), Zn(0.45, 0.70, "heel")});
        CHECK(ZonesProblem(d, 1.0).empty());
        const std::vector<PieceApply> pa = PieceApplyFor(d, 0, "heel");
        CHECK(pa.size() == 2);
        if (pa.size() == 2) {
            CHECK(pa[0].anchor && !pa[1].anchor && Near(pa[0].slice.start_offs, 0.10) &&
                  Near(pa[0].slice.start_offs + pa[0].slice.length, 0.405));
            CHECK(Near(pa[1].slice.start_offs, 0.445) && Near(pa[1].slice.length, 0.255));
            CHECK(Near(pa[0].fades.out, 0.010) && Near(pa[1].fades.in, 0.010));  // the crossfade, xl + xr
            CHECK(Near(pa[1].from_anchor, 0.295));  // in the file 0.345 after the anchor's start: 0.05 s earlier
            const double p0 = PiecePosition(10.0, pa[0].from_anchor, 1.0), p1 = PiecePosition(10.0, pa[1].from_anchor, 1.0);
            CHECK(Near(p0 + pa[0].slice.length - p1, 0.010));  // they overlap by 10 ms
            // The whole variation (a combined event): the same two pieces (a Gap is never placed).
            const std::vector<PieceApply> whole = PieceApplyFor(d, 0, "");
            CHECK(whole.size() == 2 && Near(whole[1].from_anchor, 0.295));
        }
    }
    // Row "Empty space": Heel [0.10, 0.30], Toe [0.40, 0.60]: Populate keeps the 0.10 s space.
    {
        SoundData d = Zoned({Zn(0.10, 0.30, "heel"), Zn(0.40, 0.60, "toe")});
        const std::vector<PieceApply> pa = PieceApplyFor(d, 0, "");
        CHECK(pa.size() == 2 && Near(pa[1].from_anchor, 0.30) && Near(pa[0].slice.length, 0.20) &&
              Near(pa[1].slice.start_offs, 0.40));
        if (pa.size() == 2) CHECK(Near(pa[1].from_anchor - (pa[0].from_anchor + pa[0].slice.length), 0.10));
        // Free edges: the Sound's fades (no cut, no Gap).
        CHECK(pa.size() == 2 && Near(pa[0].fades.out, kDefaultFadeOut) && Near(pa[1].fades.in, kDefaultFadeIn));
        // A part event: that part's zone alone, anchored on it.
        const std::vector<PieceApply> toe = PieceApplyFor(d, 0, "toe");
        CHECK(toe.size() == 1 && toe[0].anchor && Near(toe[0].slice.start_offs, 0.40) && Near(toe[0].from_anchor, 0.0));
    }
    // C2: a split Heel | Heel = 2 butted items, each with the At cuts fade at the split, never merged.
    {
        SoundData d = Zoned({Zn(0.10, 0.30, "heel"), Zn(0.30, 0.50, "heel")});
        d.cut_fade = 0.003;
        const std::vector<PieceApply> pa = PieceApplyFor(d, 0, "heel");
        CHECK(pa.size() == 2 && Near(pa[0].slice.length, 0.20) && Near(pa[1].slice.start_offs, 0.30) &&
              Near(pa[1].from_anchor, 0.20) && Near(pa[0].fades.out, 0.003) && Near(pa[1].fades.in, 0.003));
    }
    // A zone with its own fades and gain; the lane fader (what it shifted into the zone gains, C3) is not added.
    {
        SoundData d = Zoned({Zn(0.10, 0.40, "heel"), Zn(0.50, 0.80, "heel")});
        d.frames[0].zones[0].fade_in = 0.007;
        d.frames[0].zones[0].fade_out = 0.009;
        d.frames[0].zones[0].db = -4.5;
        d.frames[0].zones[1].db = 2.0;
        PartSettings p;
        p.fader_db = 2.0;
        SetPartSetting(d, "heel", p);
        d.frames[0].db = -6.0;                        // an old variation dB: not read for zones
        d.frames[0].part_db = {{"heel", 3.0}};        // nor part_db
        const std::vector<PieceApply> pa = PieceApplyFor(d, 0, "heel");
        CHECK(pa.size() == 2 && Near(pa[0].fades.in, 0.007) && Near(pa[0].fades.out, 0.009) && Near(pa[0].db, -4.5) &&
              Near(pa[1].db, 2.0));
        // ItemGainFor(gain + db) is what each item gets (sound_place.cpp).
        CHECK(pa.size() == 2 && Near(ItemGainFor(0.0 + pa[1].db), std::pow(10.0, 0.1), 1e-12));
    }
    // A whole-variation event over Heel · Gap · Toe: every part zone. 13-62h review (R3): a Toe event alone does not
    // crossfade against nothing: its item starts at the Gap's end with the At cuts fade, anchored on it.
    {
        SoundData d = Zoned({Zn(0.10, 0.30, "heel"), Zn(0.30, 0.35, "", 0.004, 0.004), Zn(0.35, 0.60, "toe")});
        CHECK(ZonesProblem(d, 0.0).empty());
        std::vector<PieceApply> pa = PieceApplyFor(d, 0, "");
        CHECK(pa.size() == 2 && Near(pa[0].slice.length, 0.204) && Near(pa[1].slice.start_offs, 0.346) &&
              Near(pa[1].from_anchor, 0.196) && Near(pa[0].fades.out, 0.008) && Near(pa[1].fades.in, 0.008));
        pa = PieceApplyFor(d, 0, "toe");
        CHECK(pa.size() == 1 && pa[0].anchor && Near(pa[0].slice.start_offs, 0.35) && Near(pa[0].slice.length, 0.25) &&
              Near(pa[0].fades.in, kDefaultCutFade));
        // and the Heel event ends at the Gap's start, the At cuts fade out
        pa = PieceApplyFor(d, 0, "heel");
        CHECK(pa.size() == 1 && Near(pa[0].slice.start_offs + pa[0].slice.length, 0.30) && Near(pa[0].fades.out, kDefaultCutFade));
        // Its own snap, measured from the piece's start.
        d.frames[0].zones[2].snap = 0.40;
        CHECK(!PieceSnapNeedsPeaks(d, 0, "toe"));
        pa = PieceApplyFor(d, 0, "toe");
        CHECK(pa.size() == 1 && Near(pa[0].slice.snap_offset, 0.05));
    }
    // Snap = ZoneSnap on the anchor zone: without peaks Attack = its onset + the offset (-2 ms); with peaks the
    // attack measured in it.
    {
        SoundData d = Zoned({Zn(1.0, 1.4, "heel"), Zn(1.4, 2.0, "toe")});
        d.frames[0].zones[0].onset = 1.05;
        CHECK(PieceSnapNeedsPeaks(d, 0, "heel"));
        std::vector<PieceApply> pa = PieceApplyFor(d, 0, "heel");
        CHECK(pa.size() == 1 && Near(pa[0].slice.snap_offset, 0.048, 1e-9));
        PeakData pk;
        pk.rate = 1000.0;
        pk.max_abs.assign(2000, 0.0f);
        for (int i = 1100; i < 1300; ++i) pk.max_abs[static_cast<size_t>(i)] = 0.5f;
        pa = PieceApplyFor(d, 0, "heel", false, &pk);
        CHECK(pa.size() == 1 && Near(pa[0].slice.snap_offset, 0.098, 1e-6));
        // The anchor of a whole event = the first zone; the others carry no snap.
        pa = PieceApplyFor(d, 0, "", false, &pk);
        CHECK(pa.size() == 2 && pa[0].anchor && !pa[1].anchor && Near(pa[1].slice.snap_offset, 0.0));
        // 13-62h review (R2): once the Edit window laid the zones out with the peaks, the measured snap is stored:
        // Populate needs no peaks and places the same snap whether they can be read or not.
        SoundData m = d;
        for (EditZone& z : m.frames[0].zones) z.a_hand = z.b_hand = true;
        RelayoutZones(m, &pk, 2.0);
        CHECK(!PieceSnapNeedsPeaks(m, 0, "heel"));
        pa = PieceApplyFor(m, 0, "heel");
        CHECK(pa.size() == 1 && Near(pa[0].slice.snap_offset, 0.098, 1e-6));
        pa = PieceApplyFor(m, 0, "heel", false, &pk);
        CHECK(pa.size() == 1 && Near(pa[0].slice.snap_offset, 0.098, 1e-6));
    }
    // 13-62h review (R3): Toe [0.10, 0.30] · Gap [0.30, 0.35] · Heel [0.35, 0.50] · Toe [0.60, 0.80]: a Toe event's
    // first item ends at the Gap's start (At cuts fade), the second still comes the jumped 0.05 s earlier.
    {
        SoundData d = Zoned({Zn(0.10, 0.30, "toe"), Zn(0.30, 0.35, "", 0.004, 0.004), Zn(0.35, 0.50, "heel"), Zn(0.60, 0.80, "toe")});
        d.cut_fade = 0.002;
        const std::vector<PieceApply> pa = PieceApplyFor(d, 0, "toe");
        CHECK(pa.size() == 2 && Near(pa[0].slice.length, 0.20) && Near(pa[0].fades.out, 0.002) && Near(pa[1].from_anchor, 0.45));
    }
    // A part with no zone in a variation: nothing placed; an off variation: nothing; Takes: nothing (A8).
    {
        SoundData d = Zoned({Zn(0.10, 0.30, "heel")});
        CHECK(PieceApplyFor(d, 0, "toe").empty() && PieceApplyFor(d, 0, "heel").size() == 1);
        CHECK(!PieceSnapNeedsPeaks(d, 0, "toe"));
        d.frames[0].on = false;
        CHECK(PieceApplyFor(d, 0, "heel").empty());
        d.frames[0].on = true;
        d.mode = DetectMode::Takes;
        CHECK(PieceApplyFor(d, 0, "heel").empty() && !PieceSnapNeedsPeaks(d, 0, "heel"));
    }
}
int main()
{
    TestPartPieceFades();
    TestPartFadesOnPieces();
    TestZonePlacement();
    TestZoneAnchorOwnSnap();
    TestChanMode();
    TestAsAuditioned();
    // Position (Blend offset + nudge) and clamp.
    CHECK(Near(PlacementPosition(10.0, 0.25, 12.0, 0.1), 9.862));
    CHECK(Near(PlacementPosition(0.1, 0.5, 0.0, 0.0), 0.0));
    CHECK(Near(PlacementPosition(5.0, 0.0, -20.0, 0.0), 4.98));
    // Folder child keeps its offset from the folder item.
    CHECK(Near(PileMemberPosition(9.862, 3.0, 3.25), 10.112));
    CHECK(Near(PileMemberPosition(2.0, 4.0, 4.0), 2.0));

    // Gain.
    CHECK(Near(ItemGainFor(-6.0), 0.501187, 1e-5));
    CHECK(Near(ItemGainFor(0.0), 1.0));
    CHECK(Near(ItemGainFor(std::nan("")), 1.0));

    // Slice.
    SoundFrame f;
    f.start = 2.0;
    f.length = 1.0;
    SliceApply s = SliceApplyFor(f);
    CHECK(s.set && Near(s.start_offs, 2.0) && Near(s.length, 1.0) && Near(s.snap_offset, 0.0));
    f.length = 0.0;
    CHECK(!SliceApplyFor(f).set);

    // Folder rule.
    FolderItemFacts ff;
    ff.on_folder_track = true;
    ff.empty_source = true;
    ff.children_inside = true;
    CHECK(IsFolderItemRule(ff));
    ff.children_inside = false;
    ff.nvk_item_type = "folder";
    CHECK(IsFolderItemRule(ff));
    ff.empty_source = false;  // folder track + audio
    CHECK(!IsFolderItemRule(ff));
    ff.empty_source = true;
    ff.on_folder_track = false;  // plain track + empty
    ff.children_inside = true;
    CHECK(!IsFolderItemRule(ff));
    // Span.
    CHECK(SpanInside(1.0, 2.0, 1.0005, 1.9995));
    CHECK(!SpanInside(1.0, 2.0, 0.5, 1.0));

    // NVK chunk.
    const std::string nvk =
        "<ITEM\n  POSITION 1\n  <EXTI\n    nvk_item_type folder\n  >\n  IGUID {A}\n>\n";
    CHECK(NvkItemTypeFromChunk(nvk) == "folder");
    const std::string take_only =
        "<ITEM\n  POSITION 1\n  NAME x\n  <SOURCE WAVE\n    FILE \"a.wav\"\n  >\n  <EXT\n    nvk_item_type folder\n  >\n>\n";
    CHECK(NvkItemTypeFromChunk(take_only).empty());
    CHECK(NvkItemTypeFromChunk("garbage\n>>>\n<<").empty());
    CHECK(NvkItemTypeFromChunk("").empty());

    // Copy chunk (CRLF lines kept as CRLF).
    const std::string src =
        "<ITEM\r\n  POSITION 2\r\n  SEL 1\r\n  IGUID {OLD}\r\n  NAME a\r\n  GUID {T1}\r\n  <SOURCE WAVE\r\n"
        "    FILE \"a.wav\"\r\n  >\r\n  TAKE\r\n  GUID {T2}\r\n  <TAKEFX\r\n    FXID {F1}\r\n  >\r\n>";
    int         n = 0;
    std::string out = PrepareCopyChunk(src, "{NEW}", [&n] { return "{G" + std::to_string(++n) + "}"; });
    const std::string expect =
        "<ITEM\r\n  POSITION 2\r\n  SEL 0\r\n  IGUID {NEW}\r\n  NAME a\r\n  GUID {G1}\r\n  <SOURCE WAVE\r\n"
        "    FILE \"a.wav\"\r\n  >\r\n  TAKE\r\n  GUID {G2}\r\n  <TAKEFX\r\n    FXID {G3}\r\n  >\r\n>";
    CHECK(out == expect);
    CHECK(n == 3);

    // 13-10 (Antho 2026-10-12): an attached sound is never replaced or removed; it may move.
    CHECK(PlacedItemRefusal(false, false, false, Ownership::Populated, false) == "item missing");
    CHECK(PlacedItemRefusal(true, false, false, Ownership::Populated, true) == "not a placed item (no RAV_SND tag)");
    CHECK(PlacedItemRefusal(true, true, true, Ownership::Populated, false) == "folder item");
    CHECK(PlacedItemRefusal(true, true, false, Ownership::Attached, true) == "attached by the user");
    CHECK(PlacedItemRefusal(true, true, false, Ownership::Attached, false).empty());
    CHECK(PlacedItemRefusal(true, true, false, Ownership::Spread, true).empty());
    CHECK(PlacedItemRefusal(true, true, false, Ownership::Populated, true).empty());
    CHECK(PlacedItemRefusal(true, true, true, Ownership::Spread, true) == "folder item");
    CHECK(PlacedItemRefusal(true, false, false, Ownership::Populated, false) == "not a placed item (no RAV_SND tag)");

    // 13-11 (B9): a library source is muted; its copy is placed unmuted, any other source's mute copied.
    CHECK(!PlacedCopyMuted(true, true));
    CHECK(!PlacedCopyMuted(false, true));
    CHECK(PlacedCopyMuted(true, false));
    CHECK(!PlacedCopyMuted(false, false));

    // Antho 2026-10-13: a copy plays at the library's gain alone (never on top of its source item's
    // volume), its take at unity with its polarity kept; a folder pile keeps its members' volumes.
    {
        const PlacedVolumes a = PlacedCopyVolumes(false, 16.4, 0.5, 2.0);
        CHECK(Near(a.item, 2.0) && a.set_take && Near(a.take, 1.0));
        const PlacedVolumes b = PlacedCopyVolumes(false, 1.0, -0.5, 2.0);  // polarity flipped
        CHECK(Near(b.item, 2.0) && b.set_take && Near(b.take, -1.0));
        const PlacedVolumes c = PlacedCopyVolumes(true, 0.5, 0.7, 2.0);  // a folder pile member
        CHECK(Near(c.item, 1.0) && !c.set_take);
    }

    // Antho 2026-10-12 (13-11): a File Sound is placed straight from its file.
    {
        SoundData d;
        d.mode = DetectMode::Whole;
        SoundFrame f;
        f.start = 0.25;
        f.length = 0.5;
        d.frames = {f};
        SliceApply sl = FileSliceFor(d, 0);  // the frame in every mode (Whole too)
        CHECK(sl.set && Near(sl.start_offs, 0.25) && Near(sl.length, 0.5));
        d.mode = DetectMode::Takes;
        CHECK(FileSliceFor(d, 0).set);
        CHECK(!FileSliceFor(d, 1).set && !FileSliceFor(d, -1).set);  // out of range: the whole file
        FadeApply fa = FileFadeApplyFor(d, 0);  // both sides, the Sound's default, with shapes
        CHECK(fa.set_in && fa.set_out && fa.shape_in && fa.shape_out && Near(fa.in, 0.001) && Near(fa.out, 0.005));
        d.frames[0].fade_in = 0.05;
        d.frames[0].fade_in_shape = 3;
        fa = FileFadeApplyFor(d, 0);
        CHECK(Near(fa.in, 0.05) && fa.in_shape == 3 && Near(fa.out, 0.005));
        d.fade_out = 0.02;
        fa = FileFadeApplyFor(d, 4);  // no frame: the Sound's default
        CHECK(fa.set_in && fa.set_out && Near(fa.in, 0.001) && Near(fa.out, 0.02) && fa.in_shape == 0);

        // The item: the slice, the event minus the sync plus Blend and nudge, the gain, the fades.
        FileItemFields it = FileItemFieldsFor(2.0, sl, FileFadeApplyFor(d, 0), -6.0, 10.0, 20.0, 0.5);
        CHECK(it.ok && Near(it.start_offs, 0.25) && Near(it.length, 0.5) && Near(it.snap_offset, 0.0));
        CHECK(Near(it.position, 10.52));
        CHECK(Near(it.volume, std::pow(10.0, -6.0 / 20.0)));
        CHECK(Near(it.fade_in, 0.05) && Near(it.fade_out, 0.02));  // the Sound default set above
        it = FileItemFieldsFor(3.0, SliceApply{}, FadeApply{}, 0.0, 1.0, 0.0, 0.0);  // whole file
        CHECK(it.ok && Near(it.start_offs, 0.0) && Near(it.length, 3.0) && Near(it.volume, 1.0) && Near(it.position, 1.0));
        CHECK(Near(it.fade_in, 0.0) && Near(it.fade_out, 0.0));
        SliceApply past;
        past.set = true;
        past.start_offs = 1.5;
        past.length = 1.0;
        it = FileItemFieldsFor(2.0, past, FadeApply{}, 0.0, 0.0, 0.0, 0.0);  // clamped into the file
        CHECK(it.ok && Near(it.length, 0.5));
        past.start_offs = 2.5;
        CHECK(!FileItemFieldsFor(2.0, past, FadeApply{}, 0.0, 0.0, 0.0, 0.0).ok);  // nothing left
        CHECK(FileItemFieldsFor(0.0, past, FadeApply{}, 0.0, 0.0, 0.0, 0.0).ok);   // length unknown: as is
        CHECK(!FileItemFieldsFor(0.0, SliceApply{}, FadeApply{}, 0.0, 0.0, 0.0, 0.0).ok);
        it = FileItemFieldsFor(1.0, SliceApply{}, FileFadeApplyFor(d, 0), 0.0, 0.0, -50.0, 0.0);  // clamped at 0
        CHECK(it.ok && Near(it.position, 0.0));
        FadeApply big;
        big.set_in = big.set_out = true;
        big.in = 0.8;
        big.out = 0.8;
        it = FileItemFieldsFor(1.0, SliceApply{}, big, 0.0, 0.0, 0.0, 0.0);
        CHECK(it.fade_in + it.fade_out <= 1.0 + 1e-9);  // fades clamped into the item

        CHECK(MediaCopyName("steps.wav", 0) == "steps.wav");
        CHECK(MediaCopyName("steps.wav", 1) == "steps-001.wav");
        CHECK(MediaCopyName("my.steps.flac", 12) == "my.steps-012.flac");
        CHECK(MediaCopyName("noext", 2) == "noext-002");
        CHECK(MediaCopyName(".hidden", 1) == ".hidden-001");
    }

    // Antho 2026-10-12: the fades a placed variation sets.
    {
        SoundData d;
        d.mode = DetectMode::Silence;
        SoundFrame f;
        f.start = 1.0;
        f.length = 0.5;
        d.frames = {f};
        FadeApply fa = FadeApplyFor(d, 0);  // a slice: the Sound's default on both sides
        CHECK(fa.set_in && fa.set_out && Near(fa.in, 0.001) && Near(fa.out, 0.005) && fa.in_shape == 0);
        CHECK(fa.shape_in && fa.shape_out);  // a slice writes its shapes
        fa = FadeApplyFor(d, 0, true);       // a folder-pile source: no fades placed
        CHECK(!fa.set_in && !fa.set_out);
        CHECK(!FadeApplyFor(d, 1).set_in && !FadeApplyFor(d, -1).set_out);  // out of range: nothing
        d.frames[0].fade_out = 0.1;
        d.frames[0].fade_out_shape = 2;
        fa = FadeApplyFor(d, 0);
        CHECK(Near(fa.out, 0.1) && fa.out_shape == 2 && Near(fa.in, 0.001));
        d.mode = DetectMode::Takes;  // a take too
        CHECK(FadeApplyFor(d, 0).set_in && FadeApplyFor(d, 0).set_out);
        // A Whole-item Sound keeps its item's own fades, except a side set by hand.
        d.mode = DetectMode::Whole;
        fa = FadeApplyFor(d, 0);
        CHECK(!fa.set_in && fa.set_out && Near(fa.out, 0.1) && fa.shape_out);
        d.frames[0].fade_out_shape = 0;  // a hand-set length with the default shape: the item's shape kept
        fa = FadeApplyFor(d, 0);
        CHECK(fa.set_out && !fa.shape_out);
        d.frames[0].fade_out = -1.0;
        fa = FadeApplyFor(d, 0);
        CHECK(!fa.set_in && !fa.set_out);
        // On the item: playrate 2 halves the fades like the item; the copy's own for an unset side.
        FadeApply x;
        x.set_in = true;
        x.in = 0.01;
        double in = -1.0, out = -1.0;
        FadesOnItem(x, 2.0, 0.25, 0.3, 0.02, &in, &out);
        CHECK(Near(in, 0.005) && Near(out, 0.02));
        // Clamped to the placed item's length.
        x.set_out = true;
        x.out = 1.0;
        x.in = 1.0;
        FadesOnItem(x, 1.0, 0.5, 0.0, 0.0, &in, &out);
        CHECK(Near(in, 0.25) && Near(out, 0.25));
        FadesOnItem(x, 0.0, 0.5, 0.0, 0.0, &in, &out);  // a bad rate reads as 1
        CHECK(Near(in + out, 0.5));
    }

    TestPieces();

    if (g_fail) {
        std::printf("sound_place_plan_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("sound_place_plan_test: all passed\n");
    return 0;
}
