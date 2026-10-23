// SPDX-License-Identifier: MIT
//
// Story 13-15a -- src/sound_assign.h (pure) and its text (sound_record.h): the body parts, the part
// of a block, the layered resolution (library and Switch values, inherit / None / unset, the
// sources), the character of a rig, the rids a store edit changes, the pure edits, the grammar
// (assign / character / kit / none lines, unknown text kept) and the migration of the old per-type
// sounds.

#include <algorithm>
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
    // Story 10-9a: Legs > Jump / Land, assigned like any type (story 10-9e: and Tap / Drag). Story
    // 10-9b: Legs > Knee, Arms > Elbow, Body > Sit / Fall. Story 10-9d: Arms > Clap, Hand on body.
    // Story 10-9c: Legs > Kick, Arms > Swing / Punch. Story 10-9i: Legs > Heel Tap. Story 10-9l: Arms > Slap.
    CHECK(parts[0].id == "legs" && parts[0].label == "Legs" && parts[0].types.size() == 12);
    if (parts[0].types.size() == 12)
        CHECK(parts[0].types[0].word == "step" && parts[0].types[1].word == "lift" && parts[0].types[2].word == "slide" &&
              parts[0].types[3].word == "pivot" && parts[0].types[4].word == "jump" && parts[0].types[5].word == "land" &&
              parts[0].types[6].word == "tap" && parts[0].types[7].word == "drag" && parts[0].types[8].word == "knee" &&
              parts[0].types[9].word == "kick" && parts[0].types[10].word == "heel_tap" &&
              parts[0].types[11].word == "rules");
    // Story 10-9k: Arms > Jump, Land (the arms of a jump); story 10-9l: Arms > Slap (10-9o: no Strike).
    CHECK(parts[1].id == "arms" && parts[1].types.size() == 12);
    if (parts[1].types.size() == 12)
        CHECK(parts[1].types[2].word == "hand_pivot" && parts[1].types[3].word == "elbow" &&
              parts[1].types[4].word == "clap" && parts[1].types[5].word == "hand_body" &&
              parts[1].types[6].word == "swing" && parts[1].types[7].word == "punch" &&
              parts[1].types[8].word == "arm_jump" && parts[1].types[9].word == "arm_land" &&
              parts[1].types[10].word == "slap" && parts[1].types[11].word == "rules");
    CHECK(TypeLabel("arms", "arm_jump") == "Jump" && TypeLabel("arms", "arm_land") == "Land");
    CHECK(parts[2].id == "body" && parts[2].label == "Body" && parts[2].has_category &&
          parts[2].category == AutoCategory::Body && parts[2].types.size() == 5);
    if (parts[2].types.size() == 5)  // stories 10-9g, 10-9f, 10-9b: Body > Jolt, Spin, Sit, Fall
        CHECK(parts[2].types[0].word == "jolt" && parts[2].types[1].word == "spin" && parts[2].types[2].word == "sit" &&
              parts[2].types[3].word == "fall" && parts[2].types[4].word == "rules");
    CHECK(parts[3].id == "other" && parts[3].label == "Other bones");
    CHECK(TypeLabel("legs", "step") == "Step" && TypeLabel("legs", "rules") == "Bone rules");
    CHECK(TypeLabel("arms", "grab") == "Grab");
    CHECK(AssignKey("legs", "step") == "legs.step" && AssignKey("legs", "") == "legs");
    std::string p, t;
    SplitAssignKey("arms.hand_pivot", &p, &t);
    CHECK(p == "arms" && t == "hand_pivot");
    SplitAssignKey("body", &p, &t);
    CHECK(p == "body" && t.empty());
    CHECK(AssignKeyLabel("legs.step") == std::string("Legs \xC2\xB7 Step"));
    for (int i = 0; i < kAutoTypeCount; ++i) {
        AutoType back;
        CHECK(AutoTypeFromWord(AutoTypeWord(static_cast<AutoType>(i)), &back) && back == static_cast<AutoType>(i));
    }
}


// 13-23 (spec 13-20-23-25 D1): the side-free role of a block's events.
static void TestEventRoleKey()
{
    CHECK(RoleFamilyKey(Role::LeftToeEnd) == "toe_end" && RoleFamilyKey(Role::RightHeel) == "heel" &&
          RoleFamilyKey(Role::Hips) == "hips" && RoleFamilyKey(Role::LeftUpLeg) == "hip" &&
          RoleFamilyKey(Role::RightHand) == "hand");
    // Every auto kind, both sides: heel kinds, toe kinds, hand kinds; combined ones have none.
    const char* want[kAutoKindCount] = {"",     "heel", "toe",  "",     "",     "",     "hand", "hand", "hand",
                                        "hand", "heel", "toe",  "heel", "toe",  "heel", "toe",  "heel", "toe",
                                        "",     "",   // story 10-9a: jump, land (combined: no foot part)
                                        "heel", "toe",  "heel", "toe",  "heel", "toe",   // 10-9a-3: their parts
                                        "",                                              // story 10-9g: jolt (no part)
                                        "",     "",   // story 10-9f: spin, spin end (no part)
                                        "toe",  "toe",   // story 10-9e: tap, drag (on the toe)
                                        "knee", "elbow", "",    "",                      // 10-9b: sit, fall: the body
                                        "hand", "hand",   // story 10-9d: clap, hand on body
                                        "",     "heel", "toe",  "",     "heel", "toe",   // 10-9c: kick, at the start
                                        "hand", "hand", "hand", "hand",                  // swing, punch
                                        "heel",                                          // 10-9i: heel tap
                                        // story 10-9j: the scuffs' Peak twins, as their Start kinds
                                        "",     "heel", "toe",  "heel", "toe",               // slide
                                        "",     "heel", "toe",  "heel", "toe",               // pivot
                                        "hand", "hand",                                  // hand pivot
                                        "hand", "hand", "hand",                          // 10-9k: arms jump, land; 10-9l: slap
                                        "hand"};                                         // 10-9o: punch at its peak
    for (int k = 0; k < kAutoKindCount; ++k)
        for (char side : {'L', 'R'}) CHECK(EventRoleKey(AutoB(static_cast<AutoKind>(k), side, "x")) == want[k]);
    Block unknown = AutoB(AutoKind::Heel, 'L', "x");
    unknown.auto_type = "future_kind";
    CHECK(EventRoleKey(unknown).empty());
    // Rules: the first role of the first condition; built-in roles read side-free.
    CHECK(EventRoleKey(RuleB("r", {static_cast<int>(Role::LeftToeEnd)})) == "toe_end");
    CHECK(EventRoleKey(RuleB("r", {static_cast<int>(Role::RightHeel), static_cast<int>(Role::LeftToe)})) == "heel");
    CHECK(EventRoleKey(RuleB("r", {}, {static_cast<int>(Role::LeftHand)})) == "hand");  // ref bones too
    // A custom role: its key without its side; a plain bone is skipped; nothing = "".
    const int ball = BoneRefId("role:left_ball");
    const int paw = BoneRefId("role:paw");
    const int bone = BoneRefForBone("mixamorig:LeftFoot");
    CHECK(EventRoleKey(RuleB("r", {ball})) == "ball" && EventRoleKey(RuleB("r", {paw})) == "paw");
    CHECK(EventRoleKey(RuleB("r", {BoneRefId("role:right_ball")})) == "ball");
    CHECK(EventRoleKey(RuleB("r", {bone})).empty());
    CHECK(EventRoleKey(RuleB("r", {bone, static_cast<int>(Role::LeftToe)})) == "toe");
    CHECK(EventRoleKey(RuleB("r", {bone}, {ball})) == "ball");
    Block none;
    none.marker = "Rule";
    CHECK(EventRoleKey(none).empty());
    // Only the first condition counts.
    Block two = RuleB("r", {bone});
    Condition c2;
    c2.signal.bones = {static_cast<int>(Role::LeftHeel)};
    two.conditions.push_back(c2);
    CHECK(EventRoleKey(two).empty());
}

static void TestPartOfBlock()
{
    CHECK(PartOf(RuleB("r", {static_cast<int>(Role::LeftHeel)})) == "legs.rules");
    CHECK(PartOf(RuleB("r", {static_cast<int>(Role::LeftKnee)})) == "legs.rules");
    CHECK(PartOf(RuleB("r", {static_cast<int>(Role::RightHand)})) == "arms.rules");
    CHECK(PartOf(RuleB("r", {static_cast<int>(Role::Hips)})) == "body.rules");
    CHECK(PartOf(RuleB("r", {static_cast<int>(Role::Head)})) == "body.rules");
    // A custom role / raw bone (an id from kBoneRefExtra) is "other"; the first built-in role wins.
    CHECK(PartOf(RuleB("r", {kBoneRefExtra + 3})) == "other.rules");
    CHECK(PartOf(RuleB("r", {kBoneRefExtra + 3, static_cast<int>(Role::LeftHand)})) == "arms.rules");
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
    const char* want[kAutoKindCount] = {"legs.step",  "legs.step",  "legs.step",       "legs.lift",
                                        "legs.slide", "legs.pivot", "arms.grab",      "arms.release",
                                        "arms.hand_pivot", "arms.hand_pivot", "legs.slide", "legs.slide",
                                        "legs.pivot", "legs.pivot", "legs.slide",      "legs.slide",
                                        "legs.pivot", "legs.pivot", "legs.jump", "legs.land",
                                        "legs.lift",  "legs.lift",  "legs.jump", "legs.jump", "legs.land", "legs.land",
                                        "body.jolt",                // story 10-9g
                                        "body.spin", "body.spin",   // story 10-9f
                                        "legs.tap",   "legs.drag",  // story 10-9e
                                        // story 10-9b
                                        "legs.knee",  "arms.elbow", "body.sit",  "body.fall",
                                        "arms.clap",  "arms.hand_body",  // story 10-9d
                                        // story 10-9c: kick (combined, heel, toe, then at the start), swing, punch
                                        "legs.kick", "legs.kick", "legs.kick", "legs.kick", "legs.kick", "legs.kick",
                                        "arms.swing", "arms.swing", "arms.punch", "arms.punch",
                                        "legs.heel_tap",  // story 10-9i
                                        // story 10-9j: the scuffs' Peak twins
                                        "legs.slide", "legs.slide", "legs.slide", "legs.slide", "legs.slide",
                                        "legs.pivot", "legs.pivot", "legs.pivot", "legs.pivot", "legs.pivot",
                                        "arms.hand_pivot", "arms.hand_pivot",
                                        "arms.arm_jump", "arms.arm_land",
                                        "arms.slap",  // story 10-9k, 10-9l
                                        "arms.punch"};  // story 10-9o: punch at its peak
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

    // The migration table (sound_record.cpp) agrees with PartOfBlock for every kind and side it
    // knows (frozen legacy: the kinds before story 10-9a's jump and land).
    for (int k = 0; k < static_cast<int>(AutoKind::Jump); ++k)
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
    for (const char* u : {"PP", "PT", "CP", "CT", "RL"}) AddLib(st, u);  // a deleted library reads as inherit (2026-10-21)
    ItemSounds item;
    SoundCharacter hero = Hero();
    const std::string P = "legs", T = "step";
    // Nothing set anywhere.
    EffectiveAssign e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.library.empty() && !e.none && e.level == AssignLevel::Unset && e.from == "nothing set");

    SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, "PP");
    e = ResolveAssign(st, item, nullptr, "r1", P, T);
    CHECK(e.library == "PP" && e.level == AssignLevel::Project && !e.type_specific &&
          e.from == std::string("Project \xC2\xB7 Legs"));
    SetAssignLibraryIn(st.project, {"legs.step"}, LibState::Library, "PT");
    e = ResolveAssign(st, item, nullptr, "r1", P, T);
    CHECK(e.library == "PT" && e.type_specific && e.from == std::string("Project \xC2\xB7 Legs \xC2\xB7 Step"));
    // Another type of the part reads the part-level one.
    CHECK(ResolveAssign(st, item, nullptr, "r1", P, "lift").library == "PP");
    // Another part reads nothing.
    CHECK(ResolveAssign(st, item, nullptr, "r1", "arms", "grab").library.empty());

    // The Character is above the Project, its type above its part.
    SetAssignLibraryIn(hero.assigns, {"legs"}, LibState::Library, "CP");
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.library == "CP" && e.level == AssignLevel::Character && e.from == std::string("Hero \xC2\xB7 Legs"));
    SetAssignLibraryIn(hero.assigns, {"legs.step"}, LibState::Library, "CT", true);
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.library == "CT" && e.kit && e.from == std::string("Hero \xC2\xB7 Legs \xC2\xB7 Step"));
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
    SetAssignLibraryIn(hero.assigns, {"legs.step"}, LibState::None, "");
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.none && e.library.empty() && e.level == AssignLevel::Character && e.type_specific);
    // Inherit at the type passes it on to the part.
    SetAssignLibraryIn(hero.assigns, {"legs.step"}, LibState::Inherit, "");
    CHECK(FindAssign(hero.assigns, "legs.step") == nullptr);  // an empty entry is dropped
    CHECK(ResolveAssign(st, item, &hero, "r1", P, T).library == "CP");
    // A Character part-level None hides the Project's type-level library.
    SetAssignLibraryIn(hero.assigns, {"legs"}, LibState::None, "");
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.none && e.level == AssignLevel::Character && !e.type_specific);
    // A Project part-level None.
    SetAssignLibraryIn(st.project, {"legs.step"}, LibState::Inherit, "");
    SetAssignLibraryIn(st.project, {"legs"}, LibState::None, "");
    e = ResolveAssign(st, item, nullptr, "r1", P, T);
    CHECK(e.none && e.level == AssignLevel::Project);

    // A kit is never read.
    SoundLibrary kitlib;
    kitlib.uid = "KIT";
    kitlib.kit = {"legs"};
    st.libraries.push_back(kitlib);
    SetAssignLibraryIn(st.project, {"legs"}, LibState::Inherit, "");
    CHECK(ResolveAssign(st, item, nullptr, "r1", P, T).library.empty());
}

static void TestValueChain()
{
    SoundStore st;
    ItemSounds item;
    SoundCharacter hero = Hero();
    const std::string P = "legs", T = "step";
    // Group G1 set at every level, lowest first.
    SetAssignValueIn(st.project, {"legs"}, "G1", "projpart");
    EffectiveAssign e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.layers.item.size() == 1 && e.layers.item[0].value_uid == "projpart");
    CHECK(e.value_from.size() == 1 && e.ValueFromText("G1") == std::string("Project \xC2\xB7 Legs"));
    CHECK(e.ValueFromText("GX") == "default");
    SetAssignValueIn(st.project, {"legs.step"}, "G1", "projtype");
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.layers.item[0].value_uid == "projtype" && e.ValueFromText("G1") == std::string("Project \xC2\xB7 Legs \xC2\xB7 Step"));
    SetAssignValueIn(hero.assigns, {"legs"}, "G1", "charpart");
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.layers.item[0].value_uid == "charpart" && e.ValueFromText("G1") == std::string("Hero \xC2\xB7 Legs"));
    SetAssignValueIn(hero.assigns, {"legs.step"}, "G1", "chartype");
    e = ResolveAssign(st, item, &hero, "r1", P, T);
    CHECK(e.layers.item[0].value_uid == "chartype" && e.ValueFromText("G1") == std::string("Hero \xC2\xB7 Legs \xC2\xB7 Step"));
    // The item's own values win over the inherited ones, and the merged layer keeps the others.
    SetAssignValueIn(st.project, {"legs"}, "G2", "p2");
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
    CHECK(e.ValueFromText("G1") == "item" && e.ValueFromText("G2") == std::string("Project \xC2\xB7 Legs"));
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
    SetAssignLibraryIn(hero.assigns, {"legs"}, LibState::None, "");
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

    // A Project library on legs.step: both step blocks (the one without a rid is skipped).
    SetAssignLibraryIn(after.project, {"legs.step"}, LibState::Library, "L1");
    CHECK((ChangedRids(before, after, item, blocks, "sk", "m.fbx") == std::vector<std::string>{"a1", "a2"}));
    // On the part: every foot block, not the hand.
    after = before;
    SetAssignLibraryIn(after.project, {"legs"}, LibState::Library, "L1");
    CHECK((ChangedRids(before, after, item, blocks, "sk", "m.fbx") == std::vector<std::string>{"a1", "a2", "a3", "r1"}));
    // A rule that has its own library does not change.
    RuleSounds own;
    own.rid = "a2";
    own.library = "MINE";
    AddLib(before, "MINE");
    AddLib(after, "MINE");
    item.rules.push_back(own);
    CHECK((ChangedRids(before, after, item, blocks, "sk", "m.fbx") == std::vector<std::string>{"a1", "a3", "r1"}));
    item.rules.clear();
    // A value on the type.
    SoundStore v = before;
    SetAssignValueIn(v.project, {"arms.grab"}, "G1", "V1");
    CHECK((ChangedRids(before, v, item, blocks, "sk", "m.fbx") == std::vector<std::string>{"a4"}));
    // A Character edit reaches only the items of that rig (and the file's split).
    SoundCharacter c = Hero();
    c.skeleton = "sk";
    SoundStore withc = before;
    withc.characters.push_back(c);
    SoundStore edited = withc;
    SetAssignLibraryIn(edited.characters[0].assigns, {"legs.lift"}, LibState::Library, "L1");
    CHECK((ChangedRids(withc, edited, item, blocks, "sk", "m.fbx") == std::vector<std::string>{"a3"}));
    CHECK(ChangedRids(withc, edited, item, blocks, "other_rig", "m.fbx").empty());
    // A first edit creates the record: before had none (the Project's apply), after has.
    SoundStore proj = before;
    SetAssignLibraryIn(proj.project, {"legs"}, LibState::Library, "L1");
    SoundStore born = proj;
    born.characters.push_back(c);
    SetAssignLibraryIn(born.characters[0].assigns, {"legs.step"}, LibState::None, "");
    CHECK((ChangedRids(proj, born, item, blocks, "sk", "m.fbx") == std::vector<std::string>{"a1", "a2"}));
    // Renaming changes no rule.
    SoundStore renamed = born;
    RenameCharacterIn(renamed, "C1", "Zed");
    CHECK(ChangedRids(born, renamed, item, blocks, "sk", "m.fbx").empty());
}

static void TestEdits()
{
    std::vector<SoundAssign> list;
    CHECK(!SetAssignLibraryIn(list, {"legs"}, LibState::Inherit, ""));
    CHECK(list.empty());
    CHECK(SetAssignLibraryIn(list, {"legs", "arms"}, LibState::Library, "L1"));
    CHECK(!SetAssignLibraryIn(list, {"legs", "arms"}, LibState::Library, "L1"));
    CHECK(SetAssignLibraryIn(list, {"arms"}, LibState::None, ""));
    CHECK(FindAssign(list, "arms")->state == LibState::None && FindAssign(list, "arms")->library.empty());
    CHECK(SetAssignValueIn(list, {"legs"}, "G1", "V1"));
    CHECK(!SetAssignValueIn(list, {"legs"}, "G1", "V1"));
    CHECK(SetAssignValueIn(list, {"legs"}, "G1", "V2") && FindAssign(list, "legs")->values[0].value_uid == "V2");
    // Inherit on the library keeps an entry that still has a value; one with nothing goes.
    CHECK(SetAssignLibraryIn(list, {"legs", "arms"}, LibState::Inherit, ""));
    CHECK(list.size() == 1 && list[0].key == "legs" && list[0].state == LibState::Inherit);
    CHECK(SetAssignValueIn(list, {"legs"}, "G1", "") && list.empty());
    CHECK(!SetAssignValueIn(list, {"legs"}, "G1", ""));
    CHECK(!SetAssignValueIn(list, {"legs"}, "", "V"));
    // Library state with an empty uid is inherit.
    CHECK(!SetAssignLibraryIn(list, {"legs"}, LibState::Library, ""));

    // Kit adoption: sets the part with the kit flag; never replaces a part already set.
    CHECK(AdoptKitIn(list, "legs", "KIT1"));
    CHECK(FindAssign(list, "legs")->kit && FindAssign(list, "legs")->library == "KIT1");
    CHECK(!AdoptKitIn(list, "legs", "KIT2"));
    CHECK(FindAssign(list, "legs")->library == "KIT1");
    SetAssignLibraryIn(list, {"arms.grab"}, LibState::Library, "L1");  // a typed one is not the part's key
    CHECK(PartIsSet(list, "arms") && !PartKeySet(list, "arms"));
    CHECK(AdoptKitIn(list, "arms", "KIT2"));
    CHECK(FindAssign(list, "arms")->library == "KIT2" && FindAssign(list, "arms.grab")->library == "L1");  // types kept
    CHECK(!AdoptKitIn(list, "arms", "KIT3"));
    CHECK(AdoptKitIn(list, "arms", "KIT3", true) && FindAssign(list, "arms")->library == "KIT3" &&
          FindAssign(list, "arms.grab")->library == "L1");
    CHECK(PartIsSet(list, "arms") && !PartIsSet(list, "body"));
    // Setting a library by hand clears the kit flag.
    CHECK(SetAssignLibraryIn(list, {"legs"}, LibState::Library, "L9"));
    CHECK(!FindAssign(list, "legs")->kit);

    SoundLibrary lib;
    CHECK(SetKitParts(lib, {"arms", "legs", "legs", "nope"}));
    CHECK((lib.kit == std::vector<std::string>{"legs", "arms"}));
    CHECK(!SetKitParts(lib, {"legs", "arms"}));
    CHECK(KitServes(lib, "legs") && !KitServes(lib, "body"));
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
    SetAssignLibraryIn(FindCharacterByUid(st, wuid)->assigns, {"legs"}, LibState::Library, "L1");

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
    SetAssignLibraryIn(h2->assigns, {"legs"}, LibState::None, "");
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
    a.key = "legs.step";
    a.state = LibState::Library;
    a.library = "LIB 1";
    a.kit = true;
    a.values.push_back({"G1", "V1", {"vx=1"}});
    a.kept = {"future=1"};
    a.kept_lines = {"aextra x=2"};
    s.project.push_back(a);
    SoundAssign n;
    n.key = "arms";
    n.state = LibState::None;
    s.project.push_back(n);
    SoundCharacter c = Hero();
    c.model = "Hero_Body.fbx";
    c.files = {"Hero_Body.fbx", "a,b c.fbx"};
    c.kept = {"cf=1"};
    c.kept_lines = {"cextra y"};
    SoundAssign ca;
    ca.key = "legs";
    ca.state = LibState::Library;
    ca.library = "LIB2";
    ca.values.push_back({"G1", "V2", {}});
    ca.kept_lines = {"caextra z"};
    c.assigns.push_back(ca);
    SoundAssign cn;
    cn.key = "arms.grab";
    cn.state = LibState::None;
    c.assigns.push_back(cn);
    s.characters.push_back(c);
    SoundLibrary lib;
    lib.uid = "LIB1";
    lib.name = "Wood";
    lib.root = "N1";
    lib.kit = {"legs", "arms"};
    lib.kit_offered = true;
    lib.kit_kept = {"kx=3"};
    SoundNode node;
    node.uid = "N1";
    node.type = SoundNodeType::Random;
    lib.nodes.push_back(node);
    s.libraries.push_back(lib);

    const std::string text = SerializeSoundStore(s);
    CHECK(text.find("RAVSOUNDS 1\nassign key=legs.step lib=LIB%201 kit=1 future=1\nasvalue group=G1 value=V1 vx=1\naextra x=2\n"
                    "assign key=arms lib=none\n"
                    "character uid=C1 name=Hero skel=0123456789abcdef model=Hero_Body.fbx files=Hero_Body.fbx,a%2Cb%20c.fbx cf=1\n"
                    "cextra y\ncassign key=legs lib=LIB2\ncasvalue group=G1 value=V2\ncaextra z\ncassign key=arms.grab lib=none\n"
                    "group uid=G1") == 0);
    CHECK(text.find("library uid=LIB1 name=Wood root=N1\nkit parts=legs,arms offered=1 kx=3\nnode uid=N1") != std::string::npos);
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
    CHECK(SerializeSoundStore(only) == "RAVSOUNDS 1\nassign key=arms lib=none\n");

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
    CHECK(ParseSoundStore("RAVSOUNDS 1\nasvalue group=G1 value=V1\ncassign key=legs lib=none\ncasvalue group=G value=V\n"
                          "kit parts=legs\n",
                          &stray));
    CHECK(stray.project.empty() && stray.characters.empty() && stray.kept_lines.size() == 4);
    SoundStore order;
    CHECK(ParseSoundStore("RAVSOUNDS 1\nassign key=legs\ngroup uid=G1 name=S\nvalue uid=V1 name=A\nasvalue group=G1 value=V1\n",
                          &order));
    CHECK(order.project.size() == 1 && order.project[0].values.empty() && order.groups[0].values.size() == 1 &&
          order.groups[0].kept_lines.size() == 1);
    // An unreadable lib value is kept as written, standing for the field.
    SoundStore bad;
    CHECK(ParseSoundStore("RAVSOUNDS 1\nassign key=legs lib=%zz\n", &bad));
    CHECK(bad.project[0].state == LibState::Inherit && bad.project[0].kept.size() == 1);
    CHECK(SerializeSoundStore(bad) == "RAVSOUNDS 1\nassign key=legs lib=%zz\n");
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
    const SoundAssign* step = FindAssign(s.project, "legs.step");
    CHECK(step && step->state == LibState::Library && step->library == "LL" && step->values.size() == 1);
    CHECK(FindAssign(s.project, "legs.lift") == nullptr);
    CHECK(FindAssign(s.project, "legs.slide") == nullptr);
    CHECK(FindAssign(s.project, "legs.pivot") == nullptr);
    CHECK(FindAssign(s.project, "arms.hand_pivot") && FindAssign(s.project, "arms.hand_pivot")->library == "HP");
    const SoundAssign* rel = FindAssign(s.project, "arms.release");
    CHECK(rel && rel->state == LibState::Inherit && rel->library.empty() && rel->values.size() == 1);
    CHECK(FindAssign(s.project, "arms.grab") == nullptr);
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
    if (s.libraries.empty()) AddLib(s, "LL");  // the migrated key names a library; it must exist to play
    EffectiveAssign e = ResolveAssign(s, ItemSounds{}, nullptr, "x", "legs", "step");
    CHECK(e.library == "LL");
}

static void TestRoleMenuKeys()
{
    const std::vector<std::string> k = RoleMenuKeys({"ball", "left_paw", "right_paw", "left_heel", "left_up_leg", "toe", ""});
    CHECK(k.size() == 18 && k[0] == "heel" && k[2] == "toe_end" && k[15] == "head" && k[16] == "ball" && k[17] == "paw");
    CHECK(RoleKeyLabel("toe_end") == "toe end" && RoleKeyLabel("heel") == "heel");
    // The built-in list is exactly the keys events carry (RoleFamilyKey of every role).
    const std::vector<std::string> b = RoleMenuKeys({});
    for (int r = 0; r < static_cast<int>(Role::Count); ++r) {
        const std::string fk = RoleFamilyKey(static_cast<Role>(r));
        CHECK(fk.empty() || std::find(b.begin(), b.end(), fk) != b.end());
    }
    for (const std::string& key : b) {
        bool found = false;
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) found = found || RoleFamilyKey(static_cast<Role>(r)) == key;
        CHECK(found);
    }
}

static void TestStaleLibrary()
{
    // 2026-10-21 (Antho's project): a rule pointing to a deleted Attached library read "(gone)" and Populate placed nothing.
    SoundStore st;
    AddLib(st, "FS");
    AddLib(st, "PL");
    SoundCharacter hero = Hero();
    SetAssignLibraryIn(st.project, {"legs"}, LibState::Library, "PL");
    ItemSounds item;
    SetRulesLibrary(item, {"r1"}, LibState::Library, "c24457ff005942d6");  // deleted
    SetRulesLibrary(item, {"r2"}, LibState::Library, "FS");
    EffectiveAssign e = ResolveAssign(st, item, nullptr, "r1", "legs", "step");
    CHECK(e.library == "PL" && e.level == AssignLevel::Project);  // the inherited one plays
    CHECK(e.stale_library == "c24457ff005942d6" && e.stale_from == "this type");
    e = ResolveAssign(st, item, nullptr, "r2", "legs", "step");
    CHECK(e.library == "FS" && e.stale_library.empty());
    // A deleted Project / Character library falls through to the next level (or to unset).
    SetAssignLibraryIn(hero.assigns, {"legs"}, LibState::Library, "GONE");
    e = ResolveAssign(st, ItemSounds(), &hero, "r9", "legs", "step");
    CHECK(e.library == "PL" && e.stale_library == "GONE");
    SoundStore empty;
    SetAssignLibraryIn(empty.project, {"legs"}, LibState::Library, "GONE");
    e = ResolveAssign(empty, ItemSounds(), nullptr, "r9", "legs", "step");
    CHECK(e.library.empty() && e.level == AssignLevel::Unset && e.stale_library == "GONE");
    // The reset: only the stale ones, and the rest untouched.
    CHECK(ResetStaleRuleLibraries(item, st) == 1);
    CHECK(FindRuleSounds(item, "r1")->library.empty() && FindRuleSounds(item, "r2")->library == "FS");
    CHECK(ResetStaleRuleLibraries(item, st) == 0);
    st.characters.push_back(hero);
    CHECK(ResetStaleAssignLibraries(st) == 1);
    CHECK(st.characters[0].assigns.empty() || st.characters[0].assigns[0].state == LibState::Inherit);
    CHECK(FindAssign(st.project, "legs")->library == "PL");
}

int main()
{
    TestTable();
    TestPartOfBlock();
    TestEventRoleKey();
    TestRoleMenuKeys();
    TestLibraryChain();
    TestStaleLibrary();
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
