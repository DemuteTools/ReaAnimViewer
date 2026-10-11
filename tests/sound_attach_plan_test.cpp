// SPDX-License-Identifier: MIT
//
// Host test of src/sound_attach_plan.h (13-10): eligibility, piles, nearest event, Spread
// offsets and takes, record edits, Promote positions and every library insertion branch (D8).

#include "sound_attach_plan.h"

#include <cmath>
#include <cstdio>
#include <string>

#include "sound_record.h"

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

static AttachItem Item(const char* g, int track, double pos, double len, double snap = 0.0, int takes = 1)
{
    AttachItem a;
    a.guid = g;
    a.track = track;
    a.pos = pos;
    a.len = len;
    a.snap = snap;
    a.takes = takes;
    return a;
}

static AttachEvent Ev(int block, double clip_t, double project_t)
{
    AttachEvent e;
    e.block = block;
    e.clip_t = clip_t;
    e.project_t = project_t;
    return e;
}

static SoundNode Node(const char* uid, SoundNodeType type, const char* parent = "")
{
    SoundNode n;
    n.uid = uid;
    n.type = type;
    n.parent = parent;
    return n;
}

static const SoundNode* Find(const SoundLibrary& lib, const char* uid) { return FindNode(lib, uid); }

int main()
{
    // Eligibility.
    CHECK(AttachSkipReason(true, false, false, Ownership::Populated) == "animation item");
    CHECK(AttachSkipReason(false, true, false, Ownership::Populated) == "folder item");
    CHECK(AttachSkipReason(false, false, true, Ownership::Populated) == "placed by RAV");
    CHECK(AttachSkipReason(false, false, true, Ownership::Spread) == "placed by RAV");
    CHECK(AttachSkipReason(false, false, true, Ownership::Attached).empty());
    CHECK(AttachSkipReason(false, false, false, Ownership::Populated).empty());

    // Matrix "Nearest": top on track 2, anchor 4.02 s; events at 3.5 and 4.0.
    {
        std::vector<AttachItem> items = {Item("b", 3, 3.9, 1.0), Item("a", 2, 4.0, 1.0, 0.02)};
        const int top = TopmostOf(items, {0, 1});
        CHECK(top == 1);
        const double anchor = PileAnchor(items[static_cast<size_t>(top)]);
        CHECK(Near(anchor, 4.02));
        std::vector<AttachEvent> ev = {Ev(0, 1.5, 3.5), Ev(0, 2.0, 4.0)};
        const int e = NearestEvent(ev, anchor);
        CHECK(e == 1);
        CHECK(Near(anchor - ev[1].project_t, 0.02));
        // Tie: the earlier event.
        CHECK(NearestEvent({Ev(0, 0, 3.0), Ev(0, 0, 5.0)}, 4.0) == 0);
        CHECK(NearestEvent({Ev(0, 0, 5.0), Ev(0, 0, 3.0)}, 4.0) == 1);
        // Block filter; none of the rule.
        CHECK(NearestEvent({Ev(0, 0, 4.0), Ev(1, 0, 9.0)}, 4.0, 1) == 1);
        CHECK(NearestEvent({Ev(0, 0, 4.0)}, 4.0, 2) == -1);
        CHECK(NearestEvent({}, 4.0) == -1);
        // Same track: the earlier position is the top.
        CHECK(TopmostOf({Item("x", 1, 5.0, 1.0), Item("y", 1, 2.0, 1.0)}, {0, 1}) == 1);
        CHECK(TopmostOf(items, {}) == -1);
    }

    // Matrix "Groups": A[1,2] B[1.5,3] C[6,7].
    {
        std::vector<AttachItem> items = {Item("C", 1, 6.0, 1.0), Item("A", 1, 1.0, 1.0), Item("B", 2, 1.5, 1.5)};
        auto g = OverlapGroups(items);
        CHECK(g.size() == 2);
        CHECK(g.size() == 2 && g[0].size() == 2 && g[0][0] == 1 && g[0][1] == 2);
        CHECK(g.size() == 2 && g[1].size() == 1 && g[1][0] == 0);
        // Touching spans join.
        CHECK(OverlapGroups({Item("a", 1, 0.0, 1.0), Item("b", 1, 1.0, 1.0)}).size() == 1);
        CHECK(OverlapGroups({}).empty());
    }

    // Spread: pile on event 1 of 4 skipped; member offsets; takes avoid repeats.
    {
        CHECK(PileOnEvent(1.02, 1.0));
        CHECK(!PileOnEvent(1.05, 1.0));
        // Engine: position = event - snap + offset -> event + nudge + (pos - anchor).
        const double anchor = 2.0 + 0.1;  // top pos 2, snap 0.1
        const double off = SpreadMemberOffset(anchor, 2.5, 0.2, 0.05);
        CHECK(Near(10.0 - 0.2 + off, 10.0 + 0.05 + (2.5 - anchor)));
        // The top member's sync point (pos + snap) lands on the event.
        CHECK(Near(10.0 - 0.1 + SpreadMemberOffset(anchor, 2.0, 0.1, 0.0) + 0.1, 10.0));
        CHECK(SpreadTakePick(1, {}, 1, 2) == -1);
        CHECK(SpreadTakePick(0, {}, 1, 2) == -1);
        std::vector<int> history;
        for (int i = 0; i < 3; ++i) {
            const int p = SpreadTakePick(3, history, 42, 100 + static_cast<uint64_t>(i));
            CHECK(p >= 0 && p < 3);
            for (size_t k = history.size() >= 2 ? history.size() - 2 : 0; k < history.size(); ++k) CHECK(p != history[k]);
            history.push_back(p);
        }
        CHECK(SpreadTakePick(3, {}, 42, 7) == SpreadTakePick(3, {}, 42, 7));
    }

    // Record edits: re-attach (D3) leaves the old event, joins the new one.
    {
        ItemSounds rec;
        SoundPlacement p;
        p.item_guid = "{A}";
        p.own = Ownership::Attached;
        p.offset_s = 0.02;
        AddToRecord(rec, "r1", 2.0, p);
        CHECK(rec.events.size() == 1 && rec.events[0].placed.size() == 1);
        p.item_guid = "{B}";
        AddToRecord(rec, "r1", 2.01, p);  // same event (+-30 ms)
        CHECK(rec.events.size() == 1 && rec.events[0].placed.size() == 2);
        CHECK(DetachFromRecord(rec, "{A}"));
        CHECK(rec.events.size() == 1 && rec.events[0].placed.size() == 1);
        CHECK(DetachFromRecord(rec, "{B}"));
        CHECK(rec.events.empty());  // nothing left: dropped
        CHECK(!DetachFromRecord(rec, "{B}"));
        // An event that keeps a draw stays.
        AddToRecord(rec, "r1", 3.0, p);
        SoundDraw d;
        d.node = "n";
        d.pick = 1;
        rec.events[0].draws.push_back(d);
        CHECK(DetachFromRecord(rec, "{B}"));
        CHECK(rec.events.size() == 1);
    }

    // Promote positions and Blend offsets.
    CHECK(Near(PromoteCopyPosition(100.0, 4.0, 4.5), 101.5));
    CHECK(Near(PromoteCopyPosition(100.0, 4.0, 4.0), 101.0));
    CHECK(Near(BlendChildOffsetMs(4.12, 4.02), 100.0, 1e-6));

    // D8: every insertion branch.
    const SoundNode s1 = Node("s1", SoundNodeType::Sound);
    {
        // No root.
        SoundLibrary lib;
        std::string  err;
        CHECK(AddPromotedToLibrary(lib, {s1}, "", "r", &err));
        CHECK(lib.root == "s1" && lib.nodes.size() == 1 && lib.nodes[0].parent.empty());
    }
    {
        // Root Random: new child.
        SoundLibrary lib;
        lib.root = "R";
        lib.nodes = {Node("R", SoundNodeType::Random), Node("o", SoundNodeType::Sound, "R")};
        CHECK(AddPromotedToLibrary(lib, {s1}, "", "x", nullptr));
        CHECK(Find(lib, "s1") && Find(lib, "s1")->parent == "R");
        CHECK(!Find(lib, "x"));
    }
    {
        // Root Blend: new child; a Blend with children keeps them under it.
        SoundLibrary lib;
        lib.root = "B";
        lib.nodes = {Node("B", SoundNodeType::Blend)};
        SoundNode nb = Node("nb", SoundNodeType::Blend);
        SoundNode c1 = Node("c1", SoundNodeType::Sound, "nb");
        SoundNode c2 = Node("c2", SoundNodeType::Sound, "nb");
        c2.offset_ms = 100.0;
        CHECK(AddPromotedToLibrary(lib, {nb, c1, c2}, "", "x", nullptr));
        CHECK(Find(lib, "nb")->parent == "B" && Find(lib, "c1")->parent == "nb" && Find(lib, "c2")->parent == "nb");
        CHECK(Near(Find(lib, "c2")->offset_ms, 100.0));
    }
    {
        // Root Sound: a new Random root holding old + new.
        SoundLibrary lib;
        lib.name = "Steps";
        lib.root = "o";
        lib.nodes = {Node("o", SoundNodeType::Sound)};
        CHECK(AddPromotedToLibrary(lib, {s1}, "", "nr", nullptr));
        CHECK(lib.root == "nr" && lib.nodes[0].uid == "nr" && lib.nodes[0].type == SoundNodeType::Random);
        CHECK(Find(lib, "o")->parent == "nr" && Find(lib, "s1")->parent == "nr");
        CHECK(lib.nodes[0].parent.empty());
    }
    {
        // Root Switch: no value -> error; empty value -> a case; Random child -> into it;
        // another child -> a new Random named after the value holding old + new.
        SoundLibrary lib;
        lib.root = "W";
        SoundNode w = Node("W", SoundNodeType::Switch);
        w.group_uid = "g";
        SwitchCase k;
        k.value_uid = "metal";
        k.child_uid = "mr";
        w.cases.push_back(k);
        k.value_uid = "stone";
        k.child_uid = "ss";
        w.cases.push_back(k);
        lib.nodes = {w, Node("mr", SoundNodeType::Random, "W"), Node("ss", SoundNodeType::Sound, "W")};
        std::string err;
        CHECK(!AddPromotedToLibrary(lib, {s1}, "", "x", &err) && !err.empty());
        CHECK(lib.nodes.size() == 3);

        CHECK(AddPromotedToLibrary(lib, {Node("w1", SoundNodeType::Sound)}, "wood", "x", nullptr));
        const SoundNode* root = Find(lib, "W");
        bool             wood = false;
        for (const SwitchCase& c : root->cases) wood = wood || (c.value_uid == "wood" && c.child_uid == "w1");
        CHECK(wood);
        CHECK(Find(lib, "w1")->parent == "W");

        CHECK(AddPromotedToLibrary(lib, {Node("m1", SoundNodeType::Sound)}, "metal", "x", nullptr));
        CHECK(Find(lib, "m1")->parent == "mr");
        CHECK(!Find(lib, "x"));

        CHECK(AddPromotedToLibrary(lib, {Node("t1", SoundNodeType::Sound)}, "stone", "nr", nullptr, "Stone"));
        const SoundNode* nr = Find(lib, "nr");
        CHECK(nr && nr->type == SoundNodeType::Random && nr->parent == "W" && nr->name == "Stone");
        CHECK(Find(lib, "ss")->parent == "nr" && Find(lib, "t1")->parent == "nr");
        bool stone = false;
        for (const SwitchCase& c : Find(lib, "W")->cases) stone = stone || (c.value_uid == "stone" && c.child_uid == "nr");
        CHECK(stone);
        // The new Random's line comes before its children's.
        int i_nr = -1, i_ss = -1;
        for (size_t i = 0; i < lib.nodes.size(); ++i) {
            if (lib.nodes[i].uid == "nr") i_nr = static_cast<int>(i);
            if (lib.nodes[i].uid == "ss") i_ss = static_cast<int>(i);
        }
        CHECK(i_nr >= 0 && i_nr < i_ss);
        // Round trip through the store grammar.
        SoundStore st;
        st.libraries.push_back(lib);
        SoundStore back;
        CHECK(ParseSoundStore(SerializeSoundStore(st), &back));
        CHECK(back.libraries.size() == 1 && back.libraries[0].nodes.size() == lib.nodes.size());
    }
    {
        SoundLibrary lib;
        CHECK(!AddPromotedToLibrary(lib, {}, "", "x", nullptr));
    }

    // Review: the new Random falls back to the library's name, in both branches.
    {
        SoundLibrary lib;
        lib.name = "Steps";
        lib.root = "o";
        lib.nodes = {Node("o", SoundNodeType::Sound)};
        CHECK(AddPromotedToLibrary(lib, {s1}, "", "nr", nullptr));
        CHECK(Find(lib, "nr") && Find(lib, "nr")->name == "Steps");
        // No id for the new Random: refused, nothing changed.
        SoundLibrary l2;
        l2.root = "o";
        l2.nodes = {Node("o", SoundNodeType::Sound)};
        std::string err;
        CHECK(!AddPromotedToLibrary(l2, {s1}, "", "", &err) && !err.empty() && l2.nodes.size() == 1 && l2.root == "o");
    }
    {
        SoundLibrary lib;
        lib.name = "Steps";
        lib.root = "W";
        SoundNode w = Node("W", SoundNodeType::Switch);
        SwitchCase k;
        k.value_uid = "stone";
        k.child_uid = "ss";
        w.cases.push_back(k);
        k.value_uid = "v";
        k.child_uid = "gone";  // dangling child: the case is reused
        w.cases.push_back(k);
        lib.nodes = {w, Node("ss", SoundNodeType::Sound, "W")};
        CHECK(AddPromotedToLibrary(lib, {Node("t1", SoundNodeType::Sound)}, "stone", "nr", nullptr));
        CHECK(Find(lib, "nr") && Find(lib, "nr")->name == "Steps");
        CHECK(AddPromotedToLibrary(lib, {Node("v1", SoundNodeType::Sound)}, "v", "x", nullptr));
        int n_v = 0;
        for (const SwitchCase& c : Find(lib, "W")->cases)
            if (c.value_uid == "v") {
                ++n_v;
                CHECK(c.child_uid == "v1");
            }
        CHECK(n_v == 1);
        CHECK(Find(lib, "v1")->parent == "W" && !Find(lib, "x"));
        // A Sound child with no id for the new Random: refused.
        std::string err;
        CHECK(!AddPromotedToLibrary(lib, {Node("u1", SoundNodeType::Sound)}, "v", "", &err) && !err.empty());
        CHECK(!Find(lib, "u1"));
    }

    // More overlap shapes: an item inside another; a group's end extended by a longer item.
    {
        auto g = OverlapGroups({Item("a", 1, 0.0, 5.0), Item("b", 1, 1.0, 1.0), Item("c", 1, 4.0, 0.5)});
        CHECK(g.size() == 1 && g[0].size() == 3);
        // b ends at 3.5, c (3.0-4.0) extends the group, so d at 3.8 joins it; e at 5.0 does not.
        g = OverlapGroups({Item("a", 1, 0.0, 1.0), Item("b", 1, 0.5, 3.0), Item("c", 1, 3.0, 1.0), Item("d", 1, 3.8, 1.0),
                           Item("e", 1, 5.0, 1.0)});
        CHECK(g.size() == 2 && g[0].size() == 4 && g[1].size() == 1 && g[1][0] == 4);
    }

    // Review (gap 1): the attach plan, matrix "Nearest" and "Groups".
    {
        std::vector<AttachItem>  items = {Item("b", 3, 3.9, 1.0), Item("a", 2, 4.0, 1.0, 0.02)};
        std::vector<AttachEvent> ev = {Ev(0, 1.5, 3.5), Ev(0, 2.0, 4.0)};
        auto plan = PlanAttach(items, ev, -1);
        CHECK(plan.size() == 1 && plan[0].members.size() == 2 && plan[0].event == 1);
        CHECK(plan.size() == 1 && Near(plan[0].offset_s, 0.02) && Near(plan[0].anchor, 4.02));
        AttachEvent e = ev[1];
        e.rid = "r1";
        const SoundPlacement p = AttachPlacementOf(items[0], plan[0].offset_s);
        CHECK(p.own == Ownership::Attached && p.item_guid == "b" && Near(p.offset_s, 0.02));
        const PlacedTag tag = AttachTagOf("{ANIM}", e);
        CHECK(tag.owner == "{ANIM}" && tag.rid == "r1" && Near(tag.t, 2.0) && tag.own == Ownership::Attached);
        // Groups: each to its own nearest event of the rule; none of the rule -> no event.
        std::vector<AttachItem>  g = {Item("A", 1, 1.0, 1.0), Item("B", 2, 1.5, 1.5), Item("C", 1, 6.0, 1.0)};
        std::vector<AttachEvent> ge = {Ev(0, 1.0, 1.1), Ev(0, 6.0, 6.2), Ev(1, 3.0, 3.0)};
        plan = PlanAttach(g, ge, 0);
        CHECK(plan.size() == 2 && plan[0].event == 0 && plan[1].event == 1);
        CHECK(plan.size() == 2 && Near(plan[0].offset_s, -0.1) && Near(plan[1].offset_s, -0.2));
        plan = PlanAttach(g, ge, 2);
        CHECK(plan.size() == 2 && plan[0].event == -1 && plan[1].event == -1);
        CHECK(PlanAttach({}, ge, -1).empty());
    }

    // Review (gap 2): re-Spread takes only the spread copies, keeps their nudge; the passes of one
    // event share it; an attached pile's own event is skipped.
    {
        ItemSounds     rec;
        SoundPlacement p;
        p.item_guid = "{S}";
        p.own = Ownership::Spread;
        p.offset_s = 0.05;
        AddToRecord(rec, "r1", 2.0, p);
        p.item_guid = "{A}";
        p.own = Ownership::Attached;
        p.offset_s = 0.3;
        AddToRecord(rec, "r1", 2.0, p);
        p.item_guid = "{P}";
        p.own = Ownership::Populated;
        p.offset_s = 0.0;
        AddToRecord(rec, "r1", 2.0, p);
        std::vector<AttachEvent> targets = {Ev(0, 2.0, 2.0), Ev(0, 5.0, 5.0), Ev(0, 2.0, 12.0)};
        std::vector<double>      nudges;
        auto                     taken = TakeSpreadCopies(rec, "r1", targets, &nudges);
        CHECK(taken.size() == 1 && taken[0].p.item_guid == "{S}" && Near(taken[0].t, 2.0));
        CHECK(nudges.size() == 3 && Near(nudges[0], 0.05) && Near(nudges[1], 0.0) && Near(nudges[2], 0.05));
        const EventSounds* es = FindEventSounds(rec, "r1", 2.0);
        CHECK(es && es->placed.size() == 2);
        bool a = false, pp = false;
        for (const SoundPlacement& x : es->placed) {
            a = a || x.item_guid == "{A}";
            pp = pp || x.item_guid == "{P}";
        }
        CHECK(a && pp);

        // Picks: one per clip event, the passes share it.
        CHECK(SpreadPickTakes({Item("a", 1, 0, 1, 0, 1), Item("b", 1, 0, 1, 0, 3), Item("c", 1, 0, 1, 0, 4)}) == 3);
        CHECK(SpreadPickTakes({Item("a", 1, 0, 1)}) == 0);
        auto picks = SpreadPicks(targets, 3, 42, "r1");
        CHECK(picks.size() == 3 && picks[0] >= 0 && picks[0] == picks[2] && picks[1] != picks[0]);
        picks = SpreadPicks(targets, 1, 42, "r1");
        CHECK(picks.size() == 3 && picks[0] == -1 && picks[1] == -1 && picks[2] == -1);

        // Targets: the rule's events in time order, minus the pile's and the attached one's.
        std::vector<AttachEvent> all = {Ev(0, 9.0, 9.0), Ev(0, 1.0, 1.0), Ev(1, 4.0, 4.0), Ev(0, 4.0, 4.0)};
        auto t = SpreadTargets(all, 0, 1.01, nullptr);
        CHECK(t.size() == 2 && Near(t[0].project_t, 4.0) && Near(t[1].project_t, 9.0));
        const double att = 4.0;
        t = SpreadTargets(all, 0, 1.3, &att);  // pile nudged 0.3 s off its attached event 4.0
        CHECK(t.size() == 2 && Near(t[0].project_t, 1.0) && Near(t[1].project_t, 9.0));
    }


    if (g_fail) {
        std::printf("sound_attach_plan_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("sound_attach_plan_test: all passed\n");
    return 0;
}
