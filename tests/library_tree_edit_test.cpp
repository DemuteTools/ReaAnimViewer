// SPDX-License-Identifier: MIT
//
// Host test of src/library_tree_edit.h (story 13-7a): the tree rows, Shift range by level and
// every edit of the spec's I/O matrix (value delete, moves, swaps, removes, group delete, paste
// across libraries, batch add), uid freshness on duplicate and cycle refusal.

#include "library_tree_edit.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "sound_record.h"
#include "sound_switch.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static SoundNode Node(const std::string& uid, const std::string& parent, SoundNodeType t, const std::string& name)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = t;
    n.name = name;
    return n;
}

// Group Material {Wood, Concrete, Grass}; library L: root Switch "sw" on Material, Wood = Sound
// "y", Grass = Random "gr" holding Sound "g1"; root of library M = Random "mr".
static SoundStore Store()
{
    SoundStore s;
    SwitchGroup g;
    g.uid = "gmat";
    g.name = "Material";
    g.values = {{"vwood", "Wood", {}}, {"vconc", "Concrete", {}}, {"vgrass", "Grass", {}}};
    s.groups.push_back(g);
    SoundLibrary l;
    l.uid = "L";
    l.name = "Steps";
    l.root = "sw";
    SoundNode sw = Node("sw", "", SoundNodeType::Switch, "Steps");
    sw.group_uid = "gmat";
    sw.cases = {{"vwood", "y", {}}, {"vgrass", "gr", {}}};
    sw.default_value = "vwood";
    sw.preview_value = "vwood";
    l.nodes = {sw, Node("y", "sw", SoundNodeType::Sound, "Y"), Node("gr", "sw", SoundNodeType::Random, "GrassR"),
               Node("g1", "gr", SoundNodeType::Sound, "G1")};
    s.libraries.push_back(l);
    SoundLibrary m;
    m.uid = "M";
    m.name = "Other";
    m.root = "mr";
    m.nodes = {Node("mr", "", SoundNodeType::Random, "Other")};
    s.libraries.push_back(m);
    return s;
}

static const SoundNode* N(const SoundStore& s, const char* lib, const std::string& uid)
{
    return FindNode(*FindLibrary(s, lib), uid);
}

static std::string Case(const SoundStore& s, const char* lib, const char* sw, const char* value)
{
    for (const SwitchCase& c : N(s, lib, sw)->cases)
        if (c.value_uid == value) return c.child_uid;
    return {};
}

static bool Has(const std::vector<std::string>& v, const std::string& x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}

static void TestDeleteDefaultValue()
{
    SoundStore s = Store();
    CHECK(DeleteValue(s, "gmat", "vwood"));
    const SoundNode* sw = N(s, "L", "sw");
    CHECK(Has(sw->unassigned, "y"));
    CHECK(Case(s, "L", "sw", "vwood").empty());
    CHECK(sw->default_value == "vgrass");  // Concrete has no child: Grass
    CHECK(sw->preview_value.empty());
    CHECK(FindGroup(s, "gmat")->values.size() == 2);
    // No remaining value with a child: "".
    CHECK(DeleteValue(s, "gmat", "vgrass"));
    CHECK(N(s, "L", "sw")->default_value.empty());
    CHECK(!DeleteValue(s, "gmat", "nope"));
}

static void TestMoveIntoDescendant()
{
    SoundStore s;
    SoundLibrary l;
    l.uid = "L";
    l.root = "r";
    l.nodes = {Node("r", "", SoundNodeType::Random, "R"), Node("a", "r", SoundNodeType::Blend, "A"),
               Node("b", "a", SoundNodeType::Random, "B")};
    s.libraries.push_back(l);
    const std::string before = SerializeSoundStore(s);
    DropTarget to{"L", {"a", "b"}, DropMode::Into};
    CHECK(!CanDrop(s, "L", {{"r", "a"}}, to));
    CHECK(!MoveRows(s, "L", {{"r", "a"}}, to, nullptr));
    CHECK(SerializeSoundStore(s) == before);
    // Into itself too.
    DropTarget self{"L", {"r", "a"}, DropMode::Into};
    CHECK(!MoveRows(s, "L", {{"r", "a"}}, self, nullptr));
    // The root never moves.
    CHECK(!MoveRows(s, "L", {{"", "r"}}, to, nullptr));
}

static void TestMultiDropOnValue()
{
    SoundStore s = Store();
    SoundLibrary* l = FindLibrary(s, "L");
    l->nodes.push_back(Node("s1", "gr", SoundNodeType::Sound, "S1"));
    l->nodes.push_back(Node("s2", "gr", SoundNodeType::Sound, "S2"));
    DropTarget to{"L", {"sw", "vconc"}, DropMode::OntoValue};
    std::vector<std::string> moved;
    CHECK(MoveRows(s, "L", {{"gr", "g1"}, {"gr", "s1"}, {"gr", "s2"}}, to, &moved));
    const std::string holder = Case(s, "L", "sw", "vconc");
    const SoundNode* h = N(s, "L", holder);
    CHECK(h && h->type == SoundNodeType::Random && h->name == "Concrete");
    CHECK(ChildrenOf(*FindLibrary(s, "L"), holder).size() == 3);
    CHECK(moved.size() == 3);
}

static void TestSwapOnTakenValue()
{
    SoundStore s = Store();
    SoundLibrary* l = FindLibrary(s, "L");
    l->nodes.push_back(Node("x", "gr", SoundNodeType::Sound, "X"));
    l->nodes.push_back(Node("z", "gr", SoundNodeType::Sound, "Z"));
    // X from Random gr onto Wood (holds y): Wood = x, y in gr at x's place (before z).
    DropTarget to{"L", {"sw", "vwood"}, DropMode::OntoValue};
    CHECK(MoveRows(s, "L", {{"gr", "x"}}, to, nullptr));
    CHECK(Case(s, "L", "sw", "vwood") == "x");
    std::vector<const SoundNode*> kids = ChildrenOf(*FindLibrary(s, "L"), "gr");
    CHECK(kids.size() == 3 && kids[1]->uid == "y" && kids[2]->uid == "z");
    CHECK(N(s, "L", "y")->parent == "gr");
    CHECK(N(s, "L", "x")->parent == "sw");
    // From another library: the old content goes to Unassigned.
    SoundLibrary* m = FindLibrary(s, "M");
    m->nodes.push_back(Node("o", "mr", SoundNodeType::Sound, "O"));
    CHECK(MoveRows(s, "M", {{"mr", "o"}}, to, nullptr));
    CHECK(Has(N(s, "L", "sw")->unassigned, "x"));
    CHECK(N(s, "M", "o") == nullptr);
    const std::string nw = Case(s, "L", "sw", "vwood");
    CHECK(!nw.empty() && nw != "o" && N(s, "L", nw)->name == "O");
}

static void TestRemoveSubtree()
{
    SoundStore s = Store();
    FindLibrary(s, "L")->nodes.push_back(Node("g2", "gr", SoundNodeType::Sound, "G2"));
    CHECK(RemoveRows(s, "L", {{"sw", "vgrass"}}));  // the value row: its content
    CHECK(!N(s, "L", "gr") && !N(s, "L", "g1") && !N(s, "L", "g2"));
    CHECK(Case(s, "L", "sw", "vgrass").empty());
    CHECK(N(s, "L", "sw")->cases.size() == 1);
    CHECK(!RemoveRows(s, "L", {{"", "sw"}}));  // root refused
    CHECK(N(s, "L", "sw") != nullptr);
}

static void TestDeleteGroupInUse()
{
    SoundStore s = Store();
    SoundLibrary* l = FindLibrary(s, "L");
    SoundNode sw2 = Node("sw2", "gr", SoundNodeType::Switch, "Inner");
    sw2.group_uid = "gmat";
    sw2.unassigned = {"q"};
    l->nodes.push_back(sw2);
    l->nodes.push_back(Node("q", "sw2", SoundNodeType::Sound, "Q"));
    CHECK(DeleteGroup(s, "gmat"));
    CHECK(s.groups.empty());
    CHECK(N(s, "L", "sw")->type == SoundNodeType::Random);
    CHECK(N(s, "L", "sw2")->type == SoundNodeType::Random);
    CHECK(ChildrenOf(*FindLibrary(s, "L"), "sw").size() == 2);
    CHECK(ChildrenOf(*FindLibrary(s, "L"), "sw2").size() == 1);
}

static void TestShiftRangeByLevel()
{
    SoundStore s = Store();
    SoundLibrary* l = FindLibrary(s, "L");
    l->nodes.push_back(Node("g2", "gr", SoundNodeType::Sound, "G2"));
    std::set<std::string> open = {"gr"};
    std::vector<TreeRow> rows = BuildTreeRows(s, *l, open);
    // root, Concrete(empty), Grass(gr), g1, g2, Wood(y): A to Z (13-17c; values are added in the
    // Switch Groups column)
    CHECK(rows.size() == 6);
    CHECK(rows[5].kind == TreeRowKind::Value && rows[5].is_default && rows[5].node == "y");
    CHECK(rows[3].depth == 2 && rows[4].kind == TreeRowKind::Node);
    std::vector<LibraryRow> r = RangeSelectByLevel(rows, {"sw", "vconc"}, {"gr", "g2"});
    // Anchor depth 1: Concrete, Grass (the target's row at depth 1).
    CHECK(r.size() == 2 && r[1] == (LibraryRow{"sw", "vgrass"}));
    r = RangeSelectByLevel(rows, {"gr", "g1"}, {"gr", "g2"});
    CHECK(r.size() == 2);
    r = RangeSelectByLevel(rows, {"sw", "vconc"}, {"sw", "vwood"});
    CHECK(r.size() == 3);
}

static void TestPasteOtherLibrary()
{
    SoundStore s = Store();
    DropTarget to{"M", {"", "mr"}, DropMode::Into};
    std::vector<std::string> moved;
    CHECK(MoveRows(s, "L", {{"sw", "vgrass"}}, to, &moved));
    CHECK(!N(s, "L", "gr") && !N(s, "L", "g1"));
    CHECK(Case(s, "L", "sw", "vgrass").empty());
    CHECK(moved.size() == 1 && moved[0] != "gr");
    const SoundLibrary& m = *FindLibrary(s, "M");
    CHECK(ChildrenOf(m, "mr").size() == 1 && ChildrenOf(m, moved[0]).size() == 1);
    CHECK(ChildrenOf(m, moved[0])[0]->uid != "g1");
    // Into itself: refused.
    DropTarget self{"M", RowOfNode(m, moved[0]), DropMode::Into};
    CHECK(!MoveRows(s, "M", {RowOfNode(m, moved[0])}, self, nullptr));
}

static void TestBatchAdd()
{
    SoundStore s = Store();
    FindLibrary(s, "L")->nodes.push_back(Node("s", "gr", SoundNodeType::Sound, "S"));
    NewNodeSpec r;
    r.type = SoundNodeType::Random;
    std::vector<LibraryRow> rows = {{"sw", "vwood"}, {"sw", "vconc"}, {"", "sw"}};
    int ok = 0, refused = 0;
    for (const LibraryRow& row : rows) {
        if (AddNodes(s, "L", &row, {r}, nullptr, nullptr)) ++ok;
        else ++refused;
    }
    CHECK(ok == 2 && refused == 1);  // the root Switch takes no child
    const std::string w = Case(s, "L", "sw", "vwood");
    CHECK(N(s, "L", w)->type == SoundNodeType::Random && N(s, "L", "y")->parent == w);  // wrapped
    CHECK(N(s, "L", Case(s, "L", "sw", "vconc"))->type == SoundNodeType::Random);
    // + Sound on a value playing a Sound: refused with the reason (one playing a Random, as
    // Wood now: it goes inside).
    NewNodeSpec snd;
    snd.type = SoundNodeType::Sound;
    std::string why;
    LibraryRow wood{"sw", "vwood"};
    SoundStore fresh = Store();
    CHECK(!AddNodes(fresh, "L", &wood, {snd}, nullptr, &why));
    CHECK(why.find("already plays a sound") != std::string::npos);
    std::vector<std::string> in;
    CHECK(AddNodes(s, "L", &wood, {snd}, &in, nullptr) && N(s, "L", in[0])->parent == w);
    // Next to a sound in its Random.
    LibraryRow g1{"gr", "g1"};
    std::vector<std::string> made;
    CHECK(AddNodes(s, "L", &g1, {snd, snd}, &made, nullptr));
    std::vector<const SoundNode*> kids = ChildrenOf(*FindLibrary(s, "L"), "gr");
    CHECK(made.size() == 2 && kids.size() == 4 && kids[1]->uid == made[0] && kids[2]->uid == made[1]);
}

static void TestDuplicateFreshUids()
{
    SoundStore s = Store();
    std::vector<std::string> made;
    CHECK(DuplicateRows(s, "L", {{"sw", "vgrass"}}, &made));
    CHECK(made.size() == 1 && made[0] != "gr");
    const SoundLibrary& l = *FindLibrary(s, "L");
    CHECK(Has(N(s, "L", "sw")->unassigned, made[0]));  // a value's content: to Unassigned
    std::vector<const SoundNode*> kids = ChildrenOf(l, made[0]);
    CHECK(kids.size() == 1 && kids[0]->uid != "g1" && kids[0]->name == "G1");
    std::set<std::string> uids;
    for (const SoundNode& n : l.nodes) uids.insert(n.uid);
    CHECK(uids.size() == l.nodes.size());
    // In a Random: right after the original.
    FindLibrary(s, "L")->nodes.push_back(Node("g2", "gr", SoundNodeType::Sound, "G2"));
    CHECK(DuplicateRows(s, "L", {{"gr", "g1"}}, &made));
    std::vector<const SoundNode*> gk = ChildrenOf(*FindLibrary(s, "L"), "gr");
    CHECK(gk.size() == 3 && gk[1]->uid == made[0] && gk[2]->uid == "g2");
    CHECK(!DuplicateRows(s, "L", {{"", "sw"}}, nullptr));  // root refused
}

static void TestLibrariesAndMisc()
{
    SoundStore s;
    std::string a, b;
    CHECK(NewLibrary(s, &a));
    CHECK(NewLibrary(s, &b));
    const SoundLibrary* la = FindLibrary(s, a);
    CHECK(la->name == "Library 1" && FindLibrary(s, b)->name == "Library 2");
    CHECK(la->color == 0u && N(s, a.c_str(), la->root)->type == SoundNodeType::Random);
    std::vector<TreeRow> rows = BuildTreeRows(s, *la, {});
    CHECK(rows.size() == 1);  // the root only: the UI shows the centred hint
    CHECK(DeleteLibrary(s, a) && s.libraries.size() == 1);
    // 13-15a: its Project / Character assignments go back to inherit; others stay.
    {
        SoundStore d = s;
        SoundAssign pa;
        pa.key = "feet";
        pa.state = LibState::Library;
        pa.library = b;
        pa.kit = true;
        SoundAssign other = pa;
        other.key = "hands";
        other.library = "elsewhere";
        d.project = {pa, other};
        SoundCharacter c;
        c.assigns = {pa};
        d.characters = {c};
        CHECK(DeleteLibrary(d, b) && d.libraries.empty());
        CHECK(d.project[0].state == LibState::Inherit && d.project[0].library.empty() && !d.project[0].kit);
        CHECK(d.project[1].state == LibState::Library && d.characters[0].assigns[0].state == LibState::Inherit);
    }
    // Wrap the root, make default, clear a value, rename, type change, group.
    SoundStore t = Store();
    std::vector<std::string> made;
    CHECK(WrapRows(t, "L", {{"", "sw"}}, SoundNodeType::Blend, "", &made));
    CHECK(FindLibrary(t, "L")->root == made[0] && N(t, "L", "sw")->parent == made[0]);
    CHECK(MakeDefault(t, "L", {{"sw", "vgrass"}}) && N(t, "L", "sw")->default_value == "vgrass");
    // A "(deleted value)" row never becomes the default.
    FindLibrary(t, "L")->nodes[0].cases.push_back({"vgone", "", {}});
    CHECK(!MakeDefault(t, "L", {{"sw", "vgone"}}));
    CHECK(!MakeDefault(t, "L", {{"sw", "vgrass"}}));  // nothing changes
    CHECK(ClearValue(t, "L", {"sw", "vwood"}) && Has(N(t, "L", "sw")->unassigned, "y"));
    // A value row renames the value it shows; a node row its node; the same name changes nothing.
    CHECK(RenameRow(t, "L", {"sw", "vgrass"}, "Leaves") && FindGroup(t, "gmat")->values[2].name == "Leaves");
    CHECK(N(t, "L", "gr")->name == "GrassR");
    CHECK(RenameRow(t, "L", {"gr", "g1"}, "Leaf") && N(t, "L", "g1")->name == "Leaf");
    CHECK(!RenameRow(t, "L", {"gr", "g1"}, "Leaf"));
    // A rename never makes two values / groups / libraries share a name.
    CHECK(RenameValue(t, "gmat", "vconc", "wood") && FindGroup(t, "gmat")->values[1].name == "wood 2");
    CHECK(SetRowsType(t, "L", {{"sw", "vgrass"}}, SoundNodeType::Blend, ""));
    CHECK(N(t, "L", "gr")->type == SoundNodeType::Blend);
    FindLibrary(t, "L")->nodes.push_back(Node("g2", "gr", SoundNodeType::Sound, "G2"));
    std::string grp;
    CHECK(GroupRows(t, "L", {{"gr", "g1"}, {"gr", "g2"}}, SoundNodeType::Random, "", &grp));
    CHECK(N(t, "L", "g1")->parent == grp && N(t, "L", "g2")->parent == grp && N(t, "L", grp)->parent == "gr");
    // Missing count.
    int n = CountMissing(*FindLibrary(t, "L"), [](const SoundNode& x) { return x.uid == "g1"; });
    CHECK(n == 1);
    std::string gid, vid;
    CHECK(AddGroup(t, "Material", &gid) && FindGroup(t, gid)->name == "Material 2");
    CHECK(AddValue(t, gid, "", &vid) && FindGroup(t, gid)->values[0].name == "Value");
    CHECK(RenameValue(t, gid, vid, "Stone") && RenameGroup(t, gid, "Surface"));
    CHECK(RenameGroup(t, gid, "material") && FindGroup(t, gid)->name == "material 2");
}

// Drops Before / After: in a Random (line order, several rows keep their order), in a Switch's
// Unassigned list (its index), and across libraries.
static void TestReorderBeforeAfter()
{
    SoundStore s = Store();
    SoundLibrary* l = FindLibrary(s, "L");
    for (const char* u : {"a", "b", "c", "d"}) l->nodes.push_back(Node(u, "gr", SoundNodeType::Sound, u));
    auto order = [&](const char* lib, const std::string& cid) {
        std::string o;
        for (const SoundNode* k : ChildrenOf(*FindLibrary(s, lib), cid)) o += k->uid + ",";
        return o;
    };
    CHECK(order("L", "gr") == "g1,a,b,c,d,");
    CHECK(MoveRows(s, "L", {{"gr", "c"}, {"gr", "d"}}, {"L", {"gr", "g1"}, DropMode::After}, nullptr));
    CHECK(order("L", "gr") == "g1,c,d,a,b,");
    CHECK(MoveRows(s, "L", {{"gr", "b"}}, {"L", {"gr", "g1"}, DropMode::Before}, nullptr));
    CHECK(order("L", "gr") == "b,g1,c,d,a,");
    // Unassigned: u1, u2, then a dropped After u1 / b dropped Before u1.
    SoundNode* sw = FindNode(*FindLibrary(s, "L"), "sw");
    sw->unassigned = {"u1", "u2"};
    FindLibrary(s, "L")->nodes.push_back(Node("u1", "sw", SoundNodeType::Sound, "U1"));
    FindLibrary(s, "L")->nodes.push_back(Node("u2", "sw", SoundNodeType::Sound, "U2"));
    CHECK(MoveRows(s, "L", {{"gr", "a"}}, {"L", {"sw", "u1"}, DropMode::After}, nullptr));
    CHECK((N(s, "L", "sw")->unassigned == std::vector<std::string>{"u1", "a", "u2"}));
    CHECK(N(s, "L", "a")->parent == "sw");
    CHECK(MoveRows(s, "L", {{"gr", "b"}}, {"L", {"sw", "u1"}, DropMode::Before}, nullptr));
    CHECK((N(s, "L", "sw")->unassigned == std::vector<std::string>{"b", "u1", "a", "u2"}));
    // Across libraries, After a sibling.
    FindLibrary(s, "M")->nodes.push_back(Node("m1", "mr", SoundNodeType::Sound, "M1"));
    FindLibrary(s, "M")->nodes.push_back(Node("m2", "mr", SoundNodeType::Sound, "M2"));
    std::vector<std::string> moved;
    CHECK(MoveRows(s, "L", {{"gr", "c"}}, {"M", {"mr", "m1"}, DropMode::After}, &moved));
    std::vector<const SoundNode*> mk = ChildrenOf(*FindLibrary(s, "M"), "mr");
    CHECK(moved.size() == 1 && mk.size() == 3 && mk[1]->uid == moved[0] && mk[2]->uid == "m2");
    CHECK(!N(s, "L", "c"));
    // A value's content onto its own value: nothing to do.
    CHECK(!CanDrop(s, "L", {{"sw", "vgrass"}}, {"L", {"sw", "vgrass"}, DropMode::OntoValue}));
}

// Wrap a value's content and an Unassigned row: the wrapper takes the slot.
static void TestWrapValueRow()
{
    SoundStore s = Store();
    std::vector<std::string> made;
    CHECK(WrapRows(s, "L", {{"sw", "vgrass"}}, SoundNodeType::Blend, "", &made));
    CHECK(made.size() == 1 && Case(s, "L", "sw", "vgrass") == made[0]);
    CHECK(N(s, "L", "gr")->parent == made[0] && N(s, "L", made[0])->type == SoundNodeType::Blend);
    CHECK(N(s, "L", made[0])->parent == "sw");
    SoundNode* sw = FindNode(*FindLibrary(s, "L"), "sw");
    sw->unassigned = {"u1", "u2"};
    FindLibrary(s, "L")->nodes.push_back(Node("u1", "sw", SoundNodeType::Sound, "U1"));
    FindLibrary(s, "L")->nodes.push_back(Node("u2", "sw", SoundNodeType::Sound, "U2"));
    CHECK(WrapRows(s, "L", {{"sw", "u2"}}, SoundNodeType::Random, "", &made));
    CHECK((N(s, "L", "sw")->unassigned == std::vector<std::string>{"u1", made[0]}));
    CHECK(N(s, "L", "u2")->parent == made[0]);
    // + Sound on a value playing a Random goes inside the Random.
    NewNodeSpec snd;
    snd.type = SoundNodeType::Sound;
    std::vector<std::string> added;
    LibraryRow grass{"sw", "vgrass"};
    CHECK(AddNodes(s, "L", &grass, {snd}, &added, nullptr));
    CHECK(added.size() == 1 && N(s, "L", added[0])->parent == Case(s, "L", "sw", "vgrass"));
}

// Deleting a value fixes every Switch on the group: nested and in another library.
static void TestDeleteValueEverySwitch()
{
    SoundStore s = Store();
    SoundLibrary* l = FindLibrary(s, "L");
    SoundNode sw2 = Node("sw2", "gr", SoundNodeType::Switch, "Inner");
    sw2.group_uid = "gmat";
    sw2.cases = {{"vwood", "q", {}}, {"vconc", "q2", {}}};
    sw2.default_value = "vwood";
    sw2.preview_value = "vwood";
    l->nodes.push_back(sw2);
    l->nodes.push_back(Node("q", "sw2", SoundNodeType::Sound, "Q"));
    l->nodes.push_back(Node("q2", "sw2", SoundNodeType::Sound, "Q2"));
    SoundLibrary* m = FindLibrary(s, "M");
    SoundNode sw3 = Node("sw3", "mr", SoundNodeType::Switch, "M Switch");
    sw3.group_uid = "gmat";
    sw3.cases = {{"vwood", "w", {}}};
    sw3.default_value = "vwood";
    m->nodes.push_back(sw3);
    m->nodes.push_back(Node("w", "sw3", SoundNodeType::Sound, "W"));
    CHECK(DeleteValue(s, "gmat", "vwood"));
    CHECK(Has(N(s, "L", "sw2")->unassigned, "q") && Case(s, "L", "sw2", "vwood").empty());
    // 13-18: Concrete, now the group's first value (its default), has a child: sw2 follows the group.
    CHECK(N(s, "L", "sw2")->default_value.empty() && DefaultValue(s, *N(s, "L", "sw2")) == "vconc" &&
          N(s, "L", "sw2")->preview_value.empty());
    CHECK(Has(N(s, "M", "sw3")->unassigned, "w") && N(s, "M", "sw3")->default_value.empty());
}

// Shift range going upward (target above the anchor).
// 13-18: the group's default, the override count, Follow / Keep theirs, delete, a new Switch.
static void TestGroupDefault()
{
    SoundStore s = Store();  // sw (L) has its own default Wood
    SoundNode sw2 = Node("sw2", "mr", SoundNodeType::Switch, "S2");
    sw2.group_uid = "gmat";
    sw2.default_value = "vwood";
    FindLibrary(s, "M")->nodes.push_back(sw2);
    // A Switch on another group, with its own default: never touched by Material's default.
    SwitchGroup spd;
    spd.uid = "gspd";
    spd.name = "Speed";
    spd.values = {{"vwalk", "Walk", {}}, {"vrun", "Run", {}}};
    s.groups.push_back(spd);
    SoundNode sw3 = Node("sw3", "mr", SoundNodeType::Switch, "S3");
    sw3.group_uid = "gspd";
    sw3.default_value = "vrun";
    FindLibrary(s, "M")->nodes.push_back(sw3);
    CHECK(CountDefaultOverrides(s, "gmat", "vconc") == 2);
    CHECK(CountDefaultOverrides(s, "gmat", "vwood") == 0);  // theirs is the same value
    CHECK(CountDefaultOverrides(s, "nope", "vconc") == 0);
    // Keep theirs: only the group changes.
    SoundStore k = s;
    CHECK(SetGroupDefault(k, "gmat", "vconc", false));
    CHECK(FindGroup(k, "gmat")->default_value == "vconc");
    CHECK(DefaultValue(k, *N(k, "L", "sw")) == "vwood" && DefaultValue(k, *N(k, "M", "sw2")) == "vwood");
    CHECK(!SetGroupDefault(k, "gmat", "vconc", false));  // nothing changes
    CHECK(!SetGroupDefault(k, "gmat", "vgone", false));
    // Follow: their own defaults are cleared, both play Concrete (one call = one undo point).
    SoundStore f = s;
    CHECK(SetGroupDefault(f, "gmat", "vconc", true));
    CHECK(N(f, "L", "sw")->default_value.empty() && N(f, "M", "sw2")->default_value.empty());
    CHECK(DefaultValue(f, *N(f, "L", "sw")) == "vconc" && DefaultValue(f, *N(f, "M", "sw2")) == "vconc");
    CHECK(N(f, "M", "sw3")->default_value == "vrun" && FindGroup(f, "gspd")->default_value.empty());
    // The round trip keeps it.
    SoundStore rt;
    CHECK(ParseSoundStore(SerializeSoundStore(f), &rt) && FindGroup(rt, "gmat")->default_value == "vconc");
    // The tree tags it on a Switch with no default of its own.
    std::vector<std::string> tagged;
    for (const TreeRow& r : BuildTreeRows(f, *FindLibrary(f, "L"), {}))
        if (r.kind == TreeRowKind::Value && r.is_default) tagged.push_back(r.row.key);
    CHECK(tagged.size() == 1 && tagged[0] == "vconc");
    // A node set back to its own, then to follow (the node panel's "Group default").
    CHECK(MakeDefault(f, "L", {{"sw", "vgrass"}}) && DefaultValue(f, *N(f, "L", "sw")) == "vgrass");
    CHECK(FollowGroupDefault(f, "L", "sw") && DefaultValue(f, *N(f, "L", "sw")) == "vconc");
    CHECK(!FollowGroupDefault(f, "L", "sw"));
    // A new Switch on the group added in a Random follows it.
    std::vector<std::string> in;
    LibraryRow               mrow{"", "mr"};
    NewNodeSpec              spec;
    spec.type = SoundNodeType::Switch;
    spec.group_uid = "gmat";
    CHECK(AddNodes(f, "M", &mrow, {spec}, &in, nullptr) && in.size() == 1);
    CHECK(DefaultValue(f, *N(f, "M", in[0])) == "vconc");
    // Its value deleted: the group's default is cleared (the first value plays).
    CHECK(DeleteValue(f, "gmat", "vconc"));
    CHECK(FindGroup(f, "gmat")->default_value.empty() && DefaultValue(f, *N(f, "M", in[0])) == "vwood");
    // A Switch's own default deleted while the group's default has a child there: it follows the group.
    SoundStore d = Store();  // sw: own Wood; Wood -> y, Grass -> gr
    CHECK(SetGroupDefault(d, "gmat", "vgrass", false));
    CHECK(DeleteValue(d, "gmat", "vwood"));
    CHECK(N(d, "L", "sw")->default_value.empty() && DefaultValue(d, *N(d, "L", "sw")) == "vgrass");
}

static void TestShiftRangeUpward()
{
    SoundStore s = Store();
    std::vector<TreeRow> rows = BuildTreeRows(s, *FindLibrary(s, "L"), {"gr"});
    std::vector<LibraryRow> r = RangeSelectByLevel(rows, {"sw", "vwood"}, {"sw", "vconc"});
    CHECK(r.size() == 3 && r[0] == (LibraryRow{"sw", "vconc"}));
}

// Bug 3 (13-7a): several Switch rows, batch "Switch on" a group created just before (no value yet,
// then with values): every selected Switch moves to it; value rows in the selection act on their
// Switch.
static void TestBatchSwitchOnNewGroup()
{
    SoundStore s = Store();
    SoundLibrary l;
    l.uid = "B";
    l.name = "Batch";
    l.root = "rr";
    SoundNode s1 = Node("s1", "rr", SoundNodeType::Switch, "S1");
    s1.group_uid = "gmat";
    s1.cases = {{"vwood", "a", {}}};
    SoundNode s2 = Node("s2", "rr", SoundNodeType::Switch, "S2");
    s2.group_uid = "gmat";
    s2.cases = {{"vconc", "b", {}}};
    l.nodes = {Node("rr", "", SoundNodeType::Random, "Root"), s1, Node("a", "s1", SoundNodeType::Sound, "Wood"), s2,
               Node("b", "s2", SoundNodeType::Sound, "Concrete")};
    s.libraries.push_back(l);
    std::string gid;
    CHECK(AddGroup(s, "Material", &gid));
    CHECK(!gid.empty() && FindGroup(s, gid)->name == "Material 2");
    // A group with no value yet.
    SoundStore t = s;
    CHECK(SetRowsType(t, "B", {{"rr", "s1"}, {"rr", "s2"}}, SoundNodeType::Switch, gid));
    CHECK(N(t, "B", "s1")->group_uid == gid && N(t, "B", "s2")->group_uid == gid);
    CHECK(Has(N(t, "B", "s1")->unassigned, "a") && Has(N(t, "B", "s2")->unassigned, "b"));
    // With values: matched by name.
    std::string vw, vc;
    CHECK(AddValue(s, gid, "Wood", &vw));
    CHECK(AddValue(s, gid, "Concrete", &vc));
    t = s;
    CHECK(SetRowsType(t, "B", {{"rr", "s1"}, {"rr", "s2"}}, SoundNodeType::Switch, gid));
    CHECK(Case(t, "B", "s1", vw.c_str()) == "a" && Case(t, "B", "s2", vc.c_str()) == "b");
    // The selection holds value rows too (select-all): they convert their Switch.
    t = s;
    CHECK(SetRowsType(t, "B", {{"s1", "vwood"}, {"s2", "vconc"}}, SoundNodeType::Switch, gid));
    CHECK(N(t, "B", "s1")->group_uid == gid && N(t, "B", "s2")->group_uid == gid);
}

// 13-17c: every level A to Z (display only), and the search field's filter.
static std::vector<std::string> Labels(const std::vector<TreeRow>& rows)
{
    std::vector<std::string> out;
    for (const TreeRow& r : rows) out.push_back(r.label);
    return out;
}

static void TestSortAndSearch()
{
    SoundStore    s = Store();
    SoundLibrary* l = FindLibrary(s, "L");
    // Unassigned rows A to Z.
    l->nodes.push_back(Node("u1", "sw", SoundNodeType::Sound, "Zed"));
    l->nodes.push_back(Node("u2", "sw", SoundNodeType::Sound, "alpha"));
    FindNode(*l, "sw")->unassigned = {"u1", "u2"};
    // Random children A to Z, case-insensitive, equal names in stored order.
    l->nodes.push_back(Node("g3", "gr", SoundNodeType::Sound, "b"));
    l->nodes.push_back(Node("g4", "gr", SoundNodeType::Sound, "A"));
    l->nodes.push_back(Node("g5", "gr", SoundNodeType::Sound, "b"));
    l->nodes.push_back(Node("e", "gr", SoundNodeType::Random, "Hollow"));
    const std::vector<TreeRow> rows = BuildTreeRows(s, *l, {"gr", "e"});
    CHECK((Labels(rows) == std::vector<std::string>{"Steps", "Concrete", "Grass", "A", "b", "b", "G1", "Hollow",
                                                    "Drop sounds here or use + Sound", "Wood",
                                                    "Unassigned (2) \xC2\xB7 drag them onto a value", "alpha", "Zed"}));
    CHECK(rows[4].node == "g3" && rows[5].node == "g5");
    // The stored order is unchanged.
    CHECK(ChildrenOf(*l, "gr").front()->uid == "g1");
    // Search: a match with its ancestors, folded containers shown open, case-insensitive.
    std::vector<TreeRow> f = BuildTreeRows(s, *l, {}, "g1");
    CHECK((Labels(f) == std::vector<std::string>{"Steps", "Grass", "G1"}));
    CHECK(f[1].open);
    // A value row by its content's name ("Y" under Wood).
    f = BuildTreeRows(s, *l, {"gr"}, "y");
    CHECK((Labels(f) == std::vector<std::string>{"Steps", "Wood"}));
    // An Unassigned row keeps its header; a hint row goes.
    f = BuildTreeRows(s, *l, {"gr"}, "ZED");
    CHECK(f.size() == 3 && f[1].kind == TreeRowKind::UnassignedHeader && f[2].node == "u1");
    f = BuildTreeRows(s, *l, {"gr"}, "holl");
    CHECK((Labels(f) == std::vector<std::string>{"Steps", "Grass", "Hollow"}));
    // A matching container shows without its children (only matches and their ancestors).
    f = BuildTreeRows(s, *l, {"gr"}, "grass");
    CHECK((Labels(f) == std::vector<std::string>{"Steps", "Grass"}) && f[0].open && !f[1].open);
    // Nothing matches: no row.
    CHECK(BuildTreeRows(s, *l, {"gr"}, "nothing like it").empty());
}

int main()
{
    TestSortAndSearch();
    TestDeleteDefaultValue();
    TestMoveIntoDescendant();
    TestMultiDropOnValue();
    TestSwapOnTakenValue();
    TestRemoveSubtree();
    TestDeleteGroupInUse();
    TestShiftRangeByLevel();
    TestPasteOtherLibrary();
    TestBatchAdd();
    TestDuplicateFreshUids();
    TestLibrariesAndMisc();
    TestReorderBeforeAfter();
    TestWrapValueRow();
    TestDeleteValueEverySwitch();
    TestGroupDefault();
    TestShiftRangeUpward();
    TestBatchSwitchOnNewGroup();
    if (g_fail) {
        std::printf("library_tree_edit_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("library_tree_edit_test: all passed\n");
    return 0;
}
