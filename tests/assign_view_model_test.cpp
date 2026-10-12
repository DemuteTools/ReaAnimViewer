// SPDX-License-Identifier: MIT
//
// Story 13-15b -- src/assign_view_model.h (pure): the item's targets (rule pairs, auto types), one target's
// state read from the resolver (library, Switch values and their sources, "not reached"), the summary text,
// mixed selections, the Plays / ladder / track texts and the tree's selection rules.

#include "assign_view_model.h"

#include <cstdio>
#include <string>
#include <vector>

#include "sound_auto_assign.h"
#include "sound_record.h"

using namespace rav;
using namespace rav::assign;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

static Block Rule(const std::string& marker, const std::string& rid, int role)
{
    Block b;
    b.marker = marker;
    b.rid = rid;
    Condition c;
    c.signal.bones = {role};
    b.conditions.push_back(c);
    return b;
}

static Block Auto(AutoKind k, char side, const std::string& rid)
{
    Block b = MakeAutoBlock(k, side, kAutoDefaultSens, 0.0);
    b.rid = rid;
    return b;
}

static SwitchGroup Group(const std::string& uid, const std::string& name, std::vector<std::string> values)
{
    SwitchGroup g;
    g.uid = uid;
    g.name = name;
    for (const std::string& v : values) {
        SwitchValue sv;
        sv.uid = uid + "_" + v;
        sv.name = v;
        g.values.push_back(sv);
    }
    return g;
}

static SoundNode Node(const std::string& uid, const std::string& parent, SoundNodeType t)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = t;
    n.name = uid;
    return n;
}

// "FS Hero": Switch(Loco): Walk -> s1, Run -> Switch(Feet): Boots -> s2, Shoes -> s3; Crouch -> s4.
static SoundStore MakeStore()
{
    SoundStore st;
    st.groups.push_back(Group("loco", "Loco", {"Walk", "Run", "Crouch"}));
    st.groups.push_back(Group("feet", "FeetType", {"Boots", "Shoes"}));
    SoundLibrary hero;
    hero.uid = "hero";
    hero.name = "FS Hero";
    hero.color = 0x1000000u | 0x5b8cff;
    hero.root = "sw1";
    SoundNode sw1 = Node("sw1", "", SoundNodeType::Switch);
    sw1.group_uid = "loco";
    sw1.cases = {{"loco_Walk", "s1", {}}, {"loco_Run", "sw2", {}}, {"loco_Crouch", "s4", {}}};
    sw1.default_value = "loco_Walk";
    SoundNode sw2 = Node("sw2", "sw1", SoundNodeType::Switch);
    sw2.group_uid = "feet";
    sw2.cases = {{"feet_Boots", "s2", {}}, {"feet_Shoes", "s3", {}}};
    sw2.default_value = "feet_Shoes";
    hero.nodes = {sw1, Node("s1", "sw1", SoundNodeType::Sound), sw2, Node("s2", "sw2", SoundNodeType::Sound),
                  Node("s3", "sw2", SoundNodeType::Sound), Node("s4", "sw1", SoundNodeType::Sound)};
    st.libraries.push_back(hero);
    SoundLibrary empty;
    empty.uid = "hands";
    empty.name = "Hands";
    st.libraries.push_back(empty);
    return st;
}

static void TestTargets()
{
    std::vector<Block> blocks = {Rule("Scuff L", "r0", 1), Rule("Hit", "r1", 2), Rule("Scuff R", "r2", 1),
                                 Auto(AutoKind::Step, 'L', "a0"), Auto(AutoKind::Step, 'R', "a1"),
                                 Auto(AutoKind::Lift, 'L', "a2")};
    const Targets t = BuildTargets(blocks);
    CHECK(t.rules.size() == 2);
    CHECK(t.rules[0].name == "Scuff" && t.rules[0].leaves.size() == 2);
    CHECK(t.rules[0].leaves[0].name == "Scuff L" && t.rules[0].leaves[1].name == "Scuff R");
    CHECK(t.rules[0].leaves[0].key == "b0" && t.rules[0].leaves[1].key == "b2");
    CHECK(t.rules[1].name == "Hit" && t.rules[1].leaves.size() == 1);
    CHECK(t.autos.size() == 2 && t.autos[0].name == "Step" && t.autos[0].leaves.size() == 2 && t.autos[1].name == "Lift-off");
    CHECK(t.ParentOf("b2") == &t.rules[0] && t.FindLeaf("b5")->rid == "a2" && !t.FindLeaf("b9"));
    int b = -1;
    CHECK(BlockOfKey("b12", &b) && b == 12 && !BlockOfKey("item", &b) && !BlockOfKey("b", &b));
    // A pair needs both sides; a lone "Scuff L" is its own rule.
    const Targets lone = BuildTargets({Rule("Scuff L", "x", 1)});
    CHECK(lone.rules.size() == 1 && lone.rules[0].name == "Scuff L");
    char side = 0;
    CHECK(PairStem("Foot R", &side) == "Foot" && side == 'R' && PairStem("Foot", &side).empty() && PairStem("R", &side).empty());
    // The signature follows the blocks.
    blocks[1].marker = "Hit2";
    CHECK(BuildTargets(blocks).signature != t.signature);
}

static void TestState()
{
    SoundStore st = MakeStore();
    SetAssignLibraryIn(st.project, {"feet"}, LibState::Library, "hero");
    SetAssignValueIn(st.project, {"feet"}, "feet", "feet_Boots");
    ItemSounds rec;
    rec.item_values.push_back({"loco", "loco_Run", {}});
    const Block step = Auto(AutoKind::Step, 'L', "a0");
    Target      t = MakeTarget(st, rec, nullptr, step, "b0", "Step \xC2\xB7 FS L");
    CHECK(t.lib && t.lib->uid == "hero" && !t.lib_empty && !t.lib_blocked && !t.OwnLib() && !t.Unset());
    CHECK(t.groups.size() == 2 && t.groups[0].group_uid == "loco" && t.groups[1].group_uid == "feet");
    CHECK(t.groups[0].value_name == "Run" && t.groups[0].from == "item" && !t.groups[0].own);
    CHECK(t.groups[1].value_name == "Boots" && t.groups[1].from == std::string("Project \xC2\xB7 Feet"));
    CHECK(t.groups[0].reached && t.groups[1].reached);
    CHECK(PlaysText(t) == std::string("FS Hero \xE2\x80\xBA Run \xE2\x80\xBA Boots"));
    CHECK(TrackNameOf(t) == "Run-Boots");
    CHECK(WhyText(t) == std::string("Run: item \xC2\xB7 Boots: Project \xC2\xB7 Feet"));

    // The rule's own value wins and is "own"; its inherit gives the item's.
    RuleSounds rs;
    rs.rid = "a0";
    rs.values.push_back({"feet", "feet_Shoes", {}});
    rec.rules.push_back(rs);
    t = MakeTarget(st, rec, nullptr, step, "b0", "x");
    CHECK(t.groups[1].value_name == "Shoes" && t.groups[1].own && t.groups[1].inherit_name == "Boots" &&
          t.groups[1].from == "this type");
    CHECK(t.groups[0].inherit_name == "Run" && t.groups[0].inherit_from == "item");

    // Crouch skips FeetType.
    rec.item_values[0].value_uid = "loco_Crouch";
    t = MakeTarget(st, rec, nullptr, step, "b0", "x");
    CHECK(t.groups[0].reached && !t.groups[1].reached && t.groups[1].not_reached == "not reached under Crouch");
    CHECK(PlaysText(t) == std::string("FS Hero \xE2\x80\xBA Crouch") && TrackNameOf(t) == "Crouch");
    CHECK(OverviewCell(t, "feet").kind == CellKind::NotReached && OverviewCell(t, "loco").kind == CellKind::Value);

    // An empty library, then None, then nothing.
    SetAssignLibraryIn(st.project, {"feet"}, LibState::Library, "hands");
    t = MakeTarget(st, ItemSounds(), nullptr, step, "b0", "x");
    CHECK(t.lib_empty && !t.lib_blocked && t.groups.empty() && PlaysText(t) == std::string("Hands \xE2\x80\xBA empty"));
    SetAssignLibraryIn(st.project, {"feet"}, LibState::None, "");
    t = MakeTarget(st, ItemSounds(), nullptr, step, "b0", "x");
    CHECK(!t.lib && t.ea.none && !t.Unset() && PlaysText(t) == "No sound");
    SetAssignLibraryIn(st.project, {"feet"}, LibState::Inherit, "");
    t = MakeTarget(st, ItemSounds(), nullptr, step, "b0", "x");
    CHECK(t.Unset() && Summarize(st, {&t}, true).Plain() == "nothing set");
}

static void TestDeletedGroup()
{
    SoundStore st = MakeStore();
    st.groups.erase(st.groups.begin() + 1);  // FeetType gone
    SetAssignLibraryIn(st.project, {"feet"}, LibState::Library, "hero");
    const Block step = Auto(AutoKind::Step, 'L', "a0");
    Target      t = MakeTarget(st, ItemSounds(), nullptr, step, "b0", "x");
    CHECK(t.lib_blocked && t.blocked_reason.find("Switch Group") != std::string::npos);
    CHECK(t.groups.size() == 2 && t.groups[1].deleted);
    CHECK(PlaysText(t) == std::string("FS Hero \xE2\x80\xBA blocked"));
    const Summary s = Summarize(st, {&t}, true);
    CHECK(s.blocked && s.Plain().find("! group deleted") != std::string::npos);
}

static void TestSummaryAndMix()
{
    SoundStore st = MakeStore();
    SetAssignLibraryIn(st.project, {"feet"}, LibState::Library, "hero");
    ItemSounds rec;
    RuleSounds a, b;
    a.rid = "a0";
    a.values.push_back({"feet", "feet_Boots", {}});
    b.rid = "a1";
    b.values.push_back({"feet", "feet_Shoes", {}});
    rec.rules = {a, b};
    rec.item_values.push_back({"loco", "loco_Run", {}});  // FeetType is only reached under Run
    const Target l = MakeTarget(st, rec, nullptr, Auto(AutoKind::Step, 'L', "a0"), "b0", "L");
    const Target r = MakeTarget(st, rec, nullptr, Auto(AutoKind::Step, 'R', "a1"), "b1", "R");
    const Summary s = Summarize(st, {&l, &r}, true);
    CHECK(s.has_dot && s.Plain() == std::string("FS Hero \xC2\xB7 Run \xE2\x80\xBA (per side)"));
    CHECK(s.segs[0].tone == SegTone::Inherited);  // the library is inherited from the Project
    CHECK(Summarize(st, {&l}, false).Plain() == std::string("Run \xE2\x80\xBA Boots"));
    Mix m = MixOf({&l, &r});
    CHECK(!m.lib && m.groups && m.own_settings == 1);
    // Different libraries.
    rec.rules[1].library = "hands";
    const Target r2 = MakeTarget(st, rec, nullptr, Auto(AutoKind::Step, 'R', "a1"), "b1", "R");
    m = MixOf({&l, &r2});
    CHECK(m.lib && m.own_settings == 2);
    CHECK(Summarize(st, {&l, &r2}, true).Plain() == "mixed libraries");
    CHECK(OwnLibraryArg(r2) == "hands" && OwnLibraryArg(l).empty());
    const std::vector<PlaysLine> lines = PlaysLines({&l, &r});
    CHECK(lines.size() == 2 && lines[0].titles[0] == "L");

    // The Item row: the item's own values over the defaults, for the groups the libraries use.
    const Target a2 = MakeTarget(st, rec, nullptr, Auto(AutoKind::Step, 'L', "a0"), "b0", "L");
    const Target item = MakeItemTarget(st, rec, {a2});
    CHECK(item.is_item && item.groups.size() == 2 && item.groups[0].group_uid == "loco" && item.groups[0].own &&
          item.groups[0].value_name == "Run" && item.groups[1].value_name == "Shoes" && !item.groups[1].own);
    CHECK(SummarizeItem(st, item).Plain() == "Run");
    CHECK(SummarizeItem(st, MakeItemTarget(st, ItemSounds(), {a2})).Plain() == "defaults");
    CHECK(OverviewGroups(st, {a2}) == (std::vector<std::string>{"loco", "feet"}));
    // Events that set their own value of a group.
    EventSounds ev;
    ev.rid = "a0";
    ev.t = 1.0;
    ev.overrides.push_back({"feet", "feet_Boots", {}});
    rec.events.push_back(ev);
    CHECK(EventOverrideCount(rec, {"a0", "a1"}, "feet") == 1 && EventOverrideCount(rec, {"a1"}, "feet") == 0);
}

static void TestTitleAndLadder()
{
    SoundStore st = MakeStore();
    SetAssignLibraryIn(st.project, {"feet"}, LibState::Library, "hero", true);
    st.libraries[0].kit = {"feet"};
    SoundCharacter hero;
    hero.uid = "C";
    hero.name = "Hero";
    SetAssignLibraryIn(hero.assigns, {"feet.step"}, LibState::Library, "hands");
    const std::vector<Block> blocks = {Auto(AutoKind::Step, 'L', "a0"), Auto(AutoKind::Step, 'R', "a1")};
    const Targets            tg = BuildTargets(blocks);
    const Target l = MakeTarget(st, ItemSounds(), &hero, blocks[0], "b0", "Step \xC2\xB7 FS L");
    const Target r = MakeTarget(st, ItemSounds(), &hero, blocks[1], "b1", "Step \xC2\xB7 FS R");
    CHECK(SelectionTitle(tg, {&l}) == std::string("Step \xC2\xB7 FS L"));
    CHECK(SelectionTitle(tg, {&l, &r}) == std::string("Step \xC2\xB7 both sides"));
    const Target none = MakeTarget(st, ItemSounds(), nullptr, Rule("X", "z", 1), "b9", "X");
    CHECK(SelectionTitle(tg, {&l, &none}) == "2 targets");
    const Ladder ld = BuildLadder(st, &hero, l);
    CHECK(ld.kits.size() == 1 && ld.kits[0] == "FS Hero" && ld.rows.size() == 5);
    CHECK(ld.rows[0].label == std::string("Project \xC2\xB7 Feet") && ld.rows[0].set && ld.rows[0].kit && !ld.rows[0].hit);
    CHECK(ld.rows[3].label == std::string("Hero \xC2\xB7 Feet \xC2\xB7 Step") && ld.rows[3].hit && ld.rows[3].library == "hands");
    CHECK(ld.rows[4].label == "this type" && !ld.rows[4].set);
    CHECK(ScopeLine(true, false, "p", "Hero") == "every rule of this item, unless the rule sets its own");
    CHECK(ScopeLine(false, true, "this item + 2 pool copies", "Hero") ==
          std::string("this item + 2 pool copies \xC2\xB7 character Hero"));
    const ChangeText c = LastChange("Set rule library", "Hero_Run_01", "Library = FS Hero", 2);
    CHECK(c.bold == "RAV: Set rule library" && c.rest == std::string(" \xC2\xB7 Hero_Run_01 \xC2\xB7 Library = FS Hero \xC2\xB7 2 targets"));
    CHECK(LastChange("x", "w", "v", 1).rest.find("1 target") != std::string::npos);
}

static void TestSelection()
{
    const std::vector<std::string> vis = {"item", "b0", "b1", "b2", "b3"};
    Selection s;
    ClickRow(s, "b1", false, false, vis);
    CHECK(s.keys == std::vector<std::string>{"b1"} && s.anchor == "b1");
    ClickRow(s, "b3", true, false, vis);  // range
    CHECK((s.keys == std::vector<std::string>{"b1", "b2", "b3"}) && s.anchor == "b1" && s.cursor == "b3");
    ClickRow(s, "b0", true, false, vis);  // range back from the anchor
    CHECK((s.keys == std::vector<std::string>{"b0", "b1"}));
    ClickRow(s, "b3", false, true, vis);  // ctrl adds
    CHECK(s.Has("b3") && s.keys.size() == 3);
    ClickRow(s, "b3", false, true, vis);  // ...and removes
    CHECK(!s.Has("b3"));
    // The Item row and rules are never mixed: ctrl on the item row selects only it; a range does not cross.
    ClickRow(s, "item", false, true, vis);
    CHECK(s.keys == std::vector<std::string>{"item"});
    ClickRow(s, "b2", true, false, vis);
    CHECK(s.keys == std::vector<std::string>{"b2"});
    ClickRow(s, "b1", false, false, vis);
    ClickRow(s, "b3", true, false, {"item", "b1", "b3"});  // rows between are not visible (folded)
    CHECK((s.keys == std::vector<std::string>{"b1", "b3"}));

    // A parent selects its leaves; its box toggles them.
    ClickParent(s, {"b0", "b1"}, false);
    CHECK((s.keys == std::vector<std::string>{"b0", "b1"}) && ParentState(s, {"b0", "b1"}) == 1);
    ClickParent(s, {"b1", "b2"}, true);
    CHECK(ParentState(s, {"b1", "b2"}) == 1 && s.Has("b0"));
    ClickParent(s, {"b1", "b2"}, true);
    CHECK(ParentState(s, {"b1", "b2"}) == 0 && ParentState(s, {"b0", "b1"}) == 2);
    s.keys = {"item"};
    ClickParent(s, {"b0"}, true);
    CHECK(s.keys == std::vector<std::string>{"b0"});
    Prune(s, {"b5"});
    CHECK(s.keys.empty() && s.anchor.empty());

    CHECK(FilterMatch("", "x") && FilterMatch("SCU", "Scuff") && !FilterMatch("zz", "Scuff"));
}

int main()
{
    TestTargets();
    TestState();
    TestDeletedGroup();
    TestSummaryAndMix();
    TestTitleAndLadder();
    TestSelection();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("assign_view_model: all checks passed\n");
    return 0;
}
