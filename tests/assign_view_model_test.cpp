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
    SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, "hero");
    SetAssignValueIn(st.project, {"legs"}, "feet", "feet_Boots");
    ItemSounds rec;
    rec.item_values.push_back({"loco", "loco_Run", {}});
    const Block step = Auto(AutoKind::Step, 'L', "a0");
    Target      t = MakeTarget(st, rec, nullptr, step, "b0", "Step \xC2\xB7 FS L");
    CHECK(t.lib && t.lib->uid == "hero" && !t.lib_empty && !t.lib_blocked && !t.OwnLib() && !t.Unset());
    CHECK(t.groups.size() == 2 && t.groups[0].group_uid == "loco" && t.groups[1].group_uid == "feet");
    CHECK(t.groups[0].value_name == "Run" && t.groups[0].from == "item" && !t.groups[0].own);
    CHECK(t.groups[1].value_name == "Boots" && t.groups[1].from == std::string("Project \xC2\xB7 Legs"));
    CHECK(t.groups[0].reached && t.groups[1].reached);
    CHECK(PlaysText(t) == std::string("FS Hero \xE2\x80\xBA Run \xE2\x80\xBA Boots"));
    CHECK(TrackNameOf(t) == "Run-Boots");
    CHECK(WhyText(t) == std::string("Run: item \xC2\xB7 Boots: Project \xC2\xB7 Legs"));

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
    SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, "hands");
    t = MakeTarget(st, ItemSounds(), nullptr, step, "b0", "x");
    CHECK(t.lib_empty && !t.lib_blocked && t.groups.empty() && PlaysText(t) == std::string("Hands \xE2\x80\xBA empty"));
    SetAssignLibraryIn(st.project, {"legs"}, LibState::None, "");
    t = MakeTarget(st, ItemSounds(), nullptr, step, "b0", "x");
    CHECK(!t.lib && t.ea.none && !t.Unset() && PlaysText(t) == "No sound");
    SetAssignLibraryIn(st.project, {"legs"}, LibState::Inherit, "");
    t = MakeTarget(st, ItemSounds(), nullptr, step, "b0", "x");
    CHECK(t.Unset() && Summarize(st, {&t}, true).Plain() == "nothing set");
}

// Review 13-15b: every child of a Random can be drawn, so a Switch under its second child is reached.
static void TestRandomReach()
{
    SoundStore st = MakeStore();
    SoundLibrary mix;
    mix.uid = "mix";
    mix.name = "Mix";
    mix.root = "r";
    SoundNode a = Node("swa", "r", SoundNodeType::Switch);
    a.group_uid = "loco";
    a.cases = {{"loco_Walk", "sa", {}}};
    SoundNode b = Node("swb", "r", SoundNodeType::Switch);
    b.group_uid = "feet";
    b.cases = {{"feet_Boots", "sb", {}}};
    mix.nodes = {Node("r", "", SoundNodeType::Random), a, Node("sa", "swa", SoundNodeType::Sound), b,
                 Node("sb", "swb", SoundNodeType::Sound)};
    st.libraries.push_back(mix);
    SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, "mix");
    const Target t = MakeTarget(st, ItemSounds(), nullptr, Auto(AutoKind::Step, 'L', "a0"), "b0", "x");
    CHECK(t.groups.size() == 2 && t.groups[0].reached && t.groups[1].reached);
}

static void TestDeletedGroup()
{
    SoundStore st = MakeStore();
    st.groups.erase(st.groups.begin() + 1);  // FeetType gone
    SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, "hero");
    const Block step = Auto(AutoKind::Step, 'L', "a0");
    Target      t = MakeTarget(st, ItemSounds(), nullptr, step, "b0", "x");
    CHECK(t.lib_blocked && t.blocked_reason.find("Switch Group") != std::string::npos);
    CHECK(t.groups.size() == 2 && t.groups[1].deleted);
    CHECK(PlaysText(t) == std::string("FS Hero \xE2\x80\xBA blocked"));
    const Summary s = Summarize(st, {&t}, true);
    CHECK(s.blocked && s.Plain().find("! group deleted") != std::string::npos);
}

// 13-21: a library gap warns: the library stays usable, the banner lists every reason.
static void TestGaps()
{
    SoundStore st = MakeStore();
    SoundLibrary& hero = st.libraries[0];
    hero.nodes[0].default_value = "loco_Crouch";
    hero.nodes[0].cases.pop_back();  // Crouch: no sound
    hero.nodes.pop_back();
    hero.nodes[0].name.clear();  // unnamed: its reason names the Switch Group, never the uid
    SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, "hero");
    Target t = MakeTarget(st, ItemSounds(), nullptr, Auto(AutoKind::Step, 'L', "a0"), "b0", "x");
    CHECK(!t.lib_blocked && t.Plays() && t.lib_warnings.size() == 1 && t.lib_warnings[0].node == "sw1");
    CHECK(GapsText(t) == "FS Hero has gaps: Loco: default Crouch has no sound. Nothing plays from them.");
    CHECK(!Summarize(st, {&t}, true).blocked);
    // Two gaps (an empty Blend too), then more than three.
    SoundLibrary& h2 = st.libraries[0];
    h2.nodes.push_back(Node("eb", "sw2", SoundNodeType::Blend));
    h2.nodes.back().name.clear();  // unnamed: its type
    h2.nodes[2].cases.push_back({"feet_Boots", "eb", {}});
    h2.nodes[2].cases.erase(h2.nodes[2].cases.begin());  // Boots -> the empty Blend
    t = MakeTarget(st, ItemSounds(), nullptr, Auto(AutoKind::Step, 'L', "a0"), "b0", "x");
    CHECK(t.lib_warnings.size() == 2 &&
          GapsText(t) == "FS Hero has gaps: Loco: default Crouch has no sound; Blend: empty Blend. Nothing plays from them.");
    for (int i = 0; i < 3; ++i) t.lib_warnings.push_back({"x", "more"});
    CHECK(GapsText(t) ==
          "FS Hero has gaps: Loco: default Crouch has no sound; Blend: empty Blend; more +2 more. Nothing plays from them.");
    // A blocked library has no gap banner.
    t.lib_blocked = true;
    CHECK(GapsText(t).empty());
}

static void TestSummaryAndMix()
{
    SoundStore st = MakeStore();
    SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, "hero");
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
    SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, "hero", true);
    st.libraries[0].kit = {"legs"};
    SoundCharacter hero;
    hero.uid = "C";
    hero.name = "Hero";
    SetAssignLibraryIn(hero.assigns, {"legs.step"}, LibState::Library, "hands");
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
    CHECK(ld.rows[0].label == std::string("Project \xC2\xB7 Legs") && ld.rows[0].set && ld.rows[0].kit && !ld.rows[0].hit);
    CHECK(ld.rows[3].label == std::string("Hero \xC2\xB7 Legs \xC2\xB7 Step") && ld.rows[3].hit && ld.rows[3].library == "hands");
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

// ---- 13-15c: Project and Character scopes ----

static std::vector<RigIn> TwoRigs()
{
    RigIn a;
    a.skeleton = "3f9ac21e00000000";
    a.bones = 54;
    a.items = 8;
    a.models = {{"mA", {"Hero_Body.fbx"}, 6}, {"mB", {"Hero_Armored.fbx"}, 2}};
    RigIn b;
    b.skeleton = "b7e04d0900000000";
    b.bones = 71;
    b.items = 4;
    b.models = {{"mC", {"Creature_Base.glb"}, 4}};
    return {a, b};
}

static void TestScopes()
{
    SoundStore st = MakeStore();
    SoundLibrary cr = st.libraries[0];
    cr.uid = "cr";
    cr.name = "FS Creature";
    st.libraries.push_back(cr);
    st.libraries[0].kit = {"legs"};
    st.libraries.back().kit = {"legs"};
    SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, "hero", true);
    SetAssignValueIn(st.project, {"legs.step"}, "feet", "feet_Boots");
    // Characters: virtual ones named from the first file, one per rig.
    std::vector<CharEntry> ch = BuildCharacters(st, TwoRigs());
    CHECK(ch.size() == 2 && !ch[0].stored && ch[0].ch.name == "Hero" && ch[0].items == 8 && ch[0].key == "char:v:3f9ac21e00000000");
    CHECK(ch[1].ch.name == "Creature" && ch[0].overrides == 0 && RigSig("3f9ac21e0000") == "3f9a\xC2\xB7" "c21e");
    // A stored whole-rig character with a split: the split is listed after, the whole rig reads the rest.
    SoundCharacter hero;
    hero.uid = "c1";
    hero.name = "Hero";
    hero.skeleton = "3f9ac21e00000000";
    SetAssignLibraryIn(hero.assigns, {"legs"}, LibState::Library, "cr");
    st.characters.push_back(hero);
    SoundCharacter armored = hero;
    armored.uid = "c2";
    armored.name = "Armored";
    armored.model = "mB";
    armored.assigns.clear();
    st.characters.push_back(armored);
    ch = BuildCharacters(st, TwoRigs());
    CHECK(ch.size() == 3 && ch[0].stored && ch[0].items == 6 && ch[0].overrides == 1 && ch[1].ch.name == "Armored" && ch[1].items == 2 &&
          ch[1].ref.model == "mB" && ch[2].ch.name == "Creature");
    // A key read at the Project level: own, with its inherit and sources.
    Target p = MakeKeyTarget(st, nullptr, "legs");
    CHECK(p.OwnLib() && p.own_kit && p.lib && p.lib->uid == "hero" && p.ea.from == "Project \xC2\xB7 Legs" && p.inherited.level == AssignLevel::Unset);
    Target ps = MakeKeyTarget(st, nullptr, "legs.step");
    CHECK(!ps.OwnLib() && ps.lib && ps.ea.from == "Project \xC2\xB7 Legs" && ps.Group("feet") && ps.Group("feet")->own &&
          ps.Group("feet")->value_name == "Boots");
    CHECK(ps.Group("loco") && !ps.Group("loco")->own);
    Target pt = MakeKeyTarget(st, nullptr, "arms");
    CHECK(pt.Unset() && !pt.lib);
    // At the Character level: its own library wins, "inherit" gives the Project's.
    Target c = MakeKeyTarget(st, &ch[0].ch, "legs");
    CHECK(c.OwnLib() && c.lib->uid == "cr" && c.inherited.library == "hero" && c.inherited.from == "Project \xC2\xB7 Legs");
    Target cs = MakeKeyTarget(st, &ch[0].ch, "legs.step");
    CHECK(!cs.OwnLib() && cs.ea.from == "Hero \xC2\xB7 Legs" && cs.Group("feet")->value_name == "Boots" && !cs.Group("feet")->own);
    // The ladder of a key: no rule row, no typed rows for a part.
    Ladder ld = BuildLadder(st, nullptr, p, false);
    CHECK(ld.rows.size() == 1 && ld.rows[0].hit);
    Ladder ld2 = BuildLadder(st, &ch[0].ch, cs, false);
    CHECK(ld2.rows.size() == 4 && ld2.rows[2].set && ld2.rows[2].hit);
    // Kits: hero's kit is the project default of Legs; cr's is used by the Hero character.
    std::vector<KitPart> kp = KitParts(st, st.libraries[0], ch);
    CHECK(kp.size() == 1 && kp[0].state == KitPart::State::ProjectDefault && KitStatusText(kp[0]) == "project default");
    kp = KitParts(st, st.libraries.back(), ch);
    CHECK(kp[0].state == KitPart::State::ProjectUses && kp[0].chars.size() == 1 && KitStatusText(kp[0]) == "used by Hero");
    SoundStore st2 = st;
    st2.characters.clear();
    kp = KitParts(st2, st2.libraries.back(), BuildCharacters(st2, TwoRigs()));
    CHECK(KitStatusText(kp[0]) == "not used");
    CHECK(OtherKitsServing(st, "legs", "hero").size() == 1 && OtherKitsServing(st, "legs", "hero")[0]->uid == "cr" &&
          OtherKitsServing(st, "arms", "").empty());
    CHECK(KitPartWhat("legs").find("and the bone rules on foot roles") != std::string::npos);
    CHECK(KitPartWhat("body") == "Jolt, Spin, Sit and Fall and the bone rules on hips and spine roles");  // 10-9g, 10-9f, 10-9b
    // The kit offer: only the parts free at the Project level, and once.
    st.libraries.back().kit = {"legs", "body"};
    CHECK(KitOfferParts(st, st.libraries.back()) == std::vector<std::string>({"body"}));
    st.libraries.back().kit_offered = true;
    CHECK(KitOfferParts(st, st.libraries.back()).empty());
    // Other characters and the Who plays what cells.
    const std::vector<OtherChar> oc = OtherCharacters(st, ch, 0, "legs");
    CHECK(oc.size() == 2 && oc[0].name == "Armored" && oc[0].ea.library == "hero" && oc[1].name == "Creature");
    const WhoCell w0 = WhoCellFor(st, nullptr, "legs"), w1 = WhoCellFor(st, &ch[0].ch, "legs"), w2 = WhoCellFor(st, &ch[0].ch, "legs.slide"),
                  w3 = WhoCellFor(st, nullptr, "arms");
    CHECK(w0.own && w0.kit && w0.library == "hero" && w1.own && !w1.kit && w1.library == "cr");
    CHECK(!w2.own && w2.library == "cr" && w2.from == "Hero \xC2\xB7 Legs" && w3.unset && !w3.own);
    CHECK(AllKeys().size() >= 10 && AllKeys()[0] == "legs" && AllKeys()[1] == "legs.step");
    // 13-43 Played by: a whole part reads as the part; the characters that inherit the Project count.
    CHECK(PlayedByText(LibraryPlayedBy(st, ch, "hero")) == "Legs (Project, Armored, Creature)");
    CHECK(PlayedByText(LibraryPlayedBy(st, ch, "cr")) == "Legs (Hero)");
    CHECK(LibraryPlayedBy(st, ch, "nope").empty() && PlayedByText({}).empty() && LibraryPlayedBy(st, ch, "").empty());
    {
        // One type taken by another library: the part is no longer whole, its other types are listed, grouped.
        SoundStore st3 = st;
        SetAssignLibraryIn(st3.project, {"legs.step"}, LibState::Library, "cr");
        const std::vector<PlayedByGroup> h = LibraryPlayedBy(st3, ch, "hero");
        size_t legs_types = 0;
        for (const std::string& k : AllKeys()) legs_types += k.compare(0, 5, "legs.") == 0;
        CHECK(h.size() == 1 && h[0].keys.size() == legs_types - 1 &&
              std::find(h[0].keys.begin(), h[0].keys.end(), "legs") == h[0].keys.end() &&
              std::find(h[0].keys.begin(), h[0].keys.end(), "legs.step") == h[0].keys.end() &&
              h[0].columns == std::vector<std::string>({"Project", "Armored", "Creature"}));
        // cr: the whole part in Hero's column, Step alone in the others.
        CHECK(PlayedByText(LibraryPlayedBy(st3, ch, "cr")) == "Legs (Hero); Legs \xC2\xB7 Step (Project, Armored, Creature)");
    }
    CHECK(OverrideKeys(hero.assigns) == std::vector<std::string>({"legs"}));
    // Selection: kits and characters are picked alone.
    Selection s;
    std::vector<std::string> vis = {"kit:a", "kit:b", "legs", "legs.step", "char:c1"};
    ClickRow(s, "kit:a", false, false, vis);
    ClickRow(s, "kit:b", false, true, vis);
    CHECK(s.keys.size() == 1 && s.keys[0] == "kit:b");
    ClickRow(s, "legs", false, false, vis);
    ClickRow(s, "legs.step", false, true, vis);
    CHECK(s.keys.size() == 2 && KindOf("legs.step") == RowKind::Rule && KindOf("kit:a") == RowKind::Kit && KindOf("char:x") == RowKind::Char);
    ClickRow(s, "char:c1", false, true, vis);
    CHECK(s.keys.size() == 1 && s.keys[0] == "char:c1");
}

// 13-65: what each value of a Switch Group places (Own / Default / Nothing), greyed chips and their tooltips.
static SoundLibrary MatLib(const std::string& uid, bool wood_empty, bool metal_sound)
{
    // Material Switch (default Wood): Wood -> Random (3 Sounds, or none), Metal -> Random (a Sound, or none), Grass: no case.
    SoundLibrary lib;
    lib.uid = uid;
    lib.name = uid;
    lib.root = "m";
    SoundNode m = Node("m", "", SoundNodeType::Switch);
    m.group_uid = "mat";
    m.default_value = "mat_Wood";
    m.cases = {{"mat_Wood", "rw", {}}, {"mat_Metal", "rm", {}}};
    lib.nodes = {m, Node("rw", "m", SoundNodeType::Random), Node("rm", "m", SoundNodeType::Random)};
    if (!wood_empty)
        for (const char* k : {"w1", "w2", "w3"}) lib.nodes.push_back(Node(k, "rw", SoundNodeType::Sound));
    if (metal_sound) lib.nodes.push_back(Node("m1", "rm", SoundNodeType::Sound));
    return lib;
}

static void TestValueSounds()
{
    SoundStore st;
    st.groups.push_back(Group("mat", "Material", {"Wood", "Grass", "Metal"}));
    st.groups.push_back(Group("shoes", "Shoes", {"Barefoot", "Boots"}));
    st.groups.push_back(Group("loco", "Loco", {"Walk", "Run"}));
    st.libraries.push_back(MatLib("a", false, false));  // Wood own, Grass -> default Wood, Metal empty
    st.libraries.push_back(MatLib("b", true, true));    // Wood empty: Grass places nothing (Metal own)
    st.libraries.push_back(MatLib("c", false, true));   // Metal own
    // Cascade: Shoes (default Boots): Barefoot -> a Material Switch whose every case is empty, Boots -> a Sound.
    SoundLibrary cas;
    cas.uid = "cas";
    cas.name = "cas";
    cas.root = "sh";
    SoundNode sh = Node("sh", "", SoundNodeType::Switch);
    sh.group_uid = "shoes";
    sh.default_value = "shoes_Boots";
    sh.cases = {{"shoes_Barefoot", "inner", {}}, {"shoes_Boots", "boot", {}}};
    SoundNode inner = Node("inner", "sh", SoundNodeType::Switch);
    inner.group_uid = "mat";
    inner.cases = {{"mat_Wood", "e1", {}}, {"mat_Metal", "e2", {}}};
    cas.nodes = {sh, inner, Node("e1", "inner", SoundNodeType::Random), Node("e2", "inner", SoundNodeType::Blend),
                 Node("boot", "sh", SoundNodeType::Sound)};
    st.libraries.push_back(cas);
    // Reach: Loco (default Walk): Walk -> Material (Metal -> a Sound), Run -> Material (Metal empty).
    SoundLibrary rch;
    rch.uid = "rch";
    rch.name = "rch";
    rch.root = "lo";
    SoundNode lo = Node("lo", "", SoundNodeType::Switch);
    lo.group_uid = "loco";
    lo.default_value = "loco_Walk";
    lo.cases = {{"loco_Walk", "ma", {}}, {"loco_Run", "mb", {}}};
    SoundNode ma = Node("ma", "lo", SoundNodeType::Switch);
    ma.group_uid = "mat";
    ma.default_value = "mat_Metal";
    ma.cases = {{"mat_Metal", "sm", {}}};
    SoundNode mb = Node("mb", "lo", SoundNodeType::Switch);
    mb.group_uid = "mat";
    mb.default_value = "mat_Metal";
    mb.cases = {{"mat_Metal", "em", {}}};
    rch.nodes = {lo, ma, Node("sm", "ma", SoundNodeType::Sound), mb, Node("em", "mb", SoundNodeType::Random)};
    st.libraries.push_back(rch);

    const Block step = Auto(AutoKind::Step, 'L', "a0");
    auto target = [&](const std::string& lib) {
        SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, lib);
        return MakeTarget(st, ItemSounds(), nullptr, step, "b0", "x");
    };
    // Null-safe reads (Own when missing, "?" for a missing name): a regression fails a CHECK, never crashes.
    auto vs_of = [](const Target& t, const std::string& group, const std::string& value) -> const ValueSoundState* {
        const GroupState* g = t.Group(group);
        return g ? g->SoundOf(value) : nullptr;
    };
    auto state = [&](const Target& t, const std::string& group, const std::string& value) {
        const ValueSoundState* v = vs_of(t, group, value);
        return v ? v->state : ValueSound::Own;
    };
    auto def_name = [&](const Target& t, const std::string& group, const std::string& value) {
        const ValueSoundState* v = vs_of(t, group, value);
        return v ? v->default_name : std::string("?");
    };
    auto tip = [&](const Target& t, const std::string& group, const std::string& value) {
        const ValueSoundState* v = vs_of(t, group, value);
        return v ? ChipSoundTip(*v) : std::string("?");
    };
    auto inherit_uid = [](const Target& t, const std::string& group) {
        const GroupState* g = t.Group(group);
        return g ? g->inherit_uid : std::string("?");
    };
    // The states match ResolveEvent: each value set on the item layer places a sound unless Nothing; a Default
    // one plays a sound under the default case (`default_child`).
    auto matches_resolve = [&](const Target& t, const std::string& group, const std::string& default_child) {
        const SwitchGroup* sg = FindGroup(st, group);
        CHECK(t.lib && sg && t.Group(group));
        if (!t.lib || !sg) return;
        for (const SwitchValue& v : sg->values) {
            const ValueSoundState* vs = vs_of(t, group, v.uid);
            CHECK(vs);
            if (!vs) continue;
            ValueLayers layers;
            layers.item.push_back({group, v.uid, {}});
            const Resolution res = ResolveEvent(st, *t.lib, layers, ResolveHooks());
            CHECK(res.sounds.empty() == (vs->state == ValueSound::Nothing));
            if (vs->state != ValueSound::Default) continue;
            for (const ResolvedSound& rs : res.sounds) {
                bool under = false;
                for (const SoundNode* n = FindNode(*t.lib, rs.node); n && !under; n = n->parent.empty() ? nullptr : FindNode(*t.lib, n->parent))
                    under = n->uid == default_child;
                CHECK(under);
            }
        }
    };

    // Own sound / no case with a default that has sound / empty case.
    const Target a = target("a");
    CHECK(state(a, "mat", "mat_Wood") == ValueSound::Own);
    CHECK(state(a, "mat", "mat_Grass") == ValueSound::Default && def_name(a, "mat", "mat_Grass") == "Wood");
    CHECK(state(a, "mat", "mat_Metal") == ValueSound::Nothing);
    CHECK(tip(a, "mat", "mat_Grass") == "No sound of its own: plays the default, Wood");
    CHECK(tip(a, "mat", "mat_Metal") == "No sound: events on it place nothing");
    CHECK(tip(a, "mat", "mat_Wood").empty());
    matches_resolve(a, "mat", "rw");
    // The inherit chip: the default, Wood, has its own sound.
    CHECK(inherit_uid(a, "mat") == "mat_Wood" && ChipSound({&a}, "mat", "").state == ValueSound::Own);

    // The inherit chip of a rule whose item sets Metal: the state of Metal (Nothing).
    {
        ItemSounds rec;
        rec.item_values.push_back({"mat", "mat_Metal", {}});
        RuleSounds rs;
        rs.rid = "r0";
        rs.library = "a";
        rec.rules.push_back(rs);
        const Target t = MakeTarget(st, rec, nullptr, Rule("Scuff L", "r0", 1), "b1", "x");
        CHECK(t.lib && t.lib->uid == "a");
        CHECK(inherit_uid(t, "mat") == "mat_Metal" && ChipSound({&t}, "mat", "").state == ValueSound::Nothing);
    }

    // No case, default empty: the default itself is Nothing (never Default), Grass places nothing.
    const Target b = target("b");
    CHECK(state(b, "mat", "mat_Wood") == ValueSound::Nothing && state(b, "mat", "mat_Grass") == ValueSound::Nothing);
    CHECK(ChipSound({&b}, "mat", "").state == ValueSound::Nothing);
    matches_resolve(b, "mat", "rw");

    // Cascade: Barefoot leads to a Switch whose every case is empty.
    const Target c = target("cas");
    CHECK(state(c, "shoes", "shoes_Barefoot") == ValueSound::Nothing && state(c, "shoes", "shoes_Boots") == ValueSound::Own);
    matches_resolve(c, "shoes", "boot");
    {
        const SoundLibrary* cl = FindLibrary(st, "cas");
        const SoundNode*    sh_n = cl ? FindNode(*cl, "sh") : nullptr;
        const SoundNode*    in_n = cl ? FindNode(*cl, "inner") : nullptr;
        CHECK(sh_n && in_n);
        if (sh_n && in_n) CHECK(HoldsSound(*cl, *sh_n) && !HoldsSound(*cl, *in_n));
    }

    // Only the reached Switches count: the walk takes Walk, whose Material has Metal.
    const Target r = target("rch");
    CHECK(state(r, "mat", "mat_Metal") == ValueSound::Own && state(r, "mat", "mat_Wood") == ValueSound::Default);
    matches_resolve(r, "mat", "sm");
    {
        ItemSounds run;
        run.item_values.push_back({"loco", "loco_Run", {}});
        SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, "rch");
        const Target rr = MakeTarget(st, run, nullptr, step, "b0", "x");
        CHECK(state(rr, "mat", "mat_Metal") == ValueSound::Nothing);
    }

    // Every value has its own sound: nothing greyed.
    const Target cc = target("c");
    CHECK(cc.Group("mat") && cc.Group("mat")->value_sounds.size() == 3);
    if (const GroupState* g = cc.Group("mat"))
        for (const ValueSoundState& v : g->value_sounds)
            CHECK(v.value_uid == "mat_Grass" ? v.state == ValueSound::Default : v.state == ValueSound::Own);

    // Mixed selection: Metal Nothing for one target, Own for the other -> normal; Grass Default in both, same name.
    CHECK(ChipSound({&a, &cc}, "mat", "mat_Metal").state == ValueSound::Own);
    const ValueSoundState gr = ChipSound({&a, &cc}, "mat", "mat_Grass");
    CHECK(gr.state == ValueSound::Default && gr.default_name == "Wood");
    const ValueSoundState gr2 = ChipSound({&a, &r}, "mat", "mat_Wood");  // a: Own -> Own wins
    CHECK(gr2.state == ValueSound::Own);
    // Default names that differ: the tooltip names none.
    const ValueSoundState gr3 = ChipSound({&a, &r}, "mat", "mat_Grass");  // a: Wood, r: Metal
    CHECK(gr3.state == ValueSound::Default && gr3.default_name.empty() &&
          ChipSoundTip(gr3) == "No sound of its own: plays the default");
    // A target without the group does not count; none at all: Own.
    CHECK(ChipSound({&a}, "shoes", "shoes_Barefoot").state == ValueSound::Own);
    CHECK(ChipSound({&a, &c}, "shoes", "shoes_Barefoot").state == ValueSound::Nothing);
    CHECK(ChipSound({&a, &c}, "mat", "mat_Metal").state == ValueSound::Nothing);

    // Item values row: across the item's targets that use the group.
    const Target item = MakeItemTarget(st, ItemSounds(), {a, cc, c});
    CHECK(state(item, "mat", "mat_Metal") == ValueSound::Own && state(item, "mat", "mat_Grass") == ValueSound::Default);
    const Target item2 = MakeItemTarget(st, ItemSounds(), {a, b});
    CHECK(item2.Group("mat") && state(item2, "mat", "mat_Metal") == ValueSound::Own);
    CHECK(state(item2, "mat", "mat_Grass") == ValueSound::Default);  // a plays Wood for it
    const Target item3 = MakeItemTarget(st, ItemSounds(), {a});
    CHECK(item3.Group("mat") && state(item3, "mat", "mat_Metal") == ValueSound::Nothing);
    CHECK(state(item2, "mat", "mat_Wood") == ValueSound::Own);
    CHECK(inherit_uid(item2, "mat") == "mat_Wood" && ChipSound({&item2}, "mat", "").state == ValueSound::Own);
}

// ---- 13-66: staged Switch values ----------------------------------------------------------------------

static StagedEntry Ent(StageScope sc, const std::string& owner, const std::string& key, const std::string& group, const std::string& value)
{
    StagedEntry e;
    e.scope = sc;
    e.owner = owner;
    e.key = key;
    e.group = group;
    e.value = value;
    return e;
}

static void TestStaging()
{
    SoundStore st = MakeStore();
    SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, "hero");
    std::vector<Block> blocks = {Auto(AutoKind::Step, 'L', "a0"), Auto(AutoKind::Step, 'R', "")};  // the second has no rid yet
    ItemSounds         rec;

    // Stage two targets: counts, nothing written (the record is a copy).
    Staged s;
    CHECK(s.Empty() && s.Targets() == 0);
    CHECK(s.Stage(Ent(StageScope::Item, "G1", "b0", "feet", "feet_Boots"), ""));
    CHECK(s.Stage(Ent(StageScope::Item, "G1", "item", "loco", "loco_Run"), ""));
    CHECK(s.Changes() == 2 && s.Targets() == 2 && s.Items() == 1 && !s.Empty());
    CHECK(rec.rules.empty() && rec.item_values.empty());
    // A second value of the same group on the same target replaces the entry.
    CHECK(s.Stage(Ent(StageScope::Item, "G1", "b0", "feet", "feet_Shoes"), "") && s.Changes() == 2);
    CHECK(s.Find(StageScope::Item, "G1", "b0", "feet")->value == "feet_Shoes");
    // Another item: the bar counts it ("2 items").
    CHECK(s.Stage(Ent(StageScope::Item, "G2", "b0", "feet", "feet_Boots"), "") && s.Items() == 2 && s.Targets() == 3);
    const StagedBar bar = StagedBarText(s.Changes(), s.Targets(), s.Items());
    CHECK(bar.bold == "3 Switch changes" && bar.rest == "on 3 targets \xC2\xB7 2 items, not placed yet" &&
          bar.sub == " \xC2\xB7 Commit places them in one undo step");
    const StagedBar one = StagedBarText(1, 1, 1);
    CHECK(one.bold == "1 Switch change" && one.rest == "on 1 target, not placed yet");
    CHECK(CommitChangeText("Hero_Run_01", 3, 2).rest == " \xC2\xB7 Hero_Run_01 \xC2\xB7 3 changes on 2 targets");

    // Unstage: clicking the applied value drops the entry; Drop of a missing one is a no-op.
    CHECK(!s.Stage(Ent(StageScope::Item, "G2", "b0", "feet", ""), ""));
    CHECK(s.Items() == 1 && s.Changes() == 2 && !s.Drop(StageScope::Item, "G2", "b0", "feet"));
    CHECK(s.HasTarget(StageScope::Item, "G1", "b0") && !s.HasTarget(StageScope::Item, "G2", "b0"));

    // Reach preview: Loco staged Crouch on the item row, Shoes becomes not reached (as the record would say).
    Staged r;
    r.Stage(Ent(StageScope::Item, "G1", "item", "loco", "loco_Crouch"), "loco_Run");
    ItemSounds run = rec;
    run.item_values.push_back({"loco", "loco_Run", {}});
    ItemSounds pre = run;
    std::vector<Block> pre_blocks = blocks;
    CHECK(ApplyStagedToItem(r, "G1", pre, pre_blocks) == 1 && run.item_values[0].value_uid == "loco_Run");
    Target before = MakeTarget(st, run, nullptr, blocks[0], "b0", "x");
    Target after = MakeTarget(st, pre, nullptr, pre_blocks[0], "b0", "x");
    CHECK(before.groups[1].reached && !after.groups[1].reached && after.groups[1].not_reached == "not reached under Crouch");
    CHECK(PlaysText(after) == std::string("FS Hero \xE2\x80\xBA Crouch"));

    // Inherit preview: an item value staged shows as the inherit of a rule.
    Staged inh;
    inh.Stage(Ent(StageScope::Item, "G1", "item", "feet", "feet_Boots"), "");
    ItemSounds pre2 = rec;
    std::vector<Block> pre2_blocks = blocks;
    ApplyStagedToItem(inh, "G1", pre2, pre2_blocks);
    Target t2 = MakeTarget(st, pre2, nullptr, pre2_blocks[0], "b0", "x");
    CHECK(t2.groups[1].inherit_name == "Boots" && t2.groups[1].inherit_from == "item" && !t2.groups[1].own);
    // Another item's entry does not touch this record.
    ItemSounds other = rec;
    std::vector<Block> ob = blocks;
    CHECK(ApplyStagedToItem(inh, "G9", other, ob) == 0 && other.item_values.empty());

    // A rule without rid: previewed through a provisional rid; the real block keeps none.
    Staged nr;
    nr.Stage(Ent(StageScope::Item, "G1", "b1", "feet", "feet_Boots"), "");
    ItemSounds pre3 = rec;
    std::vector<Block> pre3_blocks = blocks;
    CHECK(ApplyStagedToItem(nr, "G1", pre3, pre3_blocks) == 1);
    CHECK(pre3_blocks[1].rid == ProvisionalRid(1) && blocks[1].rid.empty());
    Target t3 = MakeTarget(st, pre3, nullptr, pre3_blocks[1], "b1", "x");
    CHECK(t3.groups[1].own && t3.groups[1].value_name == "Boots");
    CHECK(AppliedItemValue(pre3, pre3_blocks, "b1", "feet") == "feet_Boots" && AppliedItemValue(rec, blocks, "b1", "feet").empty());

    // Project and Character entries go to a store copy / a character record.
    Staged ks;
    StagedEntry pe = Ent(StageScope::Project, "", "legs", "feet", "feet_Boots");
    CHECK(ks.Stage(pe, AppliedKeyValue(st.project, "legs", "feet")));
    StagedEntry ce = Ent(StageScope::Character, CharacterOwner("sk1", ""), "legs", "loco", "loco_Run");
    ce.character.skeleton = "sk1";
    CHECK(ks.Stage(ce, "") && ks.Targets() == 2 && ks.Items() == 0);
    SoundStore copy = st;
    CHECK(ApplyStagedToStore(ks, copy) == 2 && copy.characters.size() == 1);
    CHECK(AppliedKeyValue(copy.project, "legs", "feet") == "feet_Boots" && AppliedKeyValue(st.project, "legs", "feet").empty() &&
          st.characters.empty());
    SoundCharacter ch;
    ch.skeleton = "sk1";
    CHECK(ApplyStagedToCharacter(ks, ch) == 1 && AppliedKeyValue(ch.assigns, "legs", "loco") == "loco_Run");
    SoundCharacter other_ch;
    other_ch.skeleton = "sk2";
    CHECK(ApplyStagedToCharacter(ks, other_ch) == 0);

    // An entry that becomes equal after an outside change drops out; "Commit changes nothing" = the set is empty.
    Staged eq;
    eq.Stage(Ent(StageScope::Item, "G1", "b0", "feet", "feet_Boots"), "");
    eq.Stage(Ent(StageScope::Item, "G1", "item", "loco", "loco_Run"), "");
    eq.Stage(Ent(StageScope::Item, "G7", "item", "loco", "loco_Run"), "");  // another item: not judged
    ItemSounds outside = rec;
    outside.item_values.push_back({"loco", "loco_Run", {}});
    RuleSounds orule;
    orule.rid = "a0";
    orule.values.push_back({"feet", "feet_Boots", {}});
    outside.rules.push_back(orule);
    const int dropped = eq.DropEqual([&](const StagedEntry& e, std::string* applied) {
        if (e.owner != "G1") return false;
        *applied = AppliedItemValue(outside, blocks, e.key, e.group);
        return true;
    });
    CHECK(dropped == 2 && eq.Changes() == 1 && eq.Find(StageScope::Item, "G7", "item", "loco"));

    // Cancel: Clear drops all, no entry left.
    s.Clear();
    CHECK(s.Empty() && s.Changes() == 0 && s.Items() == 0);
}

// Review 13-66: prune (rule identity, other items, deleted group / value), the commit plan, the gaps of the first run.
static StagedEntry ItemEnt(const std::string& owner, const std::string& guid, const std::string& key, const std::string& rid,
                           const std::string& sig, const std::string& group, const std::string& value)
{
    StagedEntry e = Ent(StageScope::Item, owner, key, group, value);
    e.item_guid = guid;
    e.rid = rid;
    e.sig = sig;
    return e;
}

static void TestPruneAndPlan()
{
    SoundStore st = MakeStore();
    std::vector<Block> blocks = {Auto(AutoKind::Step, 'L', "a0"), Auto(AutoKind::Step, 'R', "")};
    const std::string sig = BuildTargets(blocks).signature;
    ItemSounds        rec;
    auto view = [&](const StagedEntry&, StagedItemView* v) {
        v->rec = &rec;
        v->blocks = &blocks;
        return true;
    };

    // F1: an entry follows its rule by rid; the rules changed under it (b0 is another rule now) = it goes.
    Staged s;
    s.Stage(ItemEnt("P", "G1", "b0", "a0", sig, "feet", "feet_Boots"), "");
    s.Stage(ItemEnt("P", "G1", "b1", "", sig, "feet", "feet_Boots"), "");  // no rid yet: the signature judges
    CHECK(PruneStaged(s, st, view) == 0 && s.Changes() == 2);
    std::vector<Block> moved = blocks;
    moved[0].rid = "zz";
    auto view_moved = [&](const StagedEntry&, StagedItemView* v) {
        v->rec = &rec;
        v->blocks = &moved;
        return true;
    };
    CHECK(PruneStaged(s, st, view_moved) == 1 && s.Changes() == 1 && !s.Find(StageScope::Item, "P", "b0", "feet"));
    // Its markers changed (signature differs): the rid-less entry goes; a block out of range goes too.
    std::vector<Block> fewer = {blocks[0]};
    auto view_fewer = [&](const StagedEntry&, StagedItemView* v) {
        v->rec = &rec;
        v->blocks = &fewer;
        return true;
    };
    CHECK(PruneStaged(s, st, view_fewer) == 1 && s.Empty());
    // The rules do not read (null blocks) or the item is unknown: cannot judge, the entry stays (an item row too).
    Staged k;
    k.Stage(ItemEnt("P", "G1", "b0", "a0", sig, "feet", "feet_Boots"), "");
    CHECK(PruneStaged(k, st, [&](const StagedEntry&, StagedItemView*) { return false; }) == 0 && k.Changes() == 1);
    CHECK(PruneStaged(k, st, [&](const StagedEntry&, StagedItemView* v) { v->rec = &rec; return true; }) == 0 && k.Changes() == 1);

    // Each owner is judged on its own item (other items are not skipped any more).
    Staged two;
    two.Stage(ItemEnt("A", "GA", "b0", "a0", sig, "feet", "feet_Boots"), "");
    two.Stage(ItemEnt("B", "GB", "b0", "a0", sig, "feet", "feet_Boots"), "");
    ItemSounds b_rec;
    RuleSounds br;
    br.rid = "a0";
    br.values.push_back({"feet", "feet_Boots", {}});
    b_rec.rules.push_back(br);  // B already has it: equal
    CHECK(PruneStaged(two, st, [&](const StagedEntry& e, StagedItemView* v) {
              v->rec = e.owner == "B" ? &b_rec : &rec;
              v->blocks = &blocks;
              return true;
          }) == 1 &&
          two.Find(StageScope::Item, "A", "b0", "feet") && !two.Find(StageScope::Item, "B", "b0", "feet"));

    // F8: a deleted group or value drops the entry (Project, Character, Item); inherit ("") needs the group only.
    Staged gone;
    gone.Stage(Ent(StageScope::Project, "", "legs", "feet", "feet_Gone"), "");
    gone.Stage(Ent(StageScope::Project, "", "legs", "nogroup", "x"), "");
    gone.Stage(ItemEnt("P", "G1", "item", "", "", "feet", "feet_Gone"), "");
    gone.Stage(Ent(StageScope::Project, "", "arms", "feet", "feet_Boots"), "");  // fine
    CHECK(PruneStaged(gone, st, view) == 3 && gone.Changes() == 1 && gone.Find(StageScope::Project, "", "arms", "feet"));
    // The Character branch: judged on the stored character; equal drops, different stays.
    SoundCharacter hero;
    hero.uid = "c1";
    hero.skeleton = "sk1";
    SetAssignValueIn(hero.assigns, {"legs"}, "loco", "loco_Run");
    st.characters.push_back(hero);
    Staged ch;
    StagedEntry same = Ent(StageScope::Character, CharacterOwner("sk1", ""), "legs", "loco", "loco_Run");
    same.character.skeleton = "sk1";
    StagedEntry diff = Ent(StageScope::Character, CharacterOwner("sk1", ""), "legs", "feet", "feet_Boots");
    diff.character.skeleton = "sk1";
    ch.Stage(same, "");
    ch.Stage(diff, "");
    CHECK(PruneStaged(ch, st, view) == 1 && ch.Changes() == 1 && ch.Find(StageScope::Character, same.owner, "legs", "feet"));
    st.characters.clear();

    // F4: an owner is a pool: two stagings on the same pool + rule are ONE entry, whichever copy staged it last.
    Staged pool;
    pool.Stage(ItemEnt("pool:a|1", "G1", "b0", "a0", sig, "feet", "feet_Boots"), "");
    pool.Stage(ItemEnt("pool:a|1", "G2", "b0", "a0", sig, "feet", "feet_Shoes"), "");
    CHECK(pool.Changes() == 1 && pool.Items() == 1 && pool.Find(StageScope::Item, "pool:a|1", "b0", "feet")->value == "feet_Shoes");
    const unsigned rev = pool.Revision();
    pool.Stage(ItemEnt("pool:a|1", "G1", "b0", "a0", sig, "feet", "feet_Shoes"), "");
    CHECK(pool.Revision() != rev);  // F3: every change of the set is a new revision
    const unsigned rev2 = pool.Revision();
    pool.Clear();
    CHECK(pool.Revision() != rev2);

    // PlanCommit: found / rules read through the callback.
    auto read = [&](const std::string& guid) {
        CommitItemRead r;
        if (guid == "GONE") return r;
        r.found = true;
        r.valid = guid != "NORULES";
        if (r.valid) r.blocks = blocks;
        return r;
    };
    std::vector<StagedEntry> es;
    es.push_back(ItemEnt("pool:a|1", "G1", "b0", "a0", sig, "feet", "feet_Boots"));
    es.push_back(ItemEnt("pool:a|1", "G1", "b0", "a0", sig, "loco", "loco_Run"));  // same rule, another group
    es.push_back(ItemEnt("pool:a|1", "G2", "b1", "", sig, "feet", "feet_Boots"));  // another copy of the pool, a rid-less rule
    es.push_back(ItemEnt("pool:a|1", "G2", "item", "", "", "loco", "loco_Crouch"));
    es.push_back(Ent(StageScope::Project, "", "legs", "feet", "feet_Boots"));
    StagedEntry ce = Ent(StageScope::Character, "char:sk1|", "legs", "feet", "feet_Shoes");
    ce.character.skeleton = "sk1";
    es.push_back(ce);
    CommitPlan plan = PlanCommit(es, read);
    CHECK(plan.items.size() == 1);  // a pool is one owner: one planned item, however many copies staged
    CHECK(plan.items[0].edits.size() == 4 && plan.items[0].item_guid == "G1");
    CHECK(plan.store_edits.size() == 2 && plan.store_edits[0].scope == StageScope::Project && plan.store_edits[1].scope == StageScope::Character);
    CHECK(plan.counts.changes == 6 && plan.counts.skipped == 0);
    CHECK(plan.counts.targets == 5);  // b0, b1, item, legs (project), legs (character): b0 counts once for two groups
    CHECK(plan.items[0].NeedsRids() && plan.items[0].Whole());
    // Each rid once: b0 twice and a rid-less b1 that gets its id at commit.
    const std::set<std::string> rids = plan.items[0].Rids([](int b) { return b == 1 ? std::string("new1") : std::string(); });
    CHECK(rids.size() == 2 && rids.count("a0") && rids.count("new1"));
    // The same key under the other scope or owner is another target.
    std::vector<StagedEntry> kk = {Ent(StageScope::Project, "", "legs", "feet", "feet_Boots"), ce};
    kk[1].key = "legs";
    CHECK(PlanCommit(kk, read).counts.targets == 2);

    // F2: an item that is gone, rules that do not read, a rule that moved or went away are skipped and counted.
    std::vector<StagedEntry> bad;
    bad.push_back(ItemEnt("g", "GONE", "b0", "a0", sig, "feet", "feet_Boots"));
    bad.push_back(ItemEnt("n", "NORULES", "b0", "a0", sig, "feet", "feet_Boots"));
    bad.push_back(ItemEnt("n", "NORULES", "item", "", "", "feet", "feet_Boots"));  // the item row needs no rules: kept
    bad.push_back(ItemEnt("m", "G1", "b0", "other", sig, "feet", "feet_Boots"));   // block b0 has rid a0 now
    bad.push_back(ItemEnt("m", "G1", "b1", "", "1:x;", "feet", "feet_Boots"));    // markers changed since
    bad.push_back(ItemEnt("m", "G1", "b7", "a7", sig, "feet", "feet_Boots"));      // out of range
    StagedEntry nogroup = Ent(StageScope::Project, "", "legs", "", "x");
    bad.push_back(nogroup);
    plan = PlanCommit(bad, read);
    CHECK(plan.counts.skipped == 6 && plan.counts.changes == 1 && plan.items.size() == 1 && plan.items[0].edits[0].block == -1);
    CHECK(SkippedText(2) == "2 not applied: their item or rule is gone");
    CHECK(PlanCommit({}, read).counts.changes == 0);
    CHECK(PlanCommit({bad[0]}, read).items.empty());  // everything skipped: nothing planned (no undo point)

    // Gap 4: characters=false leaves the characters alone; a record made for nothing is not kept; order Project then Character.
    Staged mix;
    mix.Stage(Ent(StageScope::Character, CharacterOwner("sk1", ""), "legs", "loco", "loco_Run"), "");
    mix.Stage(Ent(StageScope::Project, "", "legs", "feet", "feet_Boots"), "");
    SoundStore c1 = st;
    CHECK(ApplyStagedToStore(mix, c1, false) == 1 && c1.characters.empty());
    Staged nothing;
    StagedEntry inh = Ent(StageScope::Character, CharacterOwner("sk1", ""), "legs", "loco", "");  // inherit of nothing
    inh.character.skeleton = "sk1";
    nothing.Stage(inh, "x");
    SoundStore c2 = st;
    CHECK(ApplyStagedToStore(nothing, c2) == 0 && c2.characters.empty());
    // A uid-only character (no rig) has its own owner: it does not take another one's entries.
    SoundCharacter u1, u2;
    u1.uid = "u1";
    u2.uid = "u2";
    CHECK(CharacterOwnerOf(u1) != CharacterOwnerOf(u2) && CharacterOwnerOf(u1) == CharacterOwner(CharacterRef{"u1", "", "", "", {}}));
    Staged us;
    StagedEntry ue = Ent(StageScope::Character, CharacterOwnerOf(u1), "legs", "loco", "loco_Run");
    ue.character.uid = "u1";
    us.Stage(ue, "");
    CHECK(ApplyStagedToCharacter(us, u1) == 1 && ApplyStagedToCharacter(us, u2) == 0);

    // Gap 5: re-staging moves an entry to the end (Commit applies in list order).
    Staged ord;
    ord.Stage(Ent(StageScope::Project, "", "legs", "feet", "feet_Boots"), "");
    ord.Stage(Ent(StageScope::Project, "", "arms", "feet", "feet_Boots"), "");
    ord.Stage(Ent(StageScope::Project, "", "legs", "feet", "feet_Shoes"), "");
    CHECK(ord.Entries().size() == 2 && ord.Entries()[1].key == "legs" && ord.Entries()[1].value == "feet_Shoes");
    // Gap 6: the footer's prefix and bold.
    const ChangeText ct = CommitChangeText("x", 1, 1);
    CHECK(ct.prefix == "Undo point " && ct.bold == "RAV: Set sound values" && ct.rest == " \xC2\xB7 x \xC2\xB7 1 change on 1 target");
}

int main()
{
    TestScopes();
    TestTargets();
    TestState();
    TestRandomReach();
    TestDeletedGroup();
    TestGaps();
    TestSummaryAndMix();
    TestTitleAndLadder();
    TestSelection();
    TestValueSounds();
    TestStaging();
    TestPruneAndPlan();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("assign_view_model: all checks passed\n");
    return 0;
}
