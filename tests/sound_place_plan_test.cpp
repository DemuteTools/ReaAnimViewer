// SPDX-License-Identifier: MIT
//
// Host test of src/sound_place_plan.h (13-6): position, gain, slice, folder rule, NVK chunk,
// copy chunk, pile member offset.

#include "sound_place_plan.h"
#include "sound_entries.h"
#include "sound_record.h"  // 13-62i (D): the placed tag's g / pi
#include "sound_zones.h"

#include <algorithm>
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

// 13-62i (spec 13-62i, parity): the old model's frame placement (the PieceApplyFor of 13-20..13-62h for an undivided
// Sound, kept here as the reference) against the placement of the same Sound read into zones (MigrateLegacyZones):
// the same spans, positions, snaps, gains and fades -- but P5 (Takes: the zone's Sync point, the old model placed no
// piece) and P6 (a Whole-item Sound's zones get the Sound's default fades).
static std::vector<PieceApply> LegacyPieces(const SoundData& data, int variation, bool file)
{
    std::vector<PieceApply> out;
    if (data.mode == DetectMode::Takes || variation < 0 || variation >= static_cast<int>(data.frames.size())) return out;
    const std::vector<PartSpan> spans = PlacedSpans(data, variation, "");
    if (spans.empty()) return out;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    constexpr double  kEps = 1e-6;
    const FadeApply   base = file ? FileFadeApplyFor(data, variation) : FadeApplyFor(data, variation);
    const double      cut = std::isfinite(data.cut_fade) && data.cut_fade > 0.0 ? data.cut_fade : 0.0;
    double            sync = SyncFor(data, variation, "");
    if (sync >= 0.0 && ClampIntoSpans(spans, sync) != sync) sync = -1.0;
    if (sync < 0.0) sync = AutoSync(data, nullptr, spans);
    size_t anchor = 0;
    for (size_t k = 0; k < spans.size(); ++k)
        if (sync >= spans[k].start - kEps && sync <= spans[k].start + spans[k].length + kEps) {
            anchor = k;
            break;
        }
    for (size_t k = 0; k < spans.size(); ++k) {
        const PartSpan& sp = spans[k];
        PieceApply      pa;
        pa.slice.set = true;
        pa.slice.start_offs = sp.start;
        pa.slice.length = sp.length;
        pa.fades = base;
        if (sp.start > f.start + kEps) {
            pa.fades.set_in = pa.fades.shape_in = true;
            pa.fades.in = cut;
            pa.fades.in_shape = 0;
        }
        if (sp.start + sp.length < f.start + f.length - kEps) {
            pa.fades.set_out = pa.fades.shape_out = true;
            pa.fades.out = cut;
            pa.fades.out_shape = 0;
        }
        pa.anchor = k == anchor;
        if (pa.anchor) pa.slice.snap_offset = std::min(std::max(sync - sp.start, 0.0), sp.length);
        pa.from_anchor = sp.start - spans[anchor].start;
        pa.db = FrameGainDb(f, "");  // the old model's gain (added to the event's then: the zone's now)
        out.push_back(pa);
    }
    return out;
}

// The pieces of `old` (an undivided Sound of the old model) as placed now: read into zones first.
static std::vector<PieceApply> ZonedPieces(const SoundData& old, int v, bool file)
{
    SoundData z = old;
    CHECK(MigrateLegacyZones(z));
    SanitizeZones(z);
    CHECK(ZonesProblem(z, 0.0).empty());
    return PieceApplyFor(z, v, "", file);
}

// The same items: spans, anchor, positions, gains; with `fades` also every fade side (set or not, length, shape).
static bool SamePieces(const std::vector<PieceApply>& a, const std::vector<PieceApply>& b, bool fades = true)
{
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k) {
        const PieceApply &x = a[k], &y = b[k];
        if (!Near(x.slice.start_offs, y.slice.start_offs, 1e-9) || !Near(x.slice.length, y.slice.length, 1e-9) ||
            x.anchor != y.anchor || !Near(x.from_anchor, y.from_anchor, 1e-9) || !Near(x.db, y.db, 1e-9))
            return false;
        if (x.anchor && !Near(x.slice.snap_offset, y.slice.snap_offset, 1e-9)) return false;
        if (fades && (x.fades.set_in != y.fades.set_in || x.fades.set_out != y.fades.set_out ||
                      !Near(x.fades.in, y.fades.in, 1e-9) || !Near(x.fades.out, y.fades.out, 1e-9) ||
                      x.fades.in_shape != y.fades.in_shape || x.fades.out_shape != y.fades.out_shape))
            return false;
    }
    return true;
}

static void TestPieces()
{
    PeakData pk;
    pk.rate = 1000.0;
    pk.max_abs.assign(2000, 0.0f);
    for (int i = 1020; i < 1300; ++i) pk.max_abs[static_cast<size_t>(i)] = 0.5f;
    // Silence, the attack at 1.020 measured and stored (1.018): D_STARTOFFS 1.000, D_SNAPOFFSET 0.018.
    SoundData d;
    d.mode = DetectMode::Silence;
    SoundFrame f;
    f.start = 1.0;
    f.length = 0.4;
    d.frames.push_back(f);
    d.frames[0].syncs = {1.018};
    std::vector<PieceApply> pa = ZonedPieces(d, 0, false);
    CHECK(pa.size() == 1 && pa[0].anchor && Near(pa[0].slice.start_offs, 1.0) && Near(pa[0].slice.length, 0.4));
    CHECK(Near(pa[0].slice.snap_offset, 0.018, 1e-9));
    CHECK(Near(PlacementPosition(5.0, pa[0].slice.snap_offset, 0.0, 0.0), 4.982, 1e-9));  // the sync on the event
    CHECK(SamePieces(LegacyPieces(d, 0, false), pa, true));
    // Matrix "Undivided migration": trimmed 0.10-0.70, fade in 5 ms, -3 dB: one zone, the same item.
    {
        SoundData m;
        m.mode = DetectMode::Silence;
        SoundFrame g;
        g.start = 0.10;
        g.length = 0.60;
        g.fade_in = 0.005;
        g.db = -3.0;
        g.hand = true;
        m.frames = {g};
        SoundData z = m;
        CHECK(MigrateLegacyZones(z));
        CHECK(z.frames[0].zones.size() == 1);
        const EditZone& e = z.frames[0].zones[0];
        CHECK(e.part == kWholePart && Near(e.a, 0.10) && Near(e.b, 0.70) && e.fade_in && Near(*e.fade_in, 0.005) && Near(e.db, -3.0) &&
              e.a_hand && e.b_hand && z.frames[0].fade_in < 0.0 && z.frames[0].db == 0.0);
        CHECK(SamePieces(LegacyPieces(m, 0, false), ZonedPieces(m, 0, false), true));
        CHECK(SamePieces(LegacyPieces(m, 0, true), ZonedPieces(m, 0, true), true));  // a File Sound
        // Laid out again with peaks: the hand edges stay (the same item still).
        SanitizeZones(z);
        RelayoutZones(z, &pk, 2.0);
        CHECK(Near(z.frames[0].zones[0].a, 0.10) && Near(z.frames[0].zones[0].b, 0.70));
    }
    // Without a stored sync: the first span's start + the offset (clamped), as before; a hand sync wins.
    SoundData nd = d;
    nd.frames[0].syncs.clear();
    CHECK(SamePieces(LegacyPieces(nd, 0, false), ZonedPieces(nd, 0, false), true));
    SetSyncSettings(nd, SyncMode::SliceStart, 3.0);
    CHECK(SamePieces(LegacyPieces(nd, 0, false), ZonedPieces(nd, 0, false), true));
    SoundData hd = d;
    hd.frames[0].hand_syncs = {1.25};
    hd.frames[0].syncs = {1.25};
    CHECK(SamePieces(LegacyPieces(hd, 0, false), ZonedPieces(hd, 0, false), true));
    CHECK(Near(ZonedPieces(hd, 0, false)[0].slice.snap_offset, 0.25));
    // A shaped fade of the frame's own (a File Sound: both sides set).
    SoundData fd = d;
    fd.frames[0].fade_in = 0.004;
    fd.frames[0].fade_in_shape = 2;
    pa = ZonedPieces(fd, 0, true);
    CHECK(pa.size() == 1 && Near(pa[0].fades.in, 0.004) && pa[0].fades.in_shape == 2 && pa[0].fades.set_out && Near(pa[0].from_anchor, 0.0));
    CHECK(SamePieces(LegacyPieces(fd, 0, true), pa, true));
    const FileItemFields ff = FileItemFieldsFor(10.0, pa[0].slice, pa[0].fades, 0.0, 5.0, 0.0, 0.0);
    CHECK(ff.ok && Near(ff.snap_offset, 0.018) && Near(ff.position, 4.982) && Near(ff.start_offs, 1.0));
    // Sound-level excluded zones cut the variation as before (13-20): two zones, cut_fade at the cut edges, the second
    // placed from the same anchor.
    SoundData zd = d;
    zd.zones.push_back({1.25, 0.05, "", {}});
    std::vector<PieceApply> za = ZonedPieces(zd, 0, false);
    CHECK(za.size() == 2 && Near(za[0].slice.length, 0.25) && Near(za[1].slice.start_offs, 1.3) && Near(za[1].from_anchor, 0.3) &&
          Near(za[0].fades.out, kDefaultCutFade) && Near(za[1].fades.in, kDefaultCutFade));
    CHECK(SamePieces(LegacyPieces(zd, 0, false), za, true));
    // TakeMarkers and gains: the same.
    SoundData tm = d;
    tm.mode = DetectMode::TakeMarkers;
    tm.frames.push_back(f);
    tm.frames[1].start = 1.5;
    tm.frames[1].db = 4.5;
    tm.frames[1].syncs.clear();
    for (int v = 0; v < 2; ++v) CHECK(SamePieces(LegacyPieces(tm, v, false), ZonedPieces(tm, v, false), true));
    // P6: a Whole-item Sound: the same spans and snap; its zones get the Sound's default fades (the old copy kept its
    // item's fades where the frame set none).
    SoundData wd = d;
    wd.mode = DetectMode::Whole;
    pa = ZonedPieces(wd, 0, false);
    CHECK(SamePieces(LegacyPieces(wd, 0, false), pa, false) && !SamePieces(LegacyPieces(wd, 0, false), pa));  // only the fades differ
    CHECK(!LegacyPieces(wd, 0, false)[0].fades.set_in && !LegacyPieces(wd, 0, false)[0].fades.set_out);
    CHECK(pa.size() == 1 && pa[0].fades.set_in && Near(pa[0].fades.in, kDefaultFadeIn) && Near(pa[0].fades.out, kDefaultFadeOut));
    // P5 / matrix "Takes mode": each take's zone placed (the old model placed none: the take kept its item's snap),
    // its snap the Sound's Sync point (here: no peaks, the take's start + the offset, clamped).
    SoundData tk = d;
    tk.mode = DetectMode::Takes;
    tk.frames.push_back(f);
    tk.frames[1].start = 0.2;
    tk.frames[0].syncs.clear();
    CHECK(LegacyPieces(tk, 0, false).empty());
    for (int v = 0; v < 2; ++v) {
        pa = ZonedPieces(tk, v, false);
        CHECK(pa.size() == 1 && Near(pa[0].slice.start_offs, tk.frames[static_cast<size_t>(v)].start) && Near(pa[0].slice.length, 0.4) &&
              Near(pa[0].slice.snap_offset, 0.0));
    }
    // A bad index or an off frame: none.
    CHECK(PieceApplyFor(d, 3, "").empty() && PieceApplyFor(d, -1, "").empty());
    SoundData off = d;
    off.frames[0].on = false;
    CHECK(PieceApplyFor(off, 0, "").empty());
    // ApplyMeasuredSyncs: the measured syncs written back into the store's Sound (an old model's Sound).
    {
        SoundStore   store;
        SoundLibrary lib;
        lib.uid = "L";
        SoundNode n;
        n.uid = "s";
        n.type = SoundNodeType::Sound;
        n.sound = d;  // frame [1, 1.4)
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

// 13-68 v3: an Attached source's copy keeps its take's rate (keep_rate). The Dress Boots case: a layer at rate 1.335,
// its snap 50 ms into the item: the Sound's sync is 66.75 ms of source (SourceSecondsOf, as SoundOf writes it); the
// anchor piece's snap (source s, PieceApplyFor) read back on the copy at 1.335 (ApplyPiece: ItemSecondsOf) is 50 ms
// again, its length the variation's over the rate; nothing folded. Rate 1: unchanged either way.
static void TestKeepRate()
{
    const double rate = 1.335, snap = 0.050;
    CHECK(Near(SourceSecondsOf(snap, rate), 0.06675) && Near(ItemSecondsOf(0.06675, rate), snap));
    for (double bad : {0.0, -1.0, std::nan("")}) CHECK(Near(SourceSecondsOf(snap, bad), snap) && Near(ItemSecondsOf(snap, bad), snap));
    SoundData d;
    d.mode = DetectMode::Whole;
    d.keep_rate = true;
    d.sync_mode = SyncMode::SliceStart;
    d.sync_offset_ms = SourceSecondsOf(snap, rate) * 1000.0;
    SoundFrame fr;
    fr.start = 0.2;  // the item's take offset (source s)
    fr.length = 0.534;  // a 0.4 s item at 1.335
    d.frames.push_back(fr);
    const std::vector<PieceApply> pa = PieceApplyFor(d, 0, "");
    CHECK(pa.size() == 1 && pa[0].anchor);
    if (pa.size() == 1) {
        CHECK(Near(pa[0].slice.start_offs, 0.2) && Near(pa[0].slice.snap_offset, 0.06675, 1e-9));
        CHECK(Near(ItemSecondsOf(pa[0].slice.snap_offset, rate), snap, 1e-9));  // the copy's snap: on the marker
        CHECK(Near(ItemSecondsOf(pa[0].slice.length, rate), 0.4, 1e-9));
    }
    PlayrateFold k = FoldPlayrate(rate, 0.4, snap, true);  // kept: the copy as the source has it
    CHECK(!k.reset && Near(k.rate, rate) && Near(k.length, 0.4) && Near(k.snap, snap));
    PlayrateFold p = FoldPlayrate(rate, 0.4, snap);  // as auditioned (rate 1): source seconds
    CHECK(p.reset && Near(p.length, 0.534) && Near(p.snap, 0.06675));
    const PlayrateFold one = FoldPlayrate(1.0, 0.4, snap, true);
    CHECK(Near(one.length, 0.4) && Near(one.snap, snap) && Near(SourceSecondsOf(snap, 1.0), snap));
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

// 13-62i (matrix "Undivided, Split" / "Undivided, Gap"): an undivided Sound's zone split at 0.30 places 2 items at
// the same place as before (seamless, At cuts fades); a Gap 0.40-0.45 places 2 items crossfaded, the second 0.05 s
// earlier than in the file.
static void TestUndividedPlacement()
{
    SoundData d = Zoned({Zn(0.10, 0.70, kWholePart)});
    d.divide_group.clear();
    d.divide_values.clear();
    d.frames[0].zones[0].a_hand = d.frames[0].zones[0].b_hand = true;
    d.frames[0].zones[0].snap = 0.12;
    CHECK(ZonesProblem(d, 0.0).empty());
    const std::vector<PieceApply> one = PieceApplyFor(d, 0, "");
    CHECK(one.size() == 1 && Near(one[0].slice.snap_offset, 0.02));
    {  // 13-62i: a folder pile ignores its pieces: placed at the gain + its anchor zone's gain (the variation's).
        SoundData pd = d;
        pd.frames[0].zones[0].db = -3.0;
        CHECK(Near(PileGainDb(PieceApplyFor(pd, 0, ""), -2.0), -5.0) && Near(PileGainDb({}, -2.0), -2.0));
        CHECK(SetPartFader(pd, kWholePart, 1.0) && Near(PileGainDb(PieceApplyFor(pd, 0, ""), -2.0), -4.0));  // its fader too
    }
    const double pos0 = PlacementPosition(5.0, one[0].slice.snap_offset, 0.0, 0.0);  // where the item starts
    SoundData s = d;
    CHECK(SplitZoneAt(s, 0.30));
    const std::vector<PieceApply> two = PieceApplyFor(s, 0, "");
    CHECK(two.size() == 2 && two[0].anchor && Near(two[0].slice.snap_offset, 0.02));
    const double p0 = PlacementPosition(5.0, two[0].slice.snap_offset, 0.0, 0.0), p1 = PiecePosition(p0, two[1].from_anchor, 1.0);
    CHECK(Near(p0, pos0) && Near(two[0].slice.length, 0.20) && Near(p1, p0 + 0.20) && Near(two[1].slice.start_offs, 0.30));
    CHECK(Near(two[0].fades.out, kDefaultCutFade) && Near(two[1].fades.in, kDefaultCutFade));  // At cuts, as a REAPER split
    SoundData g = d;
    bool      made = false;
    CHECK(MakeGap(g, 0.40, 0.45, {}, &made) && made);
    const std::vector<PieceApply> gp = PieceApplyFor(g, 0, "");
    CHECK(gp.size() == 2 && Near(gp[0].slice.start_offs, 0.10) && Near(gp[0].slice.length, 0.305) && Near(gp[1].slice.start_offs, 0.445));
    // In the file the second starts at 0.445 (0.345 after the first); heard / placed 0.295 after it: 0.05 s earlier.
    CHECK(Near(gp[1].from_anchor, 0.295) && Near((gp[1].slice.start_offs - gp[0].slice.start_offs) - gp[1].from_anchor, 0.05));
    CHECK(Near(gp[0].fades.out, kDefaultGapXfade) && Near(gp[1].fades.in, kDefaultGapXfade));  // crossfaded over 10 ms
    CHECK(Near(gp[0].slice.start_offs + gp[0].slice.length - gp[1].from_anchor - gp[0].slice.start_offs, kDefaultGapXfade));
}

// 13-62i (matrix "Divide by -> None"): the parts' own fades, Sync point and Pre-roll / Tail written into their zones
// first, so Populate places the same items after the parts' settings go.
static void TestDivideNonePlacement()
{
    SoundData d = Zoned({Zn(0.10, 0.30, "heel"), Zn(0.40, 0.70, "toe"), Zn(0.70, 0.90, "heel")});
    d.frames[0].zones[1].onset = 0.43;
    PartSettings t;
    t.fade_in = 0.02;
    t.fade_out = 0.03;
    t.sync_mode = SyncMode::SliceStart;
    t.sync_offset_ms = 5.0;
    t.preroll_ms = 20.0;
    SetPartSetting(d, "toe", t);
    PartSettings h;
    h.fade_out = 0.015;  // Heel's end at 0.30 is free (its own fade); at 0.90 too; at 0.70 the cut (At cuts)
    SetPartSetting(d, "heel", h);
    RelayoutZones(d, nullptr);
    const std::vector<PieceApply> before = PieceApplyFor(d, 0, "");
    CHECK(SetDivide(d, "", {}) && d.parts.empty());
    RelayoutZones(d, nullptr);
    const std::vector<PieceApply> after = PieceApplyFor(d, 0, "");
    CHECK(before.size() == 3 && after.size() == before.size());
    for (size_t k = 0; k < before.size() && k < after.size(); ++k) {
        CHECK(Near(before[k].slice.start_offs, after[k].slice.start_offs) && Near(before[k].slice.length, after[k].slice.length));
        CHECK(Near(before[k].fades.in, after[k].fades.in) && Near(before[k].fades.out, after[k].fades.out) && Near(before[k].db, after[k].db));
        CHECK(before[k].anchor == after[k].anchor && Near(before[k].from_anchor, after[k].from_anchor) &&
              (!before[k].anchor || Near(before[k].slice.snap_offset, after[k].slice.snap_offset)));
    }
    // Toe alone (its own Sync point): the same snap after.
    SoundData e = Zoned({Zn(0.40, 0.70, "toe")});
    SetPartSetting(e, "toe", t);
    const double snap0 = PieceApplyFor(e, 0, "toe")[0].slice.snap_offset;
    CHECK(SetDivide(e, "", {}) && Near(PieceApplyFor(e, 0, "")[0].slice.snap_offset, snap0) && Near(snap0, 0.005));
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
    // Snap = ZoneSnap on the anchor zone: without peaks Attack = its onset + the offset; with peaks the
    // attack measured in it.
    {
        SoundData d = Zoned({Zn(1.0, 1.4, "heel"), Zn(1.4, 2.0, "toe")});
        d.frames[0].zones[0].onset = 1.05;
        CHECK(PieceSnapNeedsPeaks(d, 0, "heel"));
        std::vector<PieceApply> pa = PieceApplyFor(d, 0, "heel");
        CHECK(pa.size() == 1 && Near(pa[0].slice.snap_offset, 0.050, 1e-9));
        PeakData pk;
        pk.rate = 1000.0;
        pk.max_abs.assign(2000, 0.0f);
        for (int i = 1100; i < 1300; ++i) pk.max_abs[static_cast<size_t>(i)] = 0.5f;
        pa = PieceApplyFor(d, 0, "heel", false, &pk);
        CHECK(pa.size() == 1 && Near(pa[0].slice.snap_offset, 0.100, 1e-6));
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
        CHECK(pa.size() == 1 && Near(pa[0].slice.snap_offset, 0.100, 1e-6));
        pa = PieceApplyFor(m, 0, "heel", false, &pk);
        CHECK(pa.size() == 1 && Near(pa[0].slice.snap_offset, 0.100, 1e-6));
    }
    // 13-62h review (R3): Toe [0.10, 0.30] · Gap [0.30, 0.35] · Heel [0.35, 0.50] · Toe [0.60, 0.80]: a Toe event's
    // first item ends at the Gap's start (At cuts fade), the second still comes the jumped 0.05 s earlier.
    {
        SoundData d = Zoned({Zn(0.10, 0.30, "toe"), Zn(0.30, 0.35, "", 0.004, 0.004), Zn(0.35, 0.50, "heel"), Zn(0.60, 0.80, "toe")});
        d.cut_fade = 0.002;
        const std::vector<PieceApply> pa = PieceApplyFor(d, 0, "toe");
        CHECK(pa.size() == 2 && Near(pa[0].slice.length, 0.20) && Near(pa[0].fades.out, 0.002) && Near(pa[1].from_anchor, 0.45));
    }
    // A part with no zone in a variation: nothing placed; an off variation: nothing; 13-62i: Takes: its zones.
    {
        SoundData d = Zoned({Zn(0.10, 0.30, "heel")});
        CHECK(PieceApplyFor(d, 0, "toe").empty() && PieceApplyFor(d, 0, "heel").size() == 1);
        CHECK(!PieceSnapNeedsPeaks(d, 0, "toe"));
        d.frames[0].on = false;
        CHECK(PieceApplyFor(d, 0, "heel").empty());
        d.frames[0].on = true;
        d.mode = DetectMode::Takes;
        CHECK(PieceApplyFor(d, 0, "heel").size() == 1);
    }
}

// 13-62i (D, P2): a kept placed item follows the library's gain now; its tag records the dB it plays at.
static void TestKeptVolume()
{
    const double m6 = ItemGainFor(-6.0), m12 = ItemGainFor(-12.0);
    // Tagged at -6, the library says -12 now: D_VOL x 10^(-6/20); the user's own +2 dB on the item kept.
    KeptVolume k = KeptItemVolume({-12.0}, false, 1, true, -6.0, 0, m6);
    CHECK(k.set && Near(k.vol, m12, 1e-12) && Near(k.g, -12.0) && k.pi == 0);
    k = KeptItemVolume({-12.0}, false, 1, true, -6.0, 0, m6 * ItemGainFor(2.0));
    CHECK(k.set && Near(k.vol, m12 * ItemGainFor(2.0), 1e-12));
    CHECK(!KeptItemVolume({-6.0}, false, 1, true, -6.0, 0, m6).set);  // unchanged: nothing written
    // Pieces: each by its own index (gain + its zone's db); an index out of range is left.
    k = KeptItemVolume({-12.0, -9.0}, false, 2, true, -3.0, 1, ItemGainFor(-3.0));
    CHECK(k.set && Near(k.vol, ItemGainFor(-9.0), 1e-12) && k.pi == 1);
    CHECK(!KeptItemVolume({-12.0}, false, 2, true, -3.0, 1, 1.0).set);
    CHECK(!KeptItemVolume({-12.0}, false, 1, true, -3.0, -1, 1.0).set);
    // A pile member (D_VOL = its source's x gain): the ratio keeps the pile's balance.
    k = KeptItemVolume({-12.0}, true, 3, true, -6.0, 0, 0.5 * m6);
    CHECK(k.set && Near(k.vol, 0.5 * m12, 1e-12));
    // An older tag (no g): a lone plain item of one piece is set as placed now; anything else is left.
    k = KeptItemVolume({-12.0}, false, 1, false, 0.0, -1, 0.9);
    CHECK(k.set && Near(k.vol, m12, 1e-12) && Near(k.g, -12.0) && k.pi == 0);
    CHECK(!KeptItemVolume({-12.0}, true, 1, false, 0.0, -1, 0.9).set);
    CHECK(!KeptItemVolume({-12.0}, false, 2, false, 0.0, -1, 0.9).set);
    CHECK(!KeptItemVolume({-12.0, -12.0}, false, 1, false, 0.0, -1, 0.9).set);
    CHECK(!KeptItemVolume({}, false, 1, true, -6.0, 0, 0.9).set);

    // The tag round trip: g / pi written and read; an older tag reads as unknown.
    PlacedTag t;
    t.owner = "{A}";
    t.rid = "r";
    t.node = "n";
    t.has_g = true;
    t.g = -7.25;
    t.pi = 2;
    PlacedTag back;
    CHECK(ParsePlacedTag(SerializePlacedTag(t), &back) && back.has_g && Near(back.g, -7.25) && back.pi == 2);
    PlacedTag old;
    old.owner = "{A}";
    const std::string old_text = SerializePlacedTag(old);
    PlacedTag ob;
    CHECK(ParsePlacedTag(old_text, &ob) && !ob.has_g && ob.pi == -1);
    t.g = 0.0;  // 0 dB is still known
    CHECK(ParsePlacedTag(SerializePlacedTag(t), &back) && back.has_g && Near(back.g, 0.0));
}
int main()
{
    SetNewSoundSyncMode(SyncMode::Attack);  // 13-67b: these fixtures were written for the old default (Attack)
    TestDivideNonePlacement();  // 13-62i
    TestUndividedPlacement();  // 13-62i
    TestKeptVolume();
    TestPartFadesOnPieces();
    TestZonePlacement();
    TestZoneAnchorOwnSnap();
    TestChanMode();
    TestAsAuditioned();
    TestKeepRate();
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

    {  // freeze audit #5: the waits of one gesture share a budget
        WaitBudget b;
        CHECK(Near(b.Grant(100.0, 3.0), 3.0));
        b.Spent(103.0, 3.0);
        CHECK(Near(b.Grant(103.1, 3.0), 3.0));
        b.Spent(106.1, 3.0);
        b.Spent(109.2, 3.0);
        CHECK(Near(b.Grant(109.3, 3.0), 1.0));  // 9 s used of 10
        b.Spent(110.3, 1.0);
        CHECK(Near(b.Grant(110.4, 3.0), 0.0));  // spent: no more waiting in this gesture
        CHECK(Near(b.Grant(113.0, 3.0), 3.0));  // idle for over 2 s: a new gesture, a fresh budget
        CHECK(Near(b.Grant(113.0, -1.0), 0.0));
        b.Spent(113.0, -5.0);                   // a bad value never gives time back
        CHECK(Near(b.Grant(113.1, 3.0), 3.0));
    }

    if (g_fail) {
        std::printf("sound_place_plan_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("sound_place_plan_test: all passed\n");
    return 0;
}
