// SPDX-License-Identifier: MIT
//
// Host test of src/attach_manager_plan.h and the "<side> only" helpers of src/sound_spread_plan.h (13-68e): the
// Marker list and its preselection, the gesture and its reasons, the run label, the Detach list's filter and rows,
// the variations of an Attached library, the RAV - Side Switch (Attach and Add to stack on one side), Detach (the
// marker left empty) and Remove from library (the other markers keep their draw).

#include "attach_manager_plan.h"

#include <cmath>
#include <cstdio>
#include <string>

#include "sound_record.h"
#include "sound_spread_plan.h"

using namespace rav;
using namespace rav::am;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

static int g_uid = 0;
static std::string Uid()
{
    char b[32];
    std::snprintf(b, sizeof(b), "u%02d", ++g_uid);
    return b;
}

static Marker Mk(int anim, const char* label, const char* kind, char side, double pos, bool clicked = false)
{
    Marker m;
    m.anim = anim;
    m.name = label;
    m.label = label;
    m.kind_key = std::string("auto:") + kind;
    m.kind_name = kind;
    m.side = side;
    m.pos = pos;
    m.clicked = clicked;
    return m;
}

static Snapshot Project()
{
    Snapshot s;
    s.anims = {{"{W1}", "Walk_01", false}, {"{W2}", "Walk_01", false}, {"{R1}", "Run_02", false}};
    return s;
}

static SoundNode Node(const char* uid, const char* parent, SoundNodeType t, const char* name = "")
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = t;
    n.name = name;
    return n;
}

static AttachedEvent Ev(int anim, const char* rid, double t, double pos, const char* lib, const char* label, char side)
{
    AttachedEvent e;
    e.anim = anim;
    e.rid = rid;
    e.t = t;
    e.pos = pos;
    e.lib_uid = lib;
    e.label = label;
    e.side = side;
    return e;
}

static Placed Pl(const char* guid, const char* name, const char* node, Ownership own, double off = 0.0)
{
    Placed p;
    p.item_guid = guid;
    p.name = name;
    p.node = node;
    p.own = own;
    p.offset_s = off;
    return p;
}

int main()
{
    // ---- Marker list: a time selection over Scuff L, FS L, FS R (two FS L): one per kind + side, its first; the
    // clicked FS R label (outside the range) added and preselected.
    {
        Snapshot s = Project();
        s.has_time_sel = true;
        s.ts0 = 1.0;
        s.ts1 = 2.0;
        s.markers = {Mk(0, "FS L", "FS", 'L', 1.4), Mk(0, "Scuff L", "Scuff", 'L', 1.15), Mk(0, "FS R", "FS", 'R', 1.9),
                     Mk(0, "FS L", "FS", 'L', 1.7), Mk(0, "FS R", "FS", 'R', 2.9, true)};
        const std::vector<int> ch = MarkerChoices(s);
        CHECK(ch.size() == 4);
        if (ch.size() == 4) {
            CHECK(ch[0] == 1 && ch[1] == 0 && ch[2] == 2 && ch[3] == 4);  // Scuff L, FS L 1.4, FS R 1.9, clicked FS R 2.9
        }
        CHECK(PreselectMarker(s, ch, "") == 4);                                   // the clicked label
        CHECK(PreselectMarker(s, ch, MarkerKey(s, s.markers[0])) == 0);           // the user's pick
        CHECK(PreselectMarker(s, ch, "gone|FS L|1") == 4);                        // a pick no longer listed
        s.markers[4].clicked = false;
        CHECK(PreselectMarker(s, MarkerChoices(s), "") == 1);                     // else the first in time
        CHECK(MarkerText(s.markers[0]) == "FS L \xC2\xB7 1.40 s");
        // A clicked label inside the range that is also its kind's first: listed once.
        s.markers[0].clicked = true;
        CHECK(MarkerChoices(s).size() == 3);
        // No time selection, no label: nothing; the run button is off with the reason.
        Snapshot e = Project();
        e.sounds = 2;
        const GestureChoice g = ChooseGesture(e, MarkerChoices(e), PreselectMarker(e, MarkerChoices(e), ""), GestureWant::Auto);
        CHECK(g.gesture == Gesture::Attach && g.marker == -1 && !g.reason.empty());
        CHECK(g.from == "No time selection, no marker label clicked.");
        // Markers Take only and nothing read: that reason instead.
        e.take_only = "RAV markers are take markers only";
        CHECK(ChooseGesture(e, {}, -1, GestureWant::Auto).reason == "RAV markers are take markers only");
    }

    // ---- The gesture from the selection.
    {
        Snapshot s = Project();
        s.has_time_sel = true;
        s.ts0 = 1.0;
        s.ts1 = 2.0;
        s.markers = {Mk(0, "FS L", "FS", 'L', 1.7)};
        s.events = {Ev(0, "r1", 1.4, 1.7, "LIB", "FS L", 'L'), Ev(0, "r1", 2.4, 2.7, "LIB", "FS L", 'L')};
        s.sounds = 2;
        s.fresh = 2;
        // Two piles selected, FS L chosen: Attach, nothing in the way.
        GestureChoice g = ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Auto);
        CHECK(g.gesture == Gesture::Attach && g.reason.empty() && g.marker == 0 && !g.stack_ok);
        CHECK(g.from == "Time selection: FS L.");
        // New items + a sound attached on FS L: Add to stack preselected, on that sound's marker.
        s.stack_refs = {{"{W1}", "r1", 1.4, 0}};
        s.sounds = 3;
        g = ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Auto);
        CHECK(g.gesture == Gesture::Stack && g.stack_ok && g.stack_event == 0 && g.reason.empty());
        // The switch back to Attach: Attach (the stack items are taken too).
        g = ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Attach);
        CHECK(g.gesture == Gesture::Attach && g.reason.empty());
        // Sounds of two markers: disabled with the reason.
        s.stack_refs.push_back({"{W1}", "r1", 2.4, 1});
        g = ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Auto);
        CHECK(g.gesture == Gesture::Stack && g.stack_events == 2 && g.reason == "Select attached sounds of one marker only (here 2 markers).");
        // Add to stack asked without a stack: the reason.
        s.stack_refs.clear();
        g = ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Stack);
        CHECK(g.gesture == Gesture::Stack && g.reason == "Select an attached sound with the new items.");
        // Nothing selected: Attach off.
        s.sounds = 0;
        s.fresh = 0;
        CHECK(ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Auto).reason == "Select the sounds to attach.");
        CHECK(RunLabel(Gesture::Attach, false, false, "FS", 'L') == "Attach \xC2\xB7 this animation \xC2\xB7 both sides");
        CHECK(RunLabel(Gesture::Stack, true, true, "FS", 'R') == "Add to stack \xC2\xB7 all with FS \xC2\xB7 R only");
    }

    // ---- What the Detach list shows: the selected animation / items / time selection / everything.
    {
        Snapshot s = Project();
        AttachedEvent a = Ev(0, "r1", 1.4, 1.7, "LIB", "FS L", 'L');
        a.placed = {Pl("{I1}", "Step_dirt_A", "", Ownership::Attached), Pl("{I2}", "Step_layer", "", Ownership::Attached, 0.02)};
        AttachedEvent b = Ev(0, "r2", 1.9, 2.2, "LIB", "FS R", 'R');
        b.placed = {Pl("{C1}", "Step_dirt_B", "n2", Ownership::Populated)};
        AttachedEvent c = Ev(2, "r1", 0.3, 9.3, "LIB", "FS L", 'L');
        c.placed = {Pl("{C2}", "Step_dirt_A", "n1", Ownership::Populated)};
        AttachedEvent d = Ev(2, "r1", 0.7, 9.7, "OTHER", "Scuff L", 'L');
        d.placed = {Pl("{C3}", "Scuff", "n9", Ownership::Populated)};
        s.events = {a, b, c, d};
        ListFilter f = MakeFilter(s);
        CHECK(f.kind == FilterKind::Everything && f.label == "everything");
        std::vector<AnimGroup> rows = MarkerRows(s, f, "LIB");
        CHECK(rows.size() == 2 && rows[0].anim == 0 && rows[0].rows.size() == 2 && rows[1].anim == 2 && rows[1].rows.size() == 1);
        if (!rows.empty() && !rows[0].rows.empty()) {
            CHECK(rows[0].rows[0].l1 == "Step_dirt_A + 1 layer");
            CHECK(rows[0].rows[0].l2 == "FS L \xC2\xB7 1.70 s \xC2\xB7 attached original");
            CHECK(rows[0].rows[1].l2 == "FS R \xC2\xB7 2.20 s \xC2\xB7 copy");
        }
        CHECK(MarkerRows(s, f, "").size() == 2 && MarkerRows(s, f, "")[1].rows.size() == 2);  // All libraries
        // A time selection: only what it covers.
        s.has_time_sel = true;
        s.ts0 = 1.5;
        s.ts1 = 2.0;
        f = MakeFilter(s);
        CHECK(f.kind == FilterKind::TimeSelection && f.label == "the time selection (1.50 \xE2\x80\x93 2.00 s)");
        rows = MarkerRows(s, f, "");
        CHECK(rows.size() == 1 && rows[0].rows.size() == 1 && rows[0].rows[0].event == 0);
        // One attached item selected: only its marker.
        s.selected_items = {GuidKey("{c1}")};
        f = MakeFilter(s);
        CHECK(f.kind == FilterKind::Items && f.label == "the selected item (Step_dirt_B)");
        rows = MarkerRows(s, f, "");
        CHECK(rows.size() == 1 && rows[0].rows.size() == 1 && rows[0].rows[0].event == 1);
        // A selected item that is not attached: the list is empty and the label says so.
        s.selected_items = {GuidKey("{PLAIN}")};
        f = MakeFilter(s);
        CHECK(f.label == "the selected item (not attached)" && MarkerRows(s, f, "").empty());
        // Run_02 selected: its markers.
        s.anims[2].selected = true;
        f = MakeFilter(s);
        CHECK(f.kind == FilterKind::Animations && f.label == "the selected animation (Run_02)");
        rows = MarkerRows(s, f, "");
        CHECK(rows.size() == 1 && rows[0].anim == 2 && rows[0].rows.size() == 2);
    }

    // ---- RAV - Side: Attach "L only" on a new library, on a both-sides library, again.
    {
        SoundStore st;
        SoundLibrary lib;
        lib.uid = "LIB";
        lib.name = "FS";
        lib.attached = true;
        st.libraries.push_back(lib);
        CHECK(SetSideContent(st, "LIB", 'L', {Node("a", "", SoundNodeType::Random, "FS"), Node("a1", "a", SoundNodeType::Sound, "A")}, &Uid));
        CHECK(st.groups.size() == 1 && IsSideGroup(st.groups[0]) && GroupDisplayName(st.groups[0]) == "RAV - Side");
        const std::string vl = SideValueOf(st.groups[0], 'L'), vr = SideValueOf(st.groups[0], 'R');
        CHECK(!vl.empty() && !vr.empty() && vl != vr);
        const SoundLibrary& l0 = st.libraries[0];
        const SoundNode*    root = FindNode(l0, l0.root);
        CHECK(root && root->type == SoundNodeType::Switch && root->group_uid == st.groups[0].uid);
        CHECK(root && root->cases.size() == 1 && root->cases[0].value_uid == vl && root->cases[0].child_uid == "a");  // R: nothing at first
        CHECK(LibraryUsesSide(st, l0));
        // Both-sides content first, then L only: R keeps it.
        SoundStore st2;
        SoundLibrary lib2;
        lib2.uid = "LIB";
        lib2.name = "FS";
        SetLibraryContent(lib2, {Node("x", "", SoundNodeType::Random, "FS"), Node("x1", "x", SoundNodeType::Sound, "X")});
        st2.libraries.push_back(lib2);
        CHECK(!LibraryUsesSide(st2, st2.libraries[0]));
        CHECK(SetSideContent(st2, "LIB", 'L', {Node("b", "", SoundNodeType::Random, "FS"), Node("b1", "b", SoundNodeType::Sound, "B")}, &Uid));
        const SoundLibrary& l2 = st2.libraries[0];
        root = FindNode(l2, l2.root);
        CHECK(root && root->type == SoundNodeType::Switch && root->cases.size() == 2);
        const std::string vl2 = SideValueOf(st2.groups[0], 'L'), vr2 = SideValueOf(st2.groups[0], 'R');
        for (const SwitchCase& c : root ? root->cases : std::vector<SwitchCase>()) {
            if (c.value_uid == vr2) CHECK(c.child_uid == "x");
            if (c.value_uid == vl2) CHECK(c.child_uid == "b");
        }
        CHECK(FindNode(l2, "x") && FindNode(l2, "x")->parent == l2.root);
        // L again: L's content replaced, R untouched, one Switch.
        CHECK(SetSideContent(st2, "LIB", 'L', {Node("c", "", SoundNodeType::Random, "FS"), Node("c1", "c", SoundNodeType::Sound, "C")}, &Uid));
        CHECK(!FindNode(l2, "b") && !FindNode(l2, "b1") && FindNode(l2, "c1") && FindNode(l2, "x1"));
        CHECK(st2.groups.size() == 1);
        int switches = 0;
        for (const SoundNode& n : l2.nodes) switches += n.type == SoundNodeType::Switch ? 1 : 0;
        CHECK(switches == 1);
        CHECK(!SetSideContent(st2, "LIB", 0, {Node("d", "", SoundNodeType::Random)}, &Uid));

        // Add to stack "R only" on that library: the layer goes into R's content (wrapped in a Blend).
        const std::string top = SideLayerTop(st2, "LIB", 'R', &Uid);
        CHECK(!top.empty() && FindNode(l2, top) && FindNode(l2, top)->type == SoundNodeType::Blend);
        CHECK(FindNode(l2, "x")->parent == top);
        {
            const SoundNode* rs = FindNode(l2, l2.root);  // the root Side Switch: its R case is the new Blend
            CHECK(rs && FindNode(l2, top)->parent == rs->uid);
            bool r_case = false;
            for (const SwitchCase& c : rs ? rs->cases : std::vector<SwitchCase>())
                if (c.value_uid == vr2) r_case = c.child_uid == top;
            CHECK(r_case);
        }
        AddStackLayer(st2.libraries[0], {Node("ly", "", SoundNodeType::Random, "Layer 2"), Node("ly1", "ly", SoundNodeType::Sound, "Cloth")}, 20.0,
                      Uid(), top);
        CHECK(FindNode(l2, "ly") && FindNode(l2, "ly")->parent == top && FindNode(l2, "ly")->offset_ms == 20.0);
    }

    // ---- Add to stack "L only" on a both-sides library: the base stays shared, the layer under a Side Switch in the
    // root Blend; a per-animation layer on one side never takes the Side Switch for its own.
    {
        SoundStore   st;
        SoundLibrary lib;
        lib.uid = "LIB";
        lib.name = "FS";
        SetLibraryContent(lib, {Node("x", "", SoundNodeType::Random, "FS"), Node("x1", "x", SoundNodeType::Sound, "X")});
        st.libraries.push_back(lib);
        const std::string top = SideLayerTop(st, "LIB", 'L', &Uid);
        const SoundLibrary& l = st.libraries[0];
        const SoundNode*    root = FindNode(l, l.root);
        CHECK(root && root->type == SoundNodeType::Blend && FindNode(l, "x")->parent == l.root);
        const SoundNode* t = FindNode(l, top);
        CHECK(t && t->type == SoundNodeType::Blend);
        const SoundNode* sw = t ? FindNode(l, t->parent) : nullptr;
        CHECK(sw && sw->type == SoundNodeType::Switch && sw->parent == l.root && sw->cases.size() == 1);
        CHECK(SideLayerTop(st, "LIB", 'L', &Uid) == top);  // reused
        const std::string sw_uid = sw ? sw->uid : std::string(), sw_group = sw ? sw->group_uid : std::string();  // `sw` moves below
        const ValueSet v = AddAnimationLayer(st, "LIB", {Node("an", "", SoundNodeType::Random, "Layer"), Node("an1", "an", SoundNodeType::Sound, "K")},
                                             10.0, "Walk", {}, "FS", &Uid, {}, top);
        CHECK(!v.group_uid.empty() && v.group_uid != sw_group);
        const SwitchGroup* g = FindGroup(st, v.group_uid);
        CHECK(g && !IsSideGroup(*g) && g->name == "FS layers");
        CHECK(LibraryUsesSide(st, st.libraries[0]));
        // A both-sides per-animation layer afterwards (top ""): its own group, the Side Switch's cases unchanged.
        const std::vector<SwitchCase> before = FindNode(st.libraries[0], sw_uid)->cases;
        const ValueSet v2 = AddAnimationLayer(st, "LIB", {Node("bn", "", SoundNodeType::Random, "Layer"), Node("bn1", "bn", SoundNodeType::Sound, "J")},
                                              5.0, "Run", {}, "FS", &Uid);
        const SwitchGroup* g2 = FindGroup(st, v2.group_uid);
        CHECK(g2 && !IsSideGroup(*g2));
        const SoundNode* sw2 = nullptr;
        for (const SoundNode& n : st.libraries[0].nodes)
            if (n.type == SoundNodeType::Switch && FindGroup(st, n.group_uid) && IsSideGroup(*FindGroup(st, n.group_uid))) sw2 = &n;
        CHECK(sw2 && sw2->cases.size() == before.size() && sw2->cases[0].child_uid == before[0].child_uid);
    }

    // ---- Missing RAV - Side values given before Populate places: per rule side, a value already set kept.
    {
        SoundStore   st;
        SoundLibrary lib;
        lib.uid = "LIB";
        lib.name = "FS";
        st.libraries.push_back(lib);
        CHECK(SetSideContent(st, "LIB", 'L', {Node("a", "", SoundNodeType::Random, "FS"), Node("a1", "a", SoundNodeType::Sound, "A")}, &Uid));
        const SwitchGroup& g = st.groups[0];
        ItemSounds rec;
        for (const char* rid : {"rl", "rr", "rk", "rn"}) {
            RuleSounds r;
            r.rid = rid;
            r.library = std::string(rid) == "rn" ? "OTHER" : "LIB";
            rec.rules.push_back(r);
        }
        rec.rules[2].values.push_back({g.uid, SideValueOf(g, 'L'), {}});  // "rk" (an R rule) already holds L: kept
        CHECK(FillMissingSideValues(st, rec, {{"rl", 'L'}, {"rr", 'R'}, {"rk", 'R'}, {"rn", 'L'}, {"rx", 'L'}}));
        CHECK(rec.rules[0].values.size() == 1 && rec.rules[0].values[0].value_uid == SideValueOf(g, 'L'));
        CHECK(rec.rules[1].values.size() == 1 && rec.rules[1].values[0].value_uid == SideValueOf(g, 'R'));
        CHECK(rec.rules[2].values.size() == 1 && rec.rules[2].values[0].value_uid == SideValueOf(g, 'L'));
        CHECK(rec.rules[3].values.empty());  // a library without a Side Switch
        CHECK(!FillMissingSideValues(st, rec, {{"rl", 'L'}, {"rr", 'R'}}));  // nothing more
    }

    // ---- The variations of an Attached library and the markers each is placed on.
    {
        SoundStore   st;
        SoundLibrary lib;
        lib.uid = "LIB";
        lib.name = "FS";
        SoundNode tk = Node("p1", "base", SoundNodeType::Sound, "Step_dirt_A");
        tk.sound.mode = DetectMode::Takes;
        tk.sound.frames.resize(4);
        SetLibraryContent(lib, {Node("root", "", SoundNodeType::Blend, "FS"), Node("base", "root", SoundNodeType::Random, "FS"), tk,
                                Node("p2", "base", SoundNodeType::Blend, "Step_dirt_B"), Node("p2a", "p2", SoundNodeType::Sound, "B1"),
                                Node("p2b", "p2", SoundNodeType::Sound, "B2"), Node("ly", "root", SoundNodeType::Random, "Cloth_rustle"),
                                Node("ly1", "ly", SoundNodeType::Sound, "Cloth_rustle")});
        st.libraries.push_back(lib);
        Snapshot s = Project();
        AttachedEvent e1 = Ev(0, "r1", 1.4, 1.7, "LIB", "FS L", 'L');
        e1.placed = {Pl("{O}", "Step_dirt_A", "", Ownership::Attached)};
        e1.draws = {{"base", 0}, {"p1", 2}};
        AttachedEvent e2 = Ev(0, "r2", 1.9, 2.2, "LIB", "FS R", 'R');
        e2.placed = {Pl("{C1}", "B1", "p2a", Ownership::Populated), Pl("{C2}", "B2", "p2b", Ownership::Populated),
                     Pl("{C3}", "Cloth", "ly1", Ownership::Populated, 0.02)};
        e2.draws = {{"base", 1}, {"ly", 0}};
        AttachedEvent e3 = Ev(2, "r1", 0.3, 9.3, "LIB", "FS L", 'L');
        e3.placed = {Pl("{C4}", "Step_dirt_A", "p1", Ownership::Populated)};
        e3.draws = {{"base#p1", 0}};  // a loop pass's draw
        s.events = {e1, e2, e3};
        const std::vector<VariationRow> rows = VariationRows(st, st.libraries[0], s);
        CHECK(rows.size() == 3);
        if (rows.size() == 3) {
            CHECK(rows[0].node == "p1" && rows[0].kind == VariationKind::Takes && rows[0].takes == 4 && rows[0].markers == 2);
            CHECK(rows[1].node == "p2" && rows[1].kind == VariationKind::Variation && rows[1].markers == 1 && rows[1].index == 1);
            CHECK(rows[2].node == "ly" && rows[2].kind == VariationKind::Layer && rows[2].markers == 1 && rows[2].random.empty());
            CHECK(VariationText(rows[0]) == "4 takes \xC2\xB7 on 2 markers");
            CHECK(VariationText(rows[2]) == "layer \xC2\xB7 on 1 marker");
            // What each select button selects: p1's copy and the original (offset 0, Sound unknown), p2's two layers, ly's.
            const std::vector<std::string> i0 = VariationItems(st.libraries[0], rows[0], s);
            CHECK(i0.size() == 2 && i0[0] == "{O}" && i0[1] == "{C4}");
            const std::vector<std::string> i1 = VariationItems(st.libraries[0], rows[1], s);
            CHECK(i1.size() == 2 && i1[0] == "{C1}" && i1[1] == "{C2}");
            const std::vector<std::string> i2 = VariationItems(st.libraries[0], rows[2], s);
            CHECK(i2.size() == 1 && i2[0] == "{C3}");
            // An event attached by hand, no draw, its sound known by its source: counted, as Remove releases it.
            AttachedEvent hand = Ev(1, "r1", 0.5, 5.1, "LIB", "FS L", 'L');
            hand.placed = {Pl("{H}", "Step_dirt_B", "p2a", Ownership::Attached)};
            CHECK(EventUsesVariation(st.libraries[0], rows[1], hand));
            RemovedVariation rv;
            SoundLibrary     cp = st.libraries[0];
            CHECK(RemoveVariationFromLibrary(cp, "p2", &rv));
            EventSounds he;
            he.rid = "r1";
            SoundPlacement hp;
            hp.item_guid = "{H}";
            hp.own = Ownership::Attached;
            he.placed = {hp};
            CHECK(EventUsedRemoved(he, rv, {{GuidKey("{H}"), "p2a"}}));
        }
        // Under a root Side Switch: each side's content, the value named.
        SoundStore st2;
        SoundLibrary l2;
        l2.uid = "LIB";
        l2.name = "FS";
        st2.libraries.push_back(l2);
        SetSideContent(st2, "LIB", 'R', {Node("r", "", SoundNodeType::Random, "FS"), Node("r1", "r", SoundNodeType::Sound, "R1"),
                                         Node("r2", "r", SoundNodeType::Sound, "R2")},
                       &Uid);
        const std::vector<VariationRow> r2 = VariationRows(st2, st2.libraries[0], Snapshot());
        CHECK(r2.size() == 2 && r2[0].where == "R" && VariationText(r2[0]) == "variation \xC2\xB7 R \xC2\xB7 on 0 markers");
    }

    // ---- Detach from this marker (Keep / Delete write the same record): its sounds marked deleted, every Sound of the
    // library too, so Populate leaves it empty.
    {
        SoundLibrary lib;
        lib.uid = "LIB";
        SetLibraryContent(lib, {Node("base", "", SoundNodeType::Random), Node("p1", "base", SoundNodeType::Sound), Node("p2", "base", SoundNodeType::Sound)});
        EventSounds ev;
        ev.rid = "r1";
        ev.t = 1.4;
        SoundPlacement a;
        a.item_guid = "{O}";
        a.own = Ownership::Attached;
        SoundPlacement b;
        b.item_guid = "{L}";
        b.own = Ownership::Attached;
        b.offset_s = 0.02;
        ev.placed = {a, b};
        ev.draws = {{"base", 0}};
        const std::vector<std::string> left = DetachEventInRecord(ev, lib);
        CHECK(left.size() == 2 && left[0] == "{O}" && left[1] == "{L}");
        int deleted = 0;
        std::set<std::string> nodes;
        for (const SoundPlacement& p : ev.placed) {
            deleted += p.own == Ownership::Deleted ? 1 : 0;
            nodes.insert(p.node);
        }
        CHECK(deleted == static_cast<int>(ev.placed.size()) && nodes.count("p1") && nodes.count("p2"));
        // Again: nothing more (no new marks, no item).
        const size_t n = ev.placed.size();
        CHECK(DetachEventInRecord(ev, lib).empty() && ev.placed.size() == n);
        // An event with no live sound at all (a stale list): unchanged.
        EventSounds empty;
        empty.rid = "r1";
        CHECK(DetachEventInRecord(empty, lib).empty() && empty.placed.empty());
    }

    // ---- Remove from library: a variation placed on markers; Delete = dropped (redrawn by Populate), Keep = marked
    // deleted with what its Random could draw instead; every other marker's draw keeps its child.
    {
        auto make = [] {
            SoundLibrary lib;
            lib.uid = "LIB";
            SetLibraryContent(lib, {Node("base", "", SoundNodeType::Random), Node("p1", "base", SoundNodeType::Sound),
                                    Node("p2", "base", SoundNodeType::Sound), Node("p3", "base", SoundNodeType::Sound)});
            return lib;
        };
        auto rec = [] {
            ItemSounds r;
            RuleSounds rs;
            rs.rid = "r1";
            rs.library = "LIB";
            r.rules.push_back(rs);
            for (int k = 0; k < 3; ++k) {
                EventSounds e;
                e.rid = "r1";
                e.t = k;
                SoundPlacement p;
                p.item_guid = "{C" + std::to_string(k) + "}";
                p.node = "p" + std::to_string(k + 1);
                e.placed.push_back(p);
                e.draws = {{"base", k}};
                r.events.push_back(e);
            }
            // The attached original on event 0 (no node: its Sound is known by its source).
            SoundPlacement o;
            o.item_guid = "{orig}";
            o.own = Ownership::Attached;
            r.events[0].placed.push_back(o);
            return r;
        };
        const std::map<std::string, std::string> node_of = {{GuidKey("{ORIG}"), "p1"}};
        // Remove p2 (index 1), Delete.
        SoundLibrary     lib = make();
        RemovedVariation rv;
        CHECK(RemoveVariationFromLibrary(lib, "p2", &rv));
        CHECK(rv.random == "base" && rv.index == 1 && rv.sounds.count("p2") && rv.siblings.size() == 3 && !FindNode(lib, "p2"));
        ItemSounds r = rec();
        int        evs = 0;
        std::vector<std::string> out = RemoveVariationInRecord(r, {"r1"}, rv, false, node_of, &evs);
        CHECK(evs == 1 && out.size() == 1 && out[0] == "{C1}");
        CHECK(r.events[1].placed.empty() && r.events[1].draws.empty());     // redrawn by Populate
        CHECK(r.events[2].draws.size() == 1 && r.events[2].draws[0].pick == 1);  // p3 is index 1 now
        CHECK(r.events[0].draws[0].pick == 0 && r.events[0].placed.size() == 2);
        // Remove p1 (the attached original's variation), Keep: its items marked deleted, p2 / p3 marked too.
        lib = make();
        CHECK(RemoveVariationFromLibrary(lib, "p1", &rv));
        r = rec();
        out = RemoveVariationInRecord(r, {"r1"}, rv, true, node_of, &evs);
        CHECK(evs == 1 && out.size() == 2);
        std::set<std::string> marked;
        for (const SoundPlacement& p : r.events[0].placed) {
            CHECK(p.own == Ownership::Deleted);
            marked.insert(p.node);
        }
        CHECK(marked.count("p2") && marked.count("p3"));
        CHECK(r.events[1].draws[0].pick == 0 && r.events[2].draws[0].pick == 1);  // the others keep their child
        // Another rule's events are not touched.
        r = rec();
        out = RemoveVariationInRecord(r, {"r9"}, rv, false, node_of, &evs);
        CHECK(out.empty() && evs == 0 && r.events[1].draws[0].pick == 1);
        // The last variation: the library's Random empty.
        SoundLibrary one;
        one.uid = "LIB";
        SetLibraryContent(one, {Node("base", "", SoundNodeType::Random), Node("p1", "base", SoundNodeType::Sound)});
        CHECK(RemoveVariationFromLibrary(one, "p1", &rv) && ChildrenOf(one, "base").empty() && one.root == "base");
        CHECK(!RemoveVariationFromLibrary(one, "nope", &rv));
    }

    // ---- RemoveSubtree drops a removed child's Switch case.
    {
        SoundLibrary lib;
        SoundNode    sw = Node("sw", "", SoundNodeType::Switch);
        sw.cases = {{"v1", "a", {}}, {"v2", "b", {}}};
        SetLibraryContent(lib, {sw, Node("a", "sw", SoundNodeType::Random), Node("a1", "a", SoundNodeType::Sound), Node("b", "sw", SoundNodeType::Sound)});
        const std::vector<std::string> gone = RemoveSubtree(lib, "a");
        CHECK(gone.size() == 2 && FindNode(lib, "sw")->cases.size() == 1 && FindNode(lib, "sw")->cases[0].child_uid == "b");
        CHECK(RemoveSubtree(lib, "sw").size() == 2 && lib.root.empty() && lib.nodes.empty());
    }

    // ---- The report of an "<side> only" run.
    {
        SpreadRunCounts c;
        c.type = "FS";
        c.side = 'L';
        c.variations = 2;
        c.layers = 2;
        c.animations = 2;
        c.events = 4;
        c.placed = 3;
        CHECK(SpreadRunReport(c) == "FS L only (this animation): new library FS, 2 variations, 2 animations, 3 sounds placed");
    }

    if (g_fail) {
        std::printf("attach_manager_plan_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("attach_manager_plan_test: all passed\n");
    return 0;
}
