// SPDX-License-Identifier: MIT
//
// Host test of src/sound_zones.h (13-62h): the zone model of a divided Sound (the validated mockup's relayout,
// cutOut, makeGap, fillGap, splitAt, trimAt, heal, move, fades, env, snap, heard pieces, normalize groups), the
// spec's I/O matrix rows about the model, the record round trip and the migration of the old model, and a seeded
// gesture fuzz (>= 20 000 gestures) that checks the invariant (ZonesProblem) and parse(write(d)) == d after each.

#include "sound_entries.h"
#include "sound_record.h"
#include "sound_zones.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

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

const char* const kHeel = "heel";
const char* const kToe = "toe";

// A divided Sound (Heel / Toe) with no zones and no frames.
SoundData Base()
{
    SoundData d;
    d.mode = DetectMode::Silence;
    d.divide_group = "g";
    d.divide_values = {kHeel, kToe};
    return d;
}

EditZone Z(const std::string& part, double a, double b)
{
    EditZone z;
    z.part = part;
    z.a = z.slot_a = a;
    z.b = z.slot_b = b;
    z.a_hand = z.b_hand = true;  // no peaks in most tests: edges stay
    return z;
}

EditZone G(double a, double b, double xl, double xr)
{
    EditZone g;
    g.a = g.slot_a = a;
    g.b = g.slot_b = b;
    g.xl = xl;
    g.xr = xr;
    return g;
}

// One variation holding `zones` (start / length = their extent).
SoundData With(std::vector<EditZone> zones)
{
    SoundData  d = Base();
    SoundFrame f;
    f.zones = zones;
    f.start = zones.front().a;
    f.length = zones.back().b - f.start;
    d.frames.push_back(f);
    return d;
}

const EditZone& At(const SoundData& d, int f, int k)
{
    return d.frames[static_cast<size_t>(f)].zones[static_cast<size_t>(k)];
}

// Peaks: rate 1000 bins/s over 3 s, silence (-90 dB) except where `loud` says (dBFS).
PeakData Peaks(const std::vector<std::pair<double, double>>& loud_spans, double db = -6.0)
{
    PeakData p;
    p.rate = 1000.0;
    p.max_abs.assign(3000, static_cast<float>(std::pow(10.0, -90.0 / 20.0)));
    for (const auto& s : loud_spans)
        for (long long i = static_cast<long long>(std::llround(s.first * 1000.0)); i < static_cast<long long>(std::llround(s.second * 1000.0)) && i < 3000; ++i)
            p.max_abs[static_cast<size_t>(i)] = static_cast<float>(std::pow(10.0, db / 20.0));
    return p;
}

bool SameZones(const SoundData& x, const SoundData& y)
{
    if (x.frames.size() != y.frames.size()) return false;
    for (size_t f = 0; f < x.frames.size(); ++f) {
        const SoundFrame &a = x.frames[f], &b = y.frames[f];
        if (a.start != b.start || a.length != b.length || a.on != b.on || a.zones.size() != b.zones.size()) return false;
        for (size_t k = 0; k < a.zones.size(); ++k) {
            const EditZone &p = a.zones[k], &q = b.zones[k];
            if (p.a != q.a || p.b != q.b || p.part != q.part || p.slot_a != q.slot_a || p.slot_b != q.slot_b ||
                p.a_hand != q.a_hand || p.b_hand != q.b_hand || p.a_auto_cut != q.a_auto_cut || p.onset != q.onset ||
                p.snap != q.snap || p.fade_in != q.fade_in || p.fade_out != q.fade_out || p.db != q.db || p.xl != q.xl ||
                p.xr != q.xr || p.kept != q.kept || p.fade_in_shape != q.fade_in_shape || p.fade_out_shape != q.fade_out_shape)
                return false;
        }
    }
    if (x.parts.size() != y.parts.size()) return false;
    for (size_t i = 0; i < x.parts.size(); ++i)
        if (x.parts[i].value != y.parts[i].value || x.parts[i].fader_db != y.parts[i].fader_db) return false;
    return x.gap_xfade == y.gap_xfade && x.fader_db == y.fader_db;  // 13-62i: the implicit part's fader too
}

SoundStore StoreOf(const SoundData& d)
{
    SoundStore   s;
    SwitchGroup  g;
    g.uid = "g";
    g.name = "FootPart";
    g.values = {{kHeel, "Heel", {}, {}}, {kToe, "Toe", {}, {}}};
    s.groups.push_back(g);
    SoundLibrary lib;
    lib.uid = "lib";
    lib.name = "L";
    SoundNode n;
    n.uid = "n1";
    n.type = SoundNodeType::Sound;
    n.name = "FS";
    n.sound = d;
    lib.nodes.push_back(n);
    lib.root = n.uid;
    s.libraries.push_back(lib);
    return s;
}

bool RoundTrips(const SoundData& d)
{
    SoundStore back;
    if (!ParseSoundStore(SerializeSoundStore(StoreOf(d)), &back) || back.libraries.size() != 1 || back.libraries[0].nodes.empty())
        return false;
    // and written again, the same text
    return SameZones(d, back.libraries[0].nodes[0].sound) && SerializeSoundStore(back) == SerializeSoundStore(StoreOf(d));
}

// ---- Matrix rows ----------------------------------------------------------------------------------------------

void TestMigration()
{
    // Old frame: cuts 0.30, own heel,toe, an excluded zone 0.50-0.55 inside Toe, part_db toe -2.
    SoundData  d = Base();
    d.mode = DetectMode::TakeMarkers;  // no margins: the raw pieces
    SoundFrame f;
    f.start = 0.1;
    f.length = 0.9;
    f.cuts = {0.30};
    f.owners = {kHeel, kToe};
    f.part_db[kToe] = -2.0;
    f.fade_in = 0.003;
    d.frames.push_back(f);
    SoundZone ex;
    ex.start = 0.50;
    ex.length = 0.05;
    d.zones.push_back(ex);
    SoundStore s = StoreOf(d);
    s.libraries[0].nodes[0].name = "FS_keep";
    const std::string text = SerializeSoundStore(s);
    CHECK(text.find("own=") != std::string::npos && text.find("fzone") == std::string::npos);
    SoundStore back;
    CHECK(ParseSoundStore(text, &back));
    const SoundData& m = back.libraries[0].nodes[0].sound;
    CHECK(m.frames.size() == 1 && m.frames[0].zones.size() == 3);
    if (m.frames.size() == 1 && m.frames[0].zones.size() == 3) {
        CHECK(At(m, 0, 0).part == kHeel && Near(At(m, 0, 0).a, 0.1) && Near(At(m, 0, 0).b, 0.30) && At(m, 0, 0).db == 0.0);
        CHECK(At(m, 0, 1).part == kToe && Near(At(m, 0, 1).a, 0.30) && Near(At(m, 0, 1).b, 0.50) && Near(At(m, 0, 1).db, -2.0));
        CHECK(At(m, 0, 2).part == kToe && Near(At(m, 0, 2).a, 0.55) && Near(At(m, 0, 2).b, 1.0) && Near(At(m, 0, 2).db, -2.0));
        CHECK(At(m, 0, 0).fade_in && Near(*At(m, 0, 0).fade_in, 0.003));
    }
    // The old keys are gone, nothing else changed (the node's name, the frame's extent); written again: zones only.
    CHECK(m.zones.empty() && m.frames[0].cuts.empty() && m.frames[0].owners.empty() && m.frames[0].part_db.empty());
    CHECK(back.libraries[0].nodes[0].name == "FS_keep" && Near(m.frames[0].start, 0.1) && Near(m.frames[0].length, 0.9));
    const std::string again = SerializeSoundStore(back);
    CHECK(again.find("own=") == std::string::npos && again.find("cuts=") == std::string::npos && again.find("fzone") != std::string::npos);
    SoundStore twice;
    CHECK(ParseSoundStore(again, &twice) && SerializeSoundStore(twice) == again);
    CHECK(ZonesProblem(m, 0.0).empty());
    // The same spans the old model placed: Heel [0.1, 0.3], Toe [0.3, 0.5] + [0.55, 1.0].
    CHECK(PlacedSpans(m, 0, kToe).size() == 2 && PlacedSpans(m, 0, kHeel).size() == 1);
    // A .ravlib reads the same (ParseRavlib uses the store parser).
}

void TestHeardPieces()
{
    // Placement / heard row: Heel [0.10, 0.40] Gap [0.40, 0.45] xl = xr = 5 ms Heel [0.45, 0.70].
    SoundData d = With({Z(kHeel, 0.10, 0.40), G(0.40, 0.45, 0.005, 0.005), Z(kHeel, 0.45, 0.70)});
    CHECK(ZonesProblem(d, 3.0).empty());
    std::vector<HeardPiece> p = HeardPieces(d, 0.10, 0.70);
    CHECK(p.size() == 2);
    if (p.size() == 2) {
        CHECK(Near(p[0].a, 0.10) && Near(p[0].b, 0.405) && Near(p[0].xo, 0.010) && p[0].z.index == 0);
        CHECK(Near(p[1].a, 0.445) && Near(p[1].b, 0.70) && Near(p[1].xo, 0.0) && p[1].z.index == 2);
        // heard: the second one starts (0.405 - 0.10) - 0.010 = 0.295 after the first, 0.05 s earlier than in the file
        CHECK(Near((p[0].b - p[0].a - p[0].xo) - (p[1].a - p[0].a), -0.05));
    }
    // The crossfade: Heel 1 fades out over the 10 ms, Heel 2 fades in.
    CHECK(Near(ZoneFade(d, {0, 0}, 1).s, 0.010) && ZoneFade(d, {0, 0}, 1).src == FadeSrc::Gap);
    CHECK(Near(ZoneEnv(d, {0, 0}, 0.395), 1.0) && Near(ZoneEnv(d, {0, 0}, 0.400), 0.5) && ZoneEnv(d, {0, 0}, 0.406) == 0.0);
    CHECK(Near(ZoneEnv(d, {0, 2}, 0.445), 0.0) && Near(ZoneEnv(d, {0, 2}, 0.450), 0.5));
    // Empty space: Heel [0.10, 0.30], Toe [0.40, 0.60]: a 0.10 s silence between them.
    SoundData e = With({Z(kHeel, 0.10, 0.30), Z(kToe, 0.40, 0.60)});
    p = HeardPieces(e, 0.10, 0.60);
    CHECK(p.size() == 3 && p[1].z.frame < 0 && Near(p[1].a, 0.30) && Near(p[1].b, 0.40));
    // From inside a Gap: it resumes at the jump's exit.
    p = HeardPieces(d, 0.42, 0.70);
    CHECK(p.size() == 1 && Near(p[0].a, 0.445));
    // Chains and Gap neighbours.
    CHECK(ChainOf(d, {0, 0}).size() == 2 && ChainOf(d, {0, 1}).size() == 2 && ChainOf(e, {0, 0}).size() == 1);
    CHECK(GapLeft(d, {0, 1}).index == 0 && GapRight(d, {0, 1}).index == 2);
}

void TestGapGestures()
{
    // Gap delete: Heel · Gap · Toe, Delete on the Gap: Heel extends over it, Toe untouched.
    SoundData d = With({Z(kHeel, 0.10, 0.40), G(0.40, 0.45, 0.005, 0.005), Z(kToe, 0.45, 0.70)});
    ZoneSel   sel = {{0, 1}};
    CHECK(DeleteZones(d, sel));
    CHECK(d.frames[0].zones.size() == 2 && Near(At(d, 0, 0).b, 0.45) && At(d, 0, 1).part == kToe && Near(At(d, 0, 1).a, 0.45) &&
          Near(At(d, 0, 1).b, 0.70));
    CHECK(sel.empty() && ZonesProblem(d, 3.0).empty());
    // Gap without neighbour: Alt+drag over a zone's end into empty space: trimmed, no Gap.
    SoundData e = With({Z(kHeel, 0.10, 0.40)});
    bool      made = true;
    CHECK(MakeGap(e, 0.35, 0.50, {}, &made) && !made);
    CHECK(e.frames[0].zones.size() == 1 && Near(At(e, 0, 0).b, 0.35) && GapCount(e) == 0);
    // A Gap between two zones: made, both edges held, crossfade = At Gaps (10 ms, half each side).
    SoundData f = With({Z(kHeel, 0.10, 0.70)});
    ZoneSel   gs;
    CHECK(MakeGap(f, 0.40, 0.45, {}, &made, &gs) && made && gs.size() == 1);
    CHECK(f.frames[0].zones.size() == 3 && At(f, 0, 1).IsGap() && Near(At(f, 0, 1).xl, 0.005) && Near(At(f, 0, 1).xr, 0.005));
    CHECK(ZonesProblem(f, 3.0).empty() && RoundTrips(f));
    // Zone in a Gap: Shift+drag inside it: Gap | new zone | Gap, each outer crossfade kept.
    SoundData g = With({Z(kHeel, 0.10, 0.40), G(0.40, 0.60, 0.007, 0.003), Z(kToe, 0.60, 0.90)});
    ZoneSel   nz;
    CHECK(NewZone(g, kToe, 0.45, 0.55, {}, &nz) && nz.size() == 1);
    CHECK(g.frames[0].zones.size() == 5 && At(g, 0, 1).IsGap() && At(g, 0, 3).IsGap() && At(g, 0, 2).part == kToe);
    CHECK(Near(At(g, 0, 1).a, 0.40) && Near(At(g, 0, 1).b, 0.45) && Near(At(g, 0, 1).xl, 0.007));
    CHECK(Near(At(g, 0, 3).a, 0.55) && Near(At(g, 0, 3).b, 0.60) && Near(At(g, 0, 3).xr, 0.003));
    CHECK(ZonesProblem(g, 3.0).empty());
    // FillGap: the zone on the left takes it.
    SoundData h = With({Z(kHeel, 0.10, 0.40), G(0.40, 0.45, 0.005, 0.005), Z(kToe, 0.45, 0.70)});
    CHECK(FillGap(h, {0, 1}) && h.frames[0].zones.size() == 2 && Near(At(h, 0, 0).b, 0.45));
    // Gap crossfade handles: each side <= half the Gap; linked or one side.
    SoundData x = With({Z(kHeel, 0.10, 0.40), G(0.40, 0.45, 0.005, 0.005), Z(kToe, 0.45, 0.70)});
    CHECK(DragGapXfade(x, {0, 1}, -1, 0.41, true) && Near(At(x, 0, 1).xl, 0.01) && Near(At(x, 0, 1).xr, 0.01));
    CHECK(DragGapXfade(x, {0, 1}, 1, 0.40, false) && Near(At(x, 0, 1).xr, 0.025) && Near(At(x, 0, 1).xl, 0.01));
    CHECK(!DragGapXfade(x, {0, 1}, 1, 0.40, false));  // nothing changed
    // Gap edge: the zone beside it follows.
    CHECK(DragGapEdge(x, {0, 1}, -1, 0.38) && Near(At(x, 0, 0).b, 0.38) && Near(At(x, 0, 1).a, 0.38));
    CHECK(ZonesProblem(x, 3.0).empty());
}

void TestHealSplitTrim()
{
    // Heal: Toe · Gap · Toe butted, selected: one Toe zone over the three.
    SoundData d = With({Z(kToe, 0.10, 0.40), G(0.40, 0.45, 0.005, 0.005), Z(kToe, 0.45, 0.70)});
    ZoneSel   sel = {{0, 0}, {0, 1}, {0, 2}};
    CHECK(HealZones(d, sel) && d.frames[0].zones.size() == 1 && Near(At(d, 0, 0).a, 0.10) && Near(At(d, 0, 0).b, 0.70));
    CHECK(sel.size() == 1 && sel[0].index == 0);
    // Not edge to edge: nothing.
    SoundData e = With({Z(kHeel, 0.10, 0.30), Z(kToe, 0.40, 0.60)});
    ZoneSel   s2 = {{0, 0}, {0, 1}};
    CHECK(!HealZones(e, s2) && e.frames[0].zones.size() == 2);
    // Heal keeps the first one's part.
    SoundData m = With({Z(kHeel, 0.10, 0.30), Z(kToe, 0.30, 0.60)});
    ZoneSel   s3 = {{0, 0}, {0, 1}};
    bool      mixed = false;
    CHECK(HealZones(m, s3, {}, &mixed) && mixed && m.frames[0].zones.size() == 1 && At(m, 0, 0).part == kHeel);
    // Split at the cursor: two butted zones, At cuts fades.
    SoundData s = With({Z(kHeel, 0.10, 0.70)});
    ZoneSel   r;
    CHECK(SplitZoneAt(s, 0.30, {}, &r) && s.frames[0].zones.size() == 2 && r.size() == 1 && r[0].index == 1);
    CHECK(ZoneFade(s, {0, 0}, 1).src == FadeSrc::Cut && ZoneFade(s, {0, 1}, -1).src == FadeSrc::Cut);
    CHECK(Near(ZoneFade(s, {0, 0}, 1).s, kDefaultCutFade));
    CHECK(!SplitZoneAt(s, 0.302));  // too close to an edge
    // Trims at the cursor: the selected zones under it, else the one under it.
    SoundData t = With({Z(kHeel, 0.10, 0.70)});
    CHECK(TrimZonesAt(t, {}, 0.20, -1) && Near(At(t, 0, 0).a, 0.20) && Near(t.frames[0].start, 0.20));
    CHECK(TrimZonesAt(t, {{0, 0}}, 0.50, 1) && Near(At(t, 0, 0).b, 0.50));
    CHECK(!TrimZonesAt(t, {}, 0.90, 1));
    // 40508: time selection 0.2-0.5 over two selected zones: both trimmed to [0.2, 0.5] ∩ zone.
    SoundData q = With({Z(kHeel, 0.10, 0.30), Z(kToe, 0.35, 0.60), Z(kToe, 0.70, 0.80)});
    CHECK(TrimZonesTo(q, {{0, 0}, {0, 1}}, 0.2, 0.5));
    CHECK(Near(At(q, 0, 0).a, 0.2) && Near(At(q, 0, 0).b, 0.3) && Near(At(q, 0, 1).a, 0.35) && Near(At(q, 0, 1).b, 0.5));
    CHECK(Near(At(q, 0, 2).a, 0.70) && Near(At(q, 0, 2).b, 0.80));  // not selected: untouched
    // Nothing selected: the zones under it.
    SoundData q2 = With({Z(kHeel, 0.10, 0.30), Z(kToe, 0.35, 0.60), Z(kToe, 0.70, 0.80)});
    CHECK(TrimZonesTo(q2, {}, 0.2, 0.5) && Near(At(q2, 0, 2).b, 0.80) && Near(At(q2, 0, 0).a, 0.2));
}

void TestMoveAndEdges()
{
    // A butted edge moves alone (stops on the neighbour, opens empty space backing off).
    SoundData d = With({Z(kHeel, 0.10, 0.30), Z(kToe, 0.30, 0.60)});
    CHECK(!DragZoneEdge(d, {0, 0}, 1, 0.40) && Near(At(d, 0, 0).b, 0.30));  // stopped on Toe: nothing moved
    CHECK(DragZoneEdge(d, {0, 0}, 1, 0.25) && Near(At(d, 0, 0).b, 0.25) && Near(At(d, 0, 1).a, 0.30));
    // The cut grab moves both.
    SoundData c = With({Z(kHeel, 0.10, 0.30), Z(kToe, 0.30, 0.60)});
    CHECK(DragCut(c, {0, 0}, 0.35) && Near(At(c, 0, 0).b, 0.35) && Near(At(c, 0, 1).a, 0.35));
    // Moving a zone drags a butted neighbour; empty space stops it.
    SoundData m = With({Z(kHeel, 0.10, 0.30), Z(kToe, 0.30, 0.60), Z(kHeel, 0.70, 0.80)});
    CHECK(MoveZone(m, {0, 1}, 0.05) && Near(At(m, 0, 1).a, 0.35) && Near(At(m, 0, 1).b, 0.65) && Near(At(m, 0, 0).b, 0.35));
    CHECK(Near(MoveRange(m, {0, 1}, 1.0), 0.05));  // up to the next zone across empty space
    CHECK(MoveZone(m, {0, 1}, 1.0) && Near(At(m, 0, 1).b, 0.70));
    CHECK(ZonesProblem(m, 3.0).empty());
    // Double-click an edge: automatic again (with peaks it follows Pre-roll / Tail).
    PeakData  pk = Peaks({{0.15, 0.25}});
    SoundData a = With({Z(kHeel, 0.10, 0.50)});
    a.silence.tail_ms = 20.0;
    ZoneContext ctx;
    ctx.peaks = &pk;
    CHECK(ResetZoneEdge(a, {0, 0}, 1, ctx) && !At(a, 0, 0).b_hand && Near(At(a, 0, 0).b, 0.27, 1e-3));
    // Fades, snap, gain.
    CHECK(SetZoneFade(a, {0, 0}, -1, 0.004) && At(a, 0, 0).fade_in && ZoneFade(a, {0, 0}, -1).src == FadeSrc::Own);
    CHECK(ResetZoneFade(a, {0, 0}, -1) && ZoneFade(a, {0, 0}, -1).src == FadeSrc::Sound);
    CHECK(SetZoneSnap(a, {0, 0}, 0.2) && ZoneSnap(a, {0, 0}, &pk).own && Near(ZoneSnap(a, {0, 0}, &pk).t, 0.2));
    CHECK(ResetZoneSnap(a, {0, 0}) && !ZoneSnap(a, {0, 0}, &pk).own);
    CHECK(Near(ZoneSnap(a, {0, 0}, &pk).t, 0.15 - 0.002, 1e-3));  // Attack, -2 ms
    CHECK(SetZoneDb(a, {0, 0}, 3.04) && Near(At(a, 0, 0).db, 3.0) && !SetZoneDb(a, {0, 0}, 3.0));
}

void TestTailWins()
{
    PeakData    pk = Peaks({{0.15, 0.20}, {0.40, 0.45}});
    SoundData   d = Base();
    SoundFrame  f;
    f.start = 0.10;
    f.length = 0.60;
    d.frames.push_back(f);
    ZoneContext ctx;
    ctx.peaks = &pk;
    RefreshParts(d, &pk);  // proposes: Heel, Toe cut before Toe's onset
    CHECK(d.frames[0].zones.size() == 2 && ZonesProblem(d, 3.0).empty());
    PartSettings t;
    t.tail_ms = 40.0;
    SetPartSetting(d, kToe, t);
    RelayoutZones(d, &pk);
    CHECK(Near(At(d, 0, 1).b, 0.49, 1e-3));
    // Toe's end dragged by hand, then its Tail 40 -> 80 ms.
    CHECK(DragZoneEdge(d, {0, 1}, 1, 0.60, ctx) && At(d, 0, 1).b_hand);
    SoundData off = d;
    t.tail_ms = 80.0;
    SetPartSetting(d, kToe, t);
    CHECK(ApplyPreTailChange(d, kToe, true, PartField::Tail, ctx) == 1 && !At(d, 0, 1).b_hand && Near(At(d, 0, 1).b, 0.53, 1e-3));
    SetPartSetting(off, kToe, t);
    CHECK(ApplyPreTailChange(off, kToe, false, PartField::Tail, ctx) == 1 && At(off, 0, 1).b_hand && Near(At(off, 0, 1).b, 0.60));
    // An edge held by a Gap never moves.
    SoundData g = With({Z(kToe, 0.10, 0.30), G(0.30, 0.35, 0.005, 0.005), Z(kToe, 0.35, 0.60)});
    CHECK(ApplyPreTailChange(g, kToe, true, PartField::Tail, ctx) == 1);  // only the last zone's free end
    CHECK(Near(At(g, 0, 0).b, 0.30) && At(g, 0, 1).IsGap());
}

void TestFaderAndNormalize()
{
    // Fader: Heel zones at -1 and +2 dB; fader 0 -> +2: +1 / +4; to +3: +2 / +5; double-click: back.
    SoundData d = With({Z(kHeel, 0.10, 0.30), Z(kToe, 0.30, 0.40), Z(kHeel, 0.50, 0.60)});
    d.frames[0].zones[0].db = -1.0;
    d.frames[0].zones[2].db = 2.0;
    CHECK(SetPartFader(d, kHeel, 2.0) && Near(At(d, 0, 0).db, 1.0) && Near(At(d, 0, 2).db, 4.0) && Near(PartFaderDb(d, kHeel), 2.0));
    CHECK(At(d, 0, 1).db == 0.0);
    CHECK(SetPartFader(d, kHeel, 3.0) && Near(At(d, 0, 0).db, 2.0) && Near(At(d, 0, 2).db, 5.0));
    CHECK(SetPartFader(d, kHeel, 0.0) && Near(At(d, 0, 0).db, -1.0) && Near(At(d, 0, 2).db, 2.0) && PartFaderDb(d, kHeel) == 0.0);
    CHECK(d.parts.empty() && !SetPartFader(d, kHeel, 0.0));
    // The fader round-trips.
    SetPartFader(d, kHeel, 1.5);
    CHECK(RoundTrips(d));
    // Normalize: two Heel zones glued by a Gap + one lone Heel zone: 2 groups, one gain each, the fader back to 0.
    SoundData n = With({Z(kHeel, 0.10, 0.30), G(0.30, 0.35, 0.005, 0.005), Z(kHeel, 0.35, 0.50), Z(kHeel, 0.60, 0.70)});
    SetPartFader(n, kHeel, 2.0);
    const std::vector<std::vector<ZoneRef>> groups = NormGroups(n, std::string(kHeel));
    CHECK(groups.size() == 2 && groups[0].size() == 2 && groups[1].size() == 1);
    const std::vector<NormRegion> regs = NormGroupRegions(n, groups, NormMode::Peak, -1.0);
    CHECK(regs.size() == 2 && regs[0].spans.size() == 2 && regs[0].part == kHeel && Near(regs[0].target, -1.0));
    CHECK(ApplyZoneNormalize(n, groups, {4.0, -2.0}, true) == 2);
    CHECK(Near(At(n, 0, 0).db, 4.0) && Near(At(n, 0, 2).db, 4.0) && Near(At(n, 0, 3).db, -2.0) && PartFaderDb(n, kHeel) == 0.0);
    // A silent group is skipped.
    CHECK(ApplyZoneNormalize(n, groups, {std::nan(""), 1.0}, false) == 1 && Near(At(n, 0, 0).db, 4.0));
    // 13-62h review: a part with a silent group keeps its fader (those zones still carry it); all groups measured:
    // back to 0.
    SetPartFader(n, kHeel, 3.0);
    CHECK(ApplyZoneNormalize(n, groups, {std::nan(""), 1.0}, true) == 1 && Near(PartFaderDb(n, kHeel), 3.0));
    CHECK(ApplyZoneNormalize(n, groups, {0.5, 1.0}, true) == 2 && PartFaderDb(n, kHeel) == 0.0);
    // From a selection: the lone zone only.
    CHECK(NormGroups(n, ZoneSel{{0, 3}}).size() == 1);
}

void TestProposeAndRecord()
{
    // SetDivide + RefreshParts propose one zone per part; undivided: no zones.
    PeakData   pk = Peaks({{0.15, 0.20}, {0.40, 0.45}, {1.20, 1.25}, {1.40, 1.45}});
    SoundData  d;
    d.mode = DetectMode::Silence;
    SoundFrame f1, f2;
    f1.start = 0.10;
    f1.length = 0.60;
    f1.db = 1.5;
    f2.start = 1.10;
    f2.length = 0.60;
    d.frames = {f1, f2};
    CHECK(SetDivide(d, "g", {kHeel, kToe}));
    RefreshParts(d, &pk);
    CHECK(d.frames[0].zones.size() == 2 && d.frames[1].zones.size() == 2 && ZonesProblem(d, 3.0).empty());
    CHECK(At(d, 0, 1).a_auto_cut && At(d, 0, 1).onset > 0.0 && Near(At(d, 0, 0).db, 1.5) && d.frames[0].db == 0.0);
    CHECK(Near(At(d, 0, 1).slot_a, At(d, 0, 1).onset - 0.005, 1e-6));
    CHECK(PartZoneCount(d) == 4 && GapCount(d) == 0 && ZonesOfPart(d, kToe).size() == 2);
    // Toe's own Pre-roll 50 ms: RAV's cut follows it before Toe's onset.
    PartSettings pre;
    pre.preroll_ms = 50.0;
    SetPartSetting(d, kToe, pre);
    RelayoutZones(d, &pk);
    CHECK(Near(At(d, 0, 1).slot_a, At(d, 0, 1).onset - 0.05, 1e-6));
    CHECK(RoundTrips(d));
    // Unknown fzone fields are kept.
    SoundData u = d;
    u.frames[0].zones[0].kept = {{"zz", "1"}, {"bare", ""}};
    CHECK(RoundTrips(u));
    // 13-62i (matrix "Divide by -> None"): the zones stay where they are, all of the implicit part, gains kept.
    SetZoneDb(d, {0, 0}, 4.5);
    SetZoneDb(d, {0, 1}, -2.0);
    const SoundData was = d;
    CHECK(SetDivide(d, "", {}));
    CHECK(d.frames[0].zones.size() == was.frames[0].zones.size() && d.frames[1].zones.size() == was.frames[1].zones.size());
    for (size_t f = 0; f < d.frames.size(); ++f)
        for (size_t k = 0; k < d.frames[f].zones.size(); ++k) {
            const EditZone &z = d.frames[f].zones[k], &w = was.frames[f].zones[k];
            CHECK(z.part == kWholePart && z.a == w.a && z.b == w.b && z.db == w.db);
        }
    CHECK(d.frames[0].db == 0.0 && d.parts.empty() && UsesZones(d) && ZonesProblem(d, 3.0).empty());
    RefreshParts(d, &pk);
    CHECK(d.frames[0].zones.size() == 2 && Near(At(d, 0, 0).db, 4.5) && Near(At(d, 0, 1).db, -2.0) && ZonesProblem(d, 3.0).empty());
    CHECK(RoundTrips(d));
    {  // 13-62i (matrix "None -> Divide by"): divided again: zones proposed as 13-62h (the first zone's gain)
        SoundData r = d;
        CHECK(SetDivide(r, "g", {kHeel, kToe}));
        RefreshParts(r, &pk);
        CHECK(r.frames[0].zones.size() == 2 && Near(At(r, 0, 0).db, 4.5) && Near(At(r, 0, 1).db, 4.5) && r.frames[0].db == 0.0);
        CHECK(At(r, 0, 0).part == kHeel && At(r, 0, 1).part == kToe && ZonesProblem(r, 3.0).empty());
    }
    // 13-62i: Takes mode: each take gets zones (proposed on its own timeline; the peaks of no take given: even split).
    SoundData tk = Base();
    tk.mode = DetectMode::Takes;
    tk.frames = {f1, f1};
    RefreshParts(tk, &pk);
    CHECK(tk.frames[0].zones.size() == 2 && tk.frames[1].zones.size() == 2 && ZonesProblem(tk, 3.0).empty());
    CHECK(At(tk, 0, 0).a_hand && At(tk, 0, 1).b_hand && !At(tk, 0, 0).b_hand);  // P7: a take keeps its outer edges
    CHECK(ZoneTimeline(tk, 1) == 1 && AllZones(tk, 1).size() == 2 && ZoneAt(tk, 0.2, 1).frame == 1 && ZoneAt(tk, 0.2).frame < 0);
}

// 13-62h (Antho 2026-10-15, v5): an item (zones joined by Gaps) moves whole (MoveChain): every member by the same
// delta, a butted neighbour's edge following, one across empty space stopping it; Heal / Delete / NormGroups of it.
void TestItems()
{
    // Heel | Gap | Heel, then a Toe butted to it, a Heel after empty space.
    SoundData d = With({Z(kHeel, 0.10, 0.30), G(0.30, 0.36, 0.005, 0.005), Z(kHeel, 0.36, 0.60), Z(kToe, 0.60, 0.80), Z(kHeel, 0.90, 1.00)});
    const ZoneSel item = {{0, 0}, {0, 1}, {0, 2}};
    CHECK(ItemOf(d, {0, 1}) == item && ItemOf(d, {0, 3}).empty() && ItemOf(d, {0, 4}).empty());
    CHECK(Near(MoveChainRange(d, {0, 0}, -1.0), -0.10));                // the file's start
    CHECK(Near(MoveChainRange(d, {0, 2}, 1.0), 0.80 - 0.006 - 0.60));   // the butted Toe keeps 6 ms
    CHECK(Near(MoveChainRange(d, {0, 2}, 0.05), 0.05) && Near(MoveChainRange(d, {0, 3}, 0.05), 0.0));
    d.frames[0].zones[2].snap = 0.40;
    ZoneSel sel;
    CHECK(MoveChain(d, {0, 1}, 0.05, {}, &sel) && sel.size() == 3);
    CHECK(Near(At(d, 0, 0).a, 0.15) && Near(At(d, 0, 0).b, 0.35) && Near(At(d, 0, 1).a, 0.35) && Near(At(d, 0, 1).b, 0.41));
    CHECK(Near(At(d, 0, 2).a, 0.41) && Near(At(d, 0, 2).b, 0.65) && At(d, 0, 2).snap && Near(*At(d, 0, 2).snap, 0.45));
    CHECK(Near(At(d, 0, 1).xl, 0.005) && Near(At(d, 0, 1).xr, 0.005));  // the Gap keeps its crossfade
    CHECK(Near(At(d, 0, 3).a, 0.65) && Near(At(d, 0, 3).b, 0.80) && Near(At(d, 0, 4).a, 0.90));  // the Toe's edge followed
    CHECK(ZonesProblem(d, 3.0).empty() && ItemOf(d, sel.front()).size() == 3);
    CHECK(!MoveChain(d, {0, 3}, 0.05) && !MoveChain(d, {0, 0}, std::nan("")));  // a zone alone; a bad number
    // Back across empty space: it stops on the zone before it (no zone eaten), the file's end bounds it.
    SoundData e = With({Z(kToe, 0.00, 0.05), Z(kHeel, 0.10, 0.30), G(0.30, 0.36, 0.005, 0.005), Z(kHeel, 0.36, 0.60)});
    CHECK(Near(MoveChainRange(e, {0, 1}, -1.0), -0.05) && Near(MoveChainRange(e, {0, 3}, 1.0, 0.70), 0.10));
    CHECK(MoveChain(e, {0, 3}, -1.0) && Near(At(e, 0, 1).a, 0.05) && Near(At(e, 0, 0).b, 0.05) && Near(At(e, 0, 0).a, 0.0));
    // Heal into one zone: its Gap absorbed. Delete it: empty space remains. Normalize: one group per part.
    SoundData h = With({Z(kHeel, 0.10, 0.30), G(0.30, 0.36, 0.005, 0.005), Z(kToe, 0.36, 0.60), Z(kHeel, 0.90, 1.00)});
    const ZoneSel it = ItemOf(h, {0, 0});
    CHECK(it.size() == 3 && NormGroups(h, it).size() == 2);
    ZoneSel hs = it;
    bool    mixed = false;
    CHECK(HealZones(h, hs, {}, &mixed) && mixed && h.frames[0].zones.size() == 2 && Near(At(h, 0, 0).b, 0.60));
    SoundData x = With({Z(kHeel, 0.10, 0.30), G(0.30, 0.36, 0.005, 0.005), Z(kHeel, 0.36, 0.60), Z(kHeel, 0.90, 1.00)});
    ZoneSel   xs = ItemOf(x, {0, 2});
    CHECK(NormGroups(x, xs).size() == 1 && NormGroups(x, xs).front().size() == 2);
    CHECK(DeleteZones(x, xs) && x.frames[0].zones.size() == 1 && Near(At(x, 0, 0).a, 0.90) && ZonesProblem(x, 3.0).empty());
}


// 13-62i (spec 13-62i matrix): an undivided Sound is zones of its one implicit part, with the same gestures and
// results; a Takes-mode Sound's takes are timelines of their own.
EditZone W(double a, double b) { return Z(kWholePart, a, b); }

SoundData Whole(std::vector<EditZone> zones)
{
    SoundData d = With(std::move(zones));
    d.divide_group.clear();
    d.divide_values.clear();
    return d;
}

void TestUndividedAndTakes()
{
    {  // Split: two zones of the implicit part, butted, one change.
        SoundData d = Whole({W(0.10, 0.70)});
        CHECK(ZonesProblem(d, 3.0).empty() && PartIndex(d, kWholePart) == 0 && PartIndex(d, kHeel) < 0);
        ZoneSel s;
        CHECK(SplitZoneAt(d, 0.30, {}, &s));
        CHECK(d.frames[0].zones.size() == 2 && At(d, 0, 0).part == kWholePart && At(d, 0, 1).part == kWholePart &&
              Near(At(d, 0, 0).b, 0.30) && Near(At(d, 0, 1).a, 0.30) && Butted(d, {0, 0}, {0, 1}));
        CHECK(ZoneFade(d, {0, 0}, 1).src == FadeSrc::Cut && ZonesProblem(d, 3.0).empty() && RoundTrips(d));
        // Heal: one zone again.
        ZoneSel h = {{0, 0}, {0, 1}};
        CHECK(HealZones(d, h) && d.frames[0].zones.size() == 1 && Near(At(d, 0, 0).a, 0.10) && Near(At(d, 0, 0).b, 0.70));
    }
    {  // Gap: Alt+drag 0.40-0.45 inside the variation: zone · Gap · zone.
        SoundData d = Whole({W(0.10, 0.70)});
        bool      made = false;
        CHECK(MakeGap(d, 0.40, 0.45, {}, &made) && made && d.frames[0].zones.size() == 3 && At(d, 0, 1).IsGap());
        CHECK(ItemOf(d, {0, 0}).size() == 3 && ZonesProblem(d, 3.0).empty() && RoundTrips(d));
        // A new zone made by Shift+drag in empty space is of the implicit part (never "": a Gap).
        CHECK(NewZone(d, kWholePart, 1.0, 1.2) && ZonesOfPart(d, kWholePart).size() == 3);
        CHECK(!NewZone(d, kHeel, 1.5, 1.6));  // not one of its parts
    }
    {  // Fader and Normalize with one part: the implicit part's fader lives on the Sound (fdr), round-trips.
        SoundData d = Whole({W(0.10, 0.30), G(0.30, 0.35, 0.005, 0.005), W(0.35, 0.50), W(0.60, 0.70)});
        d.frames[0].zones[0].db = -1.0;
        CHECK(SetPartFader(d, kWholePart, 2.0) && Near(At(d, 0, 0).db, 1.0) && Near(At(d, 0, 3).db, 2.0));
        CHECK(Near(PartFaderDb(d, kWholePart), 2.0) && d.fader_db && d.parts.empty() && RoundTrips(d));
        const std::vector<std::vector<ZoneRef>> groups = NormGroups(d, std::string());
        CHECK(groups.size() == 2 && groups[0].size() == 2);
        CHECK(ApplyZoneNormalize(d, groups, {3.0, -1.0}, true) == 2 && Near(At(d, 0, 0).db, 3.0) && Near(At(d, 0, 3).db, -1.0) &&
              !d.fader_db);
        // Fade shapes round-trip (a migrated frame's shaped fade).
        d.frames[0].zones[3].fade_out = 0.004;
        d.frames[0].zones[3].fade_out_shape = 3;
        CHECK(RoundTrips(d));
    }
    {  // Undivided migration: an old project's variation (trimmed, its own fade in, -3 dB) read as one zone.
        SoundStore back;
        CHECK(ParseSoundStore("RAVSOUNDS 1\nlibrary uid=lib name=L root=n1\nnode uid=n1 type=sound name=FS\nsound kind=item mode=silence\n"
                              "frame start=0.1 len=0.6 db=-3 hand=1 fin=0.005 fish=2\nframe start=1 len=0.5 on=0\n",
                              &back));
        const SoundData& m = back.libraries[0].nodes[0].sound;
        CHECK(m.frames.size() == 2 && m.frames[0].zones.size() == 1 && m.frames[1].zones.size() == 1 && !m.frames[1].on);
        const EditZone& z = m.frames[0].zones[0];
        CHECK(z.part == kWholePart && Near(z.a, 0.1) && Near(z.b, 0.7) && Near(z.db, -3.0) && z.fade_in && Near(*z.fade_in, 0.005) &&
              z.fade_in_shape == 2 && z.a_hand && z.b_hand && m.frames[0].db == 0.0 && m.frames[0].fade_in < 0.0);
        CHECK(ZonesProblem(m, 0.0).empty() && m.frames[0].hand);
    }
    {  // Divide by -> None -> Divide by (SetDivide): the hand-edited zones become the implicit part, then are proposed.
        SoundData d = With({Z(kHeel, 0.10, 0.30), G(0.30, 0.35, 0.005, 0.005), Z(kToe, 0.35, 0.50)});
        SetPartFader(d, kToe, 2.0);
        CHECK(SetDivide(d, "", {}) && d.frames[0].zones.size() == 3 && At(d, 0, 2).part == kWholePart && Near(At(d, 0, 2).db, 2.0) &&
              d.parts.empty() && ZonesProblem(d, 3.0).empty());
    }
    {  // The implicit part's settings are the Sound's: no parts entry ('*' read from a file is dropped), Reset = defaults.
        SoundData d = Whole({W(0.10, 0.70)});
        PartSettings bad;
        bad.value = kWholePart;
        bad.tail_ms = 9.0;
        d.parts.push_back(bad);
        CHECK(PartSettingsOf(d, kWholePart) == nullptr && OwnSettingCount(d, kWholePart) == 0);
        SanitizeZones(d);
        CHECK(d.parts.empty());
        PartSettings g;
        g.gain_db = 3.0;
        g.tail_ms = 70.0;
        SetPartSetting(d, kWholePart, g);
        CHECK(Near(PartFaderDb(d, kWholePart), 3.0) && Near(At(d, 0, 0).db, 3.0) && Near(d.silence.tail_ms, 70.0) && d.parts.empty());
        ResetPartSetting(d, kWholePart, PartField::Tail);
        ResetPartSetting(d, kWholePart, PartField::Gain);
        CHECK(d.silence.tail_ms == SilenceParams{}.tail_ms && PartFaderDb(d, kWholePart) == 0.0 && Near(At(d, 0, 0).db, 0.0));
    }
    {  // A divided Sound's takes read with Sound-level zones of the old model: proposed zones, the old zones gone.
        SoundStore back;
        CHECK(ParseSoundStore("RAVSOUNDS 1\nlibrary uid=lib name=L root=n1\nnode uid=n1 type=sound name=FS\n"
                              "sound kind=item mode=takes div=g divv=heel,toe\nzone start=0.2 len=0.1\nframe start=0.1 len=0.6\n",
                              &back));
        const SoundData& m = back.libraries[0].nodes[0].sound;
        CHECK(m.zones.empty() && m.frames[0].zones.size() == 2 && ZonesProblem(m, 0.0).empty());
    }
    {  // Takes: three takes over the same stretch of the file, each its own timeline (no overlap between them).
        SoundData t = Base();
        t.mode = DetectMode::Takes;
        for (int k = 0; k < 3; ++k) {
            SoundFrame f;
            f.start = 0.10;
            f.length = 0.60;
            t.frames.push_back(f);
        }
        RefreshParts(t, nullptr);
        CHECK(ZonesProblem(t, 3.0).empty() && PartZoneCount(t) == 6 && AllZones(t, 2).size() == 2 && AllZones(t, -1).empty());
        // A gesture by time acts on its take only.
        ZoneContext c;
        c.tl = 1;
        CHECK(SplitZoneAt(t, 0.2, c));
        CHECK(t.frames[1].zones.size() == 3 && t.frames[0].zones.size() == 2 && t.frames[2].zones.size() == 2);
        CHECK(!SplitZoneAt(t, 0.25, ZoneContext{}));  // no take given: nothing
        // Moving a zone: the other takes' zones never stop it or follow it.
        CHECK(MoveZone(t, {0, 1}, 0.1, c) && ZonesProblem(t, 3.0).empty() && t.frames[2].zones.size() == 2);
        // A new zone joins the take it is made on (none is made for a missing take).
        c.tl = 2;
        CHECK(NewZone(t, kHeel, 0.80, 0.90, c) && t.frames[2].zones.size() == 3 && t.frames.size() == 3);
        c.tl = 7;
        CHECK(!NewZone(t, kHeel, 0.80, 0.90, c) && t.frames.size() == 3);
        CHECK(RoundTrips(t) && HeardPieces(t, 0.0, 1.0, 2).size() >= 2 && HeardPieces(t, 0.0, 1.0).empty());
        CHECK(NearestVariation(t, 0.5, 1) == 1 && ZoneAt(t, 0.15, 0).frame == 0 && ZoneAt(t, 0.15, 2).frame == 2);
    }
}

// ---- The seeded fuzz --------------------------------------------------------------------------------------------

struct Rng {
    uint64_t s;
    uint64_t Next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    double U(double lo, double hi) { return lo + (hi - lo) * static_cast<double>(Next() % 1000000) / 1000000.0; }
    int    I(int n) { return n <= 0 ? 0 : static_cast<int>(Next() % static_cast<uint64_t>(n)); }
};

// 13-62i: `kind` 0 = divided (Heel / Toe), 1 = undivided (its implicit part; Silence mode or a whole item), 2 = a
// divided Sound in Takes mode (three takes, each its own timeline over the same file stretch: they overlap in time),
// 3 = an undivided Sound in Takes mode, 4 = a divided Sound in Take markers mode.
bool TakesKind(int kind) { return kind == 2 || kind == 3; }
bool UndividedKind(int kind) { return kind == 1 || kind == 3; }

SoundData FuzzSound(const PeakData& pk, int kind = 0, int salt = 0)
{
    SoundData d;
    d.mode = TakesKind(kind) ? DetectMode::Takes
           : kind == 4       ? DetectMode::TakeMarkers
           : (kind == 1 && salt % 2 ? DetectMode::Whole : DetectMode::Silence);
    if (!UndividedKind(kind)) {
        d.divide_group = "g";
        d.divide_values = {kHeel, kToe};
    }
    const std::vector<double> starts = TakesKind(kind) ? std::vector<double>{0.10, 0.40, 0.10} : std::vector<double>{0.10, 1.04, 1.98};
    for (double s : starts) {
        SoundFrame f;
        f.start = s;
        f.length = TakesKind(kind) ? 1.6 : 0.76;
        d.frames.push_back(f);
    }
    if (TakesKind(kind))
        for (int t = 0; t < 3; ++t) RefreshParts(d, &pk, t);
    else
        RefreshParts(d, &pk);
    return d;
}

ZoneRef AnyZone(const SoundData& d, Rng& r)
{
    const std::vector<ZoneRef> all = AllZones(d);
    if (all.empty()) return {};
    return all[static_cast<size_t>(r.I(static_cast<int>(all.size())))];
}

ZoneSel AnySel(const SoundData& d, Rng& r)
{
    ZoneSel s;
    const int n = 1 + r.I(3);
    for (int k = 0; k < n; ++k) {
        const ZoneRef z = AnyZone(d, r);
        if (z.frame >= 0) s.push_back(z);
    }
    if (r.I(4) == 0) {  // consecutive zones (heal-able)
        const std::vector<ZoneRef> all = AllZones(d);
        if (all.size() >= 2) {
            const int i = r.I(static_cast<int>(all.size()) - 1);
            s = {all[static_cast<size_t>(i)], all[static_cast<size_t>(i + 1)]};
            if (i + 2 < static_cast<int>(all.size()) && r.I(2)) s.push_back(all[static_cast<size_t>(i + 2)]);
        }
    }
    return s;
}


// Lot A review: the migrated spans survive a relayout with peaks; zones proposed before the peaks get RAV's cuts once
// they are read; an off variation is out of the model; snap modes; a part with no zone; 40508 without a time selection.
// Final review: a ref into an off variation is refused; Heal never crosses variations; a gesture marks only its own
// variation as hand (a settings change none); the lane fader never clamps a zone; a variation turned back on is laid
// out with the others.
void TestFinalReview()
{
    PeakData pk = Peaks({{0.12, 0.20}, {0.35, 0.40}, {0.60, 0.66}, {1.12, 1.20}});
    {
        SoundData  d = With({Z(kHeel, 0.10, 0.30)});
        SoundFrame f;
        f.on = false;
        f.zones = {Z(kToe, 0.50, 0.70), Z(kToe, 0.70, 0.80)};
        f.start = 0.5;
        f.length = 0.3;
        d.frames.push_back(f);
        const ZoneRef off = {1, 0};
        ZoneSel       sel = {off, {1, 1}};
        CHECK(!MoveZone(d, off, 0.01) && !DragZoneEdge(d, off, 1, 0.65) && !DragCut(d, off, 0.72) && !SetZoneDb(d, off, 3.0));
        CHECK(!SetZoneFade(d, off, -1, 0.01) && !SetZoneSnap(d, off, 0.6) && !ResetZoneEdge(d, off, 1) && !FillGap(d, off));
        CHECK(!HealZones(d, sel) && !DeleteZones(d, sel) && At(d, 1, 0).db == 0.0 && d.frames[1].zones.size() == 2);
    }
    {  // Heal: V1's last zone and V2's first, butted, stay apart
        SoundData  d = With({Z(kHeel, 0.10, 0.30)});
        SoundFrame f;
        f.zones = {Z(kToe, 0.30, 0.50)};
        f.start = 0.3;
        f.length = 0.2;
        d.frames.push_back(f);
        ZoneSel sel = {{0, 0}, {1, 0}};
        CHECK(!HealZones(d, sel) && d.frames[0].zones.size() == 1 && d.frames[1].zones.size() == 1 && d.frames[1].on);
    }
    {  // two variations, a gesture with peaks on variation 0: variation 1 not marked hand
        SoundData  d = With({Z(kHeel, 0.10, 0.30), Z(kToe, 0.30, 0.50)});
        SoundFrame f;
        f.zones = {Z(kHeel, 1.10, 1.30)};
        f.zones[0].a_hand = f.zones[0].b_hand = false;
        f.start = 1.1;
        f.length = 0.2;
        d.frames.push_back(f);
        ZoneContext ctx;
        ctx.peaks = &pk;
        CHECK(SetZoneDb(d, {0, 0}, 2.0, ctx) && d.frames[0].hand && !d.frames[1].hand);
        PartSettings t;
        t.tail_ms = 30.0;
        SetPartSetting(d, kHeel, t);
        SoundData e = d;
        e.frames[0].hand = false;
        ApplyPreTailChange(e, kHeel, true, PartField::Tail, ctx);
        CHECK(!e.frames[0].hand && !e.frames[1].hand);
    }
    {  // the fader: a zone at +23 dB leaves room for +1 only; back to 0 gives +23 again
        SoundData d = With({Z(kHeel, 0.10, 0.30), Z(kHeel, 0.40, 0.60)});
        d.frames[0].zones[0].db = 23.0;
        d.frames[0].zones[1].db = -2.0;
        CHECK(SetPartFader(d, kHeel, 2.0) && Near(PartFaderDb(d, kHeel), 1.0) && Near(At(d, 0, 0).db, 24.0) && Near(At(d, 0, 1).db, -1.0));
        CHECK(SetPartFader(d, kHeel, 0.0) && Near(At(d, 0, 0).db, 23.0) && Near(At(d, 0, 1).db, -2.0) && PartFaderDb(d, kHeel) == 0.0);
    }
    {  // a variation turned back on after a zone grew into its stretch
        SoundData  d = With({Z(kHeel, 0.10, 0.30)});
        SoundFrame f;
        f.on = false;
        f.zones = {Z(kToe, 0.40, 0.60)};
        f.start = 0.4;
        f.length = 0.2;
        d.frames.push_back(f);
        CHECK(DragZoneEdge(d, {0, 0}, 1, 0.50) && Near(At(d, 0, 0).b, 0.50));
        SetFrameOn(d, 1, true);
        CHECK(ZonesProblem(d, 3.0).empty());
    }
}

void TestReviewRows()
{
    PeakData pk = Peaks({{0.12, 0.20}, {0.35, 0.40}, {0.60, 0.66}});
    {  // TakeMarkers migration: every edge kept by hand (the old model never trimmed there)
        SoundData  d = Base();
        d.mode = DetectMode::TakeMarkers;
        SoundFrame f;
        f.start = 0.1;
        f.length = 0.9;
        f.cuts = {0.30};
        f.owners = {kHeel, kToe};
        d.frames.push_back(f);
        SoundZone ex;
        ex.start = 0.50;
        ex.length = 0.05;
        d.zones.push_back(ex);
        CHECK(MigrateLegacyZones(d));
        ZoneContext ctx;
        ctx.peaks = &pk;
        CHECK(SetZoneDb(d, {0, 0}, -1.0, ctx));
        CHECK(Near(At(d, 0, 0).a, 0.1) && Near(At(d, 0, 0).b, 0.3) && Near(At(d, 0, 1).a, 0.3) && Near(At(d, 0, 2).b, 1.0));
        // the old part gain shows on the fader
        SoundData g = Base();
        g.frames.push_back(f);
        PartSettings ps;
        ps.value = kToe;
        ps.gain_db = -3.0;
        g.parts.push_back(ps);
        CHECK(MigrateLegacyZones(g) && Near(PartFaderDb(g, kToe), -3.0) && Near(At(g, 0, 1).db, -3.0));
    }
    {  // proposed blind (no peaks: an even split), then the peaks arrive: RAV's cut before Toe's onset
        SoundData  d = Base();
        SoundFrame f;
        f.start = 0.1;
        f.length = 0.6;
        d.frames.push_back(f);
        RefreshParts(d, nullptr);
        CHECK(d.frames[0].zones.size() == 2 && Near(At(d, 0, 1).slot_a, 0.4));
        RefreshParts(d, &pk);
        CHECK(d.frames[0].zones.size() == 2 && At(d, 0, 1).onset > 0.0 && At(d, 0, 1).slot_a > 0.3 && At(d, 0, 1).slot_a < 0.36);
    }
    {  // an off variation: out of the heard pieces, out of NewZone's reach, zones kept
        SoundData d = With({Z(kHeel, 0.10, 0.30)});
        SoundFrame f;
        f.on = false;
        f.zones = {Z(kToe, 0.50, 0.70)};
        f.start = 0.5;
        f.length = 0.2;
        d.frames.push_back(f);
        CHECK(HeardPieces(d, 0.0, 1.0).size() == 3 && AllZones(d).size() == 1);  // silence, heel, silence
        CHECK(NewZone(d, kHeel, 0.55, 0.60) && d.frames[1].zones.size() == 1 && !d.frames[1].on && d.frames[0].zones.size() == 2);
        CHECK(ZonesProblem(d, 3.0).empty());
    }
    {  // snap: Peak mode on the peaks, a part's own offset
        SoundData d = With({Z(kHeel, 0.10, 0.30)});
        d.sync_mode = SyncMode::Peak;
        PeakData p2 = Peaks({{0.15, 0.25}});
        p2.max_abs[200] = 0.99f;
        CHECK(Near(ZoneSnap(d, {0, 0}, &p2).t, 0.198, 1e-3));
        PartSettings o;
        o.sync_offset_ms = 0.0;
        SetPartSetting(d, kHeel, o);
        CHECK(Near(ZoneSnap(d, {0, 0}, &p2).t, 0.200, 1e-3));
    }
    {  // a part with no zone in a variation: nothing placed for it there
        SoundData d = With({Z(kHeel, 0.10, 0.30)});
        CHECK(PlacedSpans(d, 0, kToe).empty() && ZonesOfPart(d, kToe).empty());
    }
    {  // a gesture marks its variation as edited by hand (a re-detection keeps it); a refresh does not
        SoundData d = With({Z(kHeel, 0.10, 0.30), Z(kToe, 0.40, 0.60)});
        RelayoutZones(d, &pk);
        CHECK(!d.frames[0].hand && !d.edited);
        CHECK(SetZoneDb(d, {0, 1}, 2.0) && d.frames[0].hand && d.edited);
        SoundData e = With({Z(kHeel, 0.10, 0.30)});
        CHECK(!SetZoneDb(e, {0, 0}, 0.0) && !e.frames[0].hand);
    }
    {  // 40508 without a time selection: nothing
        SoundData d = With({Z(kHeel, 0.10, 0.30)});
        CHECK(!TrimZonesTo(d, {{0, 0}}, 0.2, 0.2));
    }
}

// The heard start of each piece (HeardPieces laid end to end, each overlapping the next by its xo).
std::vector<double> HeardStarts(const std::vector<HeardPiece>& p)
{
    std::vector<double> at;
    double              t = 0.0;
    for (const HeardPiece& q : p) {
        at.push_back(t);
        t += (q.b - q.a) - q.xo;
    }
    return at;
}

// "" when at most two pieces sound at once and no zone's fades exceed its heard stretch (none rescaled).
std::string VoicesProblem(const SoundData& d, double A, double B, int tl = -1)
{
    const std::vector<HeardPiece> p = HeardPieces(d, A, B, tl);
    const std::vector<double>     at = HeardStarts(p);
    for (size_t i = 0; i + 2 < p.size(); ++i)
        if (at[i + 2] < at[i] + (p[i].b - p[i].a) - 1e-9) return "three voices overlap";
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i].xo < -1e-12 || p[i].xo > p[i].b - p[i].a + 1e-9) return "a piece overlaps the next by more than itself";
    for (const ZoneRef& r : AllZones(d)) {
        const EditZone* z = ZoneOf(d, r);
        if (z->IsGap()) continue;
        const EditZone* lg = ZoneOf(d, GapBefore(d, r));
        const EditZone* rg = ZoneOf(d, GapAfter(d, r));
        const double    heard = (z->b - z->a) + (lg ? lg->xr : 0.0) + (rg ? rg->xl : 0.0);
        if (ZoneFade(d, r, -1).s + ZoneFade(d, r, 1).s > heard + 1e-9) return "a zone's fades longer than its heard stretch";
        if (ZoneCutFade(d, r, -1).s + ZoneFade(d, r, 1).s > heard + 1e-9 || ZoneFade(d, r, -1).s + ZoneCutFade(d, r, 1).s > heard + 1e-9)
            return "a zone's At cuts fade and its other fade longer than it";
    }
    return std::string();
}

// 13-62h review of Lots B / C: a crossfade cut by the play's start (B1), crossfades longer than a short zone (B3),
// the measured snap kept (R2).
void TestReviewBC()
{
    {  // B1: from 0.398 (inside the crossfade) and 0.402 (inside the Gap's left part): the right piece starts that much later
        SoundData d = With({Z(kHeel, 0.10, 0.40), G(0.40, 0.45, 0.005, 0.005), Z(kHeel, 0.45, 0.70)});
        for (double A : {0.398, 0.402}) {
            const std::vector<HeardPiece> p = HeardPieces(d, A, 0.70);
            CHECK(p.size() == 2);
            if (p.size() != 2) continue;
            CHECK(Near(p[0].a, A) && Near(p[0].b, 0.405) && Near(p[0].xo, 0.405 - A));  // overlaps by what is left of it
            CHECK(Near(p[1].a, A + 0.05) && Near(p[1].b, 0.70));                       // aligned: same heard time
            CHECK(Near(HeardStarts(p)[1], 0.0));
        }
        // ending inside the crossfade: no overlap, nothing after
        const std::vector<HeardPiece> e = HeardPieces(d, 0.10, 0.402);
        CHECK(e.size() == 1 && Near(e[0].xo, 0.0));
    }
    {  // B3: Heel [0.10, 0.30] · Gap [0.30, 0.40] xl = xr = 40 ms · Heel [0.40, 0.406]: 40 ms into a 6 ms zone
        SoundData d = With({Z(kHeel, 0.10, 0.30), G(0.30, 0.40, 0.04, 0.04), Z(kHeel, 0.40, 0.406)});
        CHECK(!ZonesProblem(d, 3.0).empty());  // the invariant refuses it
        RelayoutZones(d, nullptr, 3.0);
        CHECK(ZonesProblem(d, 3.0).empty() && Near(At(d, 0, 1).xl, 0.006) && Near(At(d, 0, 1).xr, 0.04));
        CHECK(Near(ZoneFade(d, {0, 2}, 1).s, 0.0) && VoicesProblem(d, 0.0, 1.0).empty());
        // the handle can not give it more
        CHECK(!SetGapXfade(d, {0, 1}, 0.04, -1.0));
    }
    {  // B3: a 10 ms zone between two 100 ms Gaps of 40 / 40: both reaches cut to 5 ms, never three voices
        SoundData d = With({Z(kHeel, 0.10, 0.30), G(0.30, 0.40, 0.04, 0.04), Z(kToe, 0.40, 0.41), G(0.41, 0.51, 0.04, 0.04),
                            Z(kHeel, 0.51, 0.80)});
        CHECK(!VoicesProblem(d, 0.0, 1.0).empty());  // as it was: three voices
        RelayoutZones(d, nullptr, 3.0);
        CHECK(ZonesProblem(d, 3.0).empty() && VoicesProblem(d, 0.0, 1.0).empty());
        CHECK(Near(At(d, 0, 1).xl, 0.005) && Near(At(d, 0, 3).xr, 0.005) && Near(At(d, 0, 1).xr, 0.04) && Near(At(d, 0, 3).xl, 0.04));
        // the room left: freeing one side gives the other one the whole zone
        CHECK(SetGapXfade(d, {0, 3}, -1.0, 0.0) && SetGapXfade(d, {0, 1}, 0.04, -1.0) && Near(At(d, 0, 1).xl, 0.01));
        CHECK(ZonesProblem(d, 3.0).empty());
    }
    {  // B3: MakeGap next to a Gap leaves a 5 ms zone: the new Gap gets no room there, the old one is cut to it
        SoundData d = With({Z(kHeel, 0.10, 0.80)});
        d.gap_xfade = 0.08;
        CHECK(MakeGap(d, 0.30, 0.40) && MakeGap(d, 0.405, 0.50));
        CHECK(ZonesProblem(d, 3.0).empty() && VoicesProblem(d, 0.0, 1.0).empty());
    }
    {  // R2: the snap measured with the peaks is stored and kept without them; stale once the Offset changes
        SoundData d = With({Z(kHeel, 0.10, 0.30)});
        d.sync_mode = SyncMode::Peak;
        PeakData p2 = Peaks({{0.15, 0.25}});
        p2.max_abs[200] = 0.99f;
        ZoneContext ctx;
        ctx.peaks = &p2;
        CHECK(!ZoneSnapStored(d, {0, 0}));
        CHECK(Near(ZoneSnap(d, {0, 0}, nullptr).t, 0.10));  // never measured: the no-peaks fallback (its start)
        CHECK(SetZoneDb(d, {0, 0}, -1.0, ctx));
        CHECK(ZoneSnapStored(d, {0, 0}) && Near(ZoneSnap(d, {0, 0}, nullptr).t, ZoneSnap(d, {0, 0}, &p2).t) &&
              Near(ZoneSnap(d, {0, 0}, nullptr).t, 0.198, 1e-3));
        CHECK(RoundTrips(d) && SerializeSoundStore(StoreOf(d)).find(" as=") != std::string::npos);
        RelayoutZones(d, nullptr, 3.0);  // laid out again without peaks: kept
        CHECK(ZoneSnapStored(d, {0, 0}));
        PartSettings o;
        o.sync_offset_ms = 0.0;
        SetPartSetting(d, kHeel, o);
        CHECK(!ZoneSnapStored(d, {0, 0}) && Near(ZoneSnap(d, {0, 0}, nullptr).t, 0.10));  // stale: ignored
        RelayoutZones(d, &p2, 3.0);  // measured again
        CHECK(ZoneSnapStored(d, {0, 0}) && Near(ZoneSnap(d, {0, 0}, nullptr).t, 0.200, 1e-3));
        CHECK(DragZoneEdge(d, {0, 0}, -1, 0.12));  // edges moved without peaks: stale
        CHECK(!ZoneSnapStored(d, {0, 0}));
    }
}

// What the fuzz runs add up.
struct FuzzStats {
    int              bad = 0, steps = 0, changed = 0, with_gaps = 0, multi = 0, lied = 0;
    std::vector<int> ok_count;
};

// One fuzz run: `count` gestures from `seed` (a fresh Sound every 1500).
void FuzzRun(uint64_t seed, int count, FuzzStats& st, int kind = 0)
{
    PeakData pk = Peaks({{0.15, 0.22}, {0.45, 0.50}, {0.70, 0.71}, {1.10, 1.18}, {1.45, 1.52}, {2.05, 2.12}, {2.40, 2.47}, {2.62, 2.64}});
    const double T = 3.0;
    Rng       r{seed};
    SoundData d = FuzzSound(pk, kind);
    ZoneSel   cur;  // the selection a UI keeps: passed to every gesture, re-resolved by it
    int&      bad = st.bad;
    int&      steps = st.steps;
    int&      changed = st.changed;
    int&      with_gaps = st.with_gaps;
    int&      multi = st.multi;
    int&      lied = st.lied;
    const char* names[] = {"CutOut", "MakeGap", "NewZone", "FillGap", "Split", "TrimAt", "TrimTo", "Heal", "Delete", "Give",
                           "Move", "Edge", "Cut", "GapEdge", "Xfade", "Fade", "Snap", "Db", "ShiftDb", "ResetEdge", "ResetFade",
                           "ResetSnap", "Fader", "MoveChain", "PreTail", "Relayout", "NoPeaks", "Gxfade", "OnOff", "Redivide", "NaN"};
    const int   kinds = static_cast<int>(sizeof(names) / sizeof(names[0]));
    const int   kGestures = 24;  // kinds below this are gestures: false = nothing changed
    if (st.ok_count.empty()) st.ok_count.assign(static_cast<size_t>(kinds), 0);
    std::vector<int>& ok_count = st.ok_count;
    for (int i = 0; i < count; ++i) {
        if (i % 1500 == 0) {
            d = FuzzSound(pk, kind, i / 1500);
            cur.clear();
        }
        ZoneContext ctx;  // with the peaks most of the time; sometimes none (edges stay); the length sometimes from the peaks
        ctx.peaks = r.I(5) == 0 ? nullptr : &pk;
        ctx.file_len = ctx.peaks && r.I(4) == 0 ? 0.0 : T;
        ctx.tl = TakesKind(kind) ? r.I(3) : -1;  // 13-62i: a take's timeline (the same peaks stand for every take's file)
        const int    k = r.I(kinds);
        const double t0 = r.U(0.0, T), t1 = t0 + r.U(0.0, 0.3);
        const std::vector<std::string> keys = PartKeys(d);  // 13-62i: kWholePart when undivided
        const std::string part = keys[static_cast<size_t>(r.I(static_cast<int>(keys.size())))];
        ZoneSel      sel = r.I(3) == 0 && !cur.empty() ? cur : AnySel(d, r);
        const ZoneRef z = AnyZone(d, r);
        const int    side = r.I(2) ? 1 : -1;
        const std::string before = SerializeSoundStore(StoreOf(d));
        bool         ch = false;
        ZoneContext  gc = ctx;  // fix 2026-10-19: a group drag runs on the dragged zone's timeline (the host it is drawn in)
        if (TakesKind(kind) && ZoneOf(d, z)) gc.tl = ZoneTimeline(d, z.frame);
        switch (k) {
        case 0: ch = CutOut(d, t0, t1, ctx, &cur); break;
        case 1: {  // mostly inside a part zone (a real wormhole), else anywhere
            const EditZone* q = ZoneOf(d, z);
            if (q && !q->IsGap() && r.I(4) != 0 && q->b - q->a > 0.03) {
                const double a = r.U(q->a + 0.006, q->b - 0.02);
                ch = MakeGap(d, a, a + r.U(0.004, std::min(0.08, q->b - a - 0.006)), ctx, nullptr, &cur);
            } else {
                ch = MakeGap(d, t0, t1, ctx, nullptr, &cur);
            }
            break;
        }
        case 2: ch = NewZone(d, part, t0, t1, ctx, &cur); break;
        case 3: {
            ZoneRef g = {};
            for (const ZoneRef& q : AllZones(d))
                if (ZoneOf(d, q)->IsGap() && (g.frame < 0 || r.I(2))) g = q;
            ch = FillGap(d, g, ctx, &cur);
            break;
        }
        case 4: ch = SplitZoneAt(d, ZoneOf(d, z) && r.I(2) ? (ZoneOf(d, z)->a + ZoneOf(d, z)->b) / 2 : t0, ctx, &cur); break;
        case 5: ch = TrimZonesAt(d, sel, ZoneOf(d, z) && r.I(2) ? r.U(ZoneOf(d, z)->a, ZoneOf(d, z)->b) : t0, side, ctx, &cur); break;
        case 6: ch = TrimZonesTo(d, r.I(2) ? sel : ZoneSel{}, t0, r.I(8) == 0 ? t0 : t1, ctx, &cur); break;
        case 7: {  // often a run of consecutive zones (with a Gap inside when there is one)
            ZoneSel h = sel;
            const std::vector<ZoneRef> all = AllZones(d);
            for (size_t q = 0; q + 2 < all.size() && r.I(2); ++q)
                if (ZoneOf(d, all[q + 1])->IsGap()) {
                    h = {all[q], all[q + 1], all[q + 2]};
                    break;
                }
            ch = HealZones(d, h, ctx);
            cur = h;
            break;
        }
        case 8: ch = DeleteZones(d, sel, ctx); cur = sel; break;
        case 9: ch = GiveZonesPart(d, sel, part, ctx, &cur); break;
        case 10:  // fix 2026-10-19: one in three a group move of the selection
            if (i % 3 == 0) ch = MoveZones(d, sel, r.U(-0.2, 0.2), ctx, &cur);
            else ch = MoveZone(d, z, r.U(-0.2, 0.2), ctx, &cur);
            break;
        case 11: {
            const double et = ZoneOf(d, z) && r.I(2) ? (side < 0 ? ZoneOf(d, z)->a : ZoneOf(d, z)->b) + r.U(-0.05, 0.05) : t0;
            const bool grp = i % 3 == 0;  // no draw: the fuzz keeps its stream
            if (grp) ch = DragZonesEdge(d, sel, z, side, et, gc, &cur);  // fix 2026-10-19: the selection's edges
            else ch = DragZoneEdge(d, z, side, et, ctx, &cur);
            break;
        }
        case 12: ch = DragCut(d, z, ZoneOf(d, z) ? ZoneOf(d, z)->b + r.U(-0.05, 0.05) : t0, ctx, &cur); break;
        case 13: {
            ZoneRef g = z;
            for (const ZoneRef& q : AllZones(d))
                if (ZoneOf(d, q)->IsGap() && r.I(2)) g = q;
            const EditZone* q = ZoneOf(d, g);
            ch = DragGapEdge(d, g, side, q ? (side < 0 ? q->a : q->b) + r.U(-0.03, 0.03) : t0, ctx, &cur);
            break;
        }
        case 14: {
            ZoneRef g = z;
            for (const ZoneRef& q : AllZones(d))
                if (ZoneOf(d, q)->IsGap() && r.I(2)) g = q;
            const EditZone* q = ZoneOf(d, g);
            ch = DragGapXfade(d, g, side, q ? r.U(q->a, q->b) : t0, r.I(2) == 0, ctx, &cur);
            break;
        }
        case 15:
            if (i % 3 == 0) ch = SetZonesFade(d, sel, side, r.U(0.0, 0.05), gc, &cur);  // fix 2026-10-19
            else ch = SetZoneFade(d, z, side, r.U(0.0, 0.05), ctx, &cur);
            break;
        case 16: {
            const double st = ZoneOf(d, z) ? r.U(ZoneOf(d, z)->a - 0.05, ZoneOf(d, z)->b + 0.05) : t0;
            if (i % 3 == 0) ch = ShiftZonesSnap(d, sel, z, st, gc, &cur);  // fix 2026-10-19
            else ch = SetZoneSnap(d, z, st, ctx, &cur);
            break;
        }
        case 17: ch = SetZoneDb(d, z, r.U(-30.0, 30.0), ctx, &cur); break;
        case 18: ch = ShiftZonesDb(d, sel, r.U(-6.0, 6.0), {}, ctx, &cur); break;
        case 19: ch = ResetZoneEdge(d, z, side, ctx, &cur); break;
        case 20: ch = ResetZoneFade(d, z, side, ctx, &cur); break;
        case 21: ch = ResetZoneSnap(d, z, ctx, &cur); break;
        case 22: ch = r.I(3) ? SetPartFader(d, part, r.U(-24.0, 12.0), ctx, &cur) : SetPartFader(d, part, 0.0, ctx, &cur); break;
        case 23: {  // 13-62h (v5): an item moved whole -- a Gap's item mostly, else any zone (none: false)
            ZoneRef q = z;
            for (const ZoneRef& g : AllZones(d))
                if (ZoneOf(d, g)->IsGap() && r.I(2)) q = g;
            ch = MoveChain(d, q, r.U(-0.2, 0.2), ctx, &cur);
            break;
        }
        case 24: {
            PartSettings ps;
            const bool   pre = r.I(2) == 0;
            if (pre) ps.preroll_ms = r.U(0.0, 60.0);
            else ps.tail_ms = r.U(0.0, 200.0);
            const std::string who = r.I(3) == 0 ? std::string() : part;
            if (who.empty()) {
                if (pre) d.silence.preroll_ms = *ps.preroll_ms;
                else d.silence.tail_ms = *ps.tail_ms;
            } else {
                SetPartSetting(d, who, ps);
            }
            ApplyPreTailChange(d, who, r.I(2) == 0, pre ? PartField::Preroll : PartField::Tail, ctx, &cur);
            ch = true;
            break;
        }
        case 25:
        case 26:  // not a gesture: no selection carried (a UI's write carries it), so one it moved is dropped
            RelayoutZones(d, k == 25 ? &pk : nullptr, T, ctx.tl);
            if (SerializeSoundStore(StoreOf(d)) != before) cur.clear();
            break;
        case 27: d.gap_xfade = r.U(0.0, 0.05); break;
        case 28: {  // a variation switched off / on (the UI then refreshes the parts)
            if (!d.frames.empty()) {
                SoundFrame& f = d.frames[static_cast<size_t>(r.I(static_cast<int>(d.frames.size())))];
                f.on = !f.on;
                RefreshParts(d, r.I(2) ? &pk : nullptr, ctx.tl);
            }
            cur.clear();
            break;
        }
        case 29: {  // divided again (2 or 3 parts), zones proposed again
            if (r.I(6) == 0) {
                if (UndividedKind(kind)) {  // 13-62i: undivided <-> divided (Divide by None keeps the zones, all of kWholePart)
                    if (IsDivided(d)) SetDivide(d, "", {});
                    else SetDivide(d, "g", {kHeel, kToe});
                } else if (d.divide_values.size() == 2) {
                    SetDivide(d, "g3", {kHeel, kToe, "ball"});
                } else {
                    SetDivide(d, "g", {kHeel, kToe});
                }
                RefreshParts(d, r.I(3) ? &pk : nullptr, ctx.tl);
                cur.clear();
            }
            break;
        }
        case 30: {  // bad numbers: nothing changes
            const double nan = std::nan("");
            ch = CutOut(d, nan, t1, ctx) || NewZone(d, part, t0, nan, ctx) || MoveZone(d, z, nan, ctx) || MoveChain(d, z, nan, ctx) ||
                 MoveZones(d, sel, nan, ctx) || DragZonesEdge(d, sel, z, side, nan, ctx) || SetZonesFade(d, sel, side, nan, ctx) ||
                 DragZoneEdge(d, z, side, nan, ctx) || SetZoneDb(d, z, nan, ctx) || SplitZoneAt(d, nan, ctx) ||
                 SetPartFader(d, part, nan, ctx) || NewZone(d, part, t0, t0, ctx);
            if (ch || SerializeSoundStore(StoreOf(d)) != before) {
                if (bad < 5) std::printf("fuzz seed %llx step %d: a bad number changed something\n", static_cast<unsigned long long>(seed), i);
                ++bad;
            }
            break;
        }
        default: break;
        }
        ++steps;
        changed += ch ? 1 : 0;
        if (ch) ++ok_count[static_cast<size_t>(k)];
        with_gaps += GapCount(d) > 0 ? 1 : 0;
        multi += PartZoneCount(d) > 8 ? 1 : 0;
        if (k < kGestures && !ch && SerializeSoundStore(StoreOf(d)) != before) {  // false = nothing changed
            if (lied < 5) std::printf("fuzz seed %llx step %d (%s): returned false but changed the Sound\n", static_cast<unsigned long long>(seed), i, names[k]);
            ++lied;
        }
        for (const ZoneRef& q : cur)
            if (!ZoneOf(d, q) || !d.frames[static_cast<size_t>(q.frame)].on) {
                if (bad < 5) std::printf("fuzz seed %llx step %d (%s): the selection points at no zone\n", static_cast<unsigned long long>(seed), i, names[k]);
                ++bad;
                cur.clear();
                break;
            }
        std::string p = ZonesProblem(d, T);
        for (int tl = 0; tl < (TakesKind(kind) ? 3 : 1) && p.empty(); ++tl) {
            p = VoicesProblem(d, 0.0, T, tl);  // 13-62h review (B3)
            if (p.empty() && k % 3 == 0) p = VoicesProblem(d, t0, t1 + 0.2, tl);  // a play from anywhere (B1)
        }
        if (!p.empty()) {
            if (bad < 5) std::printf("fuzz seed %llx step %d (%s): %s\n", static_cast<unsigned long long>(seed), i, names[k], p.c_str());
            ++bad;
        }
        if (!RoundTrips(d)) {
            if (bad < 5) std::printf("fuzz seed %llx step %d (%s): parse(write(d)) != d\n", static_cast<unsigned long long>(seed), i, names[k]);
            if (bad < 1 && std::getenv("RAV_DUMP")) {
                SoundStore back;
                ParseSoundStore(SerializeSoundStore(StoreOf(d)), &back);
                std::printf("%s\n----\n%s\n", SerializeSoundStore(StoreOf(d)).c_str(), SerializeSoundStore(back).c_str());
            }

            ++bad;
        }
        {  // relayout is stable: twice = once
            SoundData once = d;
            RelayoutZones(once, &pk, T, ctx.tl);
            SoundData twice = once;
            RelayoutZones(twice, &pk, T, ctx.tl);
            if (!SameZones(once, twice)) {
                if (bad < 5) std::printf("fuzz seed %llx step %d (%s): relayout is not stable\n", static_cast<unsigned long long>(seed), i, names[k]);
                ++bad;
            }
        }
        if (bad > 20) break;
    }
}

// 13-62h: several fixed seeds (each a different gesture story), fewer gestures each: the same time as one long run.
// ---- Fix 2026-10-19: the snap offset lands on the sample (Antho: the H / T markers sat a few ms before the peak) ---

// A file at 48 kHz: silence (1e-5), a Heel transient rising from 0.2000 s to its peak 0.9 at frame kHeelPeak, then a
// Toe one rising from 0.2520 s to its peak 0.7 at frame kToePeak, each decaying over 40 ms.
constexpr double  kSr = 48000.0;
constexpr int64_t kHeelPeak = 9654;   // 0.201125 s: inside the 1 ms bin [0.201, 0.202)
constexpr int64_t kToePeak = 12290;   // 0.256042 s

float SynthMag(int64_t n)
{
    auto hit = [n](double rise_s, int64_t peak, float top) -> float {
        const int64_t r = static_cast<int64_t>(std::llround(rise_s * kSr));
        if (n < r || n > peak + static_cast<int64_t>(0.040 * kSr)) return 0.0f;
        if (n <= peak) return 0.05f + (top - 0.05f) * static_cast<float>(n - r) / static_cast<float>(peak - r);
        return top * static_cast<float>(std::exp(-static_cast<double>(n - peak) / (0.008 * kSr)));
    };
    return std::max({1e-5f, hit(0.2000, kHeelPeak, 0.9f), hit(0.2520, kToePeak, 0.7f)});
}

// Its bins at 1000/s (max |sample| per bin, as the reader folds them), with (`fine`) or without its samples.
PeakData SynthPeaks(bool fine)
{
    PeakData p;
    p.rate = 1000.0;
    p.max_abs.assign(3000, 0.0f);
    for (int64_t n = 0; n < static_cast<int64_t>(3.0 * kSr); ++n) {
        float& b = p.max_abs[static_cast<size_t>(static_cast<double>(n) * 1000.0 / kSr)];
        b = std::max(b, SynthMag(n));
    }
    if (fine)
        p.samples = [](double t0, double t1) {
            auto          m = std::make_shared<SampleMags>();
            const int64_t n0 = static_cast<int64_t>(std::floor(t0 * kSr)), n1 = static_cast<int64_t>(std::ceil(t1 * kSr)) + 1;
            m->t0 = static_cast<double>(n0) / kSr;
            m->sr = kSr;
            for (int64_t n = n0; n < n1; ++n) m->mag.push_back(SynthMag(n));
            return std::shared_ptr<const SampleMags>(m);
        };
    return p;
}

double FrameT(int64_t n) { return static_cast<double>(n) / kSr; }

// The first frame at or above `lin` from `from`.
int64_t FirstAt(float lin, int64_t from)
{
    for (int64_t n = from;; ++n)
        if (SynthMag(n) >= lin) return n;
}

void TestSampleAccurateSnap()
{
    const PeakData fine = SynthPeaks(true), coarse = SynthPeaks(false);
    {  // Peak, the Sound's mode, Offset 0: the very frame of the largest |sample|; on the bins alone, its bin's start
        SoundData d = With({Z(kHeel, 0.195, 0.250), Z(kToe, 0.250, 0.330)});
        d.sync_mode = SyncMode::Peak;
        d.sync_offset_ms = 0.0;
        CHECK(Near(ZoneSnap(d, {0, 0}, &fine).t, FrameT(kHeelPeak), 1e-9));
        CHECK(Near(ZoneSnap(d, {0, 1}, &fine).t, FrameT(kToePeak), 1e-9));  // the part follows its Sound's mode
        CHECK(Near(ZoneSnap(d, {0, 0}, &coarse).t, 0.201, 1e-9));           // the old measure: 0.125 ms early here
        // The default Offset (-2 ms) is applied after: 2 ms before the peak's frame, as REAPER's snap offset.
        d.sync_offset_ms = kDefaultSyncOffsetMs;
        CHECK(Near(ZoneSnap(d, {0, 0}, &fine).t, FrameT(kHeelPeak) - 0.002, 1e-9));
    }
    {  // Attack: the first frame reaching max(threshold, peak - 20 dB); a part's own Sync point wins over the Sound's
        SoundData d = With({Z(kHeel, 0.195, 0.250), Z(kToe, 0.250, 0.330)});
        d.sync_mode = SyncMode::Attack;
        d.sync_offset_ms = 0.0;
        d.silence.threshold_db = -9.9;
        const float lin = static_cast<float>(std::pow(10.0, -9.9 / 20.0));
        CHECK(Near(ZoneSnap(d, {0, 0}, &fine).t, FrameT(FirstAt(lin, 9000)), 1e-9));
        CHECK(Near(ZoneSnap(d, {0, 1}, &fine).t, FrameT(FirstAt(lin, 12000)), 1e-9));
        PartSettings o;
        o.sync_mode = SyncMode::Peak;
        SetPartSetting(d, kToe, o);
        CHECK(Near(ZoneSnap(d, {0, 1}, &fine).t, FrameT(kToePeak), 1e-9));            // Toe: its own Peak
        CHECK(Near(ZoneSnap(d, {0, 0}, &fine).t, FrameT(FirstAt(lin, 9000)), 1e-9));  // Heel: still the Sound's Attack
    }
    {  // Pre-roll > 0 (3.3 ms): the zones open before the sound (auto edges, laid out on the peaks); the snap stays on
       // the sample, wherever the Pre-roll put the zone's start; stored on the zone as measured on the samples
        SoundData d = Base();
        d.sync_mode = SyncMode::Peak;
        d.sync_offset_ms = 0.0;
        d.silence.threshold_db = -40.0;
        d.silence.preroll_ms = 3.3;
        EditZone h = Z(kHeel, 0.150, 0.250), t = Z(kToe, 0.250, 0.400);
        h.a_hand = t.b_hand = false;
        SoundFrame f;
        f.zones = {h, t};
        f.start = 0.150;
        f.length = 0.250;
        d.frames.push_back(f);
        RelayoutZones(d, &fine, 3.0);
        CHECK(At(d, 0, 0).a < 0.2000 - 0.003 && At(d, 0, 0).a > 0.2000 - 0.005);  // the Pre-roll before the sound
        CHECK(ZoneSnapStored(d, {0, 0}) && At(d, 0, 0).auto_snap.fine);
        CHECK(Near(ZoneSnap(d, {0, 0}, nullptr).t, FrameT(kHeelPeak), 1e-9));  // Populate without peaks: the same
        CHECK(Near(ZoneSnap(d, {0, 1}, nullptr).t, FrameT(kToePeak), 1e-9));
        d.sync_mode = SyncMode::Attack;
        RelayoutZones(d, &fine, 3.0);
        const float lin = static_cast<float>(std::pow(10.0, std::max(-40.0, 20.0 * std::log10(0.9) - 20.0) / 20.0));
        CHECK(Near(ZoneSnap(d, {0, 0}, nullptr).t, FrameT(FirstAt(lin, 9000)), 1e-9) && ZoneSnap(d, {0, 0}, nullptr).t >= 0.2000);
        CHECK(RoundTrips(d));
        SoundStore back;  // `fine` is saved (the 7th number of `as`)
        CHECK(ParseSoundStore(SerializeSoundStore(StoreOf(d)), &back) &&
              back.libraries[0].nodes[0].sound.frames[0].zones[0].auto_snap == At(d, 0, 0).auto_snap);
    }
    {  // a snap stored as measured on the bins (an older build) is measured again once the samples read
        SoundData d = With({Z(kHeel, 0.195, 0.250)});
        d.sync_mode = SyncMode::Peak;
        d.sync_offset_ms = 0.0;
        RelayoutZones(d, &coarse, 3.0);
        CHECK(ZoneSnapStored(d, {0, 0}) && !At(d, 0, 0).auto_snap.fine && Near(ZoneSnap(d, {0, 0}, nullptr).t, 0.201, 1e-9));
        CHECK(Near(ZoneSnap(d, {0, 0}, &fine).t, FrameT(kHeelPeak), 1e-9));  // shown and placed right at once
        RelayoutZones(d, &fine, 3.0);
        CHECK(At(d, 0, 0).auto_snap.fine && Near(ZoneSnap(d, {0, 0}, nullptr).t, FrameT(kHeelPeak), 1e-9));
        SoundStore back;  // an old `as` (6 numbers) still reads, as measured on the bins
        std::string txt = SerializeSoundStore(StoreOf(d));
        const size_t at = txt.find(" as=");
        const size_t end = at == std::string::npos ? at : txt.find_first_of(" \r\n", at + 1);
        CHECK(end != std::string::npos && txt.compare(end - 2, 2, ",1") == 0);
        if (end != std::string::npos) txt.erase(end - 2, 2);
        CHECK(ParseSoundStore(txt, &back) && back.libraries[0].nodes[0].sound.frames[0].zones[0].auto_snap.set &&
              !back.libraries[0].nodes[0].sound.frames[0].zones[0].auto_snap.fine);
    }
}

void TestFuzz()
{
    const char* names[] = {"CutOut", "MakeGap", "NewZone", "FillGap", "Split", "TrimAt", "TrimTo", "Heal", "Delete", "Give",
                           "Move", "Edge", "Cut", "GapEdge", "Xfade", "Fade", "Snap", "Db", "ShiftDb", "ResetEdge", "ResetFade",
                           "ResetSnap", "Fader", "MoveChain"};
    const int   kGestures = 24;
    FuzzStats   st;
    for (uint64_t seed : {0x13628D5EEDULL, 0xABCDEF01ULL, 0x2222ULL, 0x777ULL, 0xBEEF1234ULL, 0x1111ULL, 0x5EED77ULL, 0x0D15EA5EULL})
        FuzzRun(seed, 3000, st);
    // 13-62i: the same gestures on undivided Sounds (the implicit part) and on a divided Sound in Takes mode (three
    // takes, each its own timeline).
    FuzzStats su, sk;
    for (uint64_t seed : {0x62199ULL, 0xFACE01ULL, 0x3141ULL}) FuzzRun(seed, 3000, su, 1);
    for (uint64_t seed : {0x7A4E5ULL, 0xC0FFEEULL, 0x2718ULL}) FuzzRun(seed, 3000, sk, 2);
    CHECK(su.bad == 0 && su.lied == 0 && su.changed > su.steps / 5 && su.with_gaps > su.steps / 20);
    CHECK(sk.bad == 0 && sk.lied == 0 && sk.changed > sk.steps / 5 && sk.with_gaps > sk.steps / 20);
    // 13-62i (review): an undivided Sound in Takes mode, a divided one in Take markers mode.
    FuzzStats su2, sm;
    for (uint64_t seed : {0x31415ULL, 0x2A2AULL}) FuzzRun(seed, 3000, su2, 3);
    for (uint64_t seed : {0x7E57ULL, 0xD1CEULL}) FuzzRun(seed, 3000, sm, 4);
    CHECK(su2.bad == 0 && su2.lied == 0 && su2.changed > su2.steps / 5 && su2.with_gaps > su2.steps / 40);
    CHECK(sm.bad == 0 && sm.lied == 0 && sm.changed > sm.steps / 5 && sm.with_gaps > sm.steps / 20);
    std::printf("sound_zones fuzz (undivided Takes): %d gestures, %d changed, %d with Gaps; (Take markers): %d, %d, %d\n", su2.steps,
                su2.changed, su2.with_gaps, sm.steps, sm.changed, sm.with_gaps);
    std::printf("sound_zones fuzz (undivided): %d gestures, %d changed, %d with Gaps; (Takes): %d gestures, %d changed, %d with Gaps\n",
                su.steps, su.changed, su.with_gaps, sk.steps, sk.changed, sk.with_gaps);
    const int bad = st.bad, lied = st.lied, steps = st.steps, changed = st.changed, with_gaps = st.with_gaps, multi = st.multi;
    const std::vector<int>& ok_count = st.ok_count;
    CHECK(bad == 0 && lied == 0);
    CHECK(steps >= 20000);
    CHECK(changed > steps / 5);  // the gestures did things
    CHECK(with_gaps > steps / 10 && multi > steps / 20);  // with Gaps and many zones a good part of the time
    for (int k = 0; k < kGestures; ++k)
        if (ok_count[static_cast<size_t>(k)] < 40) std::printf("fuzz: gesture %s rarely did something (%d)\n", names[k], ok_count[static_cast<size_t>(k)]);
    std::printf("sound_zones fuzz: %d gestures, %d changed something, %d with Gaps, %d with > 8 zones\n", steps, changed,
                with_gaps, multi);
}

}  // namespace

// 13-67b: the zones a longer Tail cannot lengthen, and why.
void TestTailCaps()
{
    PeakData  pk = Peaks({{0.15, 0.20}, {0.40, 0.45}, {1.00, 1.05}, {2.95, 3.00}});
    SoundData d = Whole({W(0.10, 0.30)});
    d.silence.tail_ms = 150.0;
    d.frames[0].zones[0].a_hand = d.frames[0].zones[0].b_hand = false;
    auto add = [&d](double a, double b, bool a_hand) {
        SoundFrame f;
        EditZone   z = W(a, b);
        z.a_hand = a_hand;
        z.b_hand = false;
        f.zones = {z};
        f.start = a;
        f.length = b - a;
        d.frames.push_back(f);
    };
    add(0.30, 0.60, true);   // starts where the first one stops (0.20 + 150 ms = 0.35 wanted)
    add(0.95, 1.10, false);  // 1.05 + 150 ms = 1.20 wanted, detected up to 1.10
    add(2.90, 3.00, false);  // the file end
    RelayoutZones(d, &pk);
    ZoneContext ctx;
    ctx.peaks = &pk;
    TailCaps c = TailCappedZones(d, ctx);
    CHECK(c.by_next == 1 && c.by_file_end == 1 && c.by_detected_end == 1);
    CHECK(TailCapsText(c) == "1 zone stopped by the next zone, 1 by the file end, 1 where their variation was detected "
                             "(a Threshold change re-detects with this Tail)");
    // 13-74: each stopped zone (the waveform's yellow bar and dotted ghost), and the Tail from which the first one stops.
    auto near = [](double x, double y) { return std::fabs(x - y) < 1e-6; };
    CHECK(c.at.size() == 3);
    if (c.at.size() == 3) {
        CHECK(c.at[0].r.frame == 0 && c.at[0].why == 1 && near(c.at[0].end, 0.30) && near(c.at[0].want, 0.35));
        CHECK(c.at[1].r.frame == 2 && c.at[1].why == 3 && near(c.at[1].end, 1.10) && near(c.at[1].want, 1.20));
        CHECK(c.at[2].r.frame == 3 && c.at[2].why == 2 && near(c.at[2].end, 3.00) && near(c.at[2].want, 3.15));
    }
    CHECK(near(c.first_ms, 0.0));  // the last zone's sound runs to the file end: any Tail stops it
    CHECK(TailCappedZones(d, ZoneContext{}).by_next == 0);  // no peaks: nothing said
    d.frames[2].zones[0].b_hand = true;                       // an end dragged by hand is not counted
    c = TailCappedZones(d, ctx);
    CHECK(c.by_next == 1 && c.by_file_end == 1 && c.by_detected_end == 0 && c.at.size() == 2);
    d.frames[3].zones[0].b_hand = true;
    c = TailCappedZones(d, ctx);
    // The first one stopped: V1 at 100 ms (its sound ends at 0.20, V2 starts at 0.30); V2 at 150 ms (its slot).
    CHECK(c.at.size() == 1 && near(c.first_ms, 100.0));
    // Every part (the waveform): the same zones here (one implicit part); a part with its own Tail counts against it.
    CHECK(TailCappedZones(d, ctx, std::string(), true).at.size() == 1);
    CHECK(TailCapsText(TailCaps{2, 0, 0}) == "2 zones stopped by the next zone" && TailCapsText(TailCaps{}).empty());
}

// Fix 2026-10-19 (Antho: REAPER's several selected items): a move / an edge / a fade / a snap offset dragged on one of
// several selected zones acts on all of them.
void TestGroupGestures()
{
    // Heel 0.10-0.20, empty, Toe 0.30-0.40 | Heel 0.40-0.50 butted, empty, Toe 0.70-0.80.
    const SoundData base = With({Z(kHeel, 0.10, 0.20), Z(kToe, 0.30, 0.40), Z(kHeel, 0.40, 0.50), Z(kToe, 0.70, 0.80)});
    // Move: both by the same delta; the butted unselected Heel's start follows.
    SoundData d = base;
    ZoneSel   out;
    CHECK(Near(MoveZonesRange(d, {{0, 0}, {0, 1}}, 0.05), 0.05));
    CHECK(MoveZones(d, {{0, 0}, {0, 1}}, 0.05, {}, &out) && out.size() == 2);
    CHECK(Near(At(d, 0, 0).a, 0.15) && Near(At(d, 0, 0).b, 0.25) && Near(At(d, 0, 1).a, 0.35) && Near(At(d, 0, 1).b, 0.45));
    CHECK(Near(At(d, 0, 2).a, 0.45) && Near(At(d, 0, 2).b, 0.50) && Near(At(d, 0, 3).a, 0.70) && ZonesProblem(d, 3.0).empty());
    // The group stops at its most constrained zone: Heel 0.10 at the file's start, the others keep their spacing.
    d = base;
    CHECK(Near(MoveZonesRange(d, {{0, 0}, {0, 3}}, -1.0), -0.10));
    CHECK(MoveZones(d, {{0, 3}, {0, 0}}, -1.0) && Near(At(d, 0, 0).a, 0.0) && Near(At(d, 0, 3).a, 0.60));
    // Right: the last Toe stops at the file's end (0.85), the Heel moves the same 0.05.
    d = base;
    CHECK(MoveZones(d, {{0, 0}, {0, 3}}, 1.0, {nullptr, 0.85}) && Near(At(d, 0, 3).b, 0.85) && Near(At(d, 0, 0).a, 0.15));
    // Stopped by an unselected zone across empty space: Toe 0.30 cannot pass the Heel ending at 0.20.
    d = base;
    CHECK(Near(MoveZonesRange(d, {{0, 1}, {0, 3}}, -1.0), -0.10));
    CHECK(Near(MoveZonesRange(d, {{0, 3}, {0, 1}}, -1.0), -0.10));
    // Two selected zones butted move together (no shrink between them).
    d = base;
    CHECK(MoveZones(d, {{0, 1}, {0, 2}}, 0.10) && Near(At(d, 0, 1).a, 0.40) && Near(At(d, 0, 1).b, 0.50) &&
          Near(At(d, 0, 2).a, 0.50) && Near(At(d, 0, 2).b, 0.60) && Near(At(d, 0, 3).a, 0.70));
    // A selected part zone of an item brings its item.
    SoundData it = With({Z(kHeel, 0.10, 0.30), G(0.30, 0.36, 0.005, 0.005), Z(kHeel, 0.36, 0.60), Z(kToe, 0.90, 1.00)});
    CHECK(MoveZones(it, {{0, 3}, {0, 0}}, 0.05, {}, &out) && out.size() == 4);
    CHECK(Near(At(it, 0, 0).a, 0.15) && Near(At(it, 0, 1).a, 0.35) && Near(At(it, 0, 2).b, 0.65) && Near(At(it, 0, 3).a, 0.95));
    CHECK(ZonesProblem(it, 3.0).empty() && !MoveZones(it, {}, 0.05) && !MoveZones(it, {{0, 0}}, std::nan("")));

    // Edges: the same edge of each by the same delta, each stopping at its own limit, the others going on.
    d = base;
    CHECK(DragZonesEdge(d, {{0, 0}, {0, 3}}, {0, 0}, 1, 0.25, {}, &out) && out.size() == 2);
    CHECK(Near(At(d, 0, 0).b, 0.25) && Near(At(d, 0, 3).b, 0.85));
    d = base;  // ends +0.15: the Heel stops on the Toe at 0.30, the last Toe goes on to 0.95
    CHECK(DragZonesEdge(d, {{0, 0}, {0, 3}}, {0, 0}, 1, 0.35) && Near(At(d, 0, 0).b, 0.30) && Near(At(d, 0, 3).b, 0.95));
    d = base;  // starts -0.15: the Heel stops at the file's start, the Toe 0.70 -> 0.55
    CHECK(DragZonesEdge(d, {{0, 3}, {0, 0}}, {0, 3}, -1, 0.55) && Near(At(d, 0, 0).a, 0.0) && Near(At(d, 0, 3).a, 0.55));
    CHECK(At(d, 0, 0).a_hand && At(d, 0, 3).a_hand && ZonesProblem(d, 3.0).empty());
    // An edge held by a Gap stays; the dragged one held: nothing.
    SoundData g = With({Z(kHeel, 0.10, 0.30), G(0.30, 0.36, 0.005, 0.005), Z(kHeel, 0.36, 0.60), Z(kToe, 0.90, 1.00)});
    CHECK(DragZonesEdge(g, {{0, 3}, {0, 2}}, {0, 3}, -1, 0.85) && Near(At(g, 0, 3).a, 0.85) && Near(At(g, 0, 2).a, 0.36));
    CHECK(!DragZonesEdge(g, {{0, 2}, {0, 3}}, {0, 2}, -1, 0.30));

    // Fades: the same length on all, each within half its length; a Gap's side keeps its crossfade.
    d = base;
    CHECK(SetZonesFade(d, {{0, 0}, {0, 2}, {0, 3}}, -1, 0.03, {}, &out) && out.size() == 3);
    CHECK(At(d, 0, 0).fade_in && Near(*At(d, 0, 0).fade_in, 0.03) && Near(*At(d, 0, 3).fade_in, 0.03) && !At(d, 0, 1).fade_in);
    CHECK(SetZonesFade(d, {{0, 0}, {0, 2}}, 1, 0.08) && Near(*At(d, 0, 0).fade_out, 0.05) && Near(*At(d, 0, 2).fade_out, 0.05));
    g = With({Z(kHeel, 0.10, 0.30), G(0.30, 0.36, 0.005, 0.005), Z(kHeel, 0.36, 0.60), Z(kToe, 0.90, 1.00)});
    CHECK(SetZonesFade(g, {{0, 3}, {0, 0}}, 1, 0.02) && !At(g, 0, 0).fade_out && Near(*At(g, 0, 3).fade_out, 0.02));
    CHECK(!SetZonesFade(g, {{0, 0}}, 1, 0.02));

    // Snap offsets: each by the same delta from where it lands, clamped into its zone.
    d = base;
    d.frames[0].zones[0].snap = 0.12;
    d.frames[0].zones[3].snap = 0.78;
    CHECK(ShiftZonesSnap(d, {{0, 0}, {0, 3}}, {0, 0}, 0.15, {}, &out) && out.size() == 2);
    CHECK(Near(*At(d, 0, 0).snap, 0.15) && Near(*At(d, 0, 3).snap, 0.80));
}

int main()
{
    TestTailCaps();  // 13-67b
    TestMigration();
    TestHeardPieces();
    TestGapGestures();
    TestHealSplitTrim();
    TestMoveAndEdges();
    TestTailWins();
    TestFaderAndNormalize();
    TestProposeAndRecord();
    TestFinalReview();
    TestReviewRows();
    TestReviewBC();
    TestItems();
    TestGroupGestures();
    TestUndividedAndTakes();
    TestSampleAccurateSnap();  // fix 2026-10-19
    TestFuzz();
    if (g_fails) {
        std::printf("sound_zones_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("sound_zones_test: all passed\n");
    return 0;
}
