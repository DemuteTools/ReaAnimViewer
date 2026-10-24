// SPDX-License-Identifier: MIT
//
// Host test of src/sound_sync_plan.h (13-35): node prints and what makes a placed sound behind (volume, Blend offset,
// fades, zones, an excluded variation; measured data never), the change texts, the hand edits and what Keep /
// Overwrite do with them, the project's rule token, and the Library's view: the card, the marks, the where-dialog's
// lines, the hand list and the toast (the mockup's Stone / Grass / Jump_Land scenario).

#include "sound_sync_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "library_tree_edit.h"  // 13-35b: MoveRows (fd304ef: a drag onto a taken value)
#include "sound_record.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)
static bool Near(double a, double b, double e = 1e-9) { return std::fabs(a - b) <= e; }

static SoundNode Node(const char* uid, const char* parent, SoundNodeType type, const char* name, double vol = 0.0)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = type;
    n.name = name;
    n.volume_db = vol;
    return n;
}

static SoundNode Sound(const char* uid, const char* parent, const char* name, int frames, double vol = 0.0)
{
    SoundNode n = Node(uid, parent, SoundNodeType::Sound, name, vol);
    for (int k = 0; k < frames; ++k) {
        SoundFrame f;
        f.start = k * 1.0;
        f.length = 0.5;
        EditZone z;
        z.a = f.start;
        z.b = f.start + f.length;
        z.part = kWholePart;
        f.zones.push_back(z);
        n.sound.frames.push_back(f);
    }
    return n;
}

// Footsteps: root Random > LocomotionType Switch (Run, Walk) > Surface Switch > Stone / Grass Sounds; Jump_Land Blend >
// Body / Cloth Sounds.
static SoundStore Store()
{
    SoundStore  st;
    SwitchGroup loco;
    loco.uid = "gLoco";
    loco.name = "LocomotionType";
    loco.values = {SwitchValue{"vRun", "Run", {}, {}}, SwitchValue{"vWalk", "Walk", {}, {}}};
    SwitchGroup surf;
    surf.uid = "gSurf";
    surf.name = "Surface Material";
    surf.values = {SwitchValue{"vStone", "Stone", {}, {}}, SwitchValue{"vGrass", "Grass", {}, {}}};
    st.groups = {loco, surf};
    SoundLibrary lib;
    lib.uid = "libFS";
    lib.name = "Footsteps";
    lib.root = "root";
    lib.nodes.push_back(Node("root", "", SoundNodeType::Random, "Footsteps"));
    SoundNode sw = Node("loco", "root", SoundNodeType::Switch, "LocomotionType");
    sw.group_uid = "gLoco";
    sw.cases = {SwitchCase{"vRun", "runS", {}}, SwitchCase{"vWalk", "walkS", {}}};
    lib.nodes.push_back(sw);
    SoundNode rs = Node("runS", "loco", SoundNodeType::Switch, "Surface Material");
    rs.group_uid = "gSurf";
    rs.cases = {SwitchCase{"vStone", "runStone", {}}};
    lib.nodes.push_back(rs);
    SoundNode ws = Node("walkS", "loco", SoundNodeType::Switch, "Surface Material");
    ws.group_uid = "gSurf";
    ws.cases = {SwitchCase{"vGrass", "walkGrass", {}}};
    lib.nodes.push_back(ws);
    lib.nodes.push_back(Sound("runStone", "runS", "Stone", 3, -3.0));
    lib.nodes.push_back(Sound("walkGrass", "walkS", "Grass", 6, -6.0));
    lib.nodes.push_back(Node("jump", "root", SoundNodeType::Blend, "Jump_Land", -2.0));
    SoundNode cloth = Sound("jCloth", "jump", "FS_Land_Cloth", 2);
    cloth.offset_ms = 12.0;
    lib.nodes.push_back(cloth);
    st.libraries.push_back(lib);
    return st;
}

static SoundNode* N(SoundStore& st, const char* uid) { return FindNode(st.libraries[0], uid); }

static PlacedWrite Wr(double vol = 0.5, double len = 0.5)
{
    PlacedWrite w;
    w.vol = vol;
    w.fade_in = 0.001;
    w.fade_out = 0.005;
    w.length = len;
    return w;
}

// A placed item of `owner` for node `node` variation `va`, stamped from `st`.
static SyncPlacedItem Placed(const SoundStore& st, const char* guid, const char* owner, const char* node, int va, double t)
{
    SyncPlacedItem it;
    it.guid = guid;
    it.owner = owner;
    it.name = std::string("snd_") + guid;
    it.tag.owner = owner;
    it.tag.rid = "r1";
    it.tag.t = t;
    it.tag.node = node;
    it.tag.own = Ownership::Populated;
    it.tag.va = va;
    it.tag.lib = PathPrints(st, *LibraryOfNode(st, node), node, va);
    it.tag.has_wr = true;
    it.tag.wr = Wr();
    it.now = Wr();
    it.anchor = t;
    return it;
}

// 13-35b: a Surface Switch (Wood -> y, Grass -> Random gr {x, z}) and a Blend B {a}, under a Random root.
static SoundStore Tree()
{
    SoundStore  st;
    SwitchGroup mat;
    mat.uid = "gmat";
    mat.name = "Surface";
    mat.values = {SwitchValue{"vwood", "Wood", {}, {}}, SwitchValue{"vgrass", "Grass", {}, {}},
                  SwitchValue{"vconc", "Concrete", {}, {}}};
    st.groups = {mat};
    SoundLibrary lib;
    lib.uid = "L";
    lib.name = "Steps";
    lib.root = "root";
    lib.nodes.push_back(Node("root", "", SoundNodeType::Random, "Steps"));
    SoundNode sw = Node("sw", "root", SoundNodeType::Switch, "Surface");
    sw.group_uid = "gmat";
    sw.cases = {SwitchCase{"vwood", "y", {}}, SwitchCase{"vgrass", "gr", {}}};
    lib.nodes.push_back(sw);
    lib.nodes.push_back(Sound("y", "sw", "Y", 2));
    lib.nodes.push_back(Node("gr", "sw", SoundNodeType::Random, "GrassR"));
    lib.nodes.push_back(Sound("x", "gr", "X", 2));
    lib.nodes.push_back(Sound("z", "gr", "Z", 2));
    lib.nodes.push_back(Node("B", "root", SoundNodeType::Blend, "B"));
    lib.nodes.push_back(Sound("a", "B", "A", 2));
    st.libraries.push_back(lib);
    return st;
}

static std::string CaseOf(const SoundStore& st, const char* sw, const char* value)
{
    for (const SwitchCase& c : FindNode(st.libraries[0], sw)->cases)
        if (c.value_uid == value) return c.child_uid;
    return {};
}

static void TestReshaped()
{
    const SoundStore base = Tree();
    const PlacedTag  ty = Placed(base, "y1", "i1", "y", 0, 1.0).tag;
    const PlacedTag  tz = Placed(base, "z1", "i2", "z", 1, 1.0).tag;
    const PlacedTag  ta = Placed(base, "a1", "i3", "a", 0, 1.0).tag;
    // Every print of a container has a structure (never 0); a Sound's is 0.
    CHECK(ty.lib.size() == 3 && ty.lib[0].st == 0 && ty.lib[1].st != 0 && ty.lib[2].st != 0);
    CHECK(ReshapeOf(ty, base).kind == Reshape::None && ReshapedNodes(tz, base).empty() && ReshapedNodes(ta, base).empty());
    const DropTarget wood{"L", {"sw", "vwood"}, DropMode::OntoValue};
    // Drag onto a taken value: X onto Wood (plays Y) -> Random "Wood" {Y, X}: Y's sounds reshaped, marked on that Random;
    // Z's too (its Random lost X), marked on GrassR.
    {
        SoundStore s = base;
        CHECK(MoveRows(s, "L", {{"gr", "x"}}, wood, nullptr));
        const std::string hu = CaseOf(s, "sw", "vwood");
        CHECK(!hu.empty() && hu != "y");
        ReshapeVerdict v = ReshapeOf(ty, s);
        CHECK(v.kind == Reshape::Moved && v.mark == hu && v.lib == "L");
        CHECK(std::string(ReshapeText(v.kind)) == "moved");
        v = ReshapeOf(tz, s);
        CHECK(v.kind == Reshape::Content && v.mark == "gr");
        CHECK(ReshapedNodes(ta, s).empty());
        CHECK(BehindNodes(ty, s).empty() && BehindNodes(tz, s).empty());  // 13-35 unchanged: not behind
        // The card and the marks count them, the text says to Populate again.
        std::vector<SyncPlacedItem> items = {Placed(base, "y1", "i1", "y", 0, 1.0), Placed(base, "z1", "i2", "z", 1, 1.0),
                                             Placed(base, "a1", "i3", "a", 0, 1.0)};
        const std::vector<SyncOwner> owners = {{"i1", "Run", "T", false}, {"i2", "Walk", "T", false}, {"i3", "Jump", "T", false}};
        const SyncState st = BuildSyncState(s, items, owners);
        CHECK(st.sounds.size() == 2 && st.sounds[0].reshaped && st.sounds[0].behind.empty());
        const SyncCard c = SyncCardOf(st, "L");
        CHECK(c.sounds == 2 && c.reshaped == 2 && c.items == 2 && c.changes == 2);
        CHECK(SyncCardText(c) == "2 changes not yet on 2 placed sounds in 2 items. They follow a structure change of the library: "
                                 "Populate again to update them.");
        const std::map<std::string, int> marks = SyncMarksOf(st, "L");
        CHECK(marks.size() == 2 && marks.at(hu) == 1 && marks.at("gr") == 1);
        CHECK(SyncMarkWhat(st, hu) == "moved" && SyncMarkReshaped(st, hu));
        CHECK(SyncMarkWhat(st, "gr") == "content changed (Populate again to update)");
        std::vector<SyncItemLine> lines = SyncItemLines(st, "L", "");
        CHECK(lines.size() == 2 && lines[0].reshaped == 1 && lines[0].sounds == 1 && lines[0].nodes.size() == 1 &&
              lines[0].nodes[0] == hu);
        CHECK(SyncItemLines(st, "L", "gr").size() == 1);
    }
    // Alt swap: X onto Wood, Y goes to X's place in GrassR: Y's sounds reshaped, marked on GrassR.
    {
        SoundStore s = base;
        CHECK(MoveRows(s, "L", {{"gr", "x"}}, wood, nullptr, TakenValue::Swap));
        CHECK(CaseOf(s, "sw", "vwood") == "x" && FindNode(s.libraries[0], "y")->parent == "gr");
        const ReshapeVerdict v = ReshapeOf(ty, s);
        CHECK(v.kind == Reshape::Moved && v.mark == "gr");
    }
    // The value now plays another (Wood re-pointed to W), Y stays under the Switch (Unassigned): marked on the Switch.
    {
        SoundStore s = base;
        s.libraries[0].nodes.push_back(Sound("w", "sw", "W", 2));
        SoundNode* sw = FindNode(s.libraries[0], "sw");
        sw->cases[0].child_uid = "w";
        sw->unassigned.push_back("y");
        const ReshapeVerdict v = ReshapeOf(ty, s);
        CHECK(v.kind == Reshape::Content && v.mark == "sw");
        CHECK(std::string(ReshapeText(v.kind)) == "content changed (Populate again to update)");
        // The default value now played by another: Y played Wood (the default, the group's first value).
        SoundStore d = base;
        FindNode(d.libraries[0], "sw")->default_value = "vgrass";
        CHECK(ReshapeOf(ty, d).kind == Reshape::Content && ReshapeOf(ty, d).mark == "sw");
        // Another value given to Y too: its case changed.
        SoundStore e = base;
        FindNode(e.libraries[0], "sw")->cases.push_back(SwitchCase{"vconc", "y", {}});
        CHECK(ReshapeOf(ty, e).kind == Reshape::Content);
    }
    // The node deleted: counted on the library's root row.
    {
        SoundStore s = base;
        std::vector<SoundNode>& ns = s.libraries[0].nodes;
        ns.erase(std::remove_if(ns.begin(), ns.end(), [](const SoundNode& n) { return n.uid == "y"; }), ns.end());
        const ReshapeVerdict v = ReshapeOf(ty, s);
        CHECK(v.kind == Reshape::Removed && v.mark == "root" && v.lib == "L" && std::string(ReshapeText(v.kind)) == "removed");
        const SyncState st = BuildSyncState(s, {Placed(base, "y1", "i1", "y", 0, 1.0)}, {{"i1", "Run", "T", false}});
        CHECK(st.sounds.size() == 1 && SyncMarksOf(st, "L").at("root") == 1 && SyncCardOf(st, "L").reshaped == 1);
        // Its library gone too: nowhere to show it.
        SoundStore g = base;
        g.libraries.clear();
        CHECK(ReshapeOf(ty, g).kind == Reshape::Removed && ReshapeOf(ty, g).lib.empty());
        CHECK(BuildSyncState(g, {Placed(base, "y1", "i1", "y", 0, 1.0)}, {{"i1", "Run", "T", false}}).sounds.empty());
    }
    // A Blend layer added: A's sounds marked on the Blend; a layer's role or keep-offset too.
    {
        SoundStore s = base;
        s.libraries[0].nodes.push_back(Sound("b", "B", "Bb", 1));
        CHECK(ReshapeOf(ta, s).kind == Reshape::Content && ReshapeOf(ta, s).mark == "B");
        SoundStore r = base;
        FindNode(r.libraries[0], "a")->layer_roles = {"heel"};
        CHECK(ReshapeOf(ta, r).mark == "B");
        SoundStore k = base;
        FindNode(k.libraries[0], "a")->layer_keep_offset = true;
        CHECK(ReshapeOf(ta, k).kind == Reshape::Content);
    }
    // A Random gaining or losing a sound, or its avoid-repeat: reshaped. Its children's order: not (it picks at random).
    {
        SoundStore s = base;
        s.libraries[0].nodes.push_back(Sound("q", "gr", "Q", 1));
        CHECK(ReshapeOf(tz, s).mark == "gr");
        SoundStore o = base;
        std::vector<SoundNode>& ns = o.libraries[0].nodes;
        std::iter_swap(std::find_if(ns.begin(), ns.end(), [](const SoundNode& n) { return n.uid == "x"; }),
                       std::find_if(ns.begin(), ns.end(), [](const SoundNode& n) { return n.uid == "z"; }));
        CHECK(ReshapeOf(tz, o).kind == Reshape::None);
        SoundStore a = base;
        FindNode(a.libraries[0], "gr")->avoid_repeat = 1;
        CHECK(ReshapeOf(tz, a).kind == Reshape::Content);
    }
    // Renames, colours, lands_in, was_* fields, settings and measured data: never reshaped.
    {
        SoundStore s = base;
        FindNode(s.libraries[0], "gr")->name = "Grass bits";
        FindNode(s.libraries[0], "sw")->name = "Mat";
        FindNode(s.libraries[0], "y")->name = "Y2";
        FindNode(s.libraries[0], "gr")->lands_in = "folder";
        FindNode(s.libraries[0], "gr")->was_path = "a > b";
        FindNode(s.libraries[0], "sw")->preview_value = "vgrass";
        s.libraries[0].color = 0x1ff0000;
        s.groups[0].values[0].name = "Oak";
        FindNode(s.libraries[0], "y")->sound.frames[0].syncs = {0.1};
        CHECK(ReshapeOf(ty, s).kind == Reshape::None && ReshapeOf(tz, s).kind == Reshape::None && BehindNodes(ty, s).empty());
        FindNode(s.libraries[0], "sw")->volume_db = -3.0;  // a setting: behind (13-35), not reshaped
        CHECK(ReshapeOf(ty, s).kind == Reshape::None && BehindNodes(ty, s).size() == 1);
    }
    // An older tag (no st): only the path / gone checks.
    {
        PlacedTag old = ty;
        for (NodePrint& p : old.lib) p.st = 0;
        SoundStore s = base;
        FindNode(s.libraries[0], "sw")->cases.push_back(SwitchCase{"vconc", "y", {}});
        CHECK(ReshapeOf(old, s).kind == Reshape::None);
        SoundStore m = base;
        CHECK(MoveRows(m, "L", {{"gr", "x"}}, wood, nullptr));
        CHECK(ReshapeOf(old, m).kind == Reshape::Moved);
    }
    // Both behind and reshaped: reshaped only (Sync would place it from a stale shape); never asked about by hand; the
    // item is placed again (SplitSyncOwners), the others synced.
    {
        SoundStore s = base;
        FindNode(s.libraries[0], "a")->volume_db = -4.0;  // a: behind only
        FindNode(s.libraries[0], "y")->volume_db = -4.0;  // y: behind ...
        CHECK(MoveRows(s, "L", {{"gr", "x"}}, wood, nullptr));  // ... and reshaped
        std::vector<SyncPlacedItem> items = {Placed(base, "y1", "i1", "y", 0, 1.0), Placed(base, "a1", "i3", "a", 0, 1.0)};
        items[0].now.vol = 0.25;  // edited by hand
        items[1].now.vol = 0.25;
        const SyncState st = BuildSyncState(s, items, {{"i1", "Run", "T", false}, {"i3", "Jump", "T", false}});
        CHECK(st.sounds.size() == 2 && st.sounds[0].reshaped && st.sounds[0].behind.empty() && !st.sounds[1].reshaped &&
              st.sounds[1].behind.size() == 1);
        CHECK(!SyncMarksOf(st, "L").count("y"));
        const SyncCard c = SyncCardOf(st, "L");
        CHECK(c.sounds == 2 && c.reshaped == 1);
        CHECK(SyncCardText(c) == "2 changes not yet on 2 placed sounds in 2 items. 1 of them follows a structure change of the "
                                 "library: Populate again to update it.");
        CHECK(SyncHandSounds(st, "L", "", {"i1", "i3"}).size() == 1 && SyncHandSounds(st, "L", "", {"i1"}).empty());
        const std::vector<SyncItemLine> lines = SyncItemLines(st, "L", "");
        CHECK(lines.size() == 2 && lines[0].reshaped == 1 && lines[0].hand == 0 && lines[1].reshaped == 0 && lines[1].hand == 1);
        // An item holding a reshaped sound: its other hand-edited sounds are not asked about (Populate places it again).
        std::vector<SyncPlacedItem> mixed = items;
        mixed.push_back(Placed(base, "a2", "i1", "a", 0, 2.0));
        mixed.back().now.vol = 0.25;
        const std::vector<SyncItemLine> ml = SyncItemLines(BuildSyncState(s, mixed, {{"i1", "Run", "T", false}}), "L", "");
        CHECK(ml.size() == 1 && ml[0].reshaped == 1 && ml[0].sounds == 2 && ml[0].hand == 0);
        SyncSplit sp = SplitSyncOwners(st, "L", "", {"i1", "i3"});
        CHECK(sp.populate.size() == 1 && sp.populate[0] == "i1" && sp.sync.size() == 1 && sp.sync[0] == "i3");
        sp = SplitSyncOwners(st, "L", "a", {"i1", "i3"});  // a node-scoped dialog: i1's reshaped sound is not in it
        CHECK(sp.populate.empty() && sp.sync.size() == 2);
        SyncCounts n;
        n.adjusted = 1;
        n.repopulated = 2;
        CHECK(SyncToastText(n, false) == "One undo point \xE2\x80\x9CRAV: Sync placed sounds\xE2\x80\x9D: 1 adjusted, 2 items placed again");
    }
    // Populate after: the kept sound takes the path and structure now (the mark clears), its settings as placed (a
    // setting changed stays behind).
    {
        SoundStore s = base;
        CHECK(MoveRows(s, "L", {{"gr", "x"}}, wood, nullptr));
        FindNode(s.libraries[0], "sw")->volume_db = -2.0;
        PlacedTag kept = ty;
        CHECK(AdoptStructure(kept.lib, PathPrints(s, s.libraries[0], "y", 0)));
        CHECK(ReshapeOf(kept, s).kind == Reshape::None);
        const std::vector<std::string> b = BehindNodes(kept, s);
        CHECK(b.size() == 1 && b[0] == "sw");
        CHECK(!AdoptStructure(kept.lib, PathPrints(s, s.libraries[0], "y", 0)));  // nothing left to adopt
        // A fresh stamp (a sound placed again) clears both.
        PlacedTag fresh = ty;
        fresh.lib = PathPrints(s, s.libraries[0], "y", 0);
        CHECK(ReshapeOf(fresh, s).kind == Reshape::None && BehindNodes(fresh, s).empty());
    }
}

int main()
{
    TestReshaped();
    // ---- Prints and behind ---------------------------------------------------------------------
    {
        SoundStore     st = Store();
        const PlacedTag tag = Placed(st, "a", "i1", "runStone", 1, 1.0).tag;
        CHECK(tag.lib.size() == 4 && tag.lib[0].node == "runStone" && tag.lib[3].node == "root");
        CHECK(BehindNodes(tag, st).empty());
        // A volume change: behind on that node, described with both values.
        N(st, "runStone")->volume_db = -6.0;
        std::vector<std::string> b = BehindNodes(tag, st);
        CHECK(b.size() == 1 && b[0] == "runStone");
        const NodePrint now = PrintOf(*N(st, "runStone"), 1);
        CHECK(DescribeChange(tag.lib[0], now, 1) == "Volume \xE2\x88\x92" "3 dB \xE2\x86\x92 \xE2\x88\x92" "6 dB");
        CHECK(!ChangeRedraws(tag.lib[0], now));
        // A node above changes too: both behind, node first.
        N(st, "loco")->volume_db = 1.5;
        b = BehindNodes(tag, st);
        CHECK(b.size() == 2 && b[0] == "runStone" && b[1] == "loco");
        // Measured data (sync points, trims, an automatic snap) never makes it behind.
        SoundStore m = Store();
        N(m, "runStone")->sound.frames[1].syncs = {1.01};
        N(m, "runStone")->sound.frames[1].part_trims = {1.0, 1.4};
        N(m, "runStone")->sound.frames[1].zones[0].auto_snap.set = true;
        N(m, "runStone")->sound.frames[1].zones[0].auto_snap.t = 1.02;
        CHECK(BehindNodes(tag, m).empty());
        // Another variation's settings leave it alone; its own variation's fades, zones and level do not.
        N(m, "runStone")->sound.frames[0].db = -4.0;
        CHECK(BehindNodes(tag, m).empty());
        SoundStore f = Store();
        N(f, "runStone")->sound.frames[1].zones[0].fade_out = 0.04;
        CHECK(BehindNodes(tag, f).size() == 1 && DescribeChange(tag.lib[0], PrintOf(*N(f, "runStone"), 1), 1) == "fades");
        SoundStore z = Store();
        N(z, "runStone")->sound.frames[1].zones[0].b = 1.45;
        CHECK(DescribeChange(tag.lib[0], PrintOf(*N(z, "runStone"), 1), 1) == "zones");
        SoundStore l = Store();
        N(l, "runStone")->sound.frames[1].zones[0].db = -2.0;
        N(l, "runStone")->sound.fade_in = 0.003;
        CHECK(DescribeChange(tag.lib[0], PrintOf(*N(l, "runStone"), 1), 1) == "fades, level");
        // The variation it played excluded: a redraw, said with its number (1-based).
        SoundStore x = Store();
        N(x, "runStone")->sound.frames[1].on = false;
        const NodePrint off = PrintOf(*N(x, "runStone"), 1);
        CHECK(ChangeRedraws(tag.lib[0], off) && DescribeChange(tag.lib[0], off, 1) == "variation 2 excluded");
        // A structure change (the node moved, deleted) or an old tag: not behind (Populate's).
        SoundStore s = Store();
        N(s, "runStone")->parent = "walkS";
        N(s, "runStone")->volume_db = -9.0;
        CHECK(BehindNodes(tag, s).empty());
        SoundStore d = Store();
        d.libraries[0].nodes.erase(d.libraries[0].nodes.begin() + 4);
        CHECK(BehindNodes(tag, d).empty());
        PlacedTag old = tag;
        old.lib.clear();
        CHECK(BehindNodes(old, st).empty());
        // Blend offset.
        SoundStore o = Store();
        const PlacedTag ct = Placed(o, "c", "i4", "jCloth", 0, 2.0).tag;
        N(o, "jCloth")->offset_ms = 18.0;
        CHECK(DescribeChange(ct.lib[0], PrintOf(*N(o, "jCloth"), 0), 0) == "Blend offset 12 \xE2\x86\x92 18 ms");
        // A re-Populate applied the volume: every print's volume is adopted, whatever else differs (that stays behind).
        std::vector<NodePrint> stored = tag.lib;
        SoundStore             v = Store();
        N(v, "runStone")->volume_db = -6.0;
        CHECK(AdoptVolumes(stored, PathPrints(v, v.libraries[0], "runStone", 1)) && stored[0].vol == -6.0);
        PlacedTag adopted = tag;
        adopted.lib = stored;
        CHECK(BehindNodes(adopted, v).empty());
        std::vector<NodePrint> s2 = tag.lib;
        N(v, "runStone")->sound.frames[1].zones[0].db = 1.0;
        CHECK(AdoptVolumes(s2, PathPrints(v, v.libraries[0], "runStone", 1)) && s2[0].vol == -6.0);
        PlacedTag still = tag;
        still.lib = s2;
        CHECK(BehindNodes(still, v).size() == 1 &&
              DescribeChange(s2[0], PrintOf(*N(v, "runStone"), 1), 1) == "level");  // no "Volume" left in the chip
        // The kept item's tag after that Populate: prints adopted, what RAV wrote follows the ratio: nothing behind, no
        // hand edit.
        {
            SoundStore w = Store();
            PlacedTag  kt = tag;
            kt.has_wr = true;
            kt.wr = Wr(0.5);
            N(w, "runStone")->volume_db = -6.0;
            const double was = 0.5, now_vol = 0.5 * std::pow(10.0, -3.0 / 20.0);
            KeptTagAfterVolume(kt, PathPrints(w, w.libraries[0], "runStone", 1), was, now_vol);
            CHECK(BehindNodes(kt, w).empty());
            PlacedWrite item = Wr(now_vol);
            CHECK(Near(kt.wr.vol, now_vol) && !HandEdits(kt.wr, item, 0.0).Any());
            // A user's -4 dB on the item stays a hand edit.
            PlacedTag ut = tag;
            ut.has_wr = true;
            ut.wr = Wr(0.5);
            const double user = 0.5 * std::pow(10.0, -4.0 / 20.0);
            KeptTagAfterVolume(ut, PathPrints(w, w.libraries[0], "runStone", 1), user, user * std::pow(10.0, -3.0 / 20.0));
            PlacedWrite uitem = Wr(user * std::pow(10.0, -3.0 / 20.0));
            const HandEdit uh = HandEdits(ut.wr, uitem, 0.0);
            CHECK(uh.vol && Near(uh.vol_db, -4.0, 1e-6));
        }
        // Paths as the dialogs say them.
        CHECK(SyncNodePath(st, st.libraries[0], "runStone") == "Run \xE2\x80\xBA Stone");
        CHECK(SyncNodePath(st, st.libraries[0], "jCloth") == "Jump_Land \xE2\x80\xBA FS_Land_Cloth");
        CHECK(SyncNodePath(st, st.libraries[0], "root") == "Footsteps");
    }
    // ---- What a Sync does in Populate's engine --------------------------------------------------
    {
        CHECK(SyncStepAction(SlotAction::Keep, true) == SyncStep::KeepInPlace);
        CHECK(SyncStepAction(SlotAction::SetVariation, true) == SyncStep::Replace);
        CHECK(SyncStepAction(SlotAction::Place, true) == SyncStep::Skip);
        CHECK(SyncStepAction(SlotAction::Remove, true) == SyncStep::Skip);
        CHECK(SyncStepAction(SlotAction::ReplaceNode, true) == SyncStep::Skip);
        CHECK(SyncStepAction(SlotAction::Keep, false) == SyncStep::Skip);
        CHECK(SyncStepAction(SlotAction::SetVariation, false) == SyncStep::Skip);
        CHECK(SyncEventPlan(EventPlan::Draw, true, true) == EventPlan::Draw);
        CHECK(SyncEventPlan(EventPlan::Draw, true, false) == EventPlan::Keep);
        CHECK(SyncEventPlan(EventPlan::Draw, false, false) == EventPlan::Skip);
        CHECK(SyncEventPlan(EventPlan::Keep, true, true) == EventPlan::Keep);
        CHECK(SyncEventPlan(EventPlan::Skip, true, true) == EventPlan::Skip);
    }
    // ---- Hand edits ------------------------------------------------------------------------------
    {
        const PlacedWrite wr = Wr(0.5, 0.5);
        CHECK(!HandEdits(wr, wr, 0.0).Any());
        PlacedWrite now = wr;
        now.vol = 0.5 * std::pow(10.0, -4.0 / 20.0);
        HandEdit h = HandEdits(wr, now, 0.0);
        CHECK(h.vol && !h.fades && !h.length && !h.moved && Near(h.vol_db, -4.0, 1e-6) && h.text == "volume \xE2\x88\x92" "4 dB");
        now = wr;
        now.fade_out = 0.04;
        h = HandEdits(wr, now, 0.012);
        CHECK(h.fades && h.moved && h.text == "moved +12 ms, fade-out 40 ms");
        now = wr;
        now.vol = 0.5 * std::pow(10.0, 2.0 / 20.0);
        now.length = 0.4;
        CHECK(HandEdits(wr, now, 0.0).text == "volume +2 dB, length");
        now = wr;
        now.out_dir = 0.5;
        CHECK(HandEdits(wr, now, 0.0).text == "fade shape");
        // Within the tolerances: no edit.
        now = wr;
        now.vol = 0.5 * std::pow(10.0, 0.01 / 20.0);
        now.length = 0.5002;
        CHECK(!HandEdits(wr, now, 0.0002).Any());
        // Keep: the user's offset kept on top of the library's change; their fades / length / nudge stay. Overwrite:
        // the library's values, nudge 0.
        now = wr;
        now.vol = 0.25;  // -6 dB by hand
        now.fade_out = 0.04;
        h = HandEdits(wr, now, 0.012);
        SyncItemPlan k = PlanSyncItem(wr, now, h, false, -6.0, -3.0, 0.012);
        const double three = std::pow(10.0, -3.0 / 20.0);
        CHECK(Near(k.vol, 0.25 * three) && Near(k.wr_vol, 0.5 * three) && !k.set_fades && k.set_length && Near(k.nudge_s, 0.012));
        SyncItemPlan ow = PlanSyncItem(wr, now, h, true, -6.0, -3.0, 0.012);
        CHECK(Near(ow.vol, 0.5 * three) && ow.set_fades && ow.set_length && ow.nudge_s == 0.0);
        SyncItemPlan plain = PlanSyncItem(wr, wr, HandEdit{}, false, -3.0, -3.0, 0.0);
        CHECK(Near(plain.vol, 0.5) && plain.set_fades && plain.set_length);
    }
    // ---- The rule token --------------------------------------------------------------------------
    {
        SoundStore st;
        st.kept = {"loopvar=same"};
        CHECK(HandEditRuleOf(st) == HandEditRule::Ask && !SetHandEditRule(st, HandEditRule::Ask));
        CHECK(SetHandEditRule(st, HandEditRule::Keep) && HandEditRuleOf(st) == HandEditRule::Keep && st.kept.size() == 2);
        CHECK(SetHandEditRule(st, HandEditRule::Overwrite) && HandEditRuleOf(st) == HandEditRule::Overwrite && st.kept.size() == 2);
        CHECK(SetHandEditRule(st, HandEditRule::Ask) && st.kept.size() == 1 && st.kept[0] == "loopvar=same");
        // It travels in the store's text.
        SetHandEditRule(st, HandEditRule::Keep);
        SoundStore with = Store();
        with.kept = st.kept;
        SoundStore back;
        CHECK(ParseSoundStore(SerializeSoundStore(with), &back) && HandEditRuleOf(back) == HandEditRule::Keep);
        CHECK(std::string(HandEditRuleText(HandEditRule::Overwrite)) == "Overwrite them");
    }
    // ---- The Library's view: the mockup's scenario ---------------------------------------------
    {
        SoundStore                  st = Store();
        std::vector<SyncPlacedItem> items;
        // Knight_Run_01 (i1): 2 Stone sounds (one of 2 pieces), one edited by hand; Knight_Run_02 (i2): 1 Stone;
        // Knight_Walk (i3, Guard track): 2 Grass sounds, variations 2 and 4; i4 a Cloth.
        items.push_back(Placed(st, "a1", "i1", "runStone", 0, 1.0));
        items.push_back(Placed(st, "a2", "i1", "runStone", 0, 1.0));  // the second piece of a1's sound
        items.push_back(Placed(st, "a3", "i1", "runStone", 2, 2.0));
        items.back().now.vol = 0.5 * std::pow(10.0, -4.0 / 20.0);
        items.push_back(Placed(st, "b1", "i2", "runStone", 1, 1.0));
        items.push_back(Placed(st, "g1", "i3", "walkGrass", 2, 1.0));
        items.push_back(Placed(st, "g2", "i3", "walkGrass", 4, 3.0));
        items.push_back(Placed(st, "c1", "i4", "jCloth", 0, 1.0));
        SyncPlacedItem old = Placed(st, "o1", "i2", "runStone", 0, 5.0);
        old.tag.lib.clear();  // an older placement: never counted
        items.push_back(old);
        const std::vector<SyncOwner> owners = {{"i1", "Knight_Run_01", "Knight", true},
                                               {"i3", "Knight_Walk_Loop", "Guard", false},
                                               {"i2", "Knight_Run_02", "Knight", true},
                                               {"i4", "Knight_Jump", "Knight", false}};
        // Nothing changed: the card is clean.
        SyncState s0 = BuildSyncState(st, items, owners);
        CHECK(s0.sounds.empty() && SyncCardOf(s0, "libFS").changes == 0);
        // Stone -3 -> -6 dB, Grass variation 3 off (0-based 2), Cloth's offset 12 -> 18 ms.
        N(st, "runStone")->volume_db = -6.0;
        N(st, "walkGrass")->sound.frames[2].on = false;
        N(st, "jCloth")->offset_ms = 18.0;
        const SyncState s = BuildSyncState(st, items, owners);
        CHECK(s.sounds.size() == 5);  // a1 + a2 one sound, a3, b1, g1 (variation 3), c1; g2 plays variation 5: not behind
        const SyncCard c = SyncCardOf(s, "libFS");
        CHECK(c.changes == 3 && c.sounds == 5 && c.items == 4);
        CHECK(SyncCardText(c) == "3 changes not yet on 5 placed sounds in 4 items.");
        CHECK(SyncCardText(SyncCard{1, 1, 1}) == "1 change not yet on 1 placed sound in 1 item.");
        CHECK(SyncCardOf(s, "other").sounds == 0);
        const std::map<std::string, int> marks = SyncMarksOf(s, "libFS");
        CHECK(marks.size() == 3 && marks.at("runStone") == 3 && marks.at("walkGrass") == 1 && marks.at("jCloth") == 1);
        CHECK(s.what.at("walkGrass") == "variation 3 excluded" && s.redraws.count("walkGrass") && !s.redraws.count("runStone"));
        CHECK(s.what.at("runStone") == "Volume \xE2\x88\x92" "3 dB \xE2\x86\x92 \xE2\x88\x92" "6 dB");
        // The where-dialog: grouped by track (owners' order: Knight first), the node scope narrows it.
        std::vector<SyncItemLine> lines = SyncItemLines(s, "libFS", "");
        CHECK(lines.size() == 4 && lines[0].owner == "i1" && lines[1].owner == "i2" && lines[2].owner == "i4" &&
              lines[3].owner == "i3");
        CHECK(lines[0].sounds == 2 && lines[0].hand == 1 && lines[0].selected && lines[0].nodes.size() == 1);
        lines = SyncItemLines(s, "libFS", "runStone");
        CHECK(lines.size() == 2 && lines[0].owner == "i1" && lines[1].owner == "i2");
        // The hand list of the chosen items in scope.
        CHECK(SyncHandSounds(s, "libFS", "", {"i1", "i2"}).size() == 1);
        CHECK(SyncHandSounds(s, "libFS", "", {"i2"}).empty());
        CHECK(SyncHandSounds(s, "libFS", "walkGrass", {"i1"}).empty());
        const SyncSound* hs = SyncHandSounds(s, "libFS", "", {"i1"})[0];
        CHECK(hs->hand.text == "volume \xE2\x88\x92" "4 dB" && hs->items.size() == 1 && hs->items[0] == "a3");
        // The buttons.
        CHECK(!SyncApplyCheckedEnabled(0, 4) && SyncApplyCheckedEnabled(2, 4) && !SyncApplyCheckedEnabled(4, 4));
        CHECK(SyncRememberEnabled({}) && SyncRememberEnabled({0, 0}) && SyncRememberEnabled({1, 1}) &&
              !SyncRememberEnabled({0, 1}));
        // The toast.
        SyncCounts n;
        n.adjusted = 26;
        n.redrawn = 3;
        n.kept = 1;
        n.overwritten = 2;
        CHECK(SyncToastText(n, false) ==
              "One undo point \xE2\x80\x9CRAV: Sync placed sounds\xE2\x80\x9D: 26 adjusted, 3 redrawn, 1 hand edit kept, 2 overwritten");
        CHECK(SyncToastText(SyncCounts{}, true).find("0 adjusted \xC2\xB7 remembered") != std::string::npos);
        CHECK(SyncDbText(-4.0) == "\xE2\x88\x92" "4 dB" && SyncDbText(2.5) == "+2.5 dB" && SyncDbText(0.0) == "0 dB");
        // A Populated tag only (an Attached sound is the user's).
        std::vector<SyncPlacedItem> att = {items[4]};
        att[0].tag.own = Ownership::Attached;
        CHECK(BuildSyncState(st, att, owners).sounds.empty());
    }
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("sound_sync_plan: all checks passed\n");
    return 0;
}
