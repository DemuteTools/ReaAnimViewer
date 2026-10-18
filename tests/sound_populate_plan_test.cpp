// SPDX-License-Identifier: MIT
//
// Story 13-8 -- src/sound_populate_plan.h (pure): orphan rules, layers, the draw hooks, event
// slots on a re-Populate, the nearest pass, the nudge, the footer count and the report line.

#include "sound_populate_plan.h"
#include "sound_place_plan.h"
#include "sound_entries.h"  // 13-73: SetSidePattern

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "auto_detect.h"
#include "bone_roles.h"
#include "sound_assign.h"
#include "sound_draw.h"
#include "sound_entries.h"
#include "sound_record.h"
#include "sound_switch.h"
#include "sound_zones.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

static SoundNode Node(const std::string& uid, const std::string& parent, SoundNodeType type)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = type;
    n.name = uid;
    return n;
}

static SoundNode Leaf(const std::string& uid, const std::string& parent, int frames)
{
    SoundNode n = Node(uid, parent, SoundNodeType::Sound);
    n.sound.source.item_guid = "{" + uid + "}";
    for (int i = 0; i < frames; ++i) {
        SoundFrame f;
        f.start = i;
        f.length = 0.5;
        n.sound.frames.push_back(f);
    }
    return n;
}

static ExistingSlot Ex(int pass, const std::string& node, int var, Ownership own = Ownership::Populated)
{
    ExistingSlot x;
    x.pass = pass;
    x.node = node;
    x.variation = var;
    x.own = own;
    return x;
}

static DesiredSlot De(int pass, const std::string& node, int var)
{
    DesiredSlot d;
    d.pass = pass;
    d.node = node;
    d.variation = var;
    return d;
}

// ---- 13-20 / 13-23 / 13-25 (spec 13-20-23-25 Part B, matrix rows 2-8, 11) ----------------------

static bool Near(double a, double b, double e = 1e-9) { return std::fabs(a - b) <= e; }

// "Contact": Back <- heel, Front <- toe, toe end (the parts follow the group's order: Back first).
static SoundStore ContactStore(bool hand_on_front = false)
{
    SoundStore st;
    SwitchGroup g;
    g.uid = "gc";
    g.name = "Contact";
    SwitchValue back{"vb", "Back", {}, {"heel"}};
    SwitchValue front{"vf", "Front", {}, {"toe", "toe_end"}};
    if (hand_on_front) front.roles.push_back("hand");
    g.values = {back, front};
    st.groups.push_back(g);
    return st;
}

// A Silence Sound of 3 variations, each [i, i + 0.4) cut at i + 0.2 (Back | Front), divided by Contact;
// its sync points: Back at i + 0.018, Front at i + 0.218.
static SoundNode DividedSteps(const std::string& uid, const std::string& parent)
{
    SoundNode n = Node(uid, parent, SoundNodeType::Sound);
    n.name = "Steps";
    n.sound.mode = DetectMode::Silence;
    n.sound.divide_group = "gc";
    n.sound.divide_values = {"vb", "vf"};
    // 13-62h: a divided Sound is its zones: Back [i, i + 0.2] | Front [i + 0.2, i + 0.4], each with its detected
    // onset 20 ms in (Attack - 2 ms = 18 ms in, without peaks).
    for (int i = 0; i < 3; ++i) {
        SoundFrame f;
        f.start = i;
        f.length = 0.4;
        for (int k = 0; k < 2; ++k) {
            EditZone z;
            z.a = z.slot_a = i + 0.2 * k;
            z.b = z.slot_b = i + 0.2 * (k + 1);
            z.part = k == 0 ? "vb" : "vf";
            z.onset = z.a + 0.02;
            f.zones.push_back(z);
        }
        n.sound.frames.push_back(f);
    }
    return n;
}

// 13-62h: variation v's zones as given (its start / length their extent).
static void SetZones(SoundData& d, int v, const std::vector<EditZone>& zones)
{
    SoundFrame& f = d.frames[static_cast<size_t>(v)];
    f.zones = zones;
    f.start = zones.front().a;
    f.length = zones.back().b - zones.front().a;
}

static SoundLibrary OneSound(const SoundNode& n)
{
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = n.uid;
    lib.nodes.push_back(n);
    return lib;
}

struct Resolved {
    Resolution res;
    EventDraw  ed;
};
static Resolved ResolveFor(const SoundStore& st, const SoundLibrary& lib, const Block& b, PopulateDrawState* draw = nullptr)
{
    PopulateDrawState local;
    PopulateDrawState& ds = draw ? *draw : local;
    ds.lib = &lib;
    if (!ds.seed) ds.seed = 7;
    Resolved r;
    r.ed.rid = b.rid;
    r.ed.t = 1.0;
    ValueLayers layers;
    layers.role = EventRoleKey(b);
    r.ed.side = EventSide(b);  // 13-73: as Populate
    r.res = ResolveEvent(st, lib, layers, MakeDrawHooks(ds, r.ed, nullptr));
    return r;
}

static Block Auto(AutoKind k, char side = 'L')
{
    Block b = MakeAutoBlock(k, side, kAutoDefaultSens, 0.0);
    b.rid = "r1";
    return b;
}

static void TestParts()
{
    const SoundStore   st = ContactStore();
    const SoundLibrary lib = OneSound(DividedSteps("s", ""));
    const SoundData&   sd = lib.nodes.front().sound;

    // Row 2: a Heel auto block gets the Back part only, its sync on the event, 2 ms fade at the cut.
    {
        PopulateDrawState ds;
        Resolved          r = ResolveFor(st, lib, Auto(AutoKind::Heel), &ds);
        CHECK(r.res.sounds.size() == 1 && r.res.sounds[0].part_value == "vb");
        const int v = r.res.sounds[0].variation;
        CHECK(v >= 0 && v < 3);
        CHECK(r.ed.out.size() == 1 && r.ed.out[0].part == "vb" && r.ed.out[0].pick == v);
        CHECK(ds.history.count("s|vb") == 1 && ds.history.count("s") == 0);  // history per node and part
        const std::vector<PieceApply> pa = PieceApplyFor(sd, v, "vb");
        CHECK(pa.size() == 1 && pa[0].anchor && Near(pa[0].slice.start_offs, v) && Near(pa[0].slice.length, 0.2));
        CHECK(Near(pa[0].slice.snap_offset, 0.018, 1e-9));  // the event lands on Back's sync point
        CHECK(pa[0].fades.set_out && Near(pa[0].fades.out, kDefaultCutFade));  // the cut side
        CHECK(pa[0].fades.set_in && Near(pa[0].fades.in, kDefaultFadeIn));      // the frame's own edge
        // A Toe block: the Front part.
        Resolved t = ResolveFor(st, lib, Auto(AutoKind::Toe));
        CHECK(t.res.sounds.size() == 1 && t.res.sounds[0].part_value == "vf");
        const std::vector<PieceApply> pf = PieceApplyFor(sd, t.res.sounds[0].variation, "vf");
        CHECK(pf.size() == 1 && Near(pf[0].slice.snap_offset, 0.018) && Near(pf[0].fades.in, kDefaultCutFade) &&
              Near(pf[0].fades.out, kDefaultFadeOut));
        // Slide / pivot heel kinds read as heel too.
        CHECK(ResolveFor(st, lib, Auto(AutoKind::PivotHeel)).res.sounds[0].part_value == "vb");
    }
    // Row 3: a combined step (FS) gets the whole variation, its first part's sync. 13-62h (C2): one item per
    // zone, never merged: Back and Front butted, At cuts at the cut.
    {
        Resolved r = ResolveFor(st, lib, Auto(AutoKind::Step));
        CHECK(r.res.sounds.size() == 1 && r.res.sounds[0].part_value.empty() && r.ed.out[0].part.empty());
        const int                     v = r.res.sounds[0].variation;
        const std::vector<PieceApply> pa = PieceApplyFor(sd, v, "");
        CHECK(pa.size() == 2 && pa[0].anchor && Near(pa[0].slice.length, 0.2) && Near(pa[0].slice.snap_offset, 0.018));
        if (pa.size() == 2) {
            CHECK(Near(pa[0].fades.in, kDefaultFadeIn) && Near(pa[1].fades.out, kDefaultFadeOut));
            CHECK(Near(pa[0].fades.out, kDefaultCutFade) && Near(pa[1].fades.in, kDefaultCutFade) &&
                  Near(pa[1].from_anchor, 0.2));
        }
    }
    // Row 4: a rule whose first condition reads a toe end bone: Front.
    {
        Block b;
        b.marker = "Rule";
        b.rid = "r2";
        Condition c;
        c.signal.bones = {static_cast<int>(Role::LeftToeEnd)};
        b.conditions.push_back(c);
        CHECK(ResolveFor(st, lib, b).res.sounds[0].part_value == "vf");
    }
    // Row 5: the Sound maps heel -> Front: only this Sound changes.
    {
        SoundNode n = DividedSteps("s", "");
        n.sound.role_map = {{"heel", "vf"}};
        const SoundLibrary over = OneSound(n);
        CHECK(ResolveFor(st, over, Auto(AutoKind::Heel)).res.sounds[0].part_value == "vf");
        CHECK(ResolveFor(st, lib, Auto(AutoKind::Heel)).res.sounds[0].part_value == "vb");
    }
    // Row 6: a Grab block: whole without hand on the group; Front once the group maps hand -> Front.
    {
        CHECK(ResolveFor(st, lib, Auto(AutoKind::Grab)).res.sounds[0].part_value.empty());
        const SoundStore hands = ContactStore(true);
        CHECK(ResolveFor(hands, lib, Auto(AutoKind::Grab)).res.sounds[0].part_value == "vf");
        CHECK(ResolveFor(hands, lib, Auto(AutoKind::Release, 'R')).res.sounds[0].part_value == "vf");
    }
    // Row 7: no Front zone left in any variation: nothing placed for toe events, NoSound text; heels still play.
    // 13-62h: a variation can play a part when it has a zone of it (ActiveVariationsFor).
    {
        SoundNode n = DividedSteps("s", "");
        for (int i = 0; i < 3; ++i) SetZones(n.sound, i, {n.sound.frames[static_cast<size_t>(i)].zones[0]});
        const SoundLibrary ex = OneSound(n);
        Resolved           r = ResolveFor(st, ex, Auto(AutoKind::Toe));
        CHECK(r.res.sounds.size() == 1 && r.res.sounds[0].variation == -1);
        CHECK(r.ed.no_part.count("s") == 1 && r.ed.no_part["s"] == "vf" && r.ed.out.empty());
        CHECK(NoPartText("Steps", "Front") == "Nothing placed: Steps has no Front part left");
        CHECK(NoPartText("Steps", "") == "Nothing placed: Steps has nothing left to place");
        Resolved h = ResolveFor(st, ex, Auto(AutoKind::Heel));
        CHECK(h.res.sounds[0].variation >= 0 && h.ed.no_part.empty());
        // One variation with its Front part left: always that one.
        SoundNode m = DividedSteps("s", "");
        SetZones(m.sound, 0, {m.sound.frames[0].zones[0]});
        SetZones(m.sound, 2, {m.sound.frames[2].zones[0]});
        const SoundLibrary one = OneSound(m);
        for (int k = 0; k < 4; ++k) CHECK(ResolveFor(st, one, Auto(AutoKind::Toe)).res.sounds[0].variation == 1);
        // A variation turned off is not drawn for a part either; all off = no active variation.
        SoundNode off = DividedSteps("s", "");
        for (SoundFrame& f : off.sound.frames) f.on = false;
        const SoundLibrary none = OneSound(off);
        Resolved           z = ResolveFor(st, none, Auto(AutoKind::Toe));
        CHECK(z.ed.no_active.count("s") == 1 && z.ed.no_part.empty());
    }
    // Row 8 (13-62h: on zones): empty space inside Back (two Back zones [0, 0.08] and [0.12, 0.2]): two pieces,
    // the space kept, both placed from the first one's snap.
    {
        SoundNode     n = DividedSteps("s", "");
        std::vector<EditZone> z = n.sound.frames[0].zones;
        EditZone      b2 = z[0];
        z[0].b = z[0].slot_b = 0.08;
        b2.a = b2.slot_a = 0.12;
        b2.onset = -1.0;
        SetZones(n.sound, 0, {z[0], b2, z[1]});
        CHECK(ZonesProblem(n.sound, 0.0).empty());
        const std::vector<PieceApply> pa = PieceApplyFor(n.sound, 0, "vb");
        CHECK(pa.size() == 2);
        CHECK(Near(pa[0].slice.start_offs, 0.0) && Near(pa[0].slice.length, 0.08) && pa[0].anchor);
        CHECK(Near(pa[1].slice.start_offs, 0.12) && Near(pa[1].slice.length, 0.08) && !pa[1].anchor);
        CHECK(Near(pa[0].slice.snap_offset, 0.018) && Near(pa[1].slice.snap_offset, 0.0));
        CHECK(Near(pa[1].from_anchor, 0.12) && Near(pa[0].from_anchor, 0.0));
        CHECK(Near(pa[0].fades.out, kDefaultFadeOut) && Near(pa[1].fades.in, kDefaultFadeIn) &&
              Near(pa[1].fades.out, kDefaultCutFade));  // free edge, free edge, cut
        // On the timeline: the anchor at event - snap, the second piece 0.12 s later (the space kept).
        const double anchor = PlacementPosition(10.0, pa[0].slice.snap_offset, 0.0, 0.0);
        CHECK(Near(anchor, 9.982) && Near(PiecePosition(anchor, pa[1].from_anchor, 1.0), 10.102));
        CHECK(Near(PiecePosition(anchor, pa[1].from_anchor, 2.0), 10.042));  // playrate 2: item seconds
        // 13-62h final review: a snap set by hand on the second zone is the event's (it lands there), the first one
        // placed before it.
        n.sound.frames[0].zones[1].snap = 0.13;
        const std::vector<PieceApply> pb = PieceApplyFor(n.sound, 0, "vb");
        CHECK(pb.size() == 2 && !pb[0].anchor && pb[1].anchor && Near(pb[1].slice.snap_offset, 0.01) &&
              Near(pb[0].from_anchor, -0.12));
        // The whole variation: its three zones, each its own item (from the same anchor).
        const std::vector<PieceApply> pw = PieceApplyFor(n.sound, 0, "");
        CHECK(pw.size() == 3 && Near(pw[2].slice.length, 0.2) && Near(pw[2].fades.out, kDefaultFadeOut) &&
              Near(pw[2].from_anchor, 0.08));
        // The slot is placed again when the piece count or the part changes (re-Populate).
        std::vector<DesiredSlot>  d = {De(0, "s", 0)};
        std::vector<ExistingSlot> x = {Ex(0, "s", 0)};
        d[0].pieces = 2;
        CHECK(PlanEventSlots(d, x).size() == 1 && PlanEventSlots(d, x)[0].act == SlotAction::SetVariation);
        x.push_back(Ex(0, "s", 0));
        CHECK(PlanEventSlots(d, x)[0].act == SlotAction::Keep);
        d[0].part = "vb";
        CHECK(PlanEventSlots(d, x)[0].act == SlotAction::SetVariation);
        x[0].part = x[1].part = "vb";
        CHECK(PlanEventSlots(d, x)[0].act == SlotAction::Keep);
        // 13-25: the anchor snap is compared when known: placed before sync points (no sy) or under
        // another sync, the slot is placed again.
        d[0].sync = 0.018;
        CHECK(PlanEventSlots(d, x)[0].act == SlotAction::SetVariation);
        x[0].sync = 0.0;
        CHECK(PlanEventSlots(d, x)[0].act == SlotAction::SetVariation);
        x[0].sync = 0.018 + 1e-9;
        CHECK(PlanEventSlots(d, x)[0].act == SlotAction::Keep);
        // A piece deleted by hand (a Deleted placement of the node): the others are kept, not re-placed.
        {
            std::vector<ExistingSlot> y = {x[0], Ex(0, "s", 0, Ownership::Deleted)};
            std::vector<DesiredSlot>  w = d;
            w[0].pieces = 2;
            const std::vector<SlotStep> steps = PlanEventSlots(w, y);
            CHECK(steps.size() == 1 && steps[0].act == SlotAction::Keep);
        }
        d[0].sync = -1.0;
        d[0].pieces = 0;  // not counted (a pile): the items are not compared
        x.push_back(Ex(0, "s", 0));
        x[2].part = "vb";
        CHECK(PlanEventSlots(d, x)[0].act == SlotAction::Keep);
    }
    // Row 11 (13-62i): Takes mode is divided too: a Heel event gets the Heel part of a take (its zones), drawn among
    // the takes that have it.
    {
        SoundNode n = DividedSteps("s", "");
        n.sound.mode = DetectMode::Takes;
        n.sound.zones.push_back({0.0, 3.0, "", {}});  // a Sound-level zone of the old model: ignored by a take
        const SoundLibrary tk = OneSound(n);
        Resolved           r = ResolveFor(st, tk, Auto(AutoKind::Heel));
        CHECK(!r.res.sounds[0].part_value.empty() && r.res.sounds[0].variation >= 0 && r.ed.no_part.empty());
        CHECK(PieceApplyFor(n.sound, 0, r.res.sounds[0].part_value).size() == 1);
    }
    // 13-31 (d): a Blend picks its layers by the event's role, then each divided layer its own part.
    {
        SoundLibrary bl;
        bl.uid = "L";
        bl.root = "b";
        bl.nodes.push_back(Node("b", "", SoundNodeType::Blend));
        SoundNode a = DividedSteps("a", "b");
        a.layer_roles = {"heel"};
        SoundNode c = DividedSteps("c", "b");
        c.layer_roles = {"toe"};
        SoundNode e = DividedSteps("e", "b");  // no role: combined events only
        bl.nodes.insert(bl.nodes.end(), {a, c, e});
        Resolved r = ResolveFor(st, bl, Auto(AutoKind::Heel));
        CHECK(r.res.sounds.size() == 1 && r.res.sounds[0].node == "a" && r.res.sounds[0].part_value == "vb");
    }
    // A combined Sound on a heel event: the whole variation (A3); a value outside divide_values too.
    {
        SoundNode n = DividedSteps("s", "");
        n.sound.divide_group.clear();
        n.sound.divide_values.clear();
        for (SoundFrame& f : n.sound.frames) {
            f.zones.clear();
            f.syncs = {f.start + 0.018};
        }
        CHECK(ResolveFor(st, OneSound(n), Auto(AutoKind::Heel)).res.sounds[0].part_value.empty());
        SoundNode m = DividedSteps("s", "");
        m.sound.role_map = {{"heel", "gone"}};
        CHECK(ResolveFor(st, OneSound(m), Auto(AutoKind::Heel)).res.sounds[0].part_value.empty());
    }
}

// ---- 13-59: the variation of each loop pass ------------------------------------------------------
static void TestLoopPassDraws()
{
    // The project's choice: New by default, `loopvar=same` written for Same, the other tokens kept.
    SoundStore st;
    st.kept = {"trk=flat"};
    CHECK(LoopPassDrawOf(st) == LoopPassDraw::New);
    CHECK(!SetLoopPassDraw(st, LoopPassDraw::New));
    CHECK(SetLoopPassDraw(st, LoopPassDraw::Same));
    CHECK(LoopPassDrawOf(st) == LoopPassDraw::Same);
    CHECK(st.kept.size() == 2 && st.kept[0] == "trk=flat" && st.kept[1] == "loopvar=same");
    CHECK(!SetLoopPassDraw(st, LoopPassDraw::Same));
    CHECK(SetLoopPassDraw(st, LoopPassDraw::New));
    CHECK(st.kept.size() == 1 && st.kept[0] == "trk=flat");
    st.kept = {"loopvar=other"};  // an unknown value reads as the default
    CHECK(LoopPassDrawOf(st) == LoopPassDraw::New);

    // The keys of a pass's draws.
    long long   pass = 0;
    std::string node;
    CHECK(PassDrawKey("0123456789abcdef", 3) == "0123456789abcdef#p3");
    CHECK(ParsePassDrawKey("0123456789abcdef#p3", &pass, &node) && pass == 3 && node == "0123456789abcdef");
    CHECK(ParsePassDrawKey(PassDrawKey("n", -2), &pass, &node) && pass == -2 && node == "n");
    CHECK(!ParsePassDrawKey("0123456789abcdef", &pass, &node));
    CHECK(!ParsePassDrawKey("#p3", &pass, &node));
    CHECK(!ParsePassDrawKey("n#p", &pass, &node));
    CHECK(!ParsePassDrawKey("n#px", &pass, &node));

    // Seeds: pass 0 keeps the item's, the others differ from it and from each other.
    CHECK(PassDrawSeed(42, 0) == 42);
    CHECK(PassDrawSeed(42, 1) != 42 && PassDrawSeed(42, 1) != PassDrawSeed(42, 2));
    CHECK(PassDrawSeed(42, 1) == PassDrawSeed(42, 1));

    // What a pass reads: the event's hand picks first, then its own entries; never another pass's.
    SoundDraw hand{"h", 2, true, {}, ""};
    SoundDraw drawn{"s", 1, false, {}, ""};
    SoundDraw p1{PassDrawKey("s", 1), 4, false, {}, ""};
    SoundDraw p2{PassDrawKey("s", 2), 5, false, {}, ""};
    const std::vector<SoundDraw> draws = {hand, drawn, p1, p2};
    const std::vector<SoundDraw> r1 = PassPrevDraws(draws, 1);
    CHECK(r1.size() == 2 && r1[0].node == "h" && r1[0].hand && r1[1].node == "s" && r1[1].pick == 4 && !r1[1].hand);
    CHECK(PassPrevDraws(draws, 7).size() == 1);
    // Merging a pass's new picks replaces only that pass's entries, never by hand.
    std::vector<SoundDraw> merged = draws;
    SoundDraw out{"s", 0, true, {}, ""};
    MergePassDraws(merged, 1, {out});
    CHECK(merged.size() == 4);
    int found = 0;
    for (const SoundDraw& d : merged) {
        if (d.node == PassDrawKey("s", 1)) {
            ++found;
            CHECK(d.pick == 0 && !d.hand);
        }
        if (d.node == PassDrawKey("s", 2)) CHECK(d.pick == 5);
    }
    CHECK(found == 1);
    // The record keeps them as written (round trip).
    ItemSounds item;
    EventSounds ev;
    ev.rid = "r1";
    ev.t = 1.0;
    ev.draws = merged;
    item.events = {ev};
    ItemSounds back;
    CHECK(ParseItemSounds(SerializeItemSounds(item), &back) && back.events.size() == 1 && back.events[0].draws.size() == 4);
    // Reshuffle forgets the passes' picks with the other drawn ones (the hand pick stays).
    Reshuffle(back, 99);
    CHECK(back.events[0].draws.size() == 1 && back.events[0].draws[0].node == "h");

    // Drawing: each pass draws on its own seed and history; a pass with its stored pick keeps it.
    const SoundLibrary lib = OneSound(Leaf("s", "", 8));
    const SoundStore   empty;
    std::set<int>      picks;
    for (long long p = 1; p <= 6; ++p) {
        PopulateDrawState ps;
        ps.lib = &lib;
        ps.seed = PassDrawSeed(7, p);
        EventDraw ed;
        ed.rid = "r1";
        ed.t = 1.0;
        const Resolution res = ResolveEvent(empty, lib, ValueLayers(), MakeDrawHooks(ps, ed, nullptr));
        CHECK(res.sounds.size() == 1);
        if (!res.sounds.empty()) picks.insert(res.sounds[0].variation);
        // Stored, then drawn again: the same pick.
        std::vector<SoundDraw> stored;
        MergePassDraws(stored, p, ed.out);
        const std::vector<SoundDraw> prev = PassPrevDraws(stored, p);
        PopulateDrawState ps2;
        ps2.lib = &lib;
        ps2.seed = PassDrawSeed(1234, p);  // another seed: the stored pick still wins
        EventDraw ed2;
        ed2.rid = "r1";
        ed2.t = 1.0;
        ed2.prev = &prev;
        const Resolution again = ResolveEvent(empty, lib, ValueLayers(), MakeDrawHooks(ps2, ed2, nullptr));
        CHECK(!again.sounds.empty() && !res.sounds.empty() && again.sounds[0].variation == res.sounds[0].variation);
    }
    CHECK(picks.size() >= 2);  // the passes do not all play one variation
}

// 13-59 review: a fill on a pass that already has a placement keeps its record consistent (its entries
// seeded from what plays there), and a later Populate changes nothing; the passes cut off are pruned.
static void TestLoopPassFillRecord()
{
    // Random(a, b), each with 6 variations; pass 3 plays b, variation 4 (placed before any pass entry).
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "r";
    lib.nodes = {Node("r", "", SoundNodeType::Random), Leaf("a", "r", 6), Leaf("b", "r", 6)};
    const SoundStore empty;
    std::vector<SoundDraw> draws = {SoundDraw{"a", 2, false, {}, ""}};  // the event's own (pass 0) draw
    CHECK(!HasPassDraws(draws, 3));
    const std::vector<SoundDraw> seeded = PassDrawsFromPlaced(lib, {PlacedPick{"b", 4, ""}});
    CHECK(seeded.size() == 2);
    MergePassDraws(draws, 3, seeded);
    CHECK(HasPassDraws(draws, 3) && !HasPassDraws(draws, 2));
    CHECK(draws.size() == 3 && draws[0].node == "a");  // the event's draw untouched
    // Whatever the seed, a later Populate of pass 3 draws what is placed there: nothing replaced.
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        const std::vector<SoundDraw> prev = PassPrevDraws(draws, 3);
        PopulateDrawState ps;
        ps.lib = &lib;
        ps.seed = PassDrawSeed(seed, 3);
        EventDraw ed;
        ed.rid = "r1";
        ed.t = 1.0;
        ed.prev = &prev;
        const Resolution res = ResolveEvent(empty, lib, ValueLayers(), MakeDrawHooks(ps, ed, nullptr));
        CHECK(res.sounds.size() == 1 && res.sounds[0].node == "b" && res.sounds[0].variation == 4);
    }
    // A Sound placed as copied (no variation) and a node gone add nothing of their own.
    CHECK(PassDrawsFromPlaced(lib, {PlacedPick{"gone", 1, ""}}).empty());
    const std::vector<SoundDraw> copied = PassDrawsFromPlaced(lib, {PlacedPick{"a", -1, ""}});
    CHECK(copied.size() == 1 && copied[0].node == "r" && copied[0].pick == 0);
    // Pruning keeps the passes the event still has (and every entry that is not a pass's).
    MergePassDraws(draws, 7, seeded);
    PrunePassDraws(draws, {3});
    CHECK(draws.size() == 3 && HasPassDraws(draws, 3) && !HasPassDraws(draws, 7));
    PrunePassDraws(draws, {});
    CHECK(draws.size() == 1 && draws[0].node == "a");
}

// ---- 13-62i (D): Blend layers placed as heard (the spec's Blend matrix rows) ------------------------

// One event of the item, as Populate walks them (time order): its rule, role, stored draws, placed or not.
struct SimEvent {
    std::string            rid;
    double                 t = 0.0;
    std::string            role;
    std::vector<SoundDraw> draws;
    bool                   populated = false;
    bool                   attached = false;  // a user placement (Attached): Populate leaves it alone
};

// The record PopulateOne finds for a SimEvent (its draws, a live Populated / Attached placement).
static EventSounds RecordOf(const SimEvent& e)
{
    EventSounds ev;
    ev.rid = e.rid;
    ev.t = e.t;
    ev.draws = e.draws;
    if (e.populated) ev.placed.push_back({"{item}", "n", Ownership::Populated, 0.0, 0, "", {}});
    if (e.attached) ev.placed.push_back({"{att}", "n", Ownership::Attached, 0.0, 0, "", {}});
    return ev;
}

// The draw part of PopulateOne's event loop: an event left out (another rule than `only_rid`) feeds the
// history from its recorded draws when it is populated, and the draws before it avoid its picks (the timeline)
// (`seed_skipped`; false = before 13-62i); the others resolve with their stored draws (hand picks on a branch
// not walked kept).
static void SimPopulate(const SoundStore& st, const SoundLibrary& lib, std::vector<SimEvent>& evs, uint64_t seed,
                        const std::string& only_rid = std::string(), bool seed_skipped = true, PopulateFilter filter = {})
{
    PopulateDrawState ds;
    ds.lib = &lib;
    ds.seed = seed;
    std::map<long long, PopulateDrawState> passes;
    if (!only_rid.empty()) filter.rid = only_rid;
    // PopulateOne's own decisions (PlanPopulateEvent, SetPopulateTimeline) on these records.
    std::vector<EventSounds> recs;
    for (const SimEvent& e : evs) recs.push_back(RecordOf(e));
    std::vector<EventPlan>          plans;
    std::vector<const EventSounds*> ptrs;
    for (size_t i = 0; i < evs.size(); ++i) {
        ptrs.push_back(&recs[i]);
        plans.push_back(PlanPopulateEvent(filter, evs[i].rid, evs[i].t, &recs[i]));
    }
    if (seed_skipped) SetPopulateTimeline(ds, &passes, plans, ptrs);
    for (size_t i = 0; i < evs.size(); ++i) {
        SimEvent& e = evs[i];
        ds.at = static_cast<int>(i);
        if (plans[i] == EventPlan::Skip) continue;
        if (plans[i] == EventPlan::Keep) {
            if (seed_skipped) SeedHistory(ds, &passes, e.draws);
            continue;
        }
        const std::vector<SoundDraw> prev = e.draws;
        EventDraw                    ed;
        ed.rid = e.rid;
        ed.t = e.t;
        ed.prev = &prev;
        ValueLayers l;
        l.role = e.role;
        ResolveEvent(st, lib, l, MakeDrawHooks(ds, ed, nullptr));
        e.draws = ed.out;
        for (const SoundDraw& d : prev) {
            bool have = false;
            for (const SoundDraw& x : e.draws) have = have || x.node == d.node;
            if (!have && d.hand) e.draws.push_back(d);
        }
        e.populated = true;
    }
}

static int PickOf(const SimEvent& e, const std::string& node)
{
    for (const SoundDraw& d : e.draws)
        if (d.node == node) return d.pick;
    return -1;
}

// The layer's picks on the events of `role` in time order: how many miss or repeat one of the 2 before.
static int Repeats(const std::vector<SimEvent>& evs, const std::string& node, const std::string& role)
{
    std::vector<int> picks;
    for (const SimEvent& e : evs)
        if (e.role == role && e.populated) picks.push_back(PickOf(e, node));
    int bad = 0;
    for (size_t i = 0; i < picks.size(); ++i) {
        if (picks[i] < 0) ++bad;
        if (i >= 1 && picks[i] == picks[i - 1]) ++bad;
        if (i >= 2 && picks[i] == picks[i - 2]) ++bad;
    }
    return bad;
}

// Blend "FS" [Heel Sound (-6 dB, role heel), Toe Sound (0 dB, role toe)], 4 variations each, Avoid repeating
// the last 2.
static SoundLibrary HeelToeBlend(bool roles)
{
    SoundLibrary lib;
    lib.uid = "L";
    lib.name = "Steps";
    lib.root = "b";
    SoundNode b = Node("b", "", SoundNodeType::Blend);
    b.name = "FS";
    SoundNode h = Leaf("h", "b", 4);
    h.volume_db = -6.0;
    h.avoid_repeat = 2;
    SoundNode t = Leaf("t", "b", 4);
    t.avoid_repeat = 2;
    if (roles) {
        h.layer_roles = {"heel"};
        t.layer_roles = {"toe"};
    }
    lib.nodes = {b, h, t};
    return lib;
}

// Steps Separate: 20 heel events (L and R alternating), each with its toe event 0.1 s later.
static std::vector<SimEvent> SeparateSteps()
{
    std::vector<SimEvent> evs;
    for (int i = 0; i < 20; ++i) {
        const bool left = i % 2 == 0;
        evs.push_back({left ? "heel_l" : "heel_r", 0.5 * i, "heel", {}, false});
        evs.push_back({left ? "toe_l" : "toe_r", 0.5 * i + 0.1, "toe", {}, false});
    }
    return evs;
}

static const ResolvedSound* SoundOf(const Resolution& r, const std::string& node)
{
    for (const ResolvedSound& s : r.sounds)
        if (s.node == node) return &s;
    return nullptr;
}

static void TestBlendMatrix()
{
    const SoundStore st = ContactStore();
    const Block      hb = Auto(AutoKind::Heel);
    const Block      tb = Auto(AutoKind::Toe);
    // Blend volume: Heel items at -6 dB, Toe items at 0 dB (each piece: gain + its zone's db).
    {
        const SoundLibrary   lib = HeelToeBlend(true);
        const Resolved       rh = ResolveFor(st, lib, hb);
        const Resolved       rt = ResolveFor(st, lib, tb);
        const ResolvedSound* h = SoundOf(rh.res, "h");
        const ResolvedSound* t = SoundOf(rt.res, "t");
        CHECK(rh.res.sounds.size() == 1 && h && Near(h->gain_db, -6.0));
        CHECK(rt.res.sounds.size() == 1 && t && Near(t->gain_db, 0.0));
        if (h) {
            const SoundNode*          hn = FindNode(lib, "h");
            const std::vector<double> db = PieceDbFor(PieceApplyFor(hn->sound, h->variation, h->part_value), h->gain_db);
            CHECK(!db.empty());
            for (double d : db) CHECK(Near(d, -6.0));
        }
        CHECK(PieceDbFor({}, -6.0) == std::vector<double>{-6.0});  // no pieces: one item at the gain
        // A zoned (divided) Heel layer: each piece at the layer's gain + its zone's own gain.
        SoundLibrary zl = lib;
        SoundNode    zh = DividedSteps("h", "b");
        zh.volume_db = -6.0;
        zh.layer_roles = {"heel"};
        for (SoundFrame& f : zh.sound.frames) f.zones[0].db = 1.5;  // the Back (heel) zones
        *FindNode(zl, "h") = zh;
        const Resolved       rz = ResolveFor(st, zl, hb);
        const ResolvedSound* z = SoundOf(rz.res, "h");
        CHECK(z && z->part_value == "vb" && Near(z->gain_db, -6.0));
        if (z) {
            const std::vector<double> db = PieceDbFor(PieceApplyFor(zh.sound, z->variation, z->part_value), z->gain_db);
            CHECK(db.size() == 1 && !db.empty() && Near(db[0], -4.5));
        }
    }
    // Blend volume changed after Populate (P2): the Heel layer -6 -> -12 dB; at the next Populate the kept Heel
    // items (same draw: Keep) go to -12 dB in place, the Toe items stay.
    {
        SoundLibrary          lib = HeelToeBlend(true);
        std::vector<SimEvent> evs = SeparateSteps();
        SimPopulate(st, lib, evs, 11);
        const std::vector<SimEvent> before = evs;
        FindNode(lib, "h")->volume_db = -12.0;
        SimPopulate(st, lib, evs, 11);
        for (size_t i = 0; i < evs.size(); ++i) CHECK(PickOf(evs[i], "h") == PickOf(before[i], "h"));
        PopulateDrawState ds;
        ds.lib = &lib;
        ds.seed = 11;
        for (const SimEvent& e : evs) {
            EventDraw ed;
            ed.rid = e.rid;
            ed.t = e.t;
            ed.prev = &e.draws;
            ValueLayers l;
            l.role = e.role;
            const Resolution r = ResolveEvent(st, lib, l, MakeDrawHooks(ds, ed, nullptr));
            CHECK(r.sounds.size() == 1);
            if (r.sounds.size() != 1) continue;
            const ResolvedSound& s = r.sounds[0];
            DesiredSlot          d = De(0, s.node, s.variation);
            d.piece_db = PieceDbFor({}, s.gain_db);
            const std::vector<SlotStep> steps = PlanEventSlots({d}, {Ex(0, s.node, s.variation)});
            CHECK(steps.size() == 1 && steps[0].act == SlotAction::Keep);
            const double     placed_db = s.node == "h" ? -6.0 : 0.0;  // its tag's g
            const KeptVolume kv = KeptItemVolume(d.piece_db, false, 1, true, placed_db, 0, ItemGainFor(placed_db));
            if (s.node == "h") CHECK(kv.set && Near(kv.vol, ItemGainFor(-12.0), 1e-12) && Near(kv.g, -12.0));
            else CHECK(!kv.set);
        }
    }
    // Blend shuffle: no Heel variation repeats within any 3 consecutive Heel events (L and R) in time.
    {
        const SoundLibrary    lib = HeelToeBlend(true);
        std::vector<SimEvent> evs = SeparateSteps();
        SimPopulate(st, lib, evs, 5);
        CHECK(Repeats(evs, "h", "heel") == 0);
        CHECK(Repeats(evs, "t", "toe") == 0);
        // Idempotent: a second Populate keeps every pick.
        const std::vector<SimEvent> once = evs;
        SimPopulate(st, lib, evs, 5);
        for (size_t i = 0; i < evs.size(); ++i) CHECK(PickOf(evs[i], "h") == PickOf(once[i], "h"));
    }
    // The repro: each rule Populated alone (heel L, then heel R). Before 13-62i the skipped events fed no history
    // and every stored pick was kept: L and R repeat each other. Now the skipped events seed the history, and a
    // full Populate draws again the stored picks that repeat (P3).
    {
        const SoundLibrary    lib = HeelToeBlend(true);
        std::vector<SimEvent> old = SeparateSteps();
        SimPopulate(st, lib, old, 5, "heel_l", false);
        SimPopulate(st, lib, old, 5, "heel_r", false);
        CHECK(Repeats(old, "h", "heel") > 0);  // the bug seen: two similar variations in a row
        std::vector<SimEvent> now = SeparateSteps();
        SimPopulate(st, lib, now, 5, "heel_l");
        SimPopulate(st, lib, now, 5, "heel_r");
        CHECK(Repeats(now, "h", "heel") == 0);
        SimPopulate(st, lib, old, 5);  // the old record Populated in full: the repeats drawn again
        CHECK(Repeats(old, "h", "heel") == 0);
        SimPopulate(st, lib, old, 5, "heel_r");  // then one rule again (as a setting change does): still none
        CHECK(Repeats(old, "h", "heel") == 0);
        // Whatever the seed: a full Populate, or one rule after the other.
        for (uint64_t seed = 1; seed <= 40; ++seed) {
            std::vector<SimEvent> full = SeparateSteps();
            SimPopulate(st, lib, full, seed);
            std::vector<SimEvent> split = SeparateSteps();
            SimPopulate(st, lib, split, seed, "heel_r");
            SimPopulate(st, lib, split, seed, "heel_l");
            SimPopulate(st, lib, split, seed, "toe_l");
            SimPopulate(st, lib, split, seed, "toe_r");
            CHECK(Repeats(full, "h", "heel") == 0 && Repeats(full, "t", "toe") == 0);
            CHECK(Repeats(split, "h", "heel") == 0 && Repeats(split, "t", "toe") == 0);
        }
    }
    // P3: a pick by hand is always kept, the stored ones after it drawn again when they repeat.
    {
        const SoundLibrary    lib = HeelToeBlend(true);
        std::vector<SimEvent> evs = SeparateSteps();
        for (size_t i = 0; i < evs.size(); ++i) {
            SoundDraw d;
            d.node = evs[i].role == "heel" ? "h" : "t";
            d.pick = 0;
            d.hand = i == 0;
            evs[i].draws = {d};
            evs[i].populated = true;
        }
        SimPopulate(st, lib, evs, 3);
        CHECK(PickOf(evs[0], "h") == 0 && !evs[0].draws.empty() && evs[0].draws[0].hand);
        CHECK(Repeats(evs, "h", "heel") == 0);
        CHECK(Repeats(evs, "t", "toe") == 0);
    }
    // SeedHistory: node or node|part keys, a loop pass's entries to that pass, -1 picks dropped.
    {
        PopulateDrawState                      ds;
        std::map<long long, PopulateDrawState> passes;
        SoundDraw                              a, b, c, d;
        a.node = "h";
        a.pick = 2;
        b.node = "h";
        b.part = "vb";
        b.pick = 1;
        c.node = PassDrawKey("h", 3);
        c.pick = 0;
        d.node = "x";
        d.pick = -1;
        SeedHistory(ds, &passes, {a, b, c, d});
        CHECK(ds.history["h"] == std::vector<int>{2} && ds.history["h|vb"] == std::vector<int>{1});
        CHECK(!ds.history.count("x") && passes.size() == 1 && passes[3].history["h"] == std::vector<int>{0});
        PopulateDrawState only;
        SeedHistory(only, nullptr, {c});
        CHECK(only.history.empty());
        // The timeline: the next n events of a key ahead of `at`, the kept picks only.
        PopulateDrawState      tl;
        std::vector<SoundDraw> e0 = {a}, e1 = {a}, e2 = {a, c}, e3 = {a};
        e1[0].pick = 3;
        e3[0].pick = 1;
        SetDrawTimeline(tl, &passes, {&e0, &e1, nullptr, &e2, &e3}, {0, 1, 1, 0, 1});
        tl.at = 0;
        CHECK(AheadPicks(tl, "h", 2) == std::vector<int>{3});     // e1 kept (3), e2 drawn now
        CHECK(AheadPicks(tl, "h", 3) == (std::vector<int>{3, 1}));
        tl.at = 3;
        CHECK(AheadPicks(tl, "h", 2) == std::vector<int>{1});
        CHECK(AheadPicks(tl, "h", 0).empty() && AheadPicks(tl, "zz", 2).empty());
        CHECK(passes[3].timeline["h"].size() == 1 && passes[3].timeline["h"][0].second == -1);  // e2: a pass entry, drawn
    }
    // Blend roles (P1): Blend children without layer roles, steps Separate: every layer plays on a Heel event,
    // and the Populate report says so (Resolution::notes).
    {
        const SoundLibrary lib = HeelToeBlend(false);
        const Resolved     r = ResolveFor(st, lib, hb);
        CHECK(r.res.sounds.size() == 2 && SoundOf(r.res, "h") && SoundOf(r.res, "t"));
        CHECK(r.res.notes.size() == 1 && r.res.notes[0] == "Blend FS has no layer roles: every layer plays on every event");
        CHECK(ResolveFor(st, HeelToeBlend(true), hb).res.notes.empty());
    }
}

// 13-62i (D review): PopulateOne's own decisions (PlanPopulateEvent, SetPopulateTimeline): the time and
// populated-only filters, an attached event kept (its draws feed the history and are fixed ahead), and a kept
// two-piece item following its pieces' gains.
static void TestPopulatePlanHelpers()
{
    const SoundStore st = ContactStore();
    EventSounds      none;
    none.rid = "r";
    EventSounds populated = none;
    populated.placed.push_back({"{i}", "n", Ownership::Populated, 0.0, 0, "", {}});
    EventSounds attached = none;
    attached.placed.push_back({"{a}", "n", Ownership::Attached, 0.0, 0, "", {}});
    PopulateFilter f;
    CHECK(PlanPopulateEvent(f, "r", 1.0, nullptr) == EventPlan::Draw && PlanPopulateEvent(f, "r", 1.0, &attached) == EventPlan::Keep);
    CHECK(PlanPopulateEvent(f, "r", 1.0, &populated, false) == EventPlan::Skip);  // a disabled rule
    f.has_t = true;
    f.t = 2.0;
    CHECK(PlanPopulateEvent(f, "r", 2.02, nullptr) == EventPlan::Draw && PlanPopulateEvent(f, "r", 1.0, &populated) == EventPlan::Keep &&
          PlanPopulateEvent(f, "r", 1.0, &none) == EventPlan::Skip);
    PopulateFilter p;
    p.only_populated = true;
    CHECK(PlanPopulateEvent(p, "r", 1.0, &populated) == EventPlan::Draw && PlanPopulateEvent(p, "r", 1.0, &none) == EventPlan::Skip &&
          PlanPopulateEvent(p, "r", 1.0, nullptr) == EventPlan::Skip);
    // No kept event: no timeline (every event drawn in time order).
    PopulateDrawState ds;
    CHECK(!SetPopulateTimeline(ds, nullptr, {EventPlan::Draw, EventPlan::Skip}, {&none, nullptr}) && ds.timeline.empty());
    // Attached Heel events among the 20: their picks feed the history and are avoided ahead; no repeat around them.
    {
        const SoundLibrary    lib = HeelToeBlend(true);
        std::vector<SimEvent> evs = SeparateSteps();
        SimPopulate(st, lib, evs, 7);
        for (size_t i = 0; i < evs.size(); i += 6) evs[i].attached = true;  // some Heel events placed by the user
        for (int seed = 1; seed <= 20; ++seed) {
            std::vector<SimEvent> again = evs;
            for (SimEvent& e : again)
                if (!e.attached) e.draws.clear();  // drawn again from scratch
            SimPopulate(st, lib, again, static_cast<uint64_t>(seed));
            for (size_t i = 0; i < evs.size(); ++i)
                if (evs[i].attached) CHECK(PickOf(again[i], "h") == PickOf(evs[i], "h"));  // left alone
            CHECK(Repeats(again, "h", "heel") == 0);
        }
        // The time filter: only the event at 3.0 s drawn again, the others kept.
        PopulateFilter tf;
        tf.has_t = true;
        tf.t = 3.0;
        std::vector<SimEvent> one = evs;
        SimPopulate(st, lib, one, 9, std::string(), true, tf);
        for (size_t i = 0; i < one.size(); ++i)
            if (std::fabs(one[i].t - 3.0) > 0.03) CHECK(one[i].draws.size() == evs[i].draws.size() && PickOf(one[i], "h") == PickOf(evs[i], "h"));
        CHECK(Repeats(one, "h", "heel") == 0);
    }
    // A kept two-piece item (a Gap between two zones of the drawn part) follows each piece's own gain.
    {
        SoundNode n = DividedSteps("s", "");
        EditZone  g;  // Back [0, 0.08] Gap [0.08, 0.12] Back [0.12, 0.2] in variation 0
        g.a = g.slot_a = 0.08;
        g.b = g.slot_b = 0.12;
        g.xl = g.xr = 0.005;
        EditZone b2 = n.sound.frames[0].zones[0];
        b2.a = b2.slot_a = 0.12;
        b2.db = 2.0;
        n.sound.frames[0].zones[0].b = n.sound.frames[0].zones[0].slot_b = 0.08;
        n.sound.frames[0].zones.insert(n.sound.frames[0].zones.begin() + 1, {g, b2});
        const std::vector<PieceApply> pa = PieceApplyFor(n.sound, 0, "vb");
        const std::vector<double>     db = PieceDbFor(pa, -6.0);
        CHECK(pa.size() == 2 && db.size() == 2 && Near(db[0], -6.0) && Near(db[1], -4.0));
        // Placed at -6 / -4 (tags g, pi), the layer now -12: each piece -6 dB more, in place.
        const std::vector<double> now = PieceDbFor(pa, -12.0);
        for (int k = 0; k < 2; ++k) {
            const KeptVolume kv = KeptItemVolume(now, false, 2, true, db[static_cast<size_t>(k)], k, ItemGainFor(db[static_cast<size_t>(k)]));
            CHECK(kv.set && Near(kv.g, now[static_cast<size_t>(k)]) && Near(kv.vol, ItemGainFor(now[static_cast<size_t>(k)]), 1e-12));
        }
    }
    // 13-62i: a folder pile ignores its pieces: placed at the gain + its anchor zone's gain.
    {
        SoundNode n = DividedSteps("s", "");
        n.sound.frames[0].zones[0].db = -3.0;
        CHECK(Near(PileGainDb(PieceApplyFor(n.sound, 0, "vb"), -2.0), -5.0) && Near(PileGainDb({}, -2.0), -2.0));
    }
}

// 13-68 v2: a Blend's layers (NVK stacks; every Blend since Antho 2026-10-18, no flag needed): its two TakeMarkers
// Sounds (3 slices each) play the same variation number for a round, then mix.
static void TestLinkedBlend()
{
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "b";
    SoundNode b = Node("b", "", SoundNodeType::Blend);
    lib.nodes.push_back(b);  // linked_variations left off: the rule is global
    for (const char* uid : {"x", "y"}) {
        SoundNode s = Leaf(uid, "b", 3);
        s.sound.mode = DetectMode::TakeMarkers;
        lib.nodes.push_back(s);
    }
    const SoundStore  empty;
    PopulateDrawState ps;
    ps.lib = &lib;
    ps.seed = 42;
    // Antho 2026-10-18: the first round of 3 plays matching numbers, then the second layer shifts by one per
    // round (1-2 / 2-3 / 3-1, then 1-3 / 2-1 / 3-2): nine events, the nine pairs.
    std::set<int>                 seen;
    std::set<std::pair<int, int>> pairs;
    for (int e = 0; e < 9; ++e) {
        EventDraw ed;
        ed.rid = "r1";
        ed.t = 1.0 + e;
        const Resolution res = ResolveEvent(empty, lib, ValueLayers(), MakeDrawHooks(ps, ed, nullptr));
        CHECK(res.sounds.size() == 2);
        if (res.sounds.size() != 2) continue;
        const int a = res.sounds[0].variation, b2 = res.sounds[1].variation;
        CHECK(a >= 0 && a < 3);
        CHECK(b2 == (a + e / 3) % 3);
        seen.insert(a);
        pairs.insert({a, b2});
    }
    CHECK(seen.size() == 3 && pairs.size() == 9);
    // A child of fewer variations plays its modulo.
    CHECK(LinkedVariation(2, 3, 0) == 2 && LinkedVariation(2, 2, 1) == 0 && LinkedVariation(2, 1, -1) == 0);
    CHECK(LinkedVariation(-1, 3, 1) == 1 && LinkedVariation(2, 0, -1) == -1);
    CHECK(LinkedVariation(0, 3, -1, 1, 1) == 1 && LinkedVariation(2, 3, -1, 1, 1) == 0 && LinkedVariation(0, 3, -1, 2, 1) == 2);
    CHECK(LinkedVariation(0, 3, -1, 1, 2) == 2);  // a third layer shifts twice as far
}

// 13-73 review: a Blend of two Sounds with Left / Right on (6 variations, Alternate L R): every layer of a left event plays
// a Left variation (the second pairs by rank on that side, then mixes by that side's rounds), never the other foot.
static void TestSidesLinkedBlend()
{
    SoundLibrary lib;
    lib.uid = "L";
    lib.root = "b";
    lib.nodes.push_back(Node("b", "", SoundNodeType::Blend));
    for (const char* uid : {"x", "y"}) {
        SoundNode s = Leaf(uid, "b", 6);
        s.sound.mode = DetectMode::TakeMarkers;
        s.sound.sides = true;
        lib.nodes.push_back(s);
    }
    const SoundStore              empty;
    PopulateDrawState             ps;
    ps.lib = &lib;
    ps.seed = 7;
    std::set<std::pair<int, int>> pairs;
    bool                          ok = true;
    for (int e = 0; e < 18; ++e) {
        EventDraw ed;
        ed.rid = "r1";
        ed.t = 1.0 + e;
        ed.side = e % 2 == 0 ? 'L' : 'R';
        const Resolution res = ResolveEvent(empty, lib, ValueLayers(), MakeDrawHooks(ps, ed, nullptr));
        if (res.sounds.size() != 2) {
            ok = false;
            continue;
        }
        const int want = ed.side == 'L' ? 0 : 1, a = res.sounds[0].variation, b2 = res.sounds[1].variation;
        ok = ok && a % 2 == want && b2 % 2 == want;
        // the left events' n-th round: the second layer's rank on its side = the first's + round
        if (ed.side == 'L') {
            const int round = (e / 2) / 3;
            ok = ok && (b2 / 2) == ((a / 2) + round) % 3;
            pairs.insert({a, b2});
        }
    }
    CHECK(ok);
    CHECK(pairs.size() == 9);  // the 3 x 3 left pairs over three rounds
}

// 13-73 (Left / Right): an event with a side draws only that side's variations (Alternate L R: even = L, odd = R) when the
// Sound has Left / Right on, with Avoid repeat on that side's picks; no side or Left / Right off: all; a side with none: all.
static void TestSidesDraw()
{
    SoundLibrary lib = OneSound(Leaf("s", "", 8));
    lib.nodes[0].sound.sides = true;
    const SoundStore empty;
    PopulateDrawState ds;
    ds.lib = &lib;
    ds.seed = 11;
    std::vector<int> lefts;
    bool             ok_side = true, ok_avoid = true;
    for (int i = 0; i < 40; ++i) {
        EventDraw ed;
        ed.rid = "r1";
        ed.t = 0.5 * i;
        ed.side = i % 2 == 0 ? 'L' : 'R';
        const Resolution res = ResolveEvent(empty, lib, ValueLayers(), MakeDrawHooks(ds, ed, nullptr));
        if (res.sounds.size() != 1) {
            ok_side = false;
            continue;
        }
        const int v = res.sounds[0].variation;
        ok_side = ok_side && (v % 2 == (ed.side == 'L' ? 0 : 1));
        if (ed.side == 'L') {
            // avoid_repeat 2 among the 4 left variations: never one of the last two left picks
            for (size_t k = lefts.size() >= 2 ? lefts.size() - 2 : 0; k < lefts.size(); ++k) ok_avoid = ok_avoid && lefts[k] != v;
            lefts.push_back(v);
        }
    }
    CHECK(ok_side);
    CHECK(ok_avoid);
    std::set<int> seen(lefts.begin(), lefts.end());
    CHECK((seen == std::set<int>{0, 2, 4, 6}));
    // No side: any variation.
    std::set<int> any;
    for (int i = 0; i < 60; ++i) {
        PopulateDrawState d2;
        d2.lib = &lib;
        d2.seed = 100 + i;
        EventDraw ed;
        ed.rid = "r1";
        ed.t = 1.0;
        const Resolution res = ResolveEvent(empty, lib, ValueLayers(), MakeDrawHooks(d2, ed, nullptr));
        if (!res.sounds.empty()) any.insert(res.sounds[0].variation);
    }
    CHECK(any.size() == 8);
    // Every variation set Left (All Left): a right event plays any of them; Left / Right off: the side is ignored.
    CHECK(SetSidePattern(lib.nodes[0].sound, SidePattern::AllLeft));
    std::set<int> right_all, off_all;
    for (int i = 0; i < 60; ++i) {
        for (int off = 0; off < 2; ++off) {
            lib.nodes[0].sound.sides = off == 0;
            if (off) CHECK(SetSidePattern(lib.nodes[0].sound, SidePattern::AltLR));
            PopulateDrawState d2;
            d2.lib = &lib;
            d2.seed = 300 + i;
            EventDraw ed;
            ed.rid = "r1";
            ed.t = 1.0;
            ed.side = 'R';
            const Resolution res = ResolveEvent(empty, lib, ValueLayers(), MakeDrawHooks(d2, ed, nullptr));
            if (!res.sounds.empty()) (off ? off_all : right_all).insert(res.sounds[0].variation);
            if (off) SetSidePattern(lib.nodes[0].sound, SidePattern::AllLeft);
        }
    }
    CHECK(right_all.size() == 8);
    CHECK(off_all.size() == 8);
    // The event's side comes from its block: a step:R auto block, a Heel (sideless role key) still has its side.
    lib.nodes[0].sound.sides = true;
    SetSidePattern(lib.nodes[0].sound, SidePattern::AltLR);
    CHECK(EventSide(Auto(AutoKind::Step, 'R')) == 'R' && EventSide(Auto(AutoKind::Heel, 'L')) == 'L');
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        PopulateDrawState d2;
        d2.seed = seed;
        const Resolved r = ResolveFor(empty, lib, Auto(AutoKind::Step, 'R'), &d2);
        CHECK(r.res.sounds.size() == 1 && r.res.sounds[0].variation % 2 == 1);
    }
}

int main()
{
    TestSidesDraw();  // 13-73
    TestSidesLinkedBlend();  // 13-73 review
    SetNewSoundSyncMode(SyncMode::Attack);  // 13-67b: these fixtures were written for the old default (Attack)
    TestLinkedBlend();  // 13-68 v2
    TestPopulatePlanHelpers();  // 13-62i (D review)
    TestBlendMatrix();
    TestLoopPassDraws();
    TestLoopPassFillRecord();
    // ---- P1: orphan rules ----
    {
        ItemSounds it;
        RuleSounds a, b;
        a.rid = "aaaa";
        a.library = "L1";
        b.rid = "bbbb";
        b.library = "L2";
        it.rules = {a, b};
        EventSounds e1, e2, e3;
        e1.rid = "aaaa";
        e2.rid = "bbbb";
        SoundPlacement pp, pd, pa;
        pp.item_guid = "{P}";
        pp.own = Ownership::Populated;
        pd.item_guid = "{D}";
        pd.own = Ownership::Deleted;
        pa.item_guid = "{A}";
        pa.own = Ownership::Attached;
        e2.placed = {pp, pd};
        e3.rid = "bbbb";
        e3.t = 2.0;
        e3.placed = {pp, pa};
        it.events = {e1, e2, e3};
        const OrphanPrune r = PruneOrphanRules(it, {"aaaa"});
        CHECK(r.rules.size() == 1 && r.rules[0].library == "L2");
        CHECK(it.rules.size() == 1 && it.rules[0].rid == "aaaa");
        CHECK(r.remove_guids.size() == 2);
        CHECK(it.events.size() == 2);  // e2 dropped, e3 kept for its attached placement
        CHECK(it.events[1].placed.size() == 1 && it.events[1].placed[0].own == Ownership::Attached);
        CHECK(RuleGoneText("Steps") == "Rule gone: library \"Steps\" unassigned");
    }

    // ---- Layers and values ----
    {
        ItemSounds it;
        CHECK(SetValueIn(it.item_values, "G", "wood"));
        CHECK(!SetValueIn(it.item_values, "G", "wood"));
        CHECK(SetValueIn(it.item_values, "G", "stone"));
        CHECK(it.item_values.size() == 1 && it.item_values[0].value_uid == "stone");
        RuleSounds r;
        r.rid = "aaaa";
        SetValueIn(r.values, "G", "grass");
        it.rules.push_back(r);
        EventSounds e;
        e.rid = "aaaa";
        e.t = 1.0;
        SetValueIn(e.overrides, "G", "metal");
        it.events.push_back(e);
        ValueLayers l = LayersFor(it, "aaaa", 1.01);
        CHECK(l.item.size() == 1 && l.rule.size() == 1 && l.event.size() == 1 && l.event[0].value_uid == "metal");
        l = LayersFor(it, "aaaa", 1.5);
        CHECK(l.event.empty());
        CHECK(SetValueIn(it.item_values, "G", ""));
        CHECK(it.item_values.empty());
        CHECK(!SetValueIn(it.item_values, "G", ""));
    }

    // A library: Switch(Material) -> wood: Random(s1, s2), stone: s3; a nested Switch on Size.
    SoundStore store;
    SwitchGroup mat, size;
    mat.uid = "MAT";
    mat.name = "Material";
    mat.values = {{"wood", "Wood", {}}, {"stone", "Stone", {}}};
    size.uid = "SIZE";
    size.name = "Size";
    size.values = {{"big", "Big", {}}};
    store.groups = {mat, size};
    SoundLibrary lib;
    lib.uid = "L";
    lib.name = "Steps";
    lib.root = "sw";
    SoundNode sw = Node("sw", "", SoundNodeType::Switch);
    sw.group_uid = "MAT";
    sw.cases = {{"wood", "rnd", {}}, {"stone", "sw2", {}}};
    sw.default_value = "wood";
    SoundNode rnd = Node("rnd", "sw", SoundNodeType::Random);
    rnd.avoid_repeat = 1;
    SoundNode sw2 = Node("sw2", "sw", SoundNodeType::Switch);
    sw2.group_uid = "SIZE";
    sw2.cases = {{"big", "s3", {}}};
    lib.nodes = {sw, rnd, Leaf("s1", "rnd", 0), Leaf("s2", "rnd", 0), sw2, Leaf("s3", "sw2", 3)};
    store.libraries.push_back(lib);

    // ---- Groups used ----
    {
        const std::vector<GroupUse> g = GroupsUsed(store, lib);
        CHECK(g.size() == 2 && g[0].group_uid == "MAT" && g[1].group_uid == "SIZE");
        CHECK(g[0].first_switch && g[0].first_switch->uid == "sw");
    }

    // ---- P10: draws, avoid-repeat history, stable re-draw ----
    {
        PopulateDrawState st;
        st.lib = &lib;
        st.seed = 1234;
        std::vector<std::string> nodes;
        std::vector<std::vector<SoundDraw>> stored;
        for (int i = 0; i < 6; ++i) {
            EventDraw ev;
            ev.rid = "aaaa";
            ev.t = i * 0.5;
            const Resolution res = ResolveEvent(store, lib, ValueLayers{}, MakeDrawHooks(st, ev, nullptr));
            CHECK(!res.blocked && res.sounds.size() == 1);
            if (!res.sounds.empty()) nodes.push_back(res.sounds[0].node);
            CHECK(ev.out.size() == 1 && ev.out[0].node == "rnd");
            stored.push_back(ev.out);
        }
        for (size_t i = 1; i < nodes.size(); ++i) CHECK(nodes[i] != nodes[i - 1]);  // avoid 1 of 2 children
        // Other seed, stored draws: the same picks.
        PopulateDrawState st2;
        st2.lib = &lib;
        st2.seed = 999;
        for (int i = 0; i < 6; ++i) {
            EventDraw ev;
            ev.rid = "aaaa";
            ev.t = i * 0.5;
            ev.prev = &stored[static_cast<size_t>(i)];
            const Resolution res = ResolveEvent(store, lib, ValueLayers{}, MakeDrawHooks(st2, ev, nullptr));
            CHECK(!res.sounds.empty() && res.sounds[0].node == nodes[static_cast<size_t>(i)]);
        }
        // Stone: the Sound with frames gets a variation draw; all off -> no_active.
        ValueLayers l;
        SetValueIn(l.event, "MAT", "stone");
        EventDraw ev;
        ev.rid = "aaaa";
        Resolution res = ResolveEvent(store, lib, l, MakeDrawHooks(st, ev, nullptr));
        CHECK(res.sounds.size() == 1 && res.sounds[0].node == "s3" && res.sounds[0].variation >= 0);
        SoundLibrary off = lib;
        for (SoundFrame& f : FindNode(off, "s3")->sound.frames) f.on = false;
        PopulateDrawState st3;
        st3.lib = &off;
        st3.seed = 5;
        EventDraw ev3;
        ev3.rid = "aaaa";
        res = ResolveEvent(store, off, l, MakeDrawHooks(st3, ev3, nullptr));
        CHECK(res.sounds.size() == 1 && res.sounds[0].variation == -1 && ev3.no_active.count("s3") == 1);
        CHECK(NodeSalt("aaaa", 1.0, "x") != NodeSalt("aaaa", 1.0, "y"));
    }

    // ---- P10: a stored draw wins over any fresh draw, and a hand pick stays hand ----
    {
        SoundLibrary r3;
        r3.uid = "R3";
        r3.root = "r";
        SoundNode rr = Node("r", "", SoundNodeType::Random);
        r3.nodes = {rr, Leaf("k0", "r", 0), Leaf("k1", "r", 0), Leaf("k2", "r", 0)};
        SoundStore st3;
        st3.libraries.push_back(r3);
        for (int pick = 0; pick < 3; ++pick) {  // at least two of the three differ from a fresh draw
            PopulateDrawState ds;
            ds.lib = &r3;
            ds.seed = 77;
            std::vector<SoundDraw> prev(1);
            prev[0].node = "r";
            prev[0].pick = pick;
            prev[0].hand = pick == 1;
            EventDraw ev;
            ev.rid = "aaaa";
            ev.t = 1.0;
            ev.prev = &prev;
            const Resolution res = ResolveEvent(st3, r3, ValueLayers{}, MakeDrawHooks(ds, ev, nullptr));
            CHECK(ev.out.size() == 1 && ev.out[0].pick == pick && ev.out[0].hand == (pick == 1));
            CHECK(res.sounds.size() == 1 && res.sounds[0].node == "k" + std::to_string(pick));
        }
    }

    // ---- Blend offsets: BlendOffsetMs = the resolver's offset of each part ----
    {
        SoundLibrary bl;
        bl.uid = "B";
        bl.root = "b";
        SoundNode b = Node("b", "", SoundNodeType::Blend);
        SoundNode bn = Node("bn", "b", SoundNodeType::Blend);
        bn.offset_ms = 10.0;
        SoundNode x = Leaf("x", "bn", 0);
        x.offset_ms = 5.0;
        SoundNode r = Node("r", "b", SoundNodeType::Random);
        r.offset_ms = 20.0;
        SoundNode y = Leaf("y", "r", 0);
        y.offset_ms = 7.0;  // under a Random: not a Blend offset
        bl.nodes = {b, bn, x, r, y};
        SoundStore sb;
        sb.libraries.push_back(bl);
        PopulateDrawState ds;
        ds.lib = &bl;
        ds.seed = 3;
        EventDraw ev;
        ev.rid = "aaaa";
        const Resolution res = ResolveEvent(sb, bl, ValueLayers{}, MakeDrawHooks(ds, ev, nullptr));
        CHECK(res.sounds.size() == 2);
        for (const ResolvedSound& s : res.sounds) CHECK(std::fabs(BlendOffsetMs(bl, s.node) - s.offset_ms) < 1e-9);
        CHECK(std::fabs(BlendOffsetMs(bl, "x") - 15.0) < 1e-9 && std::fabs(BlendOffsetMs(bl, "y") - 20.0) < 1e-9);
    }
    // 13-31: on an event with a role, a role-using Blend's layer offset is dropped (unless kept), as the resolver.
    {
        SoundLibrary bl;
        bl.uid = "B";
        bl.root = "b";
        SoundNode b = Node("b", "", SoundNodeType::Blend);
        SoundNode h = Leaf("h", "b", 0);
        h.layer_roles = {"heel"};
        SoundNode t = Leaf("t", "b", 0);
        t.offset_ms = 90.0;
        t.layer_roles = {"toe"};
        bl.nodes = {b, h, t};
        SoundStore sb;
        sb.libraries.push_back(bl);
        PopulateDrawState ds;
        ds.lib = &bl;
        EventDraw ev;
        ev.rid = "aaaa";
        for (const char* role : {"", "toe"}) {
            ValueLayers l;
            l.role = role;
            const Resolution res = ResolveEvent(sb, bl, l, MakeDrawHooks(ds, ev, nullptr));
            CHECK(!res.sounds.empty());
            for (const ResolvedSound& s : res.sounds) CHECK(std::fabs(BlendOffsetMs(bl, s.node, role) - s.offset_ms) < 1e-9);
        }
        CHECK(std::fabs(BlendOffsetMs(bl, "t", "toe")) < 1e-9 && std::fabs(BlendOffsetMs(bl, "t") - 90.0) < 1e-9);
        bl.nodes[2].layer_keep_offset = true;
        CHECK(std::fabs(BlendOffsetMs(bl, "t", "toe") - 90.0) < 1e-9);
    }

    // ---- P9: event slots ----
    {
        // Re-Populate, same node and variation: kept.
        std::vector<SlotStep> s = PlanEventSlots({De(0, "s1", -1)}, {Ex(0, "s1", -1)});
        CHECK(s.size() == 1 && s[0].act == SlotAction::Keep && s[0].existing.size() == 1);
        // Same node, other variation.
        s = PlanEventSlots({De(0, "s3", 2)}, {Ex(0, "s3", 1)});
        CHECK(s.size() == 1 && s[0].act == SlotAction::SetVariation);
        // Other node: replaced in place.
        s = PlanEventSlots({De(0, "s3", 0)}, {Ex(0, "s1", -1)});
        CHECK(s.size() == 1 && s[0].act == SlotAction::ReplaceNode && s[0].existing[0] == 0);
        // Surplus removed, absent placed.
        s = PlanEventSlots({}, {Ex(0, "s1", -1)});
        CHECK(s.size() == 1 && s[0].act == SlotAction::Remove);
        s = PlanEventSlots({De(0, "s1", -1), De(1, "s1", -1)}, {Ex(0, "s1", -1)});
        CHECK(s.size() == 2 && s[0].act == SlotAction::Keep && s[1].act == SlotAction::Place && s[1].desired == 1);
        // Deleted by hand: never placed again.
        s = PlanEventSlots({De(0, "s1", -1)}, {Ex(-1, "s1", -1, Ownership::Deleted)});
        CHECK(s.empty());
        // Deleted on one pass: the copy on the other pass is kept, the deleted one not re-placed.
        s = PlanEventSlots({De(0, "s1", -1), De(1, "s1", -1)}, {Ex(0, "s1", -1), Ex(-1, "s1", -1, Ownership::Deleted)});
        CHECK(s.size() == 1 && s[0].act == SlotAction::Keep && s[0].desired == 0);
        // One item of a pile deleted: the rest of the pile is kept.
        s = PlanEventSlots({De(0, "s1", -1)}, {Ex(0, "s1", -1), Ex(-1, "s1", -1, Ownership::Deleted)});
        CHECK(s.size() == 1 && s[0].act == SlotAction::Keep && s[0].existing.size() == 1);
        // A deleted node never takes another node's slot: that slot is removed.
        s = PlanEventSlots({De(0, "s1", -1)}, {Ex(0, "s2", -1), Ex(-1, "s1", -1, Ownership::Deleted)});
        CHECK(s.size() == 1 && s[0].act == SlotAction::Remove);
        // Attached ignored (not a slot, not removed).
        s = PlanEventSlots({De(0, "s1", -1)}, {Ex(0, "s2", -1, Ownership::Attached)});
        CHECK(s.size() == 1 && s[0].act == SlotAction::Place);
        // A pile: two items of one node in one pass = one slot.
        s = PlanEventSlots({De(0, "s1", -1)}, {Ex(0, "s1", -1), Ex(0, "s1", -1)});
        CHECK(s.size() == 1 && s[0].act == SlotAction::Keep && s[0].existing.size() == 2);
        // Blend of two: one part same node kept, other node replaced.
        s = PlanEventSlots({De(0, "a", -1), De(0, "b", -1)}, {Ex(0, "c", -1), Ex(0, "a", -1)});
        CHECK(s.size() == 2 && s[0].act == SlotAction::Keep && s[0].existing[0] == 1 && s[1].act == SlotAction::ReplaceNode &&
              s[1].existing[0] == 0);
    }

    // ---- Passes, nudge, user placements ----
    {
        CHECK(NearestPass({1.0, 3.0, 5.0}, 3.4) == 1);
        CHECK(NearestPass({}, 3.4) == -1);
        CHECK(std::fabs(NudgeOf(9.862, 10.0, 0.25, 12.0) - 0.1) < 1e-9);
        // A folder pile (NVK): placed from the folder's snap, its first member offset from the folder;
        // the nudge is read back from the pile's anchor, not from the member's own position and snap.
        {
            const double folder_src = 3.0, member_src = 3.25, folder_snap = 0.4, member_snap = 0.05;
            const double anchor = PlacementPosition(10.0, folder_snap, 12.0, 0.2);
            const double member = PileMemberPosition(anchor, folder_src, member_src);
            CHECK(std::fabs(NudgeOf(PileAnchorOf(member, folder_src, member_src), 10.0, folder_snap, 12.0) - 0.2) < 1e-9);
            CHECK(std::fabs(NudgeOf(member, 10.0, member_snap, 12.0) - 0.2) > 0.1);  // the old reading: wrong
            CHECK(std::fabs(PileAnchorOf(PileMemberPosition(7.5, 2.0, 1.0), 2.0, 1.0) - 7.5) < 1e-9);
        }
        // 13-8: the anchor from any surviving member's tag (ao / sy), clamp case, legacy tag, pass by anchor.
        {
            // Folder at src 3.0 snap 0.4, members at src 2.5 / 3.0 / 3.6; event at 0.7 so member 0 clamps.
            const double folder_src = 3.0, snap = 0.4, pt = 0.7;
            const double anchor = PlacementPosition(pt, snap, 0.0, 0.05);
            const double srcs[3] = {2.5, 3.0, 3.6};
            for (double src : srcs) {
                PlacedTag tag;
                tag.owner = "{A}";
                tag.has_anchor = true;
                const double pos = std::max(0.0, PileMemberPosition(anchor, folder_src, src));
                tag.ao = pos - anchor;  // as PlaceImpl stores it (after the clamp)
                tag.sy = snap;
                PlacedTag back;
                CHECK(ParsePlacedTag(SerializePlacedTag(tag), &back));
                CHECK(back.has_anchor && std::fabs(back.ao - tag.ao) < 1e-9 && std::fabs(back.sy - snap) < 1e-9);
                double a = 0.0, sy = 0.0;
                PlacedAnchorOf(back, pos, 0.0, &a, &sy);
                CHECK(std::fabs(NudgeOf(a, pt, sy, 0.0) - 0.05) < 1e-9);  // any survivor, clamped too
            }
            CHECK(std::max(0.0, PileMemberPosition(anchor, folder_src, 2.5)) == 0.0);  // member 0 did clamp
            // On close loop passes every member is assigned the anchor's pass.
            PlacedTag far;
            far.owner = "{A}";
            far.has_anchor = true;
            far.ao = 0.7;
            far.sy = 0.0;
            double a = 0.0, sy = 0.0;
            PlacedAnchorOf(far, 1.7, 0.0, &a, &sy);
            CHECK(NearestPassOfItem({0.0, 0.8, 1.6}, a, sy, 0.0) == 1);
            CHECK(NearestPassOfItem({0.0, 0.8, 1.6}, 1.7, 0.0, 0.0) == 2);  // the member's own reading
            // A legacy tag (no ao / sy): the item's own position and snap, and it round-trips without them.
            PlacedTag leg;
            leg.owner = "{A}";
            PlacedTag lb;
            CHECK(ParsePlacedTag(SerializePlacedTag(leg), &lb) && !lb.has_anchor);
            PlacedAnchorOf(lb, 4.0, 0.25, &a, &sy);
            CHECK(std::fabs(a - 4.0) < 1e-9 && std::fabs(sy - 0.25) < 1e-9);
        }
        // A long sync offset on close passes: the pass is where the sync point lands, not the item start.
        CHECK(NearestPassOfItem({0.0, 0.8, 1.6}, 0.3, 0.5, 0.0) == 1);
        CHECK(NearestPassOfItem({0.0, 0.8, 1.6}, 1.1 + 0.012, 0.5, 12.0) == 2);
        // P9: a populated item gone from the project becomes Deleted; attached ones are untouched.
        {
            EventSounds ev;
            SoundPlacement a, b, c;
            a.item_guid = "{LIVE}";
            b.item_guid = "{GONE}";
            c.item_guid = "{GONE}";
            c.own = Ownership::Attached;
            ev.placed = {a, b, c};
            CHECK(MarkDeletedPlacements(ev, [](const std::string& g) { return g == "{LIVE}"; }) == 1);
            CHECK(ev.placed[0].own == Ownership::Populated && ev.placed[1].own == Ownership::Deleted &&
                  ev.placed[2].own == Ownership::Attached);
            CHECK(EventSkippedByPopulate(&ev) && !EventSkippedByPopulate(nullptr));
        }
        EventSounds e;
        CHECK(!EventHasUserPlacement(e) && !EventIsPopulated(e));
        SoundPlacement p;
        p.own = Ownership::Deleted;
        e.placed.push_back(p);
        CHECK(!EventIsPopulated(e));
        p.own = Ownership::Spread;
        e.placed.push_back(p);
        CHECK(EventHasUserPlacement(e));
        p.own = Ownership::Populated;
        e.placed.push_back(p);
        CHECK(EventIsPopulated(e));
    }

    // ---- Count and summary ----
    {
        CHECK(CountEvents({0, 0, 1, 2, 5}, {1, 0, 1}) == 3);
        CHECK(CountEvents({-1, 0}, {1}) == 1);
        SoundReport r;
        CHECK(ReportSummary(r, 24) == "Last populate: 24 events");
        ReportLine l;
        l.kind = ReportKind::Missing;
        r.lines = {l, l};
        l.kind = ReportKind::Replaced;
        r.lines.push_back(l);
        l.kind = ReportKind::Detached;
        r.lines.push_back(l);
        CHECK(ReportSummary(r, 24) ==
              "Last populate: 24 events \xC2\xB7 2 missing \xC2\xB7 1 replaced \xC2\xB7 1 rule gone");
        SoundReport one;
        one.lines.push_back(l);
        l.kind = ReportKind::Replaced;
        one.lines.push_back(l);
        CHECK(ReportSummary(one, -1) == "Last change: 1 replaced \xC2\xB7 1 rule gone");
        CHECK(ReportSummary(SoundReport{}, 1) == "Last populate: 1 event");
        CHECK(ReportSummary(SoundReport{}, -1) == "Last change: nothing to report");
        SoundReport bl;
        l.kind = ReportKind::Blocked;
        bl.lines = {l, l};
        CHECK(ReportSummary(bl, 3) == "Last populate: 3 events \xC2\xB7 2 blocked");
        SoundReport ns;  // 13-21: an event left with nothing by a library gap
        l.kind = ReportKind::NoSound;
        ns.lines = {l, l};
        l.kind = ReportKind::Missing;
        ns.lines.push_back(l);
        CHECK(ReportSummary(ns, 5) == "Last populate: 5 events \xC2\xB7 1 missing \xC2\xB7 2 no sound");
    }

    TestParts();

    // 13-56 review: a looped item's new passes filled: Place steps only.
    {
        // Pass 0 has n1 at variation 2 (the draw now says 3: a Populate would SetVariation it), pass 1
        // has nothing, pass 2 has another node (a Populate would ReplaceNode it), and an extra n1 on
        // pass 3 the draw does not want (a Populate would remove it).
        const std::vector<DesiredSlot>  d = {De(0, "n1", 3), De(1, "n1", 3), De(2, "n1", 3)};
        const std::vector<ExistingSlot> x = {Ex(0, "n1", 2), Ex(2, "n9", 0), Ex(3, "n1", 3)};
        bool place1 = false, other = false;
        for (const SlotStep& st : PlanEventSlots(d, x)) {
            if (st.act == SlotAction::Place && d[static_cast<size_t>(st.desired)].pass == 1) place1 = true;
            if (st.act != SlotAction::Place && st.act != SlotAction::Keep) other = true;
        }
        CHECK(place1 && other);
        const std::vector<SlotStep> f = PlanPassFill(d, x);
        CHECK(f.size() == 1 && f[0].act == SlotAction::Place && d[static_cast<size_t>(f[0].desired)].pass == 1);
        // Every pass filled: nothing to do. A node deleted by hand: never placed again.
        CHECK(PlanPassFill({De(0, "n1", 2)}, {Ex(0, "n1", 2)}).empty());
        CHECK(PlanPassFill({De(0, "n1", 2), De(1, "n1", 2)}, {Ex(0, "n1", 2), Ex(-1, "n1", 2, Ownership::Deleted)}).empty());
    }

    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("sound_populate_plan: all checks passed\n");
    return 0;
}
