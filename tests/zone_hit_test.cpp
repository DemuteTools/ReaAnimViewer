// SPDX-License-Identifier: MIT
//
// Host test of src/zone_hit.h (13-62h Lot E1): the zone waveform's pure decisions -- hit testing (the mockup's
// hitAt), what a press becomes (startDrag), the Shift+drag part, the ruler snap, our double-click, onDouble, the
// tooltips, the Variations strip and the selection carried across a write.

#include "sound_zones.h"
#include "zone_hit.h"

#include <cmath>
#include <cstdio>
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

EditZone Z(const std::string& part, double a, double b)
{
    EditZone z;
    z.part = part;
    z.a = z.slot_a = a;
    z.b = z.slot_b = b;
    z.a_hand = z.b_hand = true;
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

// Variation 1: Heel [0.10, 0.40] | Gap [0.40, 0.45] | Heel [0.45, 0.70] | Toe [0.70, 0.90] (butted);
// variation 2: Toe [1.20, 1.60]. View 0..2 s over 2000 px: 1 px = 1 ms.
SoundData Sample()
{
    SoundData d;
    d.mode = DetectMode::Silence;
    d.divide_group = "g";
    d.divide_values = {"heel", "toe"};
    SoundFrame f1;
    f1.zones = {Z("heel", 0.10, 0.40), G(0.40, 0.45, 0.005, 0.005), Z("heel", 0.45, 0.70), Z("toe", 0.70, 0.90)};
    f1.start = 0.10;
    f1.length = 0.80;
    SoundFrame f2;
    f2.zones = {Z("toe", 1.20, 1.60)};
    f2.start = 1.20;
    f2.length = 0.40;
    d.frames = {f1, f2};
    return d;
}

ZoneHitView Lane(int lane, float h = 100.0f)
{
    ZoneHitView v;
    v.v0 = 0.0;
    v.vl = 2.0;
    v.w = 2000.0f;
    v.h = h;
    v.row = ZoneRow::Lane;
    v.lane = lane;
    v.file_len = 2.0;
    return v;
}

void TestHitLane()
{
    const SoundData d = Sample();
    CHECK(ZonesProblem(d, 2.0).empty());
    ZoneHitView v = Lane(-1);
    // Inside the first Heel, top / bottom halves.
    ZoneHit h = HitZones(d, v, 250.0f, 30.0f);
    CHECK(h.kind == ZoneHitKind::Zone && h.z == (ZoneRef{0, 0}) && h.top);
    h = HitZones(d, v, 250.0f, 70.0f);
    CHECK(h.kind == ZoneHitKind::Zone && !h.top);
    CHECK(std::fabs(h.t - 0.25) < 1e-9);
    // Its start edge (no Gap there).
    h = HitZones(d, v, 102.0f, 60.0f);
    CHECK(h.kind == ZoneHitKind::Edge && h.z == (ZoneRef{0, 0}) && h.side < 0);
    // A Gap's edge, not the zone's (held by the Gap).
    h = HitZones(d, v, 400.0f, 60.0f);
    CHECK(h.kind == ZoneHitKind::GapEdge && h.side < 0);
    // A Gap's crossfade handles in the top band: a + xl, b - xr.
    h = HitZones(d, v, 405.0f, 5.0f);
    CHECK(h.kind == ZoneHitKind::Xf && h.side < 0);
    h = HitZones(d, v, 445.0f, 5.0f);
    CHECK(h.kind == ZoneHitKind::Xf && h.side > 0);
    // Inside the Gap: the Gap.
    h = HitZones(d, v, 425.0f, 60.0f);
    CHECK(h.kind == ZoneHitKind::Zone && h.z == (ZoneRef{0, 1}));
    // The cut grab between butted Heel | Toe at the bottom; higher up it is an edge.
    h = HitZones(d, v, 700.0f, 95.0f);
    CHECK(h.kind == ZoneHitKind::Cut && h.z == (ZoneRef{0, 2}) && h.z2 == (ZoneRef{0, 3}));
    h = HitZones(d, v, 701.0f, 60.0f);
    CHECK(h.kind == ZoneHitKind::Edge && h.z == (ZoneRef{0, 3}) && h.side < 0);  // the one the mouse is inside
    // A fade handle: Heel 1's fade in (Sound default 1 ms) at 0.101.
    h = HitZones(d, v, 101.0f, 5.0f);
    CHECK(h.kind == ZoneHitKind::Fade && h.side < 0);
    // Empty space.
    h = HitZones(d, v, 1000.0f, 60.0f);
    CHECK(h.kind == ZoneHitKind::None);
    // Snap tag (no peaks: its start + the -2 ms offset, clamped into it: its start) in its band.
    h = HitZones(d, v, 1201.0f, 30.0f);
    CHECK(h.kind == ZoneHitKind::Snap && h.z == (ZoneRef{1, 0}));
    // Lane view: the Toe lane sees the Heels greyed; Gaps everybody's.
    ZoneHitView vt = Lane(1);
    h = HitZones(d, vt, 250.0f, 60.0f);
    CHECK(h.kind == ZoneHitKind::Grey && h.z == (ZoneRef{0, 0}));
    h = HitZones(d, vt, 425.0f, 60.0f);
    CHECK(h.kind == ZoneHitKind::Zone);
    h = HitZones(d, vt, 102.0f, 60.0f);  // no edge of a greyed zone
    CHECK(h.kind == ZoneHitKind::Grey);
    CHECK(ZoneMine(d, ZoneRef{0, 3}, 1) && !ZoneMine(d, ZoneRef{0, 0}, 1) && ZoneMine(d, ZoneRef{0, 0}, -1));
}

void TestHitStrips()
{
    const SoundData d = Sample();
    ZoneHitView     v = Lane(-1, 20.0f);
    v.row = ZoneRow::Ruler;
    ZoneHit h = HitZones(d, v, 500.0f, 10.0f);
    CHECK(h.kind == ZoneHitKind::Ruler);
    v.has_tsel = true;
    v.ts_a = 0.2;
    v.ts_b = 0.5;
    h = HitZones(d, v, 203.0f, 10.0f);
    CHECK(h.kind == ZoneHitKind::TselEdge && h.side < 0);
    h = HitZones(d, v, 498.0f, 10.0f);
    CHECK(h.kind == ZoneHitKind::TselEdge && h.side > 0);
    v.row = ZoneRow::Vars;
    h = HitZones(d, v, 300.0f, 10.0f);
    CHECK(h.kind == ZoneHitKind::Var && h.var == 0);
    h = HitZones(d, v, 1300.0f, 10.0f);
    CHECK(h.kind == ZoneHitKind::Var && h.var == 1);
    h = HitZones(d, v, 1000.0f, 10.0f);
    CHECK(h.kind == ZoneHitKind::None);
    v.row = ZoneRow::Placed;
    h = HitZones(d, v, 800.0f, 5.0f);
    CHECK(h.kind == ZoneHitKind::Pick && h.z == (ZoneRef{0, 3}));
    const std::vector<ZoneVarBlock> b = ZoneVarBlocks(d);
    CHECK(b.size() == 2 && std::fabs(b[0].a - 0.10) < 1e-9 && std::fabs(b[0].b - 0.90) < 1e-9 && b[1].frame == 1);
    CHECK(ZoneVarLabel(1, 0.10, 0.90) == "1 \xC2\xB7 100\xE2\x80\x93" "900 ms");
}

void TestDrags()
{
    const SoundData d = Sample();
    ZoneHit         h;
    h.row = ZoneRow::Lane;
    h.kind = ZoneHitKind::Zone;
    h.z = {0, 0};
    h.top = true;
    CHECK(StartZoneDrag(h, false, false) == ZoneDragKind::Move);
    h.top = false;
    CHECK(StartZoneDrag(h, false, false) == ZoneDragKind::Scrub);
    CHECK(StartZoneDrag(h, true, false) == ZoneDragKind::MkZone);
    CHECK(StartZoneDrag(h, false, true) == ZoneDragKind::MkGap);
    h.kind = ZoneHitKind::Grey;
    CHECK(StartZoneDrag(h, false, false) == ZoneDragKind::Swipe);
    h.kind = ZoneHitKind::None;
    CHECK(StartZoneDrag(h, false, false) == ZoneDragKind::Scrub);  // empty space: the cursor (the mockup)
    h.row = ZoneRow::Ruler;
    h.kind = ZoneHitKind::Ruler;
    CHECK(StartZoneDrag(h, true, true) == ZoneDragKind::Tsel);
    h.row = ZoneRow::Vars;
    CHECK(StartZoneDrag(h, false, false) == ZoneDragKind::None);
    // Handles drag at the press; with Ctrl / Alt (but Alt on a crossfade handle = one side) the press is Pending:
    // released unmoved it is the click (Ctrl adds / removes), moved it is reserved.
    ZoneHit e;
    e.kind = ZoneHitKind::Edge;
    CHECK(PressDragKind(e, false, false) == ZoneDragKind::Edge);
    CHECK(PressDragKind(e, false, true) == ZoneDragKind::Pending && ZoneReservedOnMove(e, false, true));
    CHECK(PressDragKind(e, true, false) == ZoneDragKind::Pending && ZoneReservedOnMove(e, true, false));
    CHECK(!ZoneReservedOnMove(e, false, false));
    e.kind = ZoneHitKind::Xf;
    CHECK(PressDragKind(e, false, true) == ZoneDragKind::Xf && !ZoneReservedOnMove(e, false, true));
    CHECK(PressDragKind(e, true, false) == ZoneDragKind::Pending && ZoneReservedOnMove(e, true, false));
    for (ZoneHitKind k : {ZoneHitKind::Fade, ZoneHitKind::Cut, ZoneHitKind::GapEdge, ZoneHitKind::Snap}) {
        e.kind = k;
        CHECK(PressDragKind(e, true, false) == ZoneDragKind::Pending && PressDragKind(e, false, true) == ZoneDragKind::Pending);
        CHECK(ZoneReservedOnMove(e, true, false) && ZoneReservedOnMove(e, false, true) && !ZoneReservedOnMove(e, false, false));
    }
    e.kind = ZoneHitKind::TselEdge;
    CHECK(PressDragKind(e, true, true) == ZoneDragKind::TselEdge && !ZoneReservedOnMove(e, true, true));
    e.kind = ZoneHitKind::Zone;
    CHECK(PressDragKind(e, false, false) == ZoneDragKind::Pending);
    CHECK(ZoneReservedOnMove(e, true, false) && !ZoneReservedOnMove(e, false, true));  // Alt+drag a zone: a Gap
    // Gain: vertical first on a part zone; never on a Gap or with Shift / Alt.
    ZoneHit g;
    g.row = ZoneRow::Lane;
    g.kind = ZoneHitKind::Zone;
    g.z = {0, 0};
    CHECK(PendingBecomesGain(d, g, 1.0f, -5.0f, false, false));
    CHECK(!PendingBecomesGain(d, g, 6.0f, -5.0f, false, false));
    CHECK(!PendingBecomesGain(d, g, 0.0f, -2.0f, false, false));
    CHECK(!PendingBecomesGain(d, g, 0.0f, -5.0f, true, false));
    g.z = {0, 1};
    CHECK(!PendingBecomesGain(d, g, 0.0f, -5.0f, false, false));
    CHECK(std::fabs(GainDragDb(-15.0f) - 1.5) < 1e-9 && std::fabs(GainDragDb(20.0f) + 2.0) < 1e-9);
    // The 3 px are logical: at 2x, 5 screen px is 2.5 px (not yet), 7 is.
    g.z = {0, 0};
    CHECK(!PendingBecomesGain(d, g, 0.0f, -5.0f, false, false, 2.0f));
    CHECK(PendingBecomesGain(d, g, 0.0f, -7.0f, false, false, 2.0f));
    // The single zone's toast (the mockup's): the fader shown as what it applied.
    CHECK(ZoneGainToast("Heel", "Heel", 2.0, 1.0) == "Heel gain: +2.0 dB (+ Heel fader +1 dB = 2.0 dB)");
    CHECK(ZoneGainToast("Toe", "Toe", -1.5, -0.5) == "Toe gain: -1.5 dB (+ Toe fader -0.5 dB = -1.5 dB)");
    CHECK(ZoneGainToast("Toe", "Toe", 0.0, 0.0) == "Toe gain: 0.0 dB (+ Toe fader 0 dB = 0.0 dB)");
}

// ZoneLooks gives what the per-zone queries give (GapBefore / GapAfter / GapLeft / GapRight / ZoneFade / ZoneSnap /
// Butted), from one sorted list.
void TestLooks()
{
    SoundData d = Sample();
    d.cut_fade = 0.003;
    d.fade_in = 0.002;
    d.fade_out = 0.050;
    d.frames[0].zones[0].fade_in = 0.010;  // its own
    PartSettings ps;
    ps.value = "toe";
    ps.fade_out = 0.400;  // capped at half the zone
    d.parts.push_back(ps);
    d.frames[1].zones[0].snap = 1.30;
    d.frames[0].zones[2].db = 6.0;
    // A third variation, off: out of the model.
    SoundFrame off;
    off.on = false;
    off.zones = {Z("heel", 1.70, 1.90)};
    off.start = 1.70;
    off.length = 0.20;
    d.frames.push_back(off);
    const std::vector<ZoneLook> L = ZoneLooks(d, nullptr);
    const std::vector<ZoneRef>  all = AllZones(d);
    CHECK(L.size() == all.size() && L.size() == 5);
    for (size_t i = 0; i < L.size() && i < all.size(); ++i) {
        const ZoneRef& r = all[i];
        CHECK(L[i].ref == r);
        if (L[i].gap) {
            CHECK(L[i].left == GapLeft(d, r) && L[i].right == GapRight(d, r));
            CHECK(L[i].li >= 0 && L[static_cast<size_t>(L[i].li)].ref == L[i].left);
            CHECK(L[i].ri >= 0 && L[static_cast<size_t>(L[i].ri)].ref == L[i].right);
            continue;
        }
        CHECK(L[i].lg == (ZoneOf(d, GapBefore(d, r)) != nullptr) && L[i].rg == (ZoneOf(d, GapAfter(d, r)) != nullptr));
        CHECK(std::fabs(L[i].fi_s - ZoneFade(d, r, -1).s) < 1e-12 && std::fabs(L[i].fo_s - ZoneFade(d, r, 1).s) < 1e-12);
        CHECK(std::fabs(L[i].snap - ZoneSnap(d, r, nullptr).t) < 1e-12 && L[i].snap_own == ZoneSnap(d, r, nullptr).own);
        CHECK(L[i].butt_next == (i + 1 < all.size() && Butted(d, r, all[i + 1])));
        CHECK(L[i].k == PartIndex(d, ZoneOf(d, r)->part));
    }
    CHECK(std::fabs(L[0].fi_s - 0.010) < 1e-12);  // own
    CHECK(std::fabs(L[3].fi_s - 0.003) < 1e-12);  // a cut: the Sound's cut fade
    CHECK(std::fabs(L[4].fo_s - 0.200) < 1e-12);  // the part's, capped at half
    CHECK(std::fabs(L[2].gain - std::pow(10.0, 0.3)) < 1e-9);
    // The hits are the same with the looks given.
    ZoneHitView v = Lane(-1);
    for (float x = 90.0f; x < 1650.0f; x += 1.0f)
        for (float y : {5.0f, 30.0f, 60.0f, 95.0f}) {
            ZoneHitView vc = v;
            vc.looks = &L;
            const ZoneHit a = HitZones(d, v, x, y), b = HitZones(d, vc, x, y);
            CHECK(a.kind == b.kind && a.z == b.z && a.z2 == b.z2 && a.side == b.side);
        }
}

void TestStepAndHeal()
{
    SoundData d = Sample();
    // Variation 2 off, a third on: the strip's blocks are variations 1 and 3.
    SoundFrame f3;
    f3.zones = {Z("heel", 1.70, 1.90)};
    f3.start = 1.70;
    f3.length = 0.20;
    d.frames.push_back(f3);
    d.frames[1].on = false;
    const std::vector<ZoneVarBlock> B = ZoneVarBlocks(d);
    CHECK(B.size() == 2 && B[0].frame == 0 && B[1].frame == 2);
    CHECK(ZoneStepBlock(B, 0.2, 1) == 1);    // from variation 1: the next block (variation 3, not the off one)
    CHECK(ZoneStepBlock(B, 0.2, -1) == -1);  // already the first
    CHECK(ZoneStepBlock(B, 1.8, 1) == -1);   // already the last
    CHECK(ZoneStepBlock(B, 1.8, -1) == 0);
    CHECK(ZoneStepBlock(B, 1.4, 1) == 1);    // between blocks: the first after
    CHECK(ZoneStepBlock(B, 1.4, -1) == 0);   // the last before
    CHECK(ZoneStepBlock(B, -1.0, 1) == 0);   // no cursor: the first
    CHECK(ZoneStepBlock(B, -1.0, -1) == -1);
    CHECK(std::fabs(ZoneStepRef(d, {{2, 0}}, 0.1) - 1.80) < 1e-9);
    CHECK(std::fabs(ZoneStepRef(d, {{9, 9}}, 0.1) - 0.1) < 1e-9 && ZoneStepRef(d, {}, -1.0) < 0.0);
    // Heal: Heel | Gap | Heel | Toe all selected = 2 cuts (the Gap once); Heel | Gap = 1; Heel2 | Toe = 1.
    const SoundData s = Sample();
    CHECK(HealCutCount(s, {{0, 0}, {0, 1}, {0, 2}, {0, 3}}) == 2);
    CHECK(HealCutCount(s, {{0, 0}, {0, 1}}) == 1);
    CHECK(HealCutCount(s, {{0, 2}, {0, 3}}) == 1);
    CHECK(HealCutCount(s, {{0, 3}, {0, 2}, {0, 0}, {0, 1}}) == 2);  // any order
    CHECK(HealCutCount(s, {{0, 0}, {0, 2}}) == 0);                  // not edge to edge
    CHECK(HealCutCount(s, {{0, 1}, {0, 2}}) == 0);                  // a Gap first heals nothing (HealZones)
    SoundData h = s;
    ZoneSel   all = {{0, 0}, {0, 1}, {0, 2}, {0, 3}};
    CHECK(HealZones(h, all, {}) && h.frames[0].zones.size() == 1);
}

void TestNewPartAndSnap()
{
    const SoundData d = Sample();
    ZoneHit         h;
    h.lane = 1;
    h.t = 0.2;
    CHECK(NewZonePart(d, h, "heel") == "toe");  // Lane: the lane's part
    h.lane = -1;
    CHECK(NewZonePart(d, h, "toe") == "toe");    // Combined: Settings of
    CHECK(NewZonePart(d, h, "") == "heel");      // else the zone under the start
    h.t = 0.8;
    CHECK(NewZonePart(d, h, "") == "toe");
    h.t = 1.0;
    CHECK(NewZonePart(d, h, "") == "heel");      // empty space: the first part
    // 13-62i: an undivided Sound's new zone is of its implicit part (never "": a Gap).
    SoundData u = d;
    u.divide_group.clear();
    u.divide_values.clear();
    CHECK(NewZonePart(u, h, "") == kWholePart && NewZonePart(u, h, "heel") == kWholePart);
    // 13-62i: a Takes-mode Sound: each take its own timeline (its looks, its blocks, its hits).
    SoundData t = d;
    t.mode = DetectMode::Takes;
    CHECK(ZoneLooks(t, nullptr, 1).size() == 1 && ZoneLooks(t, nullptr, 0).size() == 4 && ZoneVarBlocks(t, 1).size() == 1 &&
          ZoneVarBlocks(t).size() == 2);
    ZoneHitView tv = Lane(-1);
    tv.tl = 1;
    CHECK(HitZones(t, tv, 1300.0f, 70.0f).z.frame == 1 && HitZones(t, tv, 300.0f, 70.0f).kind == ZoneHitKind::None);
    // Snap to a zone edge / the cursor within 6 px (1 px = 1 ms).
    CHECK(std::fabs(SnapZoneTime(d, 0.403, 1000.0, -1.0, 6.0f) - 0.40) < 1e-9);
    CHECK(std::fabs(SnapZoneTime(d, 1.0, 1000.0, -1.0, 6.0f) - 1.0) < 1e-9);
    CHECK(std::fabs(SnapZoneTime(d, 1.004, 1000.0, 1.0, 6.0f) - 1.0) < 1e-9);
}

void TestDouble()
{
    ZoneTap tap;
    CHECK(!ZoneDoubleTap(tap, "a", 1.0, 10.0f, 10.0f));
    CHECK(ZoneDoubleTap(tap, "a", 1.2, 12.0f, 10.0f));
    CHECK(!ZoneDoubleTap(tap, "a", 1.3, 12.0f, 10.0f));  // a third press starts again
    CHECK(!ZoneDoubleTap(tap, "b", 1.4, 12.0f, 10.0f));  // another thing
    CHECK(!ZoneDoubleTap(tap, "b", 2.0, 12.0f, 10.0f));  // too late
    CHECK(!ZoneDoubleTap(tap, "b", 2.1, 30.0f, 10.0f));  // too far
    ZoneHit h;
    h.kind = ZoneHitKind::None;
    CHECK(DoubleActionOf(h) == ZoneDoubleAction::RemoveTsel);
    h.kind = ZoneHitKind::Ruler;
    CHECK(DoubleActionOf(h) == ZoneDoubleAction::RemoveTsel);
    h.kind = ZoneHitKind::Var;
    CHECK(DoubleActionOf(h) == ZoneDoubleAction::TselVariation);
    h.kind = ZoneHitKind::Zone;
    CHECK(DoubleActionOf(h) == ZoneDoubleAction::TselZone);
    h.kind = ZoneHitKind::Pick;
    CHECK(DoubleActionOf(h) == ZoneDoubleAction::TselZone);
    h.kind = ZoneHitKind::Edge;
    CHECK(DoubleActionOf(h) == ZoneDoubleAction::ResetEdge);
    h.kind = ZoneHitKind::Fade;
    CHECK(DoubleActionOf(h) == ZoneDoubleAction::ResetFade);
    h.kind = ZoneHitKind::Snap;
    CHECK(DoubleActionOf(h) == ZoneDoubleAction::ResetSnap);
    h.kind = ZoneHitKind::Cut;
    CHECK(DoubleActionOf(h) == ZoneDoubleAction::None);
    ZoneHit a, b;
    a.kind = b.kind = ZoneHitKind::Zone;
    a.z = {0, 0};
    b.z = {0, 2};
    CHECK(ZoneTapKey(a) != ZoneTapKey(b));
    b.z = {0, 0};
    b.t = 0.3;
    CHECK(ZoneTapKey(a) == ZoneTapKey(b));
}

void TestTipsAndCursors()
{
    const SoundData                d = Sample();
    const std::vector<std::string> names = {"Heel", "Toe"};
    ZoneHit                        h;
    h.row = ZoneRow::Lane;
    h.kind = ZoneHitKind::Grey;
    h.lane = 1;
    h.z = {0, 0};
    CHECK(ZoneTip(d, h, names, true) == "Played by Heel: click to give it to Toe; drag: swipe a range to Toe");
    h.kind = ZoneHitKind::Zone;
    h.top = true;
    h.z = {0, 3};  // a zone alone (13-62h v5: {0, 0} is in an item)
    CHECK(ZoneTip(d, h, names, true).find("Click: select and play") != std::string::npos);
    CHECK(ZoneTip(d, h, names, false).find("and play") == std::string::npos);
    h.z = {0, 0};
    CHECK(ZoneTip(d, h, names, true).find("selects it whole and plays it") != std::string::npos);
    CHECK(ZoneTip(d, h, names, false).find("plays") == std::string::npos);
    CHECK(ZoneCursorOf(h) == ZoneCursor::Grab);
    h.top = false;
    CHECK(ZoneCursorOf(h) == ZoneCursor::Text);
    h.z = {0, 1};
    // 13-62h (v5): a Gap glues its neighbours into an item: its tip is the item's (no gain drag on a Gap).
    CHECK(ZoneTip(d, h, names, true).find("Heel + Heel glued by a Gap: one item") == 0);
    CHECK(ZoneTip(d, h, names, true).find("Up / down") == std::string::npos);
    CHECK(ZoneName(d, ZoneRef{0, 3}, names) == "Toe" && ZoneName(d, ZoneRef{0, 1}, names) == "Gap");
    h.kind = ZoneHitKind::Edge;
    h.z = {0, 0};
    h.side = 1;
    CHECK(ZoneTip(d, h, names, true) == "Heel end: drag (double-click: automatic, follows Tail)");
    CHECK(ZoneCursorOf(h) == ZoneCursor::ResizeEW);
    h.kind = ZoneHitKind::None;
    CHECK(ZoneTip(d, h, names, true).find("Nothing placed here") == 0);
    h.row = ZoneRow::Vars;
    CHECK(ZoneTip(d, h, names, true).empty());
}

void TestSelection()
{
    SoundData d = Sample();
    ZoneSel   sel;
    SelToggle(sel, {0, 0});
    SelToggle(sel, {0, 3});
    CHECK(sel.size() == 2 && SelHas(sel, ZoneRef{0, 3}));
    SelToggle(sel, {0, 0});
    CHECK(sel.size() == 1 && !SelHas(sel, ZoneRef{0, 0}));
    CHECK(SelValid(d, {{0, 9}, {0, 3}, {0, 3}}).size() == 1);
    {  // 13-62h final review: a ref into an off variation is dropped (valid and followed)
        SoundData off = d;
        off.frames[1].on = false;
        CHECK(SelValid(off, {{1, 0}, {0, 3}}) == (ZoneSel{{0, 3}}));
        CHECK(SelFollow(d, {{1, 0}}, off).empty());
    }
    CHECK(SelFrames({{1, 0}, {0, 2}, {0, 3}}) == (std::vector<int>{0, 1}));
    CHECK(ZonesOfFrame(d, 0).size() == 4 && FirstZoneOf(d, 0) == (ZoneRef{0, 0}) && FirstZoneOf(d, 1) == (ZoneRef{1, 0}));
    CHECK(SelSorted(d, {{1, 0}, {0, 2}}) == (ZoneSel{{0, 2}, {1, 0}}));
    // A write that removes the first Heel: the Toe selected stays itself at its new index.
    SoundData before = d;
    ZoneSel   del = {{0, 0}};
    CHECK(DeleteZones(d, del, {}));
    const ZoneSel kept = SelFollow(before, {{0, 3}}, d);
    CHECK(kept.size() == 1 && ZoneOf(d, kept.front()) && ZoneOf(d, kept.front())->part == "toe" &&
          std::fabs(ZoneOf(d, kept.front())->a - 0.70) < 1e-9);
    // A zone gone: dropped.
    CHECK(SelFollow(before, {{0, 0}}, d).empty());
    CHECK(ZoneRangeMs(0.1, 0.25) == "100\xE2\x80\x93" "250 ms");
    CHECK(ZoneDbText(1.54) == "+1.5 dB" && ZoneDbText(-3.0) == "-3.0 dB" && ZoneDbText(-0.01) == "0.0 dB");
}

}  // namespace

// 13-62h (Antho 2026-10-15, v5): zones joined by Gaps are one item -- selected, moved, gained, double-clicked whole.
void TestItems()
{
    const SoundData d = Sample();
    const ZoneSel   item = {{0, 0}, {0, 1}, {0, 2}};
    CHECK(ItemOf(d, {0, 0}) == item && ItemOf(d, {0, 1}) == item && ItemOf(d, {0, 2}) == item);
    CHECK(ItemOf(d, {0, 3}).empty() && ItemOf(d, {1, 0}).empty());  // butted, not glued; alone
    // The Selection panel's card: exactly one whole item (any order).
    CHECK(SelItem(d, {{0, 2}, {0, 0}, {0, 1}}) == item);
    CHECK(SelItem(d, {{0, 0}, {0, 1}}).empty() && SelItem(d, {{0, 0}, {0, 1}, {0, 2}, {0, 3}}).empty() && SelItem(d, {}).empty());
    const std::vector<std::string> names = {"Heel", "Toe"};
    CHECK(ItemNames(d, item, names) == "Heel + Heel" && ItemGlue(d, item) == "glued by a Gap");
    double a = 0.0, b = 0.0;
    CHECK(ItemEdges(d, item, &a, &b) && std::fabs(a - 0.10) < 1e-9 && std::fabs(b - 0.70) < 1e-9);
    CHECK(std::fabs(ItemHeard(d, item) - (0.60 - 0.05)) < 1e-6);  // the Gap jumped (its crossfade read past it, overlapped)
    // Ctrl+click: the whole item added, then taken out whole (what else was selected stays).
    ZoneSel sel = {{1, 0}};
    SelToggleItem(sel, item, {0, 1});
    CHECK(sel.size() == 4 && SelHas(sel, {0, 0}) && SelHas(sel, {0, 2}));
    SelToggleItem(sel, item, {0, 2});
    CHECK((sel == ZoneSel{{1, 0}}));
    // A vertical drag: unselected = all its item's part zones; selected = the selection's; a zone alone = itself.
    CHECK(GainDragZones(d, {}, {0, 2}) == (ZoneSel{{0, 0}, {0, 2}}));
    CHECK(GainDragZones(d, {{0, 2}, {1, 0}}, {0, 2}) == (ZoneSel{{0, 2}, {1, 0}}));
    CHECK((GainDragZones(d, {{1, 0}}, {0, 3}) == ZoneSel{{0, 3}}));
    // Fix 2026-10-19: a move / edge / fade / snap drag acts on the selection when the dragged zone is in it and the
    // selection reaches past its own item; else on it alone (empty).
    CHECK((GroupDragZones(d, {{1, 0}, {0, 2}}, {0, 2}) == ZoneSel{{0, 2}, {1, 0}}));
    CHECK(GroupDragZones(d, {{1, 0}}, {0, 2}).empty());                      // not selected: alone
    CHECK(GroupDragZones(d, {{0, 0}, {0, 1}, {0, 2}}, {0, 1}).empty());      // its own item only
    CHECK(GroupDragZones(d, {{1, 0}}, {1, 0}).empty());                      // itself only
    // The top of one of its zones moves the whole item; its Gap slides alone (v6 rule A); a zone alone moves alone.
    ZoneHit h;
    h.row = ZoneRow::Lane;
    h.kind = ZoneHitKind::Zone;
    h.top = true;
    h.z = {0, 1};
    CHECK(StartZoneDrag(d, h, false, false) == ZoneDragKind::Move);
    h.z = {0, 2};
    CHECK(StartZoneDrag(d, h, false, false) == ZoneDragKind::MoveChain);
    h.z = {0, 3};
    CHECK(StartZoneDrag(d, h, false, false) == ZoneDragKind::Move);
    h.z = {0, 0};
    h.top = false;
    CHECK(StartZoneDrag(d, h, false, false) == ZoneDragKind::Scrub && StartZoneDrag(d, h, false, true) == ZoneDragKind::MkGap);
    // Double-click: the item's time selection on a lane (a greyed copy too); the Placed strip's block stays one zone.
    h.top = true;
    CHECK(DoubleActionOf(d, h) == ZoneDoubleAction::TselItem);
    h.kind = ZoneHitKind::Grey;
    CHECK(DoubleActionOf(d, h) == ZoneDoubleAction::TselItem);
    h.z = {0, 3};
    CHECK(DoubleActionOf(d, h) == ZoneDoubleAction::TselZone);
    h.z = {0, 0};
    h.row = ZoneRow::Placed;
    h.kind = ZoneHitKind::Pick;
    CHECK(DoubleActionOf(d, h) == ZoneDoubleAction::TselZone);
    // Over its Gap's body: the move cursor, as over a zone (v6 rule A: it slides that Gap).
    ZoneHit gh;
    gh.row = ZoneRow::Lane;
    gh.kind = ZoneHitKind::Zone;
    gh.z = {0, 1};
    gh.top = true;
    CHECK(ZoneCursorOf(gh) == ZoneCursor::Grab);
    // Its tooltip says it is one item.
    h.row = ZoneRow::Lane;
    h.kind = ZoneHitKind::Zone;
    CHECK(ZoneTip(d, h, names, true).find("Heel + Heel glued by a Gap: one item") == 0);
}

// 13-67a: Go to previous marker / project start (40172) and the right-drag marquee.
static void TestPrevStopAndMarquee()
{
    const SoundData d = Sample();
    // Zone starts 0.10, 0.45, 0.70 (the Gap is no stop), 1.20; the file start last.
    CHECK(std::fabs(PrevEditStop(d, -1, 2.0, false, 0.0) - 1.20) < 1e-9);
    CHECK(std::fabs(PrevEditStop(d, -1, 1.0, false, 0.0) - 0.70) < 1e-9);
    CHECK(std::fabs(PrevEditStop(d, -1, 0.70, false, 0.0) - 0.45) < 1e-9);  // at a stop: the one before
    CHECK(std::fabs(PrevEditStop(d, -1, 0.45, false, 0.0) - 0.10) < 1e-9);
    CHECK(PrevEditStop(d, -1, 0.10, false, 0.0) == 0.0);
    CHECK(PrevEditStop(d, -1, 0.0, false, 0.0) == 0.0);
    CHECK(PrevEditStop(d, -1, -1.0, false, 0.0) == 0.0);  // no cursor
    // A time selection's start is a stop too, nearest first.
    CHECK(std::fabs(PrevEditStop(d, -1, 0.70, true, 0.55) - 0.55) < 1e-9);
    CHECK(std::fabs(PrevEditStop(d, -1, 0.55, true, 0.55) - 0.45) < 1e-9);
    CHECK(std::fabs(PrevEditStop(d, -1, 0.05, true, 0.02) - 0.02) < 1e-9);
    // Marquee over variation 2 only: its Toe.
    ZoneSel hits = ZonesInSpan(d, -1, 1.30, 1.10, nullptr);  // either order
    CHECK(hits.size() == 1 && hits[0].frame == 1 && hits[0].index == 0);
    // Over the first Heel: its item whole (Heel + Gap + Heel), time order.
    hits = ZonesInSpan(d, -1, 0.20, 0.30, nullptr);
    CHECK(hits.size() >= 3 && hits[0].frame == 0 && hits[0].index == 0 && SelHas(hits, ZoneRef{0, 1}) && SelHas(hits, ZoneRef{0, 2}));
    // Lane rows: only the parts covered (Toe's lane over Heel's time: nothing).
    const std::vector<std::string> toe = {"toe"};
    CHECK(ZonesInSpan(d, -1, 0.20, 0.30, &toe).empty());
    hits = ZonesInSpan(d, -1, 0.75, 1.30, &toe);
    CHECK(SelHas(hits, ZoneRef{0, 3}) && SelHas(hits, ZoneRef{1, 0}) && !SelHas(hits, ZoneRef{0, 0}));
    // Nothing under it.
    CHECK(ZonesInSpan(d, -1, 1.70, 1.90, nullptr).empty());
    // Replace, or add with Ctrl (once each).
    ZoneSel sel = {ZoneRef{0, 3}};
    SelMarquee(sel, {ZoneRef{1, 0}}, false);
    CHECK(sel.size() == 1 && sel[0].frame == 1);
    SelMarquee(sel, {ZoneRef{1, 0}, ZoneRef{0, 3}}, true);
    CHECK(sel.size() == 2 && SelHas(sel, ZoneRef{0, 3}));
    SelMarquee(sel, {}, false);
    CHECK(sel.empty());
}

// Sound Edit evening (A): Ctrl+A -- every zone, or the V filter's variation's.
static void TestSelectAll()
{
    SoundData d = Sample();
    ZoneSel   all = ZonesToSelectAll(d, -1, -1);
    CHECK(all.size() == 5 && all.front() == (ZoneRef{0, 0}) && all.back() == (ZoneRef{1, 0}));
    ZoneSel v2 = ZonesToSelectAll(d, -1, 1);
    CHECK(v2.size() == 1 && v2[0] == (ZoneRef{1, 0}));
    ZoneSel v1 = ZonesToSelectAll(d, -1, 0);
    CHECK(v1.size() == 4 && SelHas(v1, ZoneRef{0, 1}));
    CHECK(ZonesToSelectAll(d, -1, 7).size() == 5);  // a filter on no variation: All
    // Takes mode: the active take's zones only (its own timeline); no take: nothing.
    d.mode = DetectMode::Takes;
    CHECK(ZonesToSelectAll(d, 1, -1).size() == 1 && ZonesToSelectAll(d, 1, -1)[0].frame == 1);
    CHECK(ZonesToSelectAll(d, 0, -1).size() == 4);
    CHECK(ZonesToSelectAll(d, -1, -1).empty());
}

int main()
{
    TestHitLane();
    TestHitStrips();
    TestDrags();
    TestLooks();
    TestStepAndHeal();
    TestNewPartAndSnap();
    TestDouble();
    TestTipsAndCursors();
    TestSelection();
    TestItems();
    TestPrevStopAndMarquee();
    TestSelectAll();
    if (g_fails) {
        std::printf("zone_hit_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("zone_hit_test: all passed\n");
    return 0;
}
