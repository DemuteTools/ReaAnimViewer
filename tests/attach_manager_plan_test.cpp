// SPDX-License-Identifier: MIT
//
// Host test of src/attach_manager_plan.h (13-68e v5): the Marker list and its preselection, the gesture and its
// reasons, the run label and info line, the Apply to setting, the library layout (AnimSwitch / RAV - Side / Pile),
// the gestures' library edits (Attach, Add to pile, Separate, Merge, a layer deleted) per scope, the linked instances
// (selection, delete, volume / fades, a move that does not propagate) and the L and R card. One block per pure row of
// the spec's I/O matrix.

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
    s.anims = {{"{W1}", "Walk_01", false, {}}, {"{W2}", "Walk_01", false, {}}, {"{R1}", "Run_02", false, {}}};
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

// A Sound playing from the item `src`.
static SoundNode Snd(const char* uid, const char* parent, const char* src)
{
    SoundNode n = Node(uid, parent, SoundNodeType::Sound, src);
    n.sound.source.item_guid = src;
    return n;
}

// An Attach's content: a Random of one Sound (Step_dirt, 4 takes in the real thing).
static std::vector<SoundNode> Content(const char* root, const char* src)
{
    const std::string kid = std::string(root) + "s";
    return {Node(root, "", SoundNodeType::Random, "FS"), Snd(kid.c_str(), root, src)};
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

static SoundStore Store()
{
    SoundStore   st;
    SoundLibrary lib;
    lib.uid = "LIB";
    lib.name = "FS";
    lib.attached = true;
    lib.attached_type = "auto:step";
    st.libraries.push_back(lib);
    return st;
}

static PoolKey Pool(const char* name, std::vector<ValueSet> values = {})
{
    PoolKey p;
    p.anim_name = name;
    p.item_values = std::move(values);
    p.kind_name = "FS";
    return p;
}

static Instance In(const char* guid, const char* pool, const char* layer, const char* sig, char slot = 0)
{
    Instance i;
    i.guid = guid;
    i.owner = std::string("{") + pool + "}";
    i.pool = pool;
    i.lib = "LIB";
    i.layer = layer;
    i.sig = sig;
    i.slot = slot;
    return i;
}

static bool Has(const std::vector<int>& v, int x)
{
    for (int i : v)
        if (i == x) return true;
    return false;
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
        CHECK(PreselectMarker(s, ch, "") == 4);                          // the clicked label
        CHECK(PreselectMarker(s, ch, MarkerKey(s, s.markers[0])) == 0);  // the user's pick
        CHECK(PreselectMarker(s, ch, "gone|FS L|1") == 4);               // a pick no longer listed
        s.markers[4].clicked = false;
        CHECK(PreselectMarker(s, MarkerChoices(s), "") == 1);            // else the first in time
        CHECK(MarkerText(s.markers[0]) == "FS L \xC2\xB7 1.40 s");
        s.markers[0].clicked = true;  // a clicked label inside the range that is also its kind's first: listed once
        CHECK(MarkerChoices(s).size() == 3);
        // Matrix "no marker -> disabled + reason".
        Snapshot e = Project();
        e.sounds = 2;
        const GestureChoice g = ChooseGesture(e, MarkerChoices(e), PreselectMarker(e, MarkerChoices(e), ""), GestureWant::Auto);
        CHECK(g.gesture == Gesture::Attach && g.marker == -1 && g.reason == "Make a time selection over a RAV marker (or click a RAV marker label).");
        CHECK(g.from == "No time selection, no marker label clicked.");
        e.take_only = "RAV markers are take markers only";
        CHECK(ChooseGesture(e, {}, -1, GestureWant::Auto).reason == "RAV markers are take markers only");
    }

    // ---- The gesture from the selection; Add to pile with instances of 2 markers: disabled + reason.
    {
        Snapshot s = Project();
        s.has_time_sel = true;
        s.ts0 = 1.0;
        s.ts1 = 2.0;
        s.markers = {Mk(0, "FS L", "FS", 'L', 1.7)};
        s.events = {Ev(0, "r1", 1.4, 1.7, "LIB", "FS L", 'L'), Ev(0, "r1", 2.4, 2.7, "LIB", "FS L", 'L')};
        s.sounds = 1;
        s.fresh = 1;
        s.piles = 1;
        GestureChoice g = ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Auto);
        CHECK(g.gesture == Gesture::Attach && g.reason.empty() && g.marker == 0 && !g.pile_ok);
        CHECK(g.from == "Time selection: FS L.");
        std::string bold, rest;
        GestureInfo(Gesture::Attach, Scope::Anim, s, "FS", 'L', false, "FS L \xC2\xB7 1.70 s", &bold, &rest);
        CHECK(bold == "1 item" && rest == " \xC2\xB7 onto FS L \xC2\xB7 1.70 s, then every FS (L and R) of this animation");
        GestureInfo(Gesture::Attach, Scope::Project, s, "FS", 'L', true, "FS L \xC2\xB7 1.70 s", &bold, &rest);
        CHECK(rest == " \xC2\xB7 onto FS L \xC2\xB7 1.70 s, then every FS L of the project");
        GestureInfo(Gesture::Attach, Scope::Item, s, "FS", 'L', false, "FS L \xC2\xB7 1.70 s", &bold, &rest);
        CHECK(rest == " \xC2\xB7 onto FS L \xC2\xB7 1.70 s, on this marker only");
        // Cloth_rustle + an attached instance: Add to pile preselected, at its offset.
        s.stack_refs = {{"{W1}", "r1", 1.4, 0}};
        s.sounds = 2;
        s.pile_offset_s = 0.02;
        s.has_pile_offset = true;
        g = ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Auto);
        CHECK(g.gesture == Gesture::Pile && g.pile_ok && g.stack_event == 0 && g.reason.empty());
        GestureInfo(Gesture::Pile, Scope::Anim, s, "FS", 'L', false, "", &bold, &rest);
        CHECK(bold == "1 new item" && rest == " \xE2\x86\x92 a layer at +20 ms on every linked instance");
        GestureInfo(Gesture::Pile, Scope::Item, s, "FS", 'L', false, "", &bold, &rest);
        CHECK(rest == " \xE2\x86\x92 a layer at +20 ms on this instance");
        CHECK(ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Attach).gesture == Gesture::Attach);
        // Instances of 2 markers not linked (no layer): disabled + reason.
        s.stack_refs.push_back({"{W1}", "r1", 2.4, 1});
        g = ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Auto);
        CHECK(g.gesture == Gesture::Pile && g.stack_events == 2 &&
              g.reason == "Select instances of one layer only with the new items (here 2).");
        // A plain click selected one layer's linked instances on 2 markers: accepted, the one nearest the new items.
        s.stack_refs[0].layer = "LIB|lay";
        s.stack_refs[0].pos = 1.7;
        s.stack_refs[1].layer = "LIB|lay";
        s.stack_refs[1].pos = 2.7;
        s.fresh_anchor = 2.72;
        s.has_fresh_anchor = true;
        g = ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Auto);
        CHECK(g.gesture == Gesture::Pile && g.reason.empty() && g.stack_events == 1 && g.stack_event == 1);
        // Instances of two layers: refused.
        s.stack_refs[1].layer = "LIB|cloth";
        int distinct = 0;
        CHECK(PileReference(s.stack_refs, true, 2.72, &distinct) == -1 && distinct == 2);
        s.stack_refs.clear();
        g = ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Pile);
        CHECK(g.reason == "Select an attached instance with the new items.");
        s.sounds = 0;
        s.fresh = 0;
        CHECK(ChooseGesture(s, MarkerChoices(s), 0, GestureWant::Auto).reason == "Select the sound to attach.");
        CHECK(RunLabel(Gesture::Attach, Scope::Anim) == "Attach \xC2\xB7 Anim");
        CHECK(RunLabel(Gesture::Pile, Scope::Item) == "Add to pile \xC2\xB7 Item");
    }

    // ---- Apply to: a project-level setting, Anim by default, one token of the store's header line.
    {
        SoundStore st;
        CHECK(ScopeOf(st) == Scope::Anim);
        CHECK(!SetScope(st, Scope::Anim));
        CHECK(SetScope(st, Scope::Project) && ScopeOf(st) == Scope::Project && st.attach_scope == 2 && st.kept.empty());
        // Saved and read back with the store (a store holding only the setting is written too).
        SoundStore back;
        CHECK(ParseSoundStore(SerializeSoundStore(st), &back) && ScopeOf(back) == Scope::Project && back.kept.empty());
        CHECK(SetScope(st, Scope::Item) && ScopeOf(st) == Scope::Item);
        CHECK(ParseSoundStore(SerializeSoundStore(st), &back) && ScopeOf(back) == Scope::Item);
        CHECK(SetScope(st, Scope::Anim) && SerializeSoundStore(st).empty());  // Anim, the default: nothing written
        CHECK(IsDeleteUndoName("Remove items") && IsDeleteUndoName("Delete items") && !IsDeleteUndoName("Glue items") &&
              IsDeleteUndoName("Supprimer les objets", {"Supprimer les objets"}));
        CHECK(std::string(ScopeLabel(Scope::Project)) == "Project" && std::string(ScopeWords(Scope::Item)) == "this instance");
    }

    // ---- Attach, Anim: the selection on Walk_01 only (its own case of RAV - FS animations); other animations untouched.
    {
        SoundStore st = Store();
        EditOut    o = AttachContent(st, "LIB", Scope::Anim, Pool("Walk_01"), 'L', Content("a", "{STEP}"), &Uid);
        CHECK(o.ok && o.groups == 1 && o.slots.count(0) && o.remap_pool.empty());
        CHECK(st.groups.size() == 1 && IsAnimGroup(st.groups[0]) && GroupDisplayName(st.groups[0]) == "RAV - FS animations");
        CHECK(o.anim_value.group_uid == st.groups[0].uid && !o.anim_value.value_uid.empty());
        const Layout lay = ReadLayout(st, st.libraries[0]);
        CHECK(!lay.anim_switch.empty() && lay.groups.size() == 2);
        if (lay.groups.size() == 2) {
            CHECK(lay.groups[0].is_default && lay.groups[0].node.empty());  // the other animations: nothing
            CHECK(lay.groups[1].pile == "a" && !lay.groups[1].split);
        }
        CHECK(GroupOf(lay, {}) == 0 && GroupOf(lay, {o.anim_value}) == 1);
        // Attach again on Walk_01: replaced, same value, no new case.
        o = AttachContent(st, "LIB", Scope::Anim, Pool("Walk_01", {o.anim_value}), 'R', Content("b", "{GRAVEL}"), &Uid);
        const Layout l2 = ReadLayout(st, st.libraries[0]);
        CHECK(o.ok && l2.groups.size() == 2 && l2.groups[1].pile == "b" && !FindNode(st.libraries[0], "a"));
        // Item: the library is never edited.
        CHECK(!AttachContent(st, "LIB", Scope::Item, Pool("Walk_01"), 'L', Content("c", "{X}"), &Uid).ok);
    }

    // ---- Attach, Project: every animation with FS (every Group, the default's included) gets the sound.
    {
        SoundStore st = Store();
        const EditOut a = AttachContent(st, "LIB", Scope::Anim, Pool("Walk_01"), 'L', Content("a", "{STEP}"), &Uid);
        const EditOut p = AttachContent(st, "LIB", Scope::Project, Pool("Run_02"), 'L', Content("p", "{GRAVEL}"), &Uid);
        CHECK(a.ok && p.ok && p.groups == 2);
        const Layout lay = ReadLayout(st, st.libraries[0]);
        CHECK(lay.groups.size() == 2);
        if (lay.groups.size() == 2) {
            CHECK(!lay.groups[0].pile.empty() && !lay.groups[1].pile.empty() && lay.groups[0].pile != lay.groups[1].pile);
            CHECK(LayerSig(st.libraries[0], lay.groups[0].pile) == "{GRAVEL}" && LayerSig(st.libraries[0], lay.groups[1].pile) == "{GRAVEL}");
        }
        // A library with no AnimSwitch: its root replaced.
        SoundStore s2 = Store();
        CHECK(AttachContent(s2, "LIB", Scope::Project, Pool("Walk_01"), 'L', Content("r", "{STEP}"), &Uid).ok);
        CHECK(s2.libraries[0].root == "r" && s2.groups.empty());
    }

    // ---- Add to pile, Anim: a layer on every instance of the animation, at its offset; the shared content untouched.
    {
        SoundStore st = Store();
        SetLibraryContent(st.libraries[0], Content("a", "{STEP}"));  // shared by every animation (an older Project Attach)
        const EditOut o = AddPileLayer(st, "LIB", Scope::Anim, Pool("Walk_01"), 'L', {Node("c", "", SoundNodeType::Random, "FS"), Snd("cs", "c", "{CLOTH}")},
                                       20.0, &Uid);
        CHECK(o.ok && o.groups == 1 && !o.remap_pool.empty());  // Walk_01 got its own copy first
        CHECK(o.remap_pool.count("a") && o.remap_pool.count("as"));
        const SoundLibrary& lib = st.libraries[0];
        const Layout        lay = ReadLayout(st, lib);
        CHECK(lay.groups.size() == 2);
        if (lay.groups.size() == 2) {
            CHECK(lay.groups[0].pile == "a");  // the other animations keep the shared content
            const SoundNode* b = FindNode(lib, lay.groups[1].pile);
            CHECK(b && b->type == SoundNodeType::Blend);
            const std::vector<std::string> layers = PileLayers(lib, lay.groups[1].pile);
            CHECK(layers.size() == 2);
            if (layers.size() == 2) {
                CHECK(layers[0] == o.remap_pool.at("a") && layers[1] == "c");
                const SoundNode* c = FindNode(lib, "c");
                CHECK(c && std::fabs(c->offset_ms - 20.0) < 1e-9 && c->layer_keep_offset && c->name == "Layer 2");
                // The instance's layer from its Sound.
                const LayerAt la = LayerOfNode(lib, lay, "cs");
                CHECK(la.group == 1 && la.layer == "c" && la.slot == 0 && la.pile == lay.groups[1].pile);
                CHECK(LayerOfNode(lib, lay, o.remap_pool.at("as")).layer == o.remap_pool.at("a"));
                CHECK(LayerOfNode(lib, lay, "as").group == 0 && LayerOfNode(lib, lay, "as").layer == "a");
            }
        }
        // A second layer on that animation: "Layer 3", no new copy.
        const EditOut o2 = AddPileLayer(st, "LIB", Scope::Anim, Pool("Walk_01", {o.anim_value}), 'L',
                                        {Node("d", "", SoundNodeType::Random, "FS"), Snd("ds", "d", "{BREATH}")}, -5.0, &Uid);
        CHECK(o2.ok && o2.remap_pool.empty() && FindNode(st.libraries[0], "d") && FindNode(st.libraries[0], "d")->name == "Layer 3");
        // Project: every Group with content gets it (a copy each).
        const EditOut o3 = AddPileLayer(st, "LIB", Scope::Project, Pool("Walk_01"), 'L',
                                        {Node("e", "", SoundNodeType::Random, "FS"), Snd("es", "e", "{SCRAPE}")}, 0.0, &Uid);
        CHECK(o3.ok && o3.groups == 2);
        // Nothing attached: refused.
        SoundStore empty = Store();
        CHECK(!AddPileLayer(empty, "LIB", Scope::Project, Pool("W"), 'L', {Node("z", "", SoundNodeType::Random)}, 0.0, &Uid).ok);
    }

    // ---- Linked select: one instance's layer in the animation (L + R), Item = itself, Project = every copy.
    {
        std::vector<Instance> all = {In("A1", "W", "lay", "{STEP}"), In("A2", "W", "lay", "{STEP}"), In("A3", "W", "cl", "{CLOTH}"),
                                     In("B1", "R", "lay2", "{STEP}"), In("X1", "W", "", "")};
        std::vector<int> l = Linked(all, 0, Scope::Anim);
        CHECK(l.size() == 2 && l[0] == 0 && Has(l, 1));
        CHECK(Linked(all, 0, Scope::Item).size() == 1);
        l = Linked(all, 0, Scope::Project);
        CHECK(l.size() == 3 && Has(l, 3));  // Run's copy of the same sound
        CHECK(Linked(all, 4, Scope::Anim).size() == 1);  // attached on one marker only: itself
        std::vector<std::string> sel = ExpandSelection(all, {"A2"}, Scope::Anim, false);
        CHECK(sel.size() == 2 && sel[0] == "A2" && sel[1] == "A1");
        CHECK(ExpandSelection(all, {"A2"}, Scope::Anim, true).empty());       // Ctrl+click: that one only
        CHECK(ExpandSelection(all, {"A2"}, Scope::Item, false).empty());
        CHECK(ExpandSelection(all, {"A1", "A2"}, Scope::Anim, false).empty());
        CHECK(ExpandSelection(all, {"A3"}, Scope::Anim, false).empty());       // linked to nothing else
        // After Separate: L and R are two groups (their layers differ), not linked; Project keeps sides apart.
        all = {In("L1", "W", "lay", "{STEP}", 'L'), In("L2", "W", "lay", "{STEP}", 'L'), In("R1", "W", "layR", "{STEP}", 'R'),
               In("Q1", "R", "q", "{STEP}", 0)};
        CHECK(Linked(all, 2, Scope::Anim).size() == 1);
        l = Linked(all, 0, Scope::Project);
        CHECK(l.size() == 3 && !Has(l, 2) && Has(l, 3));
    }

    // ---- Separate L and R, Anim: L keeps the Pile, R a copy (two groups); Item: greyed (refused); Merge keeps a side.
    {
        SoundStore st = Store();
        EditOut    a = AttachContent(st, "LIB", Scope::Anim, Pool("Walk_01"), 'L', Content("a", "{STEP}"), &Uid);
        CHECK(!SeparateSides(st, "LIB", Scope::Item, Pool("Walk_01", {a.anim_value}), &Uid).ok);
        EditOut s = SeparateSides(st, "LIB", Scope::Anim, Pool("Walk_01", {a.anim_value}), &Uid);
        CHECK(s.ok && s.groups == 1 && s.remap_side.count("a") && s.remap_side.count("as"));
        Layout lay = ReadLayout(st, st.libraries[0]);
        CHECK(lay.groups.size() == 2);
        if (lay.groups.size() == 2) {
            const Group& g = lay.groups[1];
            CHECK(g.split && g.pile_l == "a" && g.pile_r == s.remap_side.at("a"));
            CHECK(PileOf(g, 'L') == "a" && PileOf(g, 'R') == s.remap_side.at("a") && PileOf(g, 0) == "a");
            CHECK(LayerOfNode(st.libraries[0], lay, s.remap_side.at("as")).slot == 'R');
            CHECK(LibraryUsesSide(st, st.libraries[0]));
            LrInput in;
            in.has_marker = true;
            in.kind = "FS";
            in.side = 'R';
            in.group = &g;
            const LrCard c = LrCardOf(in, Scope::Anim);
            CHECK(c.merge && c.enabled && c.pill == "Separate" && c.button == "Merge L and R (keep the R sound)" && c.text == "FS: L and R are two groups.");
        }
        CHECK(!SeparateSides(st, "LIB", Scope::Anim, Pool("Walk_01", {a.anim_value}), &Uid).ok);  // separate already
        // Add to pile on R: the R group only.
        const EditOut p = AddPileLayer(st, "LIB", Scope::Anim, Pool("Walk_01", {a.anim_value}), 'R',
                                       {Node("c", "", SoundNodeType::Random, "FS"), Snd("cs", "c", "{CLOTH}")}, 0.0, &Uid);
        CHECK(p.ok && p.slots.count('R') && PileLayers(st.libraries[0], "a").size() == 1);
        // Merge from an R marker: both sides play R's sound; L's Pile goes.
        const EditOut m = MergeSides(st, "LIB", Scope::Anim, Pool("Walk_01", {a.anim_value}), 'R', &Uid);
        CHECK(m.ok && m.dropped_side == 'L' && !FindNode(st.libraries[0], "a"));
        lay = ReadLayout(st, st.libraries[0]);
        CHECK(lay.groups.size() == 2 && !lay.groups[1].split && PileLayers(st.libraries[0], lay.groups[1].pile).size() == 2);
        CHECK(!MergeSides(st, "LIB", Scope::Anim, Pool("Walk_01", {a.anim_value}), 'R', &Uid).ok);
        // The card: together; Item greys it with the reason; a kind with no side too.
        LrInput lin;
        lin.has_marker = true;
        lin.kind = "FS";
        lin.side = 'L';
        lin.group = &lay.groups[1];
        const LrCard t = LrCardOf(lin, Scope::Item);
        CHECK(!t.merge && !t.enabled && t.pill == "Together" && t.text == "FS: L and R play the same sound." &&
              t.tip == "Anim or Project: L and R are groups of instances");
        LrInput body = lin;
        body.side = 0;
        CHECK(!LrCardOf(body, Scope::Anim).enabled);
        LrInput none = lin;
        none.group = &lay.groups[0];
        CHECK(LrCardOf(none, Scope::Anim).text == "Nothing attached on FS here yet.");
        // Project: Separate while any Group is together, Merge when every Group with content is separate.
        none.any_split = true;
        CHECK(LrCardOf(none, Scope::Project).merge && LrCardOf(none, Scope::Project).enabled);
        none.any_together = true;
        CHECK(!LrCardOf(none, Scope::Project).merge);
        LrInput unread = lin;
        unread.marker_read = false;
        CHECK(!LrCardOf(unread, Scope::Anim).enabled && !LrCardOf(unread, Scope::Anim).tip.empty());
        LrInput pick;
        CHECK(LrCardOf(pick, Scope::Anim).text == "Pick a marker.");
        // Project: every Group.
        SoundStore p2 = Store();
        SetLibraryContent(p2.libraries[0], Content("x", "{STEP}"));
        CHECK(SeparateSides(p2, "LIB", Scope::Project, Pool("Walk_01"), &Uid).ok);
        CHECK(ReadLayout(p2, p2.libraries[0]).groups[0].split);
    }

    // ---- Delete: one instance deleted by hand, Anim: its layer leaves every instance of the animation and its content.
    {
        std::vector<Instance> all = {In("A1", "W", "c", "{CLOTH}"), In("A2", "W", "c", "{CLOTH}"), In("A3", "W", "a", "{STEP}"),
                                     In("B1", "R", "c2", "{CLOTH}")};
        DeletePlan d = PlanPropagatedDelete(all, {"A1"}, Scope::Anim);
        CHECK(d.delete_guids.size() == 1 && d.delete_guids[0] == "A2" && d.removals.size() == 1 && d.removals[0].layer == "c");
        CHECK(PlanPropagatedDelete(all, {"A1"}, Scope::Item).delete_guids.empty());  // Item: that one only
        d = PlanPropagatedDelete(all, {"A1"}, Scope::Project);
        CHECK(d.delete_guids.size() == 2 && d.removals.size() == 1);
        d = PlanPropagatedDelete(all, {"A1", "A2"}, Scope::Anim);  // both deleted: one removal, nothing more to delete
        CHECK(d.delete_guids.empty() && d.removals.size() == 1);
        // The library edit: the Blend keeps Step alone.
        SoundStore st = Store();
        const EditOut a = AttachContent(st, "LIB", Scope::Anim, Pool("Walk_01"), 'L', Content("a", "{STEP}"), &Uid);
        AddPileLayer(st, "LIB", Scope::Anim, Pool("Walk_01", {a.anim_value}), 'L', {Node("c", "", SoundNodeType::Random), Snd("cs", "c", "{CLOTH}")},
                     10.0, &Uid);
        const EditOut r = RemoveLayer(st, "LIB", Scope::Anim, Pool("Walk_01", {a.anim_value}), "c", 0, &Uid);
        CHECK(r.ok && !FindNode(st.libraries[0], "c") && !FindNode(st.libraries[0], "cs") && FindNode(st.libraries[0], "a"));
        // The last layer: the Pile goes, the animation plays nothing (an empty Blend: never the other animations' sound).
        CHECK(RemoveLayer(st, "LIB", Scope::Anim, Pool("Walk_01", {a.anim_value}), "a", 0, &Uid).ok);
        const Layout le = ReadLayout(st, st.libraries[0]);
        CHECK(le.groups.size() == 2 && !le.groups[1].node.empty() && PileLayers(st.libraries[0], le.groups[1].pile).empty());
        CHECK(GroupOf(le, {a.anim_value}) == 1);
        // Anim on the shared content: the animation's own copy loses it, the others keep it.
        SoundStore sh = Store();
        SetLibraryContent(sh.libraries[0], {Node("b", "", SoundNodeType::Blend, "FS"), Node("a", "b", SoundNodeType::Random), Snd("as", "a", "{STEP}"),
                                            Node("c", "b", SoundNodeType::Random), Snd("cs", "c", "{CLOTH}")});
        const EditOut rs = RemoveLayer(sh, "LIB", Scope::Anim, Pool("Walk_01"), "c", 0, &Uid);
        const Layout  ls = ReadLayout(sh, sh.libraries[0]);
        CHECK(rs.ok && ls.groups.size() == 2 && PileLayers(sh.libraries[0], ls.groups[0].pile).size() == 2 &&
              PileLayers(sh.libraries[0], ls.groups[1].pile).size() == 1);
        // Project: every copy (same sources), in every animation's Group.
        SoundStore pj = Store();
        const EditOut w = AttachContent(pj, "LIB", Scope::Anim, Pool("Walk_01"), 'L', Content("w", "{STEP}"), &Uid);
        AttachContent(pj, "LIB", Scope::Anim, Pool("Run_02"), 'L', Content("q", "{STEP}"), &Uid);
        const EditOut rp = RemoveLayer(pj, "LIB", Scope::Project, Pool("Walk_01", {w.anim_value}), "w", 0, &Uid);
        CHECK(rp.ok && rp.groups == 2 && !FindNode(pj.libraries[0], "w") && !FindNode(pj.libraries[0], "q"));
    }

    // ---- Delete on one side after Separate: that side plays nothing (not the other side's sound), the other keeps it.
    {
        SoundStore    st = Store();
        const EditOut a = AttachContent(st, "LIB", Scope::Anim, Pool("Walk_01"), 'L', Content("a", "{STEP}"), &Uid);
        const EditOut s = SeparateSides(st, "LIB", Scope::Anim, Pool("Walk_01", {a.anim_value}), &Uid);
        const std::string r = s.remap_side.count("a") ? s.remap_side.at("a") : std::string();
        CHECK(RemoveLayer(st, "LIB", Scope::Anim, Pool("Walk_01", {a.anim_value}), r, 'R', &Uid).ok);
        const Layout lay = ReadLayout(st, st.libraries[0]);
        CHECK(lay.groups.size() == 2 && lay.groups[1].split && lay.groups[1].pile_l == "a");
        CHECK(lay.groups.size() == 2 && lay.groups[1].pile_r.empty() && !HasContent(Group()));  // an empty Blend: no content
        const SoundNode* ss = lay.groups.size() == 2 ? FindNode(st.libraries[0], lay.groups[1].node) : nullptr;
        CHECK(ss && ss->cases.size() == 2);  // the R case kept (plays nothing), never the L sound
        // Add to pile on R: nothing there, refused, the store untouched.
        const std::string before = SerializeSoundStore(st);
        CHECK(!AddPileLayer(st, "LIB", Scope::Anim, Pool("Walk_01", {a.anim_value}), 'R', {Node("z", "", SoundNodeType::Random)}, 0.0, &Uid).ok);
        CHECK(SerializeSoundStore(st) == before);
    }

    // ---- Volume / fades: the same change on the linked instances (volume by the same ratio); Item: none.
    {
        std::vector<Instance> all = {In("A1", "W", "lay", "s"), In("A2", "W", "lay", "s"), In("A3", "W", "other", "o")};
        Look l0;
        Look half = l0;
        half.vol = 0.5;
        Look quiet = l0;
        quiet.vol = 0.8;
        std::map<std::string, Look> before = {{"A1", l0}, {"A2", quiet}, {"A3", l0}}, now = before;
        now["A1"] = half;
        std::vector<LookWrite> w = PlanPropagatedLooks(all, before, now, Scope::Anim);
        CHECK(w.size() == 1 && w[0].guid == "A2" && std::fabs(w[0].look.vol - 0.4) < 1e-9);
        CHECK(PlanPropagatedLooks(all, before, now, Scope::Item).empty());
        Look faded = l0;
        faded.fade_in = 0.05;
        faded.in_shape = 2;
        now["A1"] = faded;
        w = PlanPropagatedLooks(all, before, now, Scope::Anim);
        CHECK(w.size() == 1 && std::fabs(w[0].look.fade_in - 0.05) < 1e-9 && w[0].look.in_shape == 2 && std::fabs(w[0].look.vol - 0.8) < 1e-9);
        CHECK(SameLook(l0, l0) && !SameLook(l0, faded));
    }

    // ---- Move: the drag of one instance moved its expanded selection: the others go back; a move apart is left.
    {
        const std::set<std::string>   ex = {"A1", "A2", "A3"};
        std::map<std::string, double> before = {{"A1", 1.0}, {"A2", 2.0}, {"A3", 3.0}, {"Z", 9.0}}, now = before;
        now["A1"] = 1.5;
        now["A2"] = 2.5;
        now["A3"] = 3.5;
        std::vector<MoveBack> m = PlanMoveBack(ex, "A2", before, now);
        CHECK(m.size() == 2 && m[0].guid == "A1" && std::fabs(m[0].pos - 1.0) < 1e-9 && m[1].guid == "A3");
        CHECK(PlanMoveBack(ex, "Q", before, now).empty());  // the dragged item is none of them
        now["A3"] = 3.2;
        CHECK(PlanMoveBack(ex, "A2", before, now).empty());  // moved apart
        now = before;
        now["A2"] = 2.5;
        CHECK(PlanMoveBack(ex, "A2", before, now).empty());  // only the dragged one moved
        now["A1"] = 1.5;
        now["Z"] = 9.5;
        CHECK(PlanMoveBack(ex, "A2", before, now).empty());  // something else moved too
    }

    // ---- Records and tags renamed after an edit (Separate's R copy, an animation's own copy).
    {
        EventSounds ev;
        ev.draws = {{"a", 2, false, {}, ""}, {"a#p1", 1, false, {}, ""}, {"z", 0, false, {}, ""}};
        SoundPlacement p;
        p.item_guid = "{I}";
        p.node = "as";
        ev.placed = {p};
        CHECK(RemapEventNodes(ev, {{"a", "A"}, {"as", "AS"}}));
        CHECK(ev.draws[0].node == "A" && ev.draws[1].node == "A#p1" && ev.draws[2].node == "z" && ev.placed[0].node == "AS");
        PlacedTag t;
        t.node = "as";
        PlacedTag::NodePrint np;
        np.node = "a";
        t.lib = {np};
        CHECK(RemapTagNodes(t, {{"a", "A"}, {"as", "AS"}}) && t.node == "AS" && t.lib[0].node == "A");
        CHECK(!RemapTagNodes(t, {{"q", "Q"}}));
    }

    // ---- The "<kind> animations" value: the animation's own, never one held outside its pool; an orphan group reused;
    // the Switch's own default is the default.
    {
        SoundStore  st = Store();
        SwitchGroup orphan;  // left by an earlier Switch
        orphan.uid = "G";
        orphan.name = "FS animations";
        orphan.rav_owned = true;
        orphan.values = {{"vo", "Other animations", {}, {}}, {"vw", "Walk_01", {}, {}}};
        orphan.default_value = "vo";
        st.groups.push_back(orphan);
        PoolKey p = Pool("Walk_01");
        p.held_elsewhere = {"vw"};  // another animation (not linked) holds "Walk_01"
        const EditOut a = AttachContent(st, "LIB", Scope::Anim, p, 'L', Content("a", "{STEP}"), &Uid);
        CHECK(a.ok && st.groups.size() == 1 && a.anim_value.group_uid == "G" && a.anim_value.value_uid != "vw");
        CHECK(st.groups[0].values.back().name == "Walk_01 2");
        // Its value read back from its items: the same case.
        const EditOut b = AttachContent(st, "LIB", Scope::Anim, Pool("Walk_01", {a.anim_value}), 'L', Content("b", "{X}"), &Uid);
        CHECK(b.anim_value.value_uid == a.anim_value.value_uid && ReadLayout(st, st.libraries[0]).groups.size() == 2);
        // The Switch's own default overrides the group's.
        SoundNode* sw = FindNode(st.libraries[0], st.libraries[0].root);
        CHECK(sw != nullptr);
        if (sw) sw->default_value = a.anim_value.value_uid;
        const Layout l = ReadLayout(st, st.libraries[0]);
        CHECK(l.default_value == a.anim_value.value_uid && l.groups[0].pile == "b");
    }

    // ---- Attach after Separate, on R: only R's Pile replaced, still separate (Anim and Project).
    {
        for (Scope sc : {Scope::Anim, Scope::Project}) {
            SoundStore    st = Store();
            const EditOut a = AttachContent(st, "LIB", sc, Pool("Walk_01"), 'L', Content("a", "{STEP}"), &Uid);  // Project: shared
            CHECK(SeparateSides(st, "LIB", sc, Pool("Walk_01", {a.anim_value}), &Uid).ok);
            const EditOut r = AttachContent(st, "LIB", sc, Pool("Walk_01", {a.anim_value}), 'R', Content(sc == Scope::Anim ? "r" : "rp", "{GRAVEL}"), &Uid);
            CHECK(r.ok && r.slots.size() == 1 && r.slots.count('R'));
            const Layout lay = ReadLayout(st, st.libraries[0]);
            const Group& g = lay.groups.back();  // Anim: the animation's own; Project: the shared one
            CHECK(g.split && g.pile_l == "a" && LayerSig(st.libraries[0], g.pile_r) == "{GRAVEL}");
        }
    }

    // ---- Project delete on a split Group from an L instance: the L layer goes, R's copy stays; a Pile holding two
    // copies of one layer loses both.
    {
        SoundStore    st = Store();
        SetLibraryContent(st.libraries[0], Content("a", "{STEP}"));
        CHECK(SeparateSides(st, "LIB", Scope::Project, Pool("Walk_01"), &Uid).ok);
        CHECK(RemoveLayer(st, "LIB", Scope::Project, Pool("Walk_01"), "a", 'L', &Uid).ok);
        Layout lay = ReadLayout(st, st.libraries[0]);
        CHECK(lay.groups.size() == 1 && lay.groups[0].split && lay.groups[0].pile_l.empty() && LayerSig(st.libraries[0], lay.groups[0].pile_r) == "{STEP}");
        SoundStore two = Store();
        SetLibraryContent(two.libraries[0], {Node("b", "", SoundNodeType::Blend, "FS"), Node("x", "b", SoundNodeType::Random), Snd("xs", "x", "{CLOTH}"),
                                             Node("y", "b", SoundNodeType::Random), Snd("ys", "y", "{CLOTH}"), Node("z", "b", SoundNodeType::Random),
                                             Snd("zs", "z", "{STEP}")});
        CHECK(RemoveLayer(two, "LIB", Scope::Project, Pool("Walk_01"), "x", 0, &Uid).ok);
        CHECK(!FindNode(two.libraries[0], "x") && !FindNode(two.libraries[0], "y") && FindNode(two.libraries[0], "z"));
    }

    // ---- A library an earlier version laid out (v3's "FS layers" Switch in the root Blend): every edit refused, untouched.
    {
        SoundStore  st = Store();
        SwitchGroup g;
        g.uid = "OLD";
        g.name = "FS layers";
        g.rav_owned = true;
        st.groups.push_back(g);
        SoundNode sw = Node("s", "b", SoundNodeType::Switch, "FS layers");
        sw.group_uid = "OLD";
        SetLibraryContent(st.libraries[0], {Node("b", "", SoundNodeType::Blend, "FS"), Node("a", "b", SoundNodeType::Random), Snd("as", "a", "{STEP}"), sw});
        std::string why;
        CHECK(LegacyLayout(st, st.libraries[0], &why) && !why.empty());
        const std::string before = SerializeSoundStore(st);
        const EditOut     o = AddPileLayer(st, "LIB", Scope::Project, Pool("W"), 'L', {Node("c", "", SoundNodeType::Random)}, 0.0, &Uid);
        CHECK(!o.ok && o.why == why && SerializeSoundStore(st) == before);
        CHECK(!RemoveLayer(st, "LIB", Scope::Project, Pool("W"), "a", 0, &Uid).ok);
        // A v5 library is not legacy.
        SoundStore v5 = Store();
        AttachContent(v5, "LIB", Scope::Anim, Pool("Walk_01"), 'L', Content("a", "{STEP}"), &Uid);
        CHECK(!LegacyLayout(v5, v5.libraries[0], nullptr));
    }

    // ---- The watcher's decisions.
    {
        WatchSnap last;
        last.inst = {In("A1", "W", "lay", "s"), In("A2", "W", "lay", "s")};
        for (Instance& i : last.inst) i.owner = "{OWN}";
        last.present = {"A1", "A2", "{OWN}"};
        last.owners = {{"{OWN}", 0.0}};
        last.selected = {"A1"};
        last.pos = {{"A1", 1.0}, {"A2", 2.0}};
        last.looks = {{"A1", Look()}, {"A2", Look()}};
        WatchSnap now = last;
        now.present.erase("A1");
        now.inst.erase(now.inst.begin());
        now.looks.erase("A1");
        now.pos.erase("A1");
        now.selected.clear();
        WatchIn in;
        in.top = "Remove items";
        in.new_undo = true;
        // A delete: A1 propagates.
        WatchOut o = DecideWatch(last, now, in);
        CHECK(o.gone.size() == 1 && o.gone.count("A1"));
        // Glue removes A1 too: not a delete.
        in.top = "Glue items";
        CHECK(DecideWatch(last, now, in).gone.empty());
        // A cut (REAPER's localized name): the follow's move, nothing.
        in.top = "Couper des objets";
        in.cut_names = {"Couper des objets"};
        CHECK(DecideWatch(last, now, in).gone.empty());
        in.cut_names.clear();
        // An undo (a redo waits), a RAV undo point, or no new undo point: nothing.
        in.top = "Remove items";
        in.redo = true;
        CHECK(DecideWatch(last, now, in).gone.empty());
        in.redo = false;
        in.new_undo = false;
        CHECK(DecideWatch(last, now, in).gone.empty());
        in.new_undo = true;
        in.top = "RAV: Populate";
        CHECK(DecideWatch(last, now, in).gone.empty());
        in.scope = Scope::Item;
        in.top = "Remove items";
        CHECK(DecideWatch(last, now, in).gone.empty());
        in.scope = Scope::Anim;

        // Moves: the expanded selection dragged moves back; a ripple shift (no click) or an animation moved does not.
        WatchSnap m0 = last;
        m0.selected = {"A1", "A2"};
        WatchSnap m1 = m0;
        m1.pos = {{"A1", 1.5}, {"A2", 2.5}};
        WatchIn mv;
        mv.top = "Move items";
        mv.new_undo = true;
        mv.clicked = true;
        mv.expanded = {"A1", "A2"};
        mv.anchor = "A1";
        mv.under_mouse = "A2";
        o = DecideWatch(m0, m1, mv);
        CHECK(o.back.size() == 1 && o.back[0].guid == "A1" && std::fabs(o.back[0].pos - 1.0) < 1e-9);
        mv.clicked = false;  // ripple editing, insert time, a keyboard nudge
        CHECK(DecideWatch(m0, m1, mv).back.empty());
        mv.clicked = true;
        m1.owners["{OWN}"] = 0.5;  // the animation item moved: RAV's follow moved its sounds
        CHECK(DecideWatch(m0, m1, mv).back.empty());

        // A click on one instance selects its linked instances; Ctrl+click: that one only; no click (undo, keyboard): nothing.
        WatchSnap c0 = last;
        c0.selected.clear();
        WatchSnap c1 = last;
        c1.selected = {"A2"};
        WatchIn ck;
        ck.top = "Change media item selection";
        ck.clicked = true;
        o = DecideWatch(c0, c1, ck);
        CHECK(o.select.size() == 2 && o.select[0] == "A2");
        ck.ctrl = true;
        CHECK(DecideWatch(c0, c1, ck).select.empty());
        ck.ctrl = false;
        ck.clicked = false;
        CHECK(DecideWatch(c0, c1, ck).select.empty());
    }

    if (g_fail) {
        std::printf("attach_manager_plan_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("attach_manager_plan_test: all passed\n");
    return 0;
}
