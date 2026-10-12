// SPDX-License-Identifier: MIT
//
// Story 13-15a -- src/sound_assign.h (pure) and its text (sound_record.h): the body parts, the part
// of a block, the layered resolution (library and Switch values, inherit / None / unset, the
// sources), the character of a rig, the rids a store edit changes, the pure edits, the grammar
// (assign / character / kit / none lines, unknown text kept) and the migration of the old per-type
// sounds.

#include "sound_assign.h"

#include <cstdio>
#include <string>
#include <vector>

#include "sound_auto_assign.h"
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

static const char* kDot = "\xC2\xB7";

static Block AutoB(AutoKind k, char side, const std::string& rid)
{
    Block b = MakeAutoBlock(k, side, kAutoDefaultSens, 0.0);
    b.rid = rid;
    return b;
}

// A bone rule whose first condition reads these bone-reference ids.
static Block RuleB(const std::string& rid, std::vector<int> bones, std::vector<int> ref_bones = {})
{
    Block b;
    b.marker = "Rule";
    b.rid = rid;
    Condition c;
    c.signal.bones = bones;
    c.signal.ref_bones = ref_bones;
    b.conditions.push_back(c);
    return b;
}

static std::string PartOf(const Block& b)
{
    std::string p, t;
    PartOfBlock(b, &p, &t);
    return p + "." + t;
}

static void TestTable()
{
    const std::vector<BodyPart>& parts = BodyParts();
    CHECK(parts.size() == 4);
    CHECK(parts[0].id == "feet" && parts[0].label == "Feet" && parts[0].types.size() == 5);
    CHECK(parts[0].types[0].word == "step" && parts[0].types[1].word == "lift" && parts[0].types[2].word == "slide" &&
          parts[0].types[3].word == "pivot" && parts[0].types[4].word == "rules");
    CHECK(parts[1].id == "hands" && parts[1].types.size() == 4 && parts[1].types[2].word == "hand_pivot");
    CHECK(parts[2].id == "body" && parts[2].label == "Hips & spine" && parts[2].types.size() == 1);
    CHECK(parts[3].id == "other" && parts[3].label == "Other bones");
    CHECK(TypeLabel("feet", "step") == "Step" && TypeLabel("feet", "rules") == "Bone rules");
    CHECK(TypeLabel("hands", "grab") == "Grab");
    CHECK(AssignKey("feet", "step") == "feet.step" && AssignKey("feet", "") == "feet");
    std::string p, t;
    SplitAssignKey("hands.hand_pivot", &p, &t);
    CHECK(p == "hands" && t == "hand_pivot");
    SplitAssignKey("body", &p, &t);
    CHECK(p == "body" && t.empty());
    CHECK(AssignKeyLabel("feet.step") == std::string("Feet \xC2\xB7 Step"));
    for (int i = 0; i < kAutoTypeCount; ++i) {
        AutoType back;
        CHECK(AutoTypeFromWord(AutoTypeWord(static_cast<AutoType>(i)), &back) && back == static_cast<AutoType>(i));
    }
}

static void TestPartOfBlock()
{
    CHECK(PartOf(RuleB("r", {static_cast<int>(Role::LeftHeel)})) == "feet.rules");
    CHECK(PartOf(RuleB("r", {static_cast<int>(Role::LeftKnee)})) == "feet.rules");
    CHECK(PartOf(RuleB("r", {static_cast<int>(Role::RightHand)})) == "hands.rules");
    CHECK(PartOf(RuleB("r", {static_cast<int>(Role::Hips)})) == "body.rules");
    CHECK(PartOf(RuleB("r", {static_cast<int>(Role::Head)})) == "body.rules");
    // A custom role / raw bone (an id from kBoneRefExtra) is "other"; the first built-in role wins.
    CHECK(PartOf(RuleB("r", {kBoneRefExtra + 3})) == "other.rules");
    CHECK(PartOf(RuleB("r", {kBoneRefExtra + 3, static_cast<int>(Role::LeftHand)})) == "hands.rules");
    CHECK(PartOf(RuleB("r", {}, {static_cast<int>(Role::Spine)})) == "body.rules");  // ref bones too
    Block none;
    none.marker = "Rule";
    CHECK(PartOf(none) == "other.rules");
    // Only the first condition counts.
    Block two = RuleB("r", {kBoneRefExtra + 1});
    Condition c2;
    c2.signal.bones = {static_cast<int>(Role::LeftHeel)};
    two.conditions.push_back(c2);
    CHECK(PartOf(two) == "other.rules");

    // Every auto kind: its category's part, its type's word.
    const char* want[kAutoKindCount] = {"feet.step",  "feet.step",  "feet.step",       "feet.lift",
                                        "feet.slide", "feet.pivot", "hands.grab",      "hands.release",
                                        "hands.hand_pivot", "hands.hand_pivot", "feet.slide", "feet.slide",
                                        "feet.pivot", "feet.pivot", "feet.slide",      "feet.slide",
                                        "feet.pivot", "feet.pivot"};
    for (int k = 0; k < kAutoKindCount; ++k) {
        const Block b = AutoB(static_cast<AutoKind>(k), 'L', "x");
        CHECK(PartOf(b) == want[k]);
        std::string part, type;
        CHECK(PartOfBlock(b, &part, &type));
    }
    Block unknown = AutoB(AutoKind::Step, 'L', "x");
    unknown.auto_type = "future_kind";
    std::string part, type;
    CHECK(!PartOfBlock(unknown, &part, &type) && part == "other" && type == "rules");

    // The migration table (sound_record.cpp) agrees with PartOfBlock for every kind and side.
    for (int k = 0; k < kAutoKindCount; ++k)
        for (char side : {'L', 'R'}) {
            const Block        b = AutoB(static_cast<AutoKind>(k), side, "x");
            const std::string  key = AutoSoundKeyOf(b);
            const std::string  word = key.substr(0, key.find(':'));
            const std::string  text = "RAVSOUNDS 1\nauto key=" + word + ":L lib=LIB\nauto key=" + word + ":R lib=LIB\n";
            SoundStore         s;
            CHECK(ParseSoundStore(text, &s));
            CHECK(s.project.size() == 1 && s.project[0].key == PartOf(b));
        }
}

static void AddLib(SoundStore& s, const std::string& uid)
{
    SoundLibrary l;
    l.uid = uid;
    l.name = uid;
    s.libraries.push_back(l);
}

static SoundCharacter Hero()
{
    SoundCharacter c;
    c.uid = "C1";
    c.name = "Hero";
    c.skeleton = "0123456789abcdef";
    return c;
}

static void TestLibraryChain()
{
    SoundStore st;
    ItemSounds item;
    SoundCharacter hero = Hero();
    const std::string P = "feet", T = "step";
    // Nothing set anywhere.
    EffectiveAssign e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.library.empty() && !e.none && e.level == AssignLevel::Unset && e.from == "nothing set");

    SetAssignLibraryIn(st.project, {"feet"}, LibState::Library, "PP");
    e = ResolveAssign(st, item, nullptr, "r1", P, T);
    CHECK(e.library == "PP" && e.level == AssignLevel::Project && !e.type_specific &&
          e.from == std::string("Project \xC2\xB7 Feet"));
    SetAssignLibraryIn(st.project, {"feet.step"}, LibState::Library, "PT");
    e = ResolveAssign(st, item, nullptr, "r1", P, T);
    CHECK(e.library == "PT" && e.type_specific && e.from == std::string("Project \xC2\xB7 Feet \xC2\xB7 Step"));
    // Another type of the part reads the part-level one.
    CHECK(ResolveAssign(st, item, nullptr, "r1", P, "lift").library == "PP");
    // Another part reads nothing.
    CHECK(ResolveAssign(st, item, nullptr, "r1", "hands", "grab").library.empty());

    // The Character is above the Project, its type above its part.
    SetAssignLibraryIn(hero.assigns, {"feet"}, LibState::Library, "CP");
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.library == "CP" && e.level == AssignLevel::Character && e.from == std::string("Hero \xC2\xB7 Feet"));
    SetAssignLibraryIn(hero.assigns, {"feet.step"}, LibState::Library, "CT", true);
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.library == "CT" && e.kit && e.from == std::string("Hero \xC2\xB7 Feet \xC2\xB7 Step"));
    // No character: the Project's.
    CHECK(ResolveAssign(st, item, nullptr, "r1", P, T).library == "PT");

    // The rule is above everything.
    RuleSounds rs;
    rs.rid = "r1";
    rs.library = "RL";
    item.rules.push_back(rs);
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.library == "RL" && e.level == AssignLevel::Rule && e.from == "this type");
    CHECK(ResolveAssign(st, item, &hero, "r1", P, "rules").from == "this rule");
    CHECK(ResolveAssign(st, item, &hero, "other_rid", P, T).library == "CT");  // another rule: not it

    // None on the rule stops the search; None is not inherit.
    item.rules[0].library.clear();
    item.rules[0].none = true;
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.none && e.library.empty() && e.level == AssignLevel::Rule);
    item.rules[0].none = false;  // inherit again
    CHECK(ResolveAssign(st, item, &hero, "r1", P, T).library == "CT");

    // None at Character type level stops the lookup before the Project's.
    SetAssignLibraryIn(hero.assigns, {"feet.step"}, LibState::None, "");
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.none && e.library.empty() && e.level == AssignLevel::Character && e.type_specific);
    // Inherit at the type passes it on to the part.
    SetAssignLibraryIn(hero.assigns, {"feet.step"}, LibState::Inherit, "");
    CHECK(FindAssign(hero.assigns, "feet.step") == nullptr);  // an empty entry is dropped
    CHECK(ResolveAssign(st, item, &hero, "r1", P, T).library == "CP");
    // A Character part-level None hides the Project's type-level library.
    SetAssignLibraryIn(hero.assigns, {"feet"}, LibState::None, "");
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.none && e.level == AssignLevel::Character && !e.type_specific);
    // A Project part-level None.
    SetAssignLibraryIn(st.project, {"feet.step"}, LibState::Inherit, "");
    SetAssignLibraryIn(st.project, {"feet"}, LibState::None, "");
    e = ResolveAssign(st, item, nullptr, "r1", P, T);
    CHECK(e.none && e.level == AssignLevel::Project);

    // A kit is never read.
    SoundLibrary kitlib;
    kitlib.uid = "KIT";
    kitlib.kit = {"feet"};
    st.libraries.push_back(kitlib);
    SetAssignLibraryIn(st.project, {"feet"}, LibState::Inherit, "");
    CHECK(ResolveAssign(st, item, nullptr, "r1", P, T).library.empty());
}

static void TestValueChain()
{
    SoundStore st;
    ItemSounds item;
    SoundCharacter hero = Hero();
    const std::string P = "feet", T = "step";
    // Group G1 set at every level, lowest first.
    SetAssignValueIn(st.project, {"feet"}, "G1", "projpart");
    EffectiveAssign e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.layers.item.size() == 1 && e.layers.item[0].value_uid == "projpart");
    CHECK(e.value_from.size() == 1 && e.ValueFromText("G1") == std::string("Project \xC2\xB7 Feet"));
    CHECK(e.ValueFromText("GX") == "default");
    SetAssignValueIn(st.project, {"feet.step"}, "G1", "projtype");
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.layers.item[0].value_uid == "projtype" && e.ValueFromText("G1") == std::string("Project \xC2\xB7 Feet \xC2\xB7 Step"));
    SetAssignValueIn(hero.assigns, {"feet"}, "G1", "charpart");
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.layers.item[0].value_uid == "charpart" && e.ValueFromText("G1") == std::string("Hero \xC2\xB7 Feet"));
    SetAssignValueIn(hero.assigns, {"feet.step"}, "G1", "chartype");
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.layers.item[0].value_uid == "chartype" && e.ValueFromText("G1") == std::string("Hero \xC2\xB7 Feet \xC2\xB7 Step"));
    // The item's own values win over the inherited ones, and the merged layer keeps the others.
    SetAssignValueIn(st.project, {"feet"}, "G2", "p2");
    item.item_values.push_back({"G1", "itemv", {}});
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.layers.item.size() == 2);
    ValueFrom from = ValueFrom::Default;
    SoundNode sw;
    sw.type = SoundNodeType::Switch;
    sw.group_uid = "G1";
    CHECK(EffectiveValue(sw, e.layers, &from) == "itemv" && from == ValueFrom::Item);
    sw.group_uid = "G2";
    CHECK(EffectiveValue(sw, e.layers, &from) == "p2" && from == ValueFrom::Item);  // inherited ones are in the item layer
    CHECK(e.ValueFromText("G1") == "item" && e.ValueFromText("G2") == std::string("Project \xC2\xB7 Feet"));
    // The rule's values win over the item's; the event's over the rule's.
    RuleSounds rs;
    rs.rid = "r1";
    rs.values.push_back({"G1", "rulev", {}});
    item.rules.push_back(rs);
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    sw.group_uid = "G1";
    CHECK(EffectiveValue(sw, e.layers, &from) == "rulev" && from == ValueFrom::Rule);
    CHECK(e.ValueFromText("G1") == "this type" && e.layers.rule.size() == 1);
    EventSounds ev;
    ev.rid = "r1";
    ev.t = 1.0;
    ev.overrides.push_back({"G1", "eventv", {}});
    item.events.push_back(ev);
    e = ResolveAssignFor(st, item, &hero, "r1", P, T, 1.01);
    CHECK(EffectiveValue(sw, e.layers, &from) == "eventv" && from == ValueFrom::Event && e.ValueFromText("G1") == "event");
    e = ResolveAssignFor(st, item, &hero, "r1", P, T, 3.0);  // another event: no override
    CHECK(EffectiveValue(sw, e.layers, &from) == "rulev");
    // A group nobody sets falls to the Switch's default.
    sw.group_uid = "G9";
    sw.default_value = "dflt";
    CHECK(EffectiveValue(sw, e.layers, &from) == "dflt" && from == ValueFrom::Default);
    // Values are read whatever the library state (a None still carries them).
    SetAssignLibraryIn(hero.assigns, {"feet"}, LibState::None, "");
    e = ResolveAssign(st, item, &hero, "r2", P, T);
    CHECK(e.none && e.layers.item.size() == 2);
}

static void TestCharacterFor()
{
    SoundStore st;
    SoundCharacter whole = Hero();
    SoundCharacter split = Hero();
    split.uid = "C2";
    split.name = "Hero body";
    split.model = "Hero_Body.fbx";
    SoundCharacter other = Hero();
    other.uid = "C3";
    other.skeleton = "ffffffffffffffff";
    st.characters = {whole, split, other};
    CHECK(CharacterFor(st, "0123456789abcdef", "hero_body.FBX")->uid == "C2");  // ignoring case
    CHECK(CharacterFor(st, "0123456789abcdef", "Hero_Head.fbx")->uid == "C1");  // the whole rig
    CHECK(CharacterFor(st, "0123456789abcdef", "")->uid == "C1");
    CHECK(CharacterFor(st, "ffffffffffffffff", "Hero_Body.fbx")->uid == "C3");  // its whole rig
    CHECK(CharacterFor(st, "aaaaaaaaaaaaaaaa", "Hero_Body.fbx") == nullptr);
    CHECK(CharacterFor(st, "", "x") == nullptr);
    st.characters.erase(st.characters.begin());  // no whole-rig record
    CHECK(CharacterFor(st, "0123456789abcdef", "Hero_Head.fbx") == nullptr);
    CHECK(CharacterFor(st, "0123456789abcdef", "Hero_Body.fbx")->uid == "C2");
    CHECK(FindCharacterExact(st, "0123456789abcdef", "") == nullptr);
}

static void TestChangedRids()
{
    SoundStore before;
    AddLib(before, "L1");
    const std::vector<Block> blocks = {AutoB(AutoKind::Step, 'L', "a1"), AutoB(AutoKind::Step, 'R', "a2"),
                                       AutoB(AutoKind::Lift, 'L', "a3"), AutoB(AutoKind::Grab, 'L', "a4"),
                                       RuleB("r1", {static_cast<int>(Role::LeftHeel)}), AutoB(AutoKind::Step, 'L', "")};
    ItemSounds item;
    SoundStore after = before;
    CHECK(ChangedRids(before, after, item, blocks, "sk", "m.fbx").empty());

    // A Project library on feet.step: both step blocks (the one without a rid is skipped).
    SetAssignLibraryIn(after.project, {"feet.step"}, LibState::Library, "L1");
    CHECK((ChangedRids(before, after, item, blocks, "sk", "m.fbx") == std::vector<std::string>{"a1", "a2"}));
    // On the part: every foot block, not the hand.
    after = before;
    SetAssignLibraryIn(after.project, {"feet"}, LibState::Library, "L1");
    CHECK((ChangedRids(before, after, item, blocks, "sk", "m.fbx") == std::vector<std::string>{"a1", "a2", "a3", "r1"}));
    // A rule that has its own library does not change.
    RuleSounds own;
    own.rid = "a2";
    own.library = "MINE";
    item.rules.push_back(own);
    CHECK((ChangedRids(before, after, item, blocks, "sk", "m.fbx") == std::vector<std::string>{"a1", "a3", "r1"}));
    item.rules.clear();
    // A value on the type.
    SoundStore v = before;
    SetAssignValueIn(v.project, {"hands.grab"}, "G1", "V1");
    CHECK((ChangedRids(before, v, item, blocks, "sk", "m.fbx") == std::vector<std::string>{"a4"}));
    // A Character edit reaches only the items of that rig (and the file's split).
    SoundCharacter c = Hero();
    c.skeleton = "sk";
    SoundStore withc = before;
    withc.characters.push_back(c);
    SoundStore edited = withc;
    SetAssignLibraryIn(edited.characters[0].assigns, {"feet.lift"}, LibState::Library, "L1");
    CHECK((ChangedRids(withc, edited, item, blocks, "sk", "m.fbx") == std::vector<std::string>{"a3"}));
    CHECK(ChangedRids(withc, edited, item, blocks, "other_rig", "m.fbx").empty());
    // A first edit creates the record: before had none (the Project's apply), after has.
    SoundStore proj = before;
    SetAssignLibraryIn(proj.project, {"feet"}, LibState::Library, "L1");
    SoundStore born = proj;
    born.characters.push_back(c);
    SetAssignLibraryIn(born.characters[0].assigns, {"feet.step"}, LibState::None, "");
    CHECK((ChangedRids(proj, born, item, blocks, "sk", "m.fbx") == std::vector<std::string>{"a1", "a2"}));
    // Renaming changes no rule.
    SoundStore renamed = born;
    RenameCharacterIn(renamed, "C1", "Zed");
    CHECK(ChangedRids(born, renamed, item, blocks, "sk", "m.fbx").empty());
}

static void TestEdits()
{
    std::vector<SoundAssign> list;
    CHECK(!SetAssignLibraryIn(list, {"feet"}, LibState::Inherit, ""));
    CHECK(list.empty());
    CHECK(SetAssignLibraryIn(list, {"feet", "hands"}, LibState::Library, "L1"));
    CHECK(!SetAssignLibraryIn(list, {"feet", "hands"}, LibState::Library, "L1"));
    CHECK(SetAssignLibraryIn(list, {"hands"}, LibState::None, ""));
    CHECK(FindAssign(list, "hands")->state == LibState::None && FindAssign(list, "hands")->library.empty());
    CHECK(SetAssignValueIn(list, {"feet"}, "G1", "V1"));
    CHECK(!SetAssignValueIn(list, {"feet"}, "G1", "V1"));
    CHECK(SetAssignValueIn(list, {"feet"}, "G1", "V2") && FindAssign(list, "feet")->values[0].value_uid == "V2");
    // Inherit on the library keeps an entry that still has a value; one with nothing goes.
    CHECK(SetAssignLibraryIn(list, {"feet", "hands"}, LibState::Inherit, ""));
    CHECK(list.size() == 1 && list[0].key == "feet" && list[0].state == LibState::Inherit);
    CHECK(SetAssignValueIn(list, {"feet"}, "G1", "") && list.empty());
    CHECK(!SetAssignValueIn(list, {"feet"}, "G1", ""));
    CHECK(!SetAssignValueIn(list, {"feet"}, "", "V"));
    // Library state with an empty uid is inherit.
    CHECK(!SetAssignLibraryIn(list, {"feet"}, LibState::Library, ""));

    // Kit adoption: sets the part with the kit flag; never replaces a part already set.
    CHECK(AdoptKitIn(list, "feet", "KIT1"));
    CHECK(FindAssign(list, "feet")->kit && FindAssign(list, "feet")->library == "KIT1");
    CHECK(!AdoptKitIn(list, "feet", "KIT2"));
    CHECK(FindAssign(list, "feet")->library == "KIT1");
    SetAssignLibraryIn(list, {"hands.grab"}, LibState::Library, "L1");  // a typed one is not the part's key
    CHECK(PartIsSet(list, "hands") && !PartKeySet(list, "hands"));
    CHECK(AdoptKitIn(list, "hands", "KIT2"));
    CHECK(FindAssign(list, "hands")->library == "KIT2" && FindAssign(list, "hands.grab")->library == "L1");  // types kept
    CHECK(!AdoptKitIn(list, "hands", "KIT3"));
    CHECK(AdoptKitIn(list, "hands", "KIT3", true) && FindAssign(list, "hands")->library == "KIT3" &&
          FindAssign(list, "hands.grab")->library == "L1");
    CHECK(PartIsSet(list, "hands") && !PartIsSet(list, "body"));
    // Setting a library by hand clears the kit flag.
    CHECK(SetAssignLibraryIn(list, {"feet"}, LibState::Library, "L9"));
    CHECK(!FindAssign(list, "feet")->kit);

    SoundLibrary lib;
    CHECK(SetKitParts(lib, {"hands", "feet", "feet", "nope"}));
    CHECK((lib.kit == std::vector<std::string>{"feet", "hands"}));
    CHECK(!SetKitParts(lib, {"feet", "hands"}));
    CHECK(KitServes(lib, "feet") && !KitServes(lib, "body"));
    CHECK(SetKitParts(lib, {}) && lib.kit.empty());

    CHECK(DefaultCharacterName("Hero_Body.fbx") == "Hero");
    CHECK(DefaultCharacterName("Hero.glb") == "Hero");
    CHECK(DefaultCharacterName("C:\\models\\Orc_Chief_LOD0.fbx") == "Orc");
    CHECK(DefaultCharacterName("noext") == "noext");
    CHECK(DefaultCharacterName("_x.fbx") == "_x");
}

static void TestCharacterEdits()
{
    SoundStore st;
    CharacterRef r;
    CHECK(EnsureCharacter(st, r) == nullptr);
    r.skeleton = "sk1";
    r.files = {"Hero_Body.fbx"};
    SoundCharacter* w = EnsureCharacter(st, r);  // the whole rig, named from its first file
    CHECK(w && st.characters.size() == 1 && w->name == "Hero" && w->model.empty() && IsSoundUid(w->uid));
    const std::string wuid = w->uid;
    CHECK(EnsureCharacter(st, r)->uid == wuid && st.characters.size() == 1);  // exact match, not another
    CharacterRef byuid;
    byuid.uid = wuid;
    CHECK(EnsureCharacter(st, byuid)->uid == wuid);
    SetAssignLibraryIn(FindCharacterByUid(st, wuid)->assigns, {"feet"}, LibState::Library, "L1");

    // A split made at its first edit starts as a copy of the whole rig (its name and settings).
    CharacterRef split;
    split.skeleton = "sk1";
    split.model = "m-armored";
    SoundCharacter* a = EnsureCharacter(st, split);
    CHECK(a && st.characters.size() == 2 && a->uid != wuid && a->name == "Hero" && a->model == "m-armored" &&
          a->assigns.size() == 1 && a->assigns[0].library == "L1");

    CHECK(RenameCharacterIn(st, wuid, "Knight"));
    CHECK(!RenameCharacterIn(st, wuid, "Knight") && !RenameCharacterIn(st, wuid, "") && !RenameCharacterIn(st, "nope", "x"));
    // Split: one per model, a copy of the settings; the model with a record is skipped.
    const std::vector<ModelRef> models = {{"m-body", "Hero_Body"}, {"m-head", "Hero_Head"}, {"m-armored", "Hero_Armored"}};
    CHECK(SplitCharacterIn(st, wuid, models) == 2 && st.characters.size() == 4);
    const SoundCharacter* head = FindCharacterExact(st, "sk1", "m-head");
    CHECK(head && head->name == "Hero_Head" && head->assigns.size() == 1 && head->assigns[0].library == "L1");
    CHECK(SplitCharacterIn(st, wuid, models) == 0);
    CHECK(SplitCharacterIn(st, "nope", {{"m-x", "x"}}) == 0);
    CHECK(SplitCharacterIn(st, wuid, {{"", "empty"}}) == 0);
    // The split is independent of the rig after the copy.
    SoundCharacter* h2 = FindCharacterByUid(st, head->uid);
    SetAssignLibraryIn(h2->assigns, {"feet"}, LibState::None, "");
    CHECK(FindCharacterByUid(st, wuid)->assigns[0].state == LibState::Library);
    // CharacterFor: the split of that model, else the whole rig, else none.
    CHECK(CharacterFor(st, "sk1", "m-head") == FindCharacterByUid(st, head->uid));
    CHECK(CharacterFor(st, "sk1", "m-unknown") == FindCharacterByUid(st, wuid));
    CHECK(CharacterFor(st, "sk2", "m-head") == nullptr);
}

static void TestModelSignature()
{
    CHECK(ModelSignature({}).empty());
    MeshShape body{1200, 3600, 0, true}, sword{300, 900, 1, false};
    const std::string a = ModelSignature({body, sword});
    CHECK(a.size() == 16 && a == ModelSignature({sword, body}));  // order ignored
    MeshShape armor{800, 2400, 2, true};
    CHECK(ModelSignature({body, sword, armor}) != a);
    MeshShape body2 = body;
    body2.skinned = false;
    CHECK(ModelSignature({body2, sword}) != a);
    CHECK(SplitCharacterName("C:\\m\\Hero_Armored.fbx") == "Hero_Armored" && SplitCharacterName("x") == "x");
}

static void TestGrammar()
{
    SoundStore s;
    SwitchGroup g;
    g.uid = "G1";
    g.name = "Surface";
    s.groups.push_back(g);
    SoundAssign a;
    a.key = "feet.step";
    a.state = LibState::Library;
    a.library = "LIB 1";
    a.kit = true;
    a.values.push_back({"G1", "V1", {"vx=1"}});
    a.kept = {"future=1"};
    a.kept_lines = {"aextra x=2"};
    s.project.push_back(a);
    SoundAssign n;
    n.key = "hands";
    n.state = LibState::None;
    s.project.push_back(n);
    SoundCharacter c = Hero();
    c.model = "Hero_Body.fbx";
    c.files = {"Hero_Body.fbx", "a,b c.fbx"};
    c.kept = {"cf=1"};
    c.kept_lines = {"cextra y"};
    SoundAssign ca;
    ca.key = "feet";
    ca.state = LibState::Library;
    ca.library = "LIB2";
    ca.values.push_back({"G1", "V2", {}});
    ca.kept_lines = {"caextra z"};
    c.assigns.push_back(ca);
    SoundAssign cn;
    cn.key = "hands.grab";
    cn.state = LibState::None;
    c.assigns.push_back(cn);
    s.characters.push_back(c);
    SoundLibrary lib;
    lib.uid = "LIB1";
    lib.name = "Wood";
    lib.root = "N1";
    lib.kit = {"feet", "hands"};
    lib.kit_offered = true;
    lib.kit_kept = {"kx=3"};
    SoundNode node;
    node.uid = "N1";
    node.type = SoundNodeType::Random;
    lib.nodes.push_back(node);
    s.libraries.push_back(lib);

    const std::string text = SerializeSoundStore(s);
    CHECK(text.find("RAVSOUNDS 1\nassign key=feet.step lib=LIB%201 kit=1 future=1\nasvalue group=G1 value=V1 vx=1\naextra x=2\n"
                    "assign key=hands lib=none\n"
                    "character uid=C1 name=Hero skel=0123456789abcdef model=Hero_Body.fbx files=Hero_Body.fbx,a%2Cb%20c.fbx cf=1\n"
                    "cextra y\ncassign key=feet lib=LIB2\ncasvalue group=G1 value=V2\ncaextra z\ncassign key=hands.grab lib=none\n"
                    "group uid=G1") == 0);
    CHECK(text.find("library uid=LIB1 name=Wood root=N1\nkit parts=feet,hands offered=1 kx=3\nnode uid=N1") != std::string::npos);
    SoundStore r;
    CHECK(ParseSoundStore(text, &r));
    CHECK(SerializeSoundStore(r) == text);
    CHECK(r.project.size() == 2 && r.project[0].state == LibState::Library && r.project[0].library == "LIB 1" &&
          r.project[0].kit && r.project[0].values.size() == 1 && r.project[0].values[0].kept.size() == 1 &&
          r.project[0].kept.size() == 1 && r.project[0].kept_lines.size() == 1);
    CHECK(r.project[1].state == LibState::None && r.project[1].library.empty());
    CHECK(r.characters.size() == 1 && r.characters[0].assigns.size() == 2 && r.characters[0].kept_lines.size() == 1 &&
          r.characters[0].assigns[0].kept_lines.size() == 1 && r.characters[0].assigns[0].values.size() == 1 &&
          r.characters[0].files.size() == 2 && r.characters[0].files[1] == "a,b c.fbx" &&
          r.characters[0].model == "Hero_Body.fbx" && r.characters[0].kept.size() == 1);
    CHECK(r.characters[0].assigns[1].state == LibState::None);
    CHECK(r.libraries.size() == 1 && r.libraries[0].kit.size() == 2 && r.libraries[0].kit_offered &&
          r.libraries[0].kit_kept.size() == 1 && r.libraries[0].kept.empty());
    CHECK(r.kept_lines.empty() && r.groups.size() == 1);

    // A store of only assignments is not empty (it is saved).
    SoundStore only;
    only.project.push_back(n);
    CHECK(!SoundStoreEmpty(only));
    CHECK(SerializeSoundStore(only) == "RAVSOUNDS 1\nassign key=hands lib=none\n");

    // The rule's None.
    ItemSounds item;
    RuleSounds rs;
    rs.rid = "r1";
    rs.none = true;
    item.rules.push_back(rs);
    RuleSounds rl;
    rl.rid = "r2";
    rl.library = "LIB";
    item.rules.push_back(rl);
    const std::string it = SerializeItemSounds(item);
    CHECK(it == "RAVITEMSND 1\nrule rid=r1 none=1\nrule rid=r2 lib=LIB\n");
    ItemSounds ri;
    CHECK(ParseItemSounds(it, &ri) && ri.rules[0].none && !ri.rules[1].none && ri.rules[1].library == "LIB");
    CHECK(SerializeItemSounds(ri) == it);

    // A store written before (no assign line) reads with none and writes back the same.
    const std::string old = "RAVSOUNDS 1\ngroup uid=G1 name=Surface\nvalue uid=V1 name=Grass\n"
                            "library uid=L1 name=Feet root=N1\nnode uid=N1 type=random\n";
    SoundStore o;
    CHECK(ParseSoundStore(old, &o));
    CHECK(o.project.empty() && o.characters.empty() && o.libraries.size() == 1 && o.libraries[0].kit.empty());
    CHECK(SerializeSoundStore(o) == old);

    // A stray child line (no parent) is kept as an unknown line; a group closes an assign.
    SoundStore stray;
    CHECK(ParseSoundStore("RAVSOUNDS 1\nasvalue group=G1 value=V1\ncassign key=feet lib=none\ncasvalue group=G value=V\n"
                          "kit parts=feet\n",
                          &stray));
    CHECK(stray.project.empty() && stray.characters.empty() && stray.kept_lines.size() == 4);
    SoundStore order;
    CHECK(ParseSoundStore("RAVSOUNDS 1\nassign key=feet\ngroup uid=G1 name=S\nvalue uid=V1 name=A\nasvalue group=G1 value=V1\n",
                          &order));
    CHECK(order.project.size() == 1 && order.project[0].values.empty() && order.groups[0].values.size() == 1 &&
          order.groups[0].kept_lines.size() == 1);
    // An unreadable lib value is kept as written, standing for the field.
    SoundStore bad;
    CHECK(ParseSoundStore("RAVSOUNDS 1\nassign key=feet lib=%zz\n", &bad));
    CHECK(bad.project[0].state == LibState::Inherit && bad.project[0].kept.size() == 1);
    CHECK(SerializeSoundStore(bad) == "RAVSOUNDS 1\nassign key=feet lib=%zz\n");
}

static void TestMigration()
{
    // Review 13-15a: a target is migrated only when every old entry folding into it is set on both
    // sides and agrees, so no marker that played nothing starts playing.
    const std::string text =
        "RAVSOUNDS 1\n"
        "auto key=step:R lib=LL\n"
        "avalue group=G1 value=V1\n"
        "auto key=step:L lib=LL\n"
        "avalue group=G1 value=V1\n"            // both sides agree: migrated
        "auto key=lift:R lib=LIFTR\n"           // no L: left lifts played nothing, not migrated
        "auto key=slide_heel:L lib=SL\n"
        "auto key=slide_heel:R lib=SL\n"
        "auto key=slide_toe:L lib=OTHER\n"      // another slide kind disagrees: not migrated
        "auto key=slide_toe:R lib=OTHER\n"
        "auto key=hand_pivot:L lib=HP\n"
        "auto key=hand_pivot:R lib=HP\n"
        "keepme line=1\n"                       // an unknown line: kept
        "auto key=grab:L\n"                     // no library, no value: nothing
        "auto key=grab:R\n"
        "auto key=release:L\n"
        "avalue group=G2 value=V2\n"            // values alone, both sides: kept (inherit)
        "auto key=release:R\n"
        "avalue group=G2 value=V2\n"
        "auto key=pivot:L lib=P1\n"
        "auto key=pivot:R lib=P2\n"             // sides differ: not migrated
        "auto key=future_kind:L lib=X\n"
        "group uid=G1 name=Surface\n";
    SoundStore s;
    CHECK(ParseSoundStore(text, &s));
    CHECK(s.auto_sounds.empty());
    CHECK(s.project.size() == 3);
    const SoundAssign* step = FindAssign(s.project, "feet.step");
    CHECK(step && step->state == LibState::Library && step->library == "LL" && step->values.size() == 1);
    CHECK(FindAssign(s.project, "feet.lift") == nullptr);
    CHECK(FindAssign(s.project, "feet.slide") == nullptr);
    CHECK(FindAssign(s.project, "feet.pivot") == nullptr);
    CHECK(FindAssign(s.project, "hands.hand_pivot") && FindAssign(s.project, "hands.hand_pivot")->library == "HP");
    const SoundAssign* rel = FindAssign(s.project, "hands.release");
    CHECK(rel && rel->state == LibState::Inherit && rel->library.empty() && rel->values.size() == 1);
    CHECK(FindAssign(s.project, "hands.grab") == nullptr);
    CHECK(s.groups.size() == 1);
    CHECK(SerializeSoundStore(s).find("keepme line=1") != std::string::npos);
    // A heel step set alone: the only step kind with an entry, so migrated (a toe step, if the project
    // had one with no sound, then inherits it: accepted, Epic 13 is not released).
    SoundStore h;
    CHECK(ParseSoundStore("RAVSOUNDS 1\nauto key=heel:L lib=HEEL\nauto key=heel:R lib=HEEL\n", &h));
    CHECK(h.project.size() == 1 && h.project[0].library == "HEEL");  // the only step kind set: migrated
    // Never written back as auto lines; reading the result again changes nothing.
    const std::string out = SerializeSoundStore(s);
    CHECK(out.find("auto ") == std::string::npos);
    SoundStore again;
    CHECK(ParseSoundStore(out, &again) && SerializeSoundStore(again) == out);
    // Existing rules keep their library (they play the same).
    ItemSounds item;
    CHECK(ParseItemSounds("RAVITEMSND 1\nrule rid=a lib=OLD\n", &item) && item.rules[0].library == "OLD" && !item.rules[0].none);
    // After the migration the old per-type sound plays through the Project layer.
    EffectiveAssign e = ResolveAssign(s, ItemSounds{}, nullptr, "x", "feet", "step");
    CHECK(e.library == "LL");
}

int main()
{
    TestTable();
    TestPartOfBlock();
    TestLibraryChain();
    TestValueChain();
    TestCharacterFor();
    TestChangedRids();
    TestEdits();
    TestCharacterEdits();
    TestModelSignature();
    TestGrammar();
    TestMigration();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("sound_assign: all checks passed\n");
    return 0;
}
