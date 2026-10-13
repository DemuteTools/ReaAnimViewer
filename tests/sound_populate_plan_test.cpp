// SPDX-License-Identifier: MIT
//
// Story 13-8 -- src/sound_populate_plan.h (pure): orphan rules, layers, the draw hooks, event
// slots on a re-Populate, the nearest pass, the nudge, the footer count and the report line.

#include "sound_populate_plan.h"
#include "sound_place_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "auto_detect.h"
#include "bone_roles.h"
#include "sound_assign.h"
#include "sound_draw.h"
#include "sound_entries.h"
#include "sound_record.h"
#include "sound_switch.h"

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
    for (int i = 0; i < 3; ++i) {
        SoundFrame f;
        f.start = i;
        f.length = 0.4;
        f.cuts = {i + 0.2};
        f.syncs = {i + 0.018, i + 0.218};
        n.sound.frames.push_back(f);
    }
    return n;
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
    // Row 3: a combined step (FS) gets the whole variation, its first part's sync.
    {
        Resolved r = ResolveFor(st, lib, Auto(AutoKind::Step));
        CHECK(r.res.sounds.size() == 1 && r.res.sounds[0].part_value.empty() && r.ed.out[0].part.empty());
        const int                     v = r.res.sounds[0].variation;
        const std::vector<PieceApply> pa = PieceApplyFor(sd, v, "");
        CHECK(pa.size() == 1 && Near(pa[0].slice.length, 0.4) && Near(pa[0].slice.snap_offset, 0.018));
        CHECK(Near(pa[0].fades.in, kDefaultFadeIn) && Near(pa[0].fades.out, kDefaultFadeOut));  // no cut side
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
    // Row 7: every Front part excluded: nothing placed for toe events, NoSound text; heels still play.
    {
        SoundNode n = DividedSteps("s", "");
        for (int i = 0; i < 3; ++i) AddZone(n.sound, i + 0.2, 0.2, "");
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
        AddZone(m.sound, 0.2, 0.2, "");
        AddZone(m.sound, 2.2, 0.2, "");
        const SoundLibrary one = OneSound(m);
        for (int k = 0; k < 4; ++k) CHECK(ResolveFor(st, one, Auto(AutoKind::Toe)).res.sounds[0].variation == 1);
        // A variation turned off is not drawn for a part either; all off = no active variation.
        SoundNode off = DividedSteps("s", "");
        for (SoundFrame& f : off.sound.frames) f.on = false;
        const SoundLibrary none = OneSound(off);
        Resolved           z = ResolveFor(st, none, Auto(AutoKind::Toe));
        CHECK(z.ed.no_active.count("s") == 1 && z.ed.no_part.empty());
    }
    // Row 8: an excluded zone inside Back: two pieces, the gap empty, both placed from Back's sync.
    {
        SoundNode n = DividedSteps("s", "");
        AddZone(n.sound, 0.08, 0.04, "");  // Back of variation 0 = [0, 0.08) + [0.12, 0.2)
        const std::vector<PieceApply> pa = PieceApplyFor(n.sound, 0, "vb");
        CHECK(pa.size() == 2);
        CHECK(Near(pa[0].slice.start_offs, 0.0) && Near(pa[0].slice.length, 0.08) && pa[0].anchor);
        CHECK(Near(pa[1].slice.start_offs, 0.12) && Near(pa[1].slice.length, 0.08) && !pa[1].anchor);
        CHECK(Near(pa[0].slice.snap_offset, 0.018) && Near(pa[1].slice.snap_offset, 0.0));
        CHECK(Near(pa[1].from_anchor, 0.12) && Near(pa[0].from_anchor, 0.0));
        CHECK(Near(pa[0].fades.out, kDefaultCutFade) && Near(pa[1].fades.in, kDefaultCutFade) &&
              Near(pa[1].fades.out, kDefaultCutFade));  // zone edge, zone edge, cut
        // On the timeline: the anchor at event - snap, the second piece 0.12 s later (the gap kept).
        const double anchor = PlacementPosition(10.0, pa[0].slice.snap_offset, 0.0, 0.0);
        CHECK(Near(anchor, 9.982) && Near(PiecePosition(anchor, pa[1].from_anchor, 1.0), 10.102));
        CHECK(Near(PiecePosition(anchor, pa[1].from_anchor, 2.0), 10.042));  // playrate 2: item seconds
        // The sync in the second piece: the first piece sits before the anchor.
        n.sound.frames[0].syncs = {0.13, 0.218};
        const std::vector<PieceApply> pb = PieceApplyFor(n.sound, 0, "vb");
        CHECK(pb.size() == 2 && !pb[0].anchor && pb[1].anchor && Near(pb[0].from_anchor, -0.12) &&
              Near(pb[1].slice.snap_offset, 0.01));
        // The whole variation around it: two pieces too, the cut inside the second one keeps no fade.
        const std::vector<PieceApply> pw = PieceApplyFor(n.sound, 0, "");
        CHECK(pw.size() == 2 && Near(pw[1].slice.length, 0.28) && Near(pw[1].fades.out, kDefaultFadeOut));
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
    // Row 11: Takes mode: no part (the take is placed whole, its item's snap), every on take drawn.
    {
        SoundNode n = DividedSteps("s", "");
        n.sound.mode = DetectMode::Takes;
        AddZone(n.sound, 0.0, 3.0, "");  // a zone set before switching to Takes is ignored
        const SoundLibrary tk = OneSound(n);
        Resolved           r = ResolveFor(st, tk, Auto(AutoKind::Heel));
        CHECK(r.res.sounds[0].part_value.empty() && r.res.sounds[0].variation >= 0 && r.ed.no_part.empty());
        CHECK(PieceApplyFor(n.sound, 0, "").empty());
    }
    // A9: a Blend layer's own part wins over the event's role; "*" = the whole variation; a value that
    // is not one of its divide values reads as the whole.
    {
        SoundLibrary bl;
        bl.uid = "L";
        bl.root = "b";
        bl.nodes.push_back(Node("b", "", SoundNodeType::Blend));
        SoundNode a = DividedSteps("a", "b");
        a.layer_part = "vf";
        SoundNode c = DividedSteps("c", "b");
        c.layer_part = kLayerPartWhole;
        SoundNode e = DividedSteps("e", "b");
        e.layer_part = "nope";
        SoundNode f = DividedSteps("f", "b");  // by event
        bl.nodes.insert(bl.nodes.end(), {a, c, e, f});
        Resolved r = ResolveFor(st, bl, Auto(AutoKind::Heel));
        CHECK(r.res.sounds.size() == 4);
        CHECK(r.res.sounds[0].part_value == "vf" && r.res.sounds[1].part_value.empty() &&
              r.res.sounds[2].part_value.empty() && r.res.sounds[3].part_value == "vb");
        // Outside a Blend, a stale layer part is ignored: the event's role decides.
        SoundNode solo = DividedSteps("s", "");
        solo.layer_part = "vf";
        CHECK(ResolveFor(st, OneSound(solo), Auto(AutoKind::Heel)).res.sounds[0].part_value == "vb");
    }
    // A combined Sound on a heel event: the whole variation (A3); a value outside divide_values too.
    {
        SoundNode n = DividedSteps("s", "");
        n.sound.divide_group.clear();
        n.sound.divide_values.clear();
        for (SoundFrame& f : n.sound.frames) {
            f.cuts.clear();
            f.syncs = {f.start + 0.018};
        }
        CHECK(ResolveFor(st, OneSound(n), Auto(AutoKind::Heel)).res.sounds[0].part_value.empty());
        SoundNode m = DividedSteps("s", "");
        m.sound.role_map = {{"heel", "gone"}};
        CHECK(ResolveFor(st, OneSound(m), Auto(AutoKind::Heel)).res.sounds[0].part_value.empty());
    }
}

int main()
{
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

    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("sound_populate_plan: all checks passed\n");
    return 0;
}
