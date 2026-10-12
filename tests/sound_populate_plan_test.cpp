// SPDX-License-Identifier: MIT
//
// Story 13-8 -- src/sound_populate_plan.h (pure): orphan rules, layers, the draw hooks, event
// slots on a re-Populate, the nearest pass, the nudge, the footer count and the report line.

#include "sound_populate_plan.h"
#include "sound_place_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
    }

    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("sound_populate_plan: all checks passed\n");
    return 0;
}
