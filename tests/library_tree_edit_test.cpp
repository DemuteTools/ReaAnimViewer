// SPDX-License-Identifier: MIT
//
// Host test of src/library_tree_edit.h (story 13-7a): the tree rows, Shift range by level and
// every edit of the spec's I/O matrix (value delete, moves, swaps, removes, group delete, paste
// across libraries, batch add), uid freshness on duplicate and cycle refusal.

#include "library_tree_edit.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "sound_entries.h"  // ToggleGroupRole
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

// 13-68 v3: a RAV-owned group: shown "RAV - <name>", listed after the user's, and every hand gesture on it (rename,
// delete, add / rename / remove a value, a value row's rename, a value's roles) refuses it, the store unchanged.
static void TestRavOwnedGroup()
{
    SoundStore s = Store();
    SwitchGroup side;
    side.uid = "gside";
    side.name = "Side";
    side.rav_owned = true;
    side.values = {{"vl", "L", {}}, {"vr", "R", {}}};
    s.groups.insert(s.groups.begin(), side);  // stored first: still listed after the user's
    SwitchGroup user;
    user.uid = "guser";
    user.name = "Mood";
    s.groups.push_back(user);
    CHECK(GroupDisplayName(s.groups[0]) == "RAV - Side" && GroupDisplayName(s.groups[1]) == "Material");
    const std::vector<const SwitchGroup*> order = GroupsInListOrder(s);
    CHECK(order.size() == 3 && order[0]->uid == "gmat" && order[1]->uid == "guser" && order[2]->uid == "gside");
    SoundLibrary* l = FindLibrary(s, "L");
    l->nodes[0].group_uid = "gside";  // the Switch on Side: its value rows are RAV's
    l->nodes[0].cases = {{"vl", "y", {}}, {"vr", "gr", {}}};
    l->nodes[0].default_value = l->nodes[0].preview_value = "vl";
    const SoundStore before = s;
    std::string      uid = "x";
    CHECK(!RenameGroup(s, "gside", "Sides"));
    CHECK(!DeleteGroup(s, "gside"));
    CHECK(!AddValue(s, "gside", "Middle", &uid) && uid == "x");
    CHECK(!RenameValue(s, "gside", "vl", "Left"));
    CHECK(!DeleteValue(s, "gside", "vr"));
    CHECK(!RenameRow(s, "L", {"sw", "vl"}, "Left") && !RenameRow(s, "L", {"sw", "vr"}, "Right"));
    CHECK(!ToggleGroupRole(s, "gside", "vl", "heel"));
    // 13-68 v3 review: its default is RAV's too (the group's, a Switch's own, following the group's).
    CHECK(!SetGroupDefault(s, "gside", "vr", false) && !SetGroupDefault(s, "gside", "vr", true));
    CHECK(!MakeDefault(s, "L", {{"sw", "vr"}}) && N(s, "L", "sw")->default_value == "vl");
    CHECK(!FollowGroupDefault(s, "L", "sw") && N(s, "L", "sw")->default_value == "vl");
    CHECK(s.groups.size() == 3 && s.groups[0].name == "Side" && s.groups[0].values.size() == 2 &&
          s.groups[0].values[0].name == "L" && s.groups[0].values[1].name == "R" && s.groups[0].values[0].roles.empty());
    CHECK(N(s, "L", "sw")->type == SoundNodeType::Switch && N(s, "L", "gr")->name == "GrassR" &&
          N(s, "L", "sw")->cases.size() == 2 && SerializeSoundStore(s) == SerializeSoundStore(before));
    // The user's groups are untouched by the rule.
    CHECK(RenameGroup(s, "guser", "Mood 2") && AddValue(s, "guser", "Calm", &uid) && RenameValue(s, "guser", uid, "Quiet") &&
          DeleteValue(s, "guser", uid) && DeleteGroup(s, "guser"));
    // 13-68 v3 review: a RAV group no Switch uses any more (an orphan) can be deleted.
    l = FindLibrary(s, "L");
    l->nodes[0].group_uid = "gmat";
    CHECK(DeleteGroup(s, "gside") && !FindGroup(s, "gside") && N(s, "L", "sw")->type == SoundNodeType::Switch);
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
        pa.key = "legs";
        pa.state = LibState::Library;
        pa.library = b;
        pa.kit = true;
        SoundAssign other = pa;
        other.key = "arms";
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
    // The missing list (on a copy), the one rule (spec missing-sounds-window): a File Sound whose file is not found;
    // an item Sound whose item is gone and whose remembered file is not found (file known) or was never remembered
    // (file unknown); an item Sound whose item is gone but whose file is on disk is not missing.
    {
        SoundLibrary L = *FindLibrary(t, "L");
        for (SoundNode& x : L.nodes) {
            if (x.uid == "g2") x.sound.source.kind = SoundSourceKind::File;
            if (x.uid == "g2") x.sound.source.asset.last_path = "E:/Old/Step.wav";
            if (x.uid == "g1") x.sound.source.kind = SoundSourceKind::Item;
            if (x.uid == "g2") x.name.clear();
        }
        bool g1_file_found = false;
        auto gone = [](const SoundNode& x) { return x.uid == "g1"; };
        auto file = [&g1_file_found](const SoundNode& x) {
            if (x.uid == "g1") return !g1_file_found;
            return x.sound.source.kind == SoundSourceKind::File && x.uid == "g2";
        };
        // Old item Sound (asset empty, item gone): file unknown.
        std::vector<MissingSound> m = ListMissing(L, gone, file);
        CHECK(m.size() == 2 && m[0].uid == "g1" && m[0].why == MissingWhy::ItemGoneFileUnknown && m[0].lib_uid == "L");
        CHECK(MissingStatus(m[0]) == "Item not in the project \xC2\xB7 file unknown");
        CHECK(m[1].uid == "g2" && m[1].why == MissingWhy::FileNotFound && m[1].name == "Sound");  // unnamed
        // File moved: "File not found · was <path>".
        CHECK(m[1].path == "E:/Old/Step.wav" && MissingStatus(m[1]) == "File not found \xC2\xB7 was E:/Old/Step.wav");
        // Item deleted, file moved (asset known, not on disk): listed with its file's name.
        for (SoundNode& x : L.nodes)
            if (x.uid == "g1") {
                x.sound.source.asset.name = "FEETHmn_Wood Walk 01.wav";
                x.sound.source.asset.last_path = "C:/Proj/Media Files/FEETHmn_Wood Walk 01.wav";
            }
        m = ListMissing(L, gone, file);
        CHECK(m.size() == 2 && m[0].why == MissingWhy::ItemGoneFileKnown && m[0].file == "FEETHmn_Wood Walk 01.wav");
        CHECK(MissingStatus(m[0]) == "Item not in the project \xC2\xB7 file not found either: FEETHmn_Wood Walk 01.wav");
        MissingWhy why = MissingWhy::FileNotFound;
        CHECK(SoundIsMissing(*FindNode(L, "g1"), gone, file, &why) && why == MissingWhy::ItemGoneFileKnown);
        // Item deleted, file on disk: plays the file, not missing.
        g1_file_found = true;
        m = ListMissing(L, gone, file);
        CHECK(m.size() == 1 && m[0].uid == "g2");
        CHECK(!SoundIsMissing(*FindNode(L, "g1"), gone, file));
        // Its item still there: never missing, whatever its remembered file.
        g1_file_found = false;
        CHECK(!SoundIsMissing(*FindNode(L, "g1"), [](const SoundNode&) { return false; }, file));
        // A File Sound is never "item gone", even when the item test would say so.
        for (SoundNode& x : L.nodes)
            if (x.uid == "g1") x.sound.source.kind = SoundSourceKind::File;
        m = ListMissing(L, gone, file);
        CHECK(m.size() == 2 && m[0].uid == "g1" && m[0].why == MissingWhy::FileNotFound);
        CHECK(ListMissing(L, nullptr, nullptr).empty());
        // A fixed row's status.
        CHECK(FixedStatus("E:/Boom/Walk 03.wav", false) == "Fixed \xC2\xB7 E:/Boom/Walk 03.wav");
        CHECK(FixedStatus("E:/Boom/Handle.wav", true) == "Found by name: check it \xC2\xB7 E:/Boom/Handle.wav");
        // By name: the stem, ignoring case; several matches: the first by path order.
        CHECK(StemMatches("E:\\Boom\\METLHndl_Shoehorn Handle 02_B00M_ONE.wav", "metlhndl_shoehorn handle 02_b00m_one"));
        CHECK(!StemMatches("E:/Boom/Ladder Grip 2.wav", "Ladder Grip") && !StemMatches("E:/a.wav", ""));
        CHECK(StemMatches("E:/x/Grip.v2.wav", "Grip.v2") && StemMatches("Grip", "grip"));
        CHECK(FirstByStem({"E:/z/Grip.wav", "E:/A/grip.flac", "E:/b/Other.wav"}, "Grip") == "E:/A/grip.flac");
        CHECK(FirstByStem({"E:/b/Other.wav"}, "Grip").empty());
        // Search a folder's toast.
        CHECK(SearchToastText(5, 2, "E:\\Boom", 3) == "Found 5 in E:\\Boom (2 by name: check them), 3 still missing. Ctrl+Z undoes.");
        CHECK(SearchToastText(4, 0, "E:\\Boom", 0) == "Found 4 in E:\\Boom. Ctrl+Z undoes.");
        CHECK(SearchToastText(0, 0, "E:\\Boom", 2) == "Nothing found in E:\\Boom, 2 still missing");
        // A sound with several candidates stays missing: said so.
        CHECK(SearchToastText(3, 0, "E:\\Boom", 2, 2) ==
              "Found 3 in E:\\Boom, 2 still missing, 2 with several candidates: Locate them one by one. Ctrl+Z undoes.");
        CHECK(SearchToastText(0, 0, "E:\\Boom", 1, 1) == "Nothing found in E:\\Boom, 1 still missing, 1 with several candidates: Locate it");
        CHECK(AmbiguousText(0).empty());
        const std::string tip = MissingTip({{"a", "Hit", MissingWhy::ItemGoneFileKnown}, {"b", "Step", MissingWhy::FileNotFound}});
        CHECK(tip.find("2 missing sounds") == 0 && tip.find("Hit (its item is not in the project, its file not found either)") != std::string::npos &&
              tip.find("Step (file not found)") != std::string::npos);
        CHECK(MissingTip({{"a", "A", MissingWhy::ItemGoneFileUnknown}, {"b", "B", MissingWhy::ItemGoneFileUnknown}}, 1).find("and 1 more") !=
              std::string::npos);
        CHECK(MissingTip({{"a", "A", MissingWhy::ItemGoneFileUnknown}}, 1) == "1 missing sound:\n- A (its item is not in the project, file unknown)");
        CHECK(MissingTip({}).empty());
    }
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

// Unwrap: a one-node container gives its place to that node (a Switch case, the root).
static void TestUnwrap()
{
    SoundStore s = Store();
    const SoundLibrary* l = FindLibrary(s, "L");
    CHECK(CanUnwrap(*l, "gr") && !CanUnwrap(*l, "sw") && !CanUnwrap(*l, "g1"));
    FindNode(s.libraries[0], "gr")->volume_db = -3.0;
    CHECK(UnwrapRows(s, "L", {RowOfNode(*FindLibrary(s, "L"), "gr")}));
    l = FindLibrary(s, "L");
    CHECK(!FindNode(*l, "gr") && FindNode(*l, "g1")->parent == "sw" && FindNode(*l, "g1")->volume_db == -3.0);
    bool on_grass = false;
    for (const SwitchCase& c : FindNode(*l, "sw")->cases) on_grass = on_grass || (c.value_uid == "vgrass" && c.child_uid == "g1");
    CHECK(on_grass);
    // The root: wrapped in a Random, then unwrapped back (Antho: a Switch out of the library's Random).
    std::vector<std::string> made;
    CHECK(WrapRows(s, "L", {RowOfNode(*FindLibrary(s, "L"), "sw")}, SoundNodeType::Random, std::string(), &made) && made.size() == 1);
    FindNode(s.libraries[0], made[0])->lands_in = "Foley";
    CHECK(FindLibrary(s, "L")->root == made[0]);
    CHECK(UnwrapRows(s, "L", {RowOfNode(*FindLibrary(s, "L"), made[0])}));
    l = FindLibrary(s, "L");
    CHECK(l->root == "sw" && FindNode(*l, "sw")->parent.empty() && !FindNode(*l, made[0]) && FindNode(*l, "sw")->lands_in == "Foley");
    // In a Blend: a one-layer Blend's offset adds to its layer's; a Random's stale child offset gives
    // way to the Random's; the node keeps the container's place among its siblings and its own Lands in.
    SoundStore b;
    SoundLibrary bl;
    bl.uid = "B";
    bl.root = "mb";
    SoundNode inner = Node("bl", "mb", SoundNodeType::Blend, "Inner");
    inner.offset_ms = 50.0;
    inner.lands_in = "Box";
    SoundNode b1 = Node("b1", "bl", SoundNodeType::Sound, "B1");
    b1.offset_ms = 20.0;
    b1.lands_in = "Own";
    SoundNode rr = Node("rr", "mb", SoundNodeType::Random, "R");
    rr.offset_ms = 30.0;
    rr.lands_in = "Box";
    SoundNode r1 = Node("r1", "rr", SoundNodeType::Sound, "R1");
    r1.offset_ms = 99.0;
    rr.layer_roles = {"toe"};  // 13-31: the layer's roles go to the node taking its place
    rr.layer_keep_offset = true;
    bl.nodes = {Node("mb", "", SoundNodeType::Blend, "Top"), Node("a", "mb", SoundNodeType::Sound, "A"), inner, rr,
                Node("z", "mb", SoundNodeType::Sound, "Z"), b1, r1};
    b.libraries.push_back(bl);
    CHECK(UnwrapRows(b, "B", {RowOfNode(*FindLibrary(b, "B"), "bl"), RowOfNode(*FindLibrary(b, "B"), "rr")}));
    const SoundLibrary* lb = FindLibrary(b, "B");
    CHECK(!FindNode(*lb, "bl") && !FindNode(*lb, "rr"));
    CHECK(FindNode(*lb, "b1")->parent == "mb" && FindNode(*lb, "b1")->offset_ms == 70.0 && FindNode(*lb, "b1")->lands_in == "Own");
    CHECK(FindNode(*lb, "r1")->parent == "mb" && FindNode(*lb, "r1")->offset_ms == 30.0 && FindNode(*lb, "r1")->lands_in == "Box");
    std::vector<std::string> order;
    for (const SoundNode* k : ChildrenOf(*lb, "mb")) order.push_back(k->uid);
    CHECK((order == std::vector<std::string>{"a", "b1", "r1", "z"}));
    CHECK(FindNode(*lb, "r1")->layer_roles == std::vector<std::string>{"toe"} && FindNode(*lb, "r1")->layer_keep_offset);
    CHECK(FindNode(*lb, "b1")->layer_roles.empty());
    // 13-31: Wrap moves a Blend layer's roles onto the wrapper.
    std::vector<std::string> wmade;
    CHECK(WrapRows(b, "B", {RowOfNode(*FindLibrary(b, "B"), "r1")}, SoundNodeType::Random, std::string(), &wmade) &&
          wmade.size() == 1);
    lb = FindLibrary(b, "B");
    const SoundNode* wr = FindNode(*lb, wmade[0]);
    CHECK(wr && wr->layer_roles == std::vector<std::string>{"toe"} && wr->layer_keep_offset);
    CHECK(FindNode(*lb, "r1")->layer_roles.empty() && !FindNode(*lb, "r1")->layer_keep_offset);
}

// 13-70: Copy / Paste = a deep copy at the paste target (CopyRows), the originals stay.
static void TestCopyPaste()
{
    SoundStore s = Store();
    FindLibrary(s, "L")->nodes[2].volume_db = -6.0;  // "gr": a setting the copy keeps
    // Across libraries: GrassR + G1 into M's root, new uids, L untouched.
    DropTarget into_m;
    into_m.lib = "M";
    into_m.row = {"", "mr"};
    into_m.mode = DropMode::Into;
    std::vector<std::string> made;
    CHECK(CanDrop(s, "L", {{"sw", "vgrass"}}, into_m, true));
    CHECK(CopyRows(s, "L", {{"sw", "vgrass"}}, into_m, &made));
    CHECK(made.size() == 1 && made[0] != "gr");
    const SoundNode* c = N(s, "M", made[0]);
    CHECK(c && c->parent == "mr" && c->type == SoundNodeType::Random && c->name == "GrassR" && c->volume_db == -6.0);
    std::vector<const SoundNode*> ck = ChildrenOf(*FindLibrary(s, "M"), made[0]);
    CHECK(ck.size() == 1 && ck[0]->uid != "g1" && ck[0]->name == "G1");
    CHECK(Case(s, "L", "sw", "vgrass") == "gr" && N(s, "L", "g1")->parent == "gr");  // original kept
    CHECK(FindLibrary(s, "L")->nodes.size() == 4);
    // A Switch (cases, group, default) copied into another library's Random.
    SoundNode ms = Node("ms", "mr", SoundNodeType::Switch, "Mat");
    ms.group_uid = "gmat";
    ms.cases = {{"vconc", "mc", {}}};
    ms.default_value = "vconc";
    FindLibrary(s, "M")->nodes.push_back(ms);
    FindLibrary(s, "M")->nodes.push_back(Node("mc", "ms", SoundNodeType::Sound, "Conc"));
    DropTarget into_gr;
    into_gr.lib = "L";
    into_gr.row = {"sw", "vgrass"};  // the value row playing GrassR: Into acts on its Random
    into_gr.mode = DropMode::Into;
    CHECK(CopyRows(s, "M", {{"mr", "ms"}}, into_gr, &made));
    const SoundNode* cs = N(s, "L", made[0]);
    CHECK(cs && cs->type == SoundNodeType::Switch && cs->parent == "gr" && cs->group_uid == "gmat" &&
          cs->default_value == "vconc" && cs->cases.size() == 1);
    const std::string cc = Case(s, "L", made[0].c_str(), "vconc");
    CHECK(!cc.empty() && cc != "mc" && N(s, "L", cc)->name == "Conc" && N(s, "L", cc)->parent == made[0]);
    CHECK(N(s, "M", "ms") && N(s, "M", "mc")->parent == "ms");  // original kept
    // Into itself: refused for a move, a copy goes inside (taken before it is placed).
    DropTarget self;
    self.lib = "L";
    self.row = {"sw", "vgrass"};
    self.mode = DropMode::Into;
    CHECK(!CanDrop(s, "L", {{"sw", "vgrass"}}, self));
    const size_t before = FindLibrary(s, "L")->nodes.size();
    CHECK(CopyRows(s, "L", {{"sw", "vgrass"}}, self, &made));
    CHECK(N(s, "L", made[0])->parent == "gr" && FindLibrary(s, "L")->nodes.size() == before + 4);
    // Next to itself: a copy right after the original.
    DropTarget after;
    after.lib = "L";
    after.row = {"gr", "g1"};
    after.mode = DropMode::After;
    CHECK(CopyRows(s, "L", {{"gr", "g1"}}, after, &made));
    std::vector<const SoundNode*> gk = ChildrenOf(*FindLibrary(s, "L"), "gr");
    CHECK(gk.size() >= 2 && gk[0]->uid == "g1" && gk[1]->uid == made[0] && gk[1]->name == "G1");
    // Onto a value: the copy plays it, what it played goes to Unassigned; onto its own value: refused.
    DropTarget conc;
    conc.lib = "L";
    conc.row = {"sw", "vconc"};
    conc.mode = DropMode::OntoValue;
    CHECK(CopyRows(s, "L", {{"sw", "vwood"}}, conc, &made));
    CHECK(Case(s, "L", "sw", "vconc") == made[0] && Case(s, "L", "sw", "vwood") == "y");
    DropTarget wood = conc;
    wood.row = {"sw", "vwood"};
    CHECK(!CanDrop(s, "L", {{"sw", "vwood"}}, wood, true));
    CHECK(!CanDrop(s, "L", {{"sw", "vwood"}, {"sw", "vconc"}}, wood, true));  // review: several rows too
    CHECK(CanDrop(s, "L", {{"sw", "vwood"}, {"sw", "vconc"}}, wood));            // a move still groups them
    CHECK(!CopyRows(s, "L", {{"", "sw"}}, into_m, nullptr));  // the root is refused (as Cut)
    std::set<std::string> uids;
    for (const SoundNode& n : FindLibrary(s, "L")->nodes) uids.insert(n.uid);
    CHECK(uids.size() == FindLibrary(s, "L")->nodes.size());
}

// 13-72: Antho's case. Group Loco {Run, Walk}, group Feet {Boots, Bare} (default Bare); library T:
// root Switch "lt" on Loco, Run = Switch "smr" (Surface), Walk = Switch "smw"; each holds a Sound.
static SoundStore LocoStore()
{
    SoundStore s;
    SwitchGroup loco;
    loco.uid = "gloco";
    loco.name = "LocomotionType";
    loco.values = {{"vrun", "Run", {}}, {"vwalk", "Walk", {}}, {"vsprint", "Sprint", {}}};
    SwitchGroup feet;
    feet.uid = "gfeet";
    feet.name = "FeetType";
    feet.values = {{"vboots", "Boots", {}}, {"vbare", "Bare", {}}};
    feet.default_value = "vbare";
    SwitchGroup surf;
    surf.uid = "gsurf";
    surf.name = "Surface Material";
    surf.values = {{"vwood", "Wood", {}}};
    s.groups = {loco, feet, surf};
    SoundLibrary l;
    l.uid = "T";
    l.name = "Steps";
    l.root = "lt";
    SoundNode lt = Node("lt", "", SoundNodeType::Switch, "LocomotionType");
    lt.group_uid = "gloco";
    lt.cases = {{"vrun", "smr", {}}, {"vwalk", "smw", {}}};
    SoundNode smr = Node("smr", "lt", SoundNodeType::Switch, "Switch");
    smr.group_uid = "gsurf";
    smr.cases = {{"vwood", "sr", {}}};
    SoundNode smw = smr;
    smw.uid = "smw";
    smw.cases = {{"vwood", "sw2", {}}};
    l.nodes = {lt, smr, Node("sr", "smr", SoundNodeType::Sound, "RunWood"), smw,
               Node("sw2", "smw", SoundNodeType::Sound, "WalkWood")};
    s.libraries.push_back(l);
    return s;
}

static void TestWrapEachAndValueContents()
{
    // Wrap each in: Run and Walk picked, one FeetType Switch around each, each on its own value,
    // the Surface Switch on FeetType's default value (Bare), not Unassigned.
    SoundStore s = LocoStore();
    std::vector<std::string> made;
    CHECK(WrapRows(s, "T", {{"lt", "vrun"}, {"lt", "vwalk"}}, SoundNodeType::Switch, "gfeet", &made));
    CHECK(made.size() == 2);
    CHECK(Case(s, "T", "lt", "vrun") == made[0] && Case(s, "T", "lt", "vwalk") == made[1]);
    for (size_t i = 0; i < made.size(); ++i) {
        const SoundNode* w = N(s, "T", made[i]);
        CHECK(w->type == SoundNodeType::Switch && w->group_uid == "gfeet" && w->unassigned.empty());
        CHECK(Case(s, "T", made[i].c_str(), "vbare") == (i == 0 ? "smr" : "smw"));
        CHECK(w->default_value.empty());  // plays the group's default (Bare)
    }
    CHECK(N(s, "T", "smr")->parent == made[0] && N(s, "T", "sr")->parent == "smr");
    // A row picked twice (same node) is wrapped once.
    SoundStore d = LocoStore();
    CHECK(WrapRows(d, "T", {{"lt", "vrun"}, {"lt", "vrun"}}, SoundNodeType::Random, "", &made) && made.size() == 1);

    // Wrap value contents in, on the Switch row: the same in one gesture; the Switch stays.
    SoundStore v = LocoStore();
    const SoundLibrary& vl = *FindLibrary(v, "T");
    CHECK((ValueContentRows(vl, {{"", "lt"}}) == std::vector<LibraryRow>{{"lt", "vrun"}, {"lt", "vwalk"}}));
    CHECK(ValueContentRows(vl, {{"lt", "vsprint"}}).empty());  // an empty value: nothing to wrap
    CHECK(WrapValueContents(v, "T", {{"", "lt"}}, SoundNodeType::Switch, "gfeet", &made));
    CHECK(made.size() == 2 && FindLibrary(v, "T")->root == "lt");
    CHECK(SerializeSoundStore(v) != SerializeSoundStore(LocoStore()));
    CHECK(Case(v, "T", "lt", "vrun") == made[0] && Case(v, "T", made[0].c_str(), "vbare") == "smr");
    CHECK(Case(v, "T", "lt", "vwalk") == made[1] && Case(v, "T", made[1].c_str(), "vbare") == "smw");
    // On a value row: only that value's content.
    SoundStore one = LocoStore();
    CHECK(WrapValueContents(one, "T", {{"lt", "vwalk"}}, SoundNodeType::Blend, "", &made) && made.size() == 1);
    CHECK(Case(one, "T", "lt", "vwalk") == made[0] && N(one, "T", "smw")->parent == made[0] &&
          Case(one, "T", "lt", "vrun") == "smr");
    CHECK(!WrapValueContents(one, "T", {{"lt", "vsprint"}}, SoundNodeType::Random, "", &made));

    // A name that matches a value wins over the default; a group with no value: Unassigned.
    SoundStore m = LocoStore();
    FindLibrary(m, "T")->nodes[1].name = "Boots";  // smr
    CHECK(WrapRows(m, "T", {{"lt", "vrun"}}, SoundNodeType::Switch, "gfeet", &made));
    CHECK(Case(m, "T", made[0].c_str(), "vboots") == "smr" && N(m, "T", made[0])->unassigned.empty());
    SoundStore e = LocoStore();
    SwitchGroup empty;
    empty.uid = "gempty";
    empty.name = "Empty";
    e.groups.push_back(empty);
    CHECK(WrapRows(e, "T", {{"lt", "vrun"}}, SoundNodeType::Switch, "gempty", &made));
    CHECK((N(e, "T", made[0])->unassigned == std::vector<std::string>{"smr"}));
}

static void TestOrphanStoryAndPutBack()
{
    // Change the root's group to FeetType: Run / Walk match nothing, both go to Unassigned and keep
    // where they were (path, Switch, value), saved with the store.
    SoundStore s = LocoStore();
    SoundAssign a;
    a.key = "legs.step";
    a.values = {{"gloco", "vrun", {}}};
    s.project.push_back(a);
    SoundCharacter ch;
    ch.uid = "c1";
    ch.name = "Knight";
    ch.assigns = {a};
    s.characters.push_back(ch);
    CHECK(SetRowsType(s, "T", {{"", "lt"}}, SoundNodeType::Switch, "gfeet"));
    const SoundNode* r = N(s, "T", "smr");
    CHECK((N(s, "T", "lt")->unassigned == std::vector<std::string>{"smr", "smw"}));
    CHECK(r->was_path == "LocomotionType \xE2\x80\xBA Run" && r->was_switch == "lt" && r->was_value == "vrun");
    CHECK(N(s, "T", "sr")->was_path.empty());  // its children did not move
    const auto used = OrphanUsedBy(s, "T", *r);
    CHECK(used.size() == 2 && used[0].first.empty() && used[0].second == "legs.step" && used[1].first == "Knight");
    {  // a target playing another library, or none, does not use it
        SoundStore o = s;
        o.project[0].state = LibState::Library;
        o.project[0].library = "Other";
        o.characters[0].assigns[0].state = LibState::None;
        CHECK(OrphanUsedBy(o, "T", *N(o, "T", "smr")).empty());
        o.project[0].library = "T";
        CHECK(OrphanUsedBy(o, "T", *N(o, "T", "smr")).size() == 1);
    }
    CHECK(OrphanUsedBy(s, "T", *N(s, "T", "smw")).empty());
    SoundStore back;
    CHECK(ParseSoundStore(SerializeSoundStore(s), &back));
    CHECK(N(back, "T", "smr")->was_value == "vrun" && N(back, "T", "smr")->was_path == r->was_path);
    // Its value is gone from the Switch's group: cannot be put back yet.
    std::string why;
    CHECK(!CanPutBack(s, *FindLibrary(s, "T"), "smr", &why) && !why.empty());
    CHECK(!PutBackRows(s, "T", {{"lt", "smr"}}, nullptr));
    // Back on LocomotionType: both go back onto Run / Walk, and forget their story.
    CHECK(SetRowsType(s, "T", {{"", "lt"}}, SoundNodeType::Switch, "gloco"));
    // (a Switch -> Switch change maps by value name: none matched, they stay Unassigned, story kept)
    CHECK(N(s, "T", "smr")->was_value == "vrun");
    std::vector<std::string> placed;
    CHECK(CanPutBack(s, *FindLibrary(s, "T"), "smr", &why));
    CHECK(PutBackRows(s, "T", {{"lt", "smr"}, {"lt", "smw"}}, &placed) && placed.size() == 2);
    CHECK(Case(s, "T", "lt", "vrun") == "smr" && Case(s, "T", "lt", "vwalk") == "smw" && N(s, "T", "lt")->unassigned.empty());
    CHECK(N(s, "T", "smr")->was_path.empty() && N(s, "T", "smr")->was_value.empty());
    // A value taken since: what it plays goes to Unassigned (with its own story).
    SoundStore t = LocoStore();
    CHECK(ClearValue(t, "T", {"lt", "vrun"}));
    CHECK(N(t, "T", "smr")->was_value == "vrun");
    DropTarget onto;
    onto.lib = "T";
    onto.row = {"lt", "vrun"};
    onto.mode = DropMode::OntoValue;
    std::vector<std::string> moved;
    CHECK(MoveRows(t, "T", {{"lt", "vwalk"}}, onto, &moved));  // Walk's content now plays Run
    CHECK(PutBackRows(t, "T", {{"lt", "smr"}}, &placed));
    CHECK(Case(t, "T", "lt", "vrun") == "smr" && Has(N(t, "T", "lt")->unassigned, "smw"));
    CHECK(N(t, "T", "smw")->was_value == "vrun");
    // Not an orphan / never on a value: refused.
    CHECK(!CanPutBack(t, *FindLibrary(t, "T"), "sr", &why));
}

static void TestPasteOnto()
{
    // Replace contents across types: Random "rb" (two Sounds) takes Switch "smr"'s contents and type.
    SoundStore s = LocoStore();
    SoundLibrary& l = *FindLibrary(s, "T");
    SoundNode rb = Node("rb", "lt", SoundNodeType::Random, "Bank");
    rb.volume_db = -3.0;
    l.nodes.push_back(rb);
    l.nodes.push_back(Node("b1", "rb", SoundNodeType::Sound, "B1"));
    l.nodes.push_back(Node("b2", "rb", SoundNodeType::Sound, "B2"));
    FindNode(l, "lt")->cases.push_back({"vsprint", "rb", {}});
    FindNode(l, "smr")->default_value = "vwood";
    std::string why;
    CHECK(CanPasteOnto(s, "T", "smr", "T", "rb", PasteOntoWhat::Contents, &why));
    CHECK(PasteOnto(s, "T", "smr", "T", "rb", PasteOntoWhat::Contents));
    const SoundNode* t = N(s, "T", "rb");
    CHECK(t->type == SoundNodeType::Switch && t->group_uid == "gsurf" && t->default_value == "vwood");
    CHECK(t->name == "Bank" && t->volume_db == -3.0 && Case(s, "T", "lt", "vsprint") == "rb");  // keeps its place
    const std::string k = Case(s, "T", "rb", "vwood");
    CHECK(!k.empty() && k != "sr" && N(s, "T", k)->name == "RunWood" && N(s, "T", k)->parent == "rb");
    CHECK(!N(s, "T", "b1") && !N(s, "T", "b2"));      // its old children are gone
    CHECK(N(s, "T", "sr")->parent == "smr");           // the source is untouched
    // And back: a Switch target takes a Random's contents (cases cleared, children kept as a list).
    SoundStore r = LocoStore();
    SoundLibrary& rl = *FindLibrary(r, "T");
    rl.nodes.push_back(Node("rb", "lt", SoundNodeType::Random, "Bank"));
    rl.nodes.push_back(Node("b1", "rb", SoundNodeType::Sound, "B1"));
    FindNode(rl, "lt")->cases.push_back({"vsprint", "rb", {}});
    CHECK(PasteOnto(r, "T", "rb", "T", "smw", PasteOntoWhat::Contents));
    const SoundNode* w = N(r, "T", "smw");
    CHECK(w->type == SoundNodeType::Random && w->group_uid.empty() && w->cases.empty() && !N(r, "T", "sw2"));
    CHECK(ChildrenOf(*FindLibrary(r, "T"), "smw").size() == 1 && ChildrenOf(*FindLibrary(r, "T"), "smw")[0]->name == "B1");
    // Refused: one inside the other, a Sound's contents, itself.
    CHECK(!CanPasteOnto(r, "T", "lt", "T", "smr", PasteOntoWhat::Contents, &why) && !why.empty());
    CHECK(!CanPasteOnto(r, "T", "sr", "T", "smr", PasteOntoWhat::Contents, &why));
    CHECK(!CanPasteOnto(r, "T", "smr", "T", "smr", PasteOntoWhat::Settings, &why));
    CHECK(!PasteOnto(r, "T", "lt", "T", "smr", PasteOntoWhat::Contents));
    // Across libraries: the copy lands in the target's library.
    SoundStore x = Store();
    CHECK(PasteOnto(x, "L", "gr", "M", "mr", PasteOntoWhat::Contents));
    CHECK(ChildrenOf(*FindLibrary(x, "M"), "mr").size() == 1 && N(x, "L", "g1")->parent == "gr");

    // Replace settings: dB, Avoid repeat; the default between two Switches on one group; nothing else.
    SoundStore st = LocoStore();
    SoundLibrary& sl = *FindLibrary(st, "T");
    FindNode(sl, "smr")->volume_db = -9.0;
    FindNode(sl, "smr")->avoid_repeat = 4;
    FindNode(sl, "smr")->default_value = "vwood";
    CHECK(PasteOnto(st, "T", "smr", "T", "smw", PasteOntoWhat::Settings));
    const SoundNode* sw = N(st, "T", "smw");
    CHECK(sw->volume_db == -9.0 && sw->avoid_repeat == 4 && sw->default_value == "vwood");
    CHECK(sw->name == "Switch" && Case(st, "T", "smw", "vwood") == "sw2");  // contents kept
    CHECK(PasteOnto(st, "T", "sr", "T", "sw2", PasteOntoWhat::Settings) == false);  // nothing changed (both 0 dB)
    CHECK(!CanPasteOnto(st, "T", "sr", "T", "smw", PasteOntoWhat::Settings, &why));  // a Sound onto a container
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
    TestUnwrap();
    TestCopyPaste();
    TestWrapEachAndValueContents();
    TestOrphanStoryAndPutBack();
    TestPasteOnto();
    TestRavOwnedGroup();
    if (g_fail) {
        std::printf("library_tree_edit_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("library_tree_edit_test: all passed\n");
    return 0;
}
