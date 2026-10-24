// SPDX-License-Identifier: MIT
//
// Host test of the auto event types' table (src/auto_types.h, story 10-9-0): one row per enum
// value in enum order, unique save words and sound words, each kind's type and each type's
// category known, every row complete, and the categories' types (the Tagging view's rows, the
// sound parts' types) each type once, in enum order. No REAPER.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "auto_types.h"
#include "sound_assign.h"
#include "sound_auto_assign.h"  // story 10-9g: AutoSoundKey

#include <cmath>
#include <cstdio>
#include <limits>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

bool Filled(const char* s)
{
    return s && s[0];
}

void TestOrder()
{
    for (int c = 0; c < kAutoCategoryCount; ++c)
        CHECK(static_cast<int>(AutoCategoryRow(static_cast<AutoCategory>(c)).category) == c);
    for (int t = 0; t < kAutoTypeCount; ++t) CHECK(static_cast<int>(AutoTypeRow(static_cast<AutoType>(t)).type) == t);
    for (int k = 0; k < kAutoKindCount; ++k) CHECK(static_cast<int>(AutoKindRow(static_cast<AutoKind>(k)).kind) == k);
}

void TestUniqueWords()
{
    std::set<std::string> kind_words, sound_words, cat_parts, cat_labels;
    for (int k = 0; k < kAutoKindCount; ++k) {
        const AutoKindInfo& r = AutoKindRow(static_cast<AutoKind>(k));
        CHECK(Filled(r.word));
        CHECK(kind_words.insert(r.word).second);
        AutoKind back = AutoKind::Step;
        CHECK(AutoKindFromWord(r.word, &back) && back == r.kind);
    }
    for (int t = 0; t < kAutoTypeCount; ++t) {
        const AutoTypeInfo& r = AutoTypeRow(static_cast<AutoType>(t));
        CHECK(Filled(r.sound_word));
        CHECK(sound_words.insert(r.sound_word).second);
        AutoType back = AutoType::Step;
        CHECK(AutoTypeFromWord(r.sound_word, &back) && back == r.type);
    }
    for (int c = 0; c < kAutoCategoryCount; ++c) {
        const AutoCategoryInfo& r = AutoCategoryRow(static_cast<AutoCategory>(c));
        CHECK(Filled(r.label) && Filled(r.sound_part));
        CHECK(cat_parts.insert(r.sound_part).second);
        CHECK(cat_labels.insert(r.label).second);
        CHECK(FindBodyPart(r.sound_part) != nullptr);
    }
}

void TestRowsComplete()
{
    for (int t = 0; t < kAutoTypeCount; ++t) {
        const AutoTypeInfo& r = AutoTypeRow(static_cast<AutoType>(t));
        CHECK(Filled(r.label) && Filled(r.undo_label) && Filled(r.hint));
        CHECK(static_cast<int>(r.category) >= 0 && static_cast<int>(r.category) < kAutoCategoryCount);
        CHECK(r.sens[0].field != nullptr);  // the sensitivity moves a constant
        CHECK(r.parts != nullptr);
        CHECK(!AutoKindsOf(r.type).empty());  // a type has a kind
    }
    for (int k = 0; k < kAutoKindCount; ++k) {
        const AutoKindInfo& r = AutoKindRow(static_cast<AutoKind>(k));
        CHECK(static_cast<int>(r.type) >= 0 && static_cast<int>(r.type) < kAutoTypeCount);  // the kind's type exists
        CHECK(Filled(r.marker));
        CHECK(r.role_key != nullptr);
        CHECK(r.part_missing && r.lanes && r.roles);
        // Story 10-9a-2: at least one signature, every used slot named with a source and a match,
        // the used slots first (contiguous), names unique within the kind.
        const int nsig = AutoSignatureCount(r);
        CHECK(nsig >= 1 && nsig <= kAutoMaxSignatures);
        for (int g = 0; g < kAutoMaxSignatures; ++g) {
            const AutoSignature& sg = r.sigs[g];
            if (g < nsig) {
                CHECK(sg.source && sg.match && Filled(sg.name));
                for (int h = 0; h < g; ++h) CHECK(std::strcmp(r.sigs[h].name, sg.name) != 0);
            } else {
                CHECK(!sg.source && !sg.match && !sg.name);
            }
        }
        CHECK(!r.roles('L').empty() && !r.roles('R').empty());
        // The sound key's kind is a kind of the same type with the same marker.
        const AutoKindInfo& key = AutoKindRow(r.sound_key);
        CHECK(key.type == r.type);
        CHECK(std::strcmp(key.marker, r.marker) == 0);
        CHECK(key.sound_key == key.kind);
        CHECK((r.color[0] & 0xFF000000u) == 0 && (r.color[1] & 0xFF000000u) == 0);
    }
}

// The categories' types: each type once, under its category, in enum order; the sound parts list
// the same types. Story 10-9-1: every category has types (Body: Jolt, story 10-9g; Spin, story 10-9f;
// sit and fall, story 10-9b).
void TestCategoryTypes()
{
    std::vector<int> seen(kAutoTypeCount, 0);
    for (int c = 0; c < kAutoCategoryCount; ++c) {
        const AutoCategory          cat = static_cast<AutoCategory>(c);
        const std::vector<AutoType> types = AutoTypesOf(cat);
        CHECK(!types.empty());
        for (size_t i = 0; i < types.size(); ++i) {
            CHECK(AutoCategoryOf(types[i]) == cat);
            if (i) CHECK(static_cast<int>(types[i - 1]) < static_cast<int>(types[i]));
            ++seen[static_cast<size_t>(types[i])];
        }
        const BodyPart* part = FindBodyPart(AutoCategoryRow(cat).sound_part);
        CHECK(part && part->has_category && part->category == cat);
        if (part) {
            std::vector<AutoType> part_types;
            for (const PartType& pt : part->types)
                if (pt.is_auto) part_types.push_back(pt.auto_type);
            CHECK(part_types == types);
        }
    }
    for (int t = 0; t < kAutoTypeCount; ++t) CHECK(seen[static_cast<size_t>(t)] == 1);
    // Each type's kinds, in enum order, all of that type.
    for (int t = 0; t < kAutoTypeCount; ++t) {
        const std::vector<AutoKind> kinds = AutoKindsOf(static_cast<AutoType>(t));
        for (size_t i = 0; i < kinds.size(); ++i) {
            CHECK(AutoTypeOfKind(kinds[i]) == static_cast<AutoType>(t));
            if (i) CHECK(static_cast<int>(kinds[i - 1]) < static_cast<int>(kinds[i]));
        }
    }
}

// A few rows read back through the public functions (the values saved data and the UI rely on).
void TestKnownValues()
{
    CHECK(std::string(AutoKindWord(AutoKind::HandPivotAny)) == "hand_pivot_any");
    CHECK(AutoMarkerName(AutoKind::Step, 'L') == "FS L");
    CHECK(AutoMarkerName(AutoKind::PivotToeLongest, 'R') == "Pivot Toe R");
    CHECK(AutoDefaultColor(AutoKind::Grab, 'R') == (0x1000000u | 0xDDB45Fu));
    CHECK(std::string(AutoTypeLabel(AutoType::HandPivot)) == "Pivot scuff");
    CHECK(std::string(AutoTypeRow(AutoType::HandPivot).undo_label) == "Arms Pivot scuff");
    CHECK(std::string(AutoTypeWord(AutoType::LiftOff)) == "lift");
    CHECK(std::string(AutoCategoryLabel(AutoCategory::Arms)) == "Arms");
    // Story 10-9-1: the categories' labels and sound parts; Body holds Jolt (10-9g) and Spin (10-9f).
    CHECK(std::string(AutoCategoryLabel(AutoCategory::Legs)) == "Legs");
    CHECK(std::string(AutoCategoryRow(AutoCategory::Legs).sound_part) == "legs");
    CHECK(std::string(AutoCategoryRow(AutoCategory::Arms).sound_part) == "arms");
    CHECK(std::string(AutoCategoryLabel(AutoCategory::Body)) == "Body");
    CHECK(std::string(AutoCategoryRow(AutoCategory::Body).sound_part) == "body");
    // Stories 10-9g, 10-9f, 10-9b: Body holds Jolt, Spin, Sit and Fall (sideless), Legs Knee, Arms Elbow.
    CHECK(AutoTypesOf(AutoCategory::Body) ==
          std::vector<AutoType>({AutoType::Jolt, AutoType::Spin, AutoType::Sit, AutoType::Fall}));
    {
        const BodyPart* body = FindBodyPart("body");
        CHECK(body && body->has_category && body->category == AutoCategory::Body && body->label == "Body" &&
              body->types.size() == 5 && body->types[0].word == "jolt" && body->types[1].word == "spin" &&
              body->types[2].word == "sit" && body->types[3].word == "fall" && body->types[4].word == "rules");
    }
    CHECK(AutoCategoryOf(AutoType::Knee) == AutoCategory::Legs && AutoCategoryOf(AutoType::Elbow) == AutoCategory::Arms);
    CHECK(AutoMarkerName(AutoKind::Knee, 'L') == "Knee L" && AutoMarkerName(AutoKind::Elbow, 'R') == "Elbow R" &&
          AutoMarkerName(AutoKind::Sit, 'L') == "Sit" && AutoMarkerName(AutoKind::Fall, 'R') == "Fall");
    CHECK(std::string(AutoKindWord(AutoKind::Knee)) == "knee" && std::string(AutoKindWord(AutoKind::Elbow)) == "elbow" &&
          std::string(AutoKindWord(AutoKind::Sit)) == "sit" && std::string(AutoKindWord(AutoKind::Fall)) == "fall");
    CHECK(std::string(AutoKindSides(AutoKind::Sit)) == "B" && std::string(AutoKindSides(AutoKind::Knee)) == "LR");
    {
        const PhysicsParams d0;
        CHECK(AutoPhysicsParams(AutoType::Knee, 100.0, false).knee_contact_speed == d0.knee_contact_speed * 2.0);
        CHECK(AutoPhysicsParams(AutoType::Elbow, 0.0, false).elbow_contact_speed == d0.elbow_contact_speed / 2.0);
        CHECK(AutoPhysicsParams(AutoType::Sit, 100.0, false).sit_contact_speed == d0.sit_contact_speed * 2.0);
        CHECK(AutoPhysicsParams(AutoType::Fall, 100.0, false).fall_min_speed == d0.fall_min_speed / 2.0);
        CHECK(AutoPhysicsParams(AutoType::Knee, 100.0, false).contact_speed == d0.contact_speed);
    }
    // Story 10-9k: Arms > Jump, Land: markers apart from Legs Jump / Land, replacing the Swing of
    // their hand within their run, the sensitivity dividing the arm speed.
    {
        CHECK(std::string(AutoTypeLabel(AutoType::ArmJump)) == "Jump" &&
              std::string(AutoTypeLabel(AutoType::ArmLand)) == "Land");
        CHECK(std::string(AutoTypeRow(AutoType::ArmJump).undo_label) == "Arms Jump" &&
              std::string(AutoTypeRow(AutoType::ArmLand).undo_label) == "Arms Land");
        CHECK(AutoCategoryOf(AutoType::ArmJump) == AutoCategory::Arms && AutoCategoryOf(AutoType::ArmLand) == AutoCategory::Arms);
        CHECK(AutoMarkerName(AutoKind::ArmJump, 'L') == "Arms Jump L" && AutoMarkerName(AutoKind::ArmLand, 'R') == "Arms Land R");
        CHECK(std::string(AutoKindWord(AutoKind::ArmJump)) == "arm_jump" &&
              std::string(AutoKindWord(AutoKind::ArmLand)) == "arm_land");
        CHECK(std::string(AutoTypeWord(AutoType::ArmJump)) == "arm_jump" &&
              std::string(AutoTypeWord(AutoType::ArmLand)) == "arm_land");
        for (AutoType t : {AutoType::ArmJump, AutoType::ArmLand}) {
            const AutoTypeInfo& r = AutoTypeRow(t);
            CHECK(r.replaces == -1 && r.replaces_in_span == static_cast<int>(AutoType::Swing) && r.replaces_own_side);
            CHECK(!r.start_option && !r.part_split && !r.sideless && !r.shared_sides);
            CHECK(AutoKindsOf(t).size() == 1);
        }
        const PhysicsParams d0;
        CHECK(AutoPhysicsParams(AutoType::ArmJump, 100.0, false).arm_jump_speed == d0.arm_jump_speed / 2.0);
        CHECK(AutoPhysicsParams(AutoType::ArmLand, 0.0, false).arm_land_speed == d0.arm_land_speed * 2.0);
        CHECK(AutoPhysicsParams(AutoType::ArmJump, 100.0, false).swing_speed == d0.swing_speed);
    }
    // Story 10-9o: Arms > Punch, every blow (10-9n's Strike folded in): replacing its hand's Swing
    // over its run and its Arms Land over its span, the sensitivity
    // dividing both its speeds (straight and arc); Stop (punch), Peak (punch_peak), Start.
    {
        CHECK(std::string(AutoTypeLabel(AutoType::Punch)) == "Punch" && kAutoTypeCount == 26);
        CHECK(AutoMarkerName(AutoKind::Punch, 'L') == "Punch L" && AutoMarkerName(AutoKind::PunchPeak, 'R') == "Punch R");
        CHECK(std::string(AutoKindWord(AutoKind::PunchPeak)) == "punch_peak" && AutoTypeOfKind(AutoKind::PunchPeak) == AutoType::Punch);
        CHECK(!AutoKindFromWord("strike", nullptr));
        const AutoTypeInfo& r = AutoTypeRow(AutoType::Punch);
        CHECK(r.replaces == static_cast<int>(AutoType::ArmLand) && r.replaces_in_span == -1 &&
              r.replaces_own_side && r.replaces_span);
        CHECK(r.start_option && !r.part_split && !r.sideless && !r.shared_sides);
        CHECK(AutoKindsOf(AutoType::Punch).size() == 3 && AutoKindRow(AutoKind::PunchPeak).sound_key == AutoKind::Punch);
        CHECK(r.hint && std::string(r.hint).find("Switch") != std::string::npos);
        const PhysicsParams d0;
        CHECK(AutoPhysicsParams(AutoType::Punch, 100.0, false).punch_arc_speed == d0.punch_arc_speed / 2.0);
        CHECK(AutoPhysicsParams(AutoType::Punch, 0.0, false).punch_speed == d0.punch_speed * 2.0);
        CHECK(AutoPhysicsParams(AutoType::Punch, 100.0, false).swing_speed == d0.swing_speed);
    }
    CHECK(AutoKindRow(AutoKind::PivotHeel).degrees && !AutoKindRow(AutoKind::SlideHeel).degrees);
    CHECK(AutoKindRow(AutoKind::SlideToeLongest).scuff == ScuffParts::Longest && !AutoKindRow(AutoKind::SlideToeLongest).scuff_heel);
    const PhysicsParams d;
    const PhysicsParams p = AutoPhysicsParams(AutoType::Pivot, 100.0, false);
    CHECK(p.pivot_min_deg == d.pivot_min_deg / 2.0 && p.pivot_rate_dps == d.pivot_rate_dps / 2.0);
    CHECK(p.contact_speed == d.contact_speed && p.slide_speed == d.slide_speed);
    // Story 10-9a: jump and land, Legs types; the sensitivity divides the minimum flight. Story
    // 10-9a-3: on the foot (no type is sideless), Combined / Separate on lift-off, jump and land.
    for (int t = 0; t < kAutoTypeCount; ++t) {
        const AutoType tt = static_cast<AutoType>(t);
        CHECK(AutoTypeRow(tt).sideless == (tt == AutoType::Jolt || tt == AutoType::Spin || tt == AutoType::Sit ||
                                           tt == AutoType::Fall));  // stories 10-9g, 10-9f, 10-9b
        CHECK(AutoTypeRow(tt).part_split ==
              (tt == AutoType::LiftOff || tt == AutoType::Jump || tt == AutoType::Land || tt == AutoType::Kick));
        // Story 10-9c: Peak / Start on the whooshes. Story 10-9j:
        // and on the scuffs, Start by default.
        const bool whoosh = tt == AutoType::Kick || tt == AutoType::Swing || tt == AutoType::Punch;
        const bool scuff = tt == AutoType::Slide || tt == AutoType::Pivot || tt == AutoType::HandPivot;
        CHECK(AutoTypeRow(tt).start_option == (whoosh || scuff));
        CHECK(AutoStartDefault(tt) == scuff);
        CHECK(AutoSettings().at_start[t] == scuff);
    }
    CHECK(AutoCategoryOf(AutoType::Jump) == AutoCategory::Legs && AutoCategoryOf(AutoType::Land) == AutoCategory::Legs);
    CHECK(std::string(AutoKindWord(AutoKind::Jump)) == "jump" && std::string(AutoKindWord(AutoKind::Land)) == "land");
    CHECK(std::string(AutoTypeWord(AutoType::Jump)) == "jump" && std::string(AutoTypeWord(AutoType::Land)) == "land");
    CHECK(AutoMarkerName(AutoKind::Jump, 'R') == "Jump R" && AutoMarkerName(AutoKind::Land, 'L') == "Land L");
    CHECK(AutoMarkerName(AutoKind::LiftHeel, 'L') == "Lift Heel L" && AutoMarkerName(AutoKind::JumpToe, 'R') == "Jump Toe R" &&
          AutoMarkerName(AutoKind::LandHeel, 'L') == "Land Heel L");
    CHECK(AutoDefaultColor(AutoKind::Jump, 'L') != AutoDefaultColor(AutoKind::Jump, 'R'));
    CHECK(std::string(AutoKindSides(AutoKind::Jump)) == "LR" && std::string(AutoKindSides(AutoKind::Step)) == "LR");
    // The part kinds: their type's, one heel and one toe per part_split type, after its combined kind.
    for (AutoType t : {AutoType::LiftOff, AutoType::Jump, AutoType::Land}) {
        const std::vector<AutoKind> ks = AutoKindsOf(t);
        CHECK(ks.size() == 3);
        if (ks.size() == 3)
            CHECK(std::string(AutoKindRow(ks[0]).role_key).empty() && std::string(AutoKindRow(ks[1]).role_key) == "heel" &&
                  std::string(AutoKindRow(ks[2]).role_key) == "toe" && AutoKindRow(ks[1]).sound_key == ks[1] &&
                  AutoKindRow(ks[2]).sound_key == ks[2]);
    }
    const PhysicsParams j = AutoPhysicsParams(AutoType::Jump, 100.0, false);
    const PhysicsParams l = AutoPhysicsParams(AutoType::Land, 0.0, false);
    CHECK(j.jump_min_flight_s == d.jump_min_flight_s / 2.0 && l.jump_min_flight_s == d.jump_min_flight_s * 2.0);
    CHECK(j.contact_speed == d.contact_speed && !SameAutoAnalysis(j, d));
    // Story 10-9a-2: Jump has two signatures (flight, then push-off), Land one (flight); every
    // other kind one. Story 10-9a-3: and a third, free fall, appended (also on the jump parts).
    // Jump's sensitivity also divides the push-off threshold, Land's does not.
    for (AutoKind jk : {AutoKind::Jump, AutoKind::JumpHeel, AutoKind::JumpToe}) {
        CHECK(AutoSignatureCount(AutoKindRow(jk)) == 3);
        CHECK(std::string(AutoKindRow(jk).sigs[0].name) == "flight" && std::string(AutoKindRow(jk).sigs[1].name) == "push-off" &&
              std::string(AutoKindRow(jk).sigs[2].name) == "free fall");
    }
    CHECK(AutoSignatureCount(AutoKindRow(AutoKind::Land)) == 1);
    CHECK(AutoKindRow(AutoKind::Jump).sigs[0].source == AutoKindRow(AutoKind::Land).sigs[0].source);
    for (int k = 0; k < kAutoKindCount; ++k) {
        const AutoKind kk = static_cast<AutoKind>(k);
        if (kk != AutoKind::Jump && kk != AutoKind::JumpHeel && kk != AutoKind::JumpToe && kk != AutoKind::Slap)
            CHECK(AutoSignatureCount(AutoKindRow(kk)) == 1);
    }
    // Story 10-9l: Slap, Grab's contacts then Hand on body's touches.
    CHECK(AutoSignatureCount(AutoKindRow(AutoKind::Slap)) == 2);
    CHECK(j.jump_impulse_g == d.jump_impulse_g / 2.0 && l.jump_impulse_g == d.jump_impulse_g);
    PhysicsParams q = d;
    q.jump_impulse_g = d.jump_impulse_g / 2.0;
    CHECK(!SameAutoAnalysis(q, d));  // the push-off threshold tells analyses apart
    // Story 10-9g: Jolt, Body, sideless (one block, side B, sound key "jolt:B"), strength in g;
    // the sensitivity divides its threshold, which tells analyses apart.
    CHECK(AutoTypeRow(AutoType::Jolt).category == AutoCategory::Body && AutoTypeRow(AutoType::Jolt).sideless &&
          !AutoTypeRow(AutoType::Jolt).part_split && !AutoTypeRow(AutoType::Jolt).shared_sides);
    CHECK(std::string(AutoTypeWord(AutoType::Jolt)) == "jolt" && std::string(AutoTypeLabel(AutoType::Jolt)) == "Jolt");
    CHECK(AutoKindsOf(AutoType::Jolt) == std::vector<AutoKind>({AutoKind::Jolt}));
    CHECK(std::string(AutoKindRow(AutoKind::Jolt).strength_unit) == "g" && !AutoKindRow(AutoKind::Jolt).degrees);
    CHECK(AutoMarkerName(AutoKind::Jolt, 'L') == "Jolt" && AutoMarkerName(AutoKind::Jolt, 'B') == "Jolt");
    CHECK(AutoSoundKey(AutoKind::Jolt, 'L') == "jolt:B" && AutoSoundKey(AutoKind::Jolt, 'B') == "jolt:B");
    for (int k = 0; k < kAutoKindCount; ++k)
        if (static_cast<AutoKind>(k) != AutoKind::Jolt) CHECK(AutoKindRow(static_cast<AutoKind>(k)).strength_unit == nullptr);
    const PhysicsParams jo = AutoPhysicsParams(AutoType::Jolt, 100.0, false);
    CHECK(jo.jolt_min_g == d.jolt_min_g / 2.0 && jo.contact_speed == d.contact_speed && !SameAutoAnalysis(jo, d));
}

// AutoTypeSeparate: steps, slides and pivots have the mode; story 10-9a-3: and lift-off, jump and
// land (separate_parts).
void TestTypeSeparate()
{
    AutoSettings s;
    for (int t = 0; t < kAutoTypeCount; ++t) CHECK(!AutoTypeSeparate(static_cast<AutoType>(t), s));
    s.separate_steps = true;
    CHECK(AutoTypeSeparate(AutoType::Step, s));
    CHECK(!AutoTypeSeparate(AutoType::LiftOff, s) && !AutoTypeSeparate(AutoType::Slide, s) &&
          !AutoTypeSeparate(AutoType::Pivot, s));
    for (ScuffParts m : {ScuffParts::EachPart, ScuffParts::Longest}) {
        AutoSettings sl;
        sl.slide_parts = m;
        CHECK(AutoTypeSeparate(AutoType::Slide, sl) && !AutoTypeSeparate(AutoType::Pivot, sl) &&
              !AutoTypeSeparate(AutoType::Step, sl));
        AutoSettings pv;
        pv.pivot_parts = m;
        CHECK(AutoTypeSeparate(AutoType::Pivot, pv) && !AutoTypeSeparate(AutoType::Slide, pv) &&
              !AutoTypeSeparate(AutoType::Step, pv));
    }
    AutoSettings all;
    all.separate_steps = true;
    all.hand_pivot_any = true;
    all.slide_parts = ScuffParts::EachPart;
    all.pivot_parts = ScuffParts::Longest;
    for (AutoType t : {AutoType::LiftOff, AutoType::Grab, AutoType::Release, AutoType::HandPivot})
        CHECK(!AutoTypeSeparate(t, all));
    AutoSettings parts;
    for (bool& b : parts.separate_parts) b = true;
    for (int t = 0; t < kAutoTypeCount; ++t)
        CHECK(AutoTypeSeparate(static_cast<AutoType>(t), parts) == AutoTypeRow(static_cast<AutoType>(t)).part_split);
}

// The kinds' highlighted roles: a hand kind its hand, a foot kind its foot's parts in order, a
// jump or land kind (story 10-9a; its parts too) the hips then both feet's parts, whatever the side.
// Story 10-9b: a knee / elbow kind its knee / elbow; sit the hips and chest, fall the hips, spine
// and chest (side B only).
void TestRoles()
{
    for (int k = 0; k < kAutoKindCount; ++k) {
        const AutoKindInfo& r = AutoKindRow(static_cast<AutoKind>(k));
        for (char side : {'L', 'R', 'B'}) {
            std::vector<Role> want;
            if (r.type == AutoType::Sit || r.type == AutoType::Fall) {  // story 10-9b: side B only
                if (side != 'B') continue;
                want = r.type == AutoType::Sit ? std::vector<Role>{Role::Hips, Role::Chest}
                                               : std::vector<Role>{Role::Hips, Role::Spine, Role::Chest};
            } else if (r.type == AutoType::Knee) {
                if (side == 'B') continue;
                want.push_back(side == 'R' ? Role::RightKnee : Role::LeftKnee);
            } else if (r.type == AutoType::Elbow) {
                if (side == 'B') continue;
                want.push_back(side == 'R' ? Role::RightElbow : Role::LeftElbow);
            } else if (r.type == AutoType::Jump || r.type == AutoType::Land || r.type == AutoType::Jolt) {
                if (side == 'B' && r.type != AutoType::Jolt) continue;  // story 10-9g: jolt, sideless
                want.push_back(Role::Hips);
                for (char fs : {'L', 'R'})
                    for (int q = 0; q < kFootPartCount; ++q) want.push_back(FootPartRole(fs, q));
            } else if (r.type == AutoType::Spin) {  // story 10-9f: the hips and the hip line
                want = {Role::Hips, Role::LeftUpLeg, Role::RightUpLeg};
            } else if (side == 'B') {
                continue;  // a sided kind has no B block
            } else if (r.type == AutoType::Slap) {  // story 10-9l: its hand, then its finger
                want = side == 'R' ? std::vector<Role>{Role::RightHand, Role::RightKnuckle, Role::RightFingertip}
                                   : std::vector<Role>{Role::LeftHand, Role::LeftKnuckle, Role::LeftFingertip};
            } else if (r.type == AutoType::Clap) {  // story 10-9d: its hand, then the other
                want = side == 'R' ? std::vector<Role>{Role::RightHand, Role::LeftHand}
                                   : std::vector<Role>{Role::LeftHand, Role::RightHand};
            } else if (AutoCategoryOf(r.type) == AutoCategory::Arms) {
                want.push_back(side == 'R' ? Role::RightHand : Role::LeftHand);
            } else {
                for (int q = 0; q < kFootPartCount; ++q) want.push_back(FootPartRole(side, q));
            }
            CHECK(r.roles(side) == want);
        }
    }
}

// Story 10-9j: a scuff's Peak twin's Start kind ("pivot_heel_peak" -> pivot_heel); any other kind
// itself.
AutoKind StartKindOf(AutoKind k)
{
    const std::string w = AutoKindWord(k), tail = "_peak";
    AutoKind          out = k;
    if (w.size() > tail.size() && w.compare(w.size() - tail.size(), tail.size(), tail) == 0)
        CHECK(AutoKindFromWord(w.substr(0, w.size() - tail.size()), &out));
    return out;
}

// The kinds' flags agree with each other and with their types.
void TestRowInvariants()
{
    for (int k = 0; k < kAutoKindCount; ++k) {
        const AutoKindInfo& r = AutoKindRow(static_cast<AutoKind>(k));
        if (r.scuff_heel) CHECK(r.scuff != ScuffParts::Combined);
        if (r.scuff != ScuffParts::Combined) CHECK(r.type == AutoType::Slide || r.type == AutoType::Pivot);
        CHECK(r.degrees == (r.type == AutoType::Pivot || r.type == AutoType::HandPivot || r.type == AutoType::Spin));
        CHECK((std::string(r.role_key) == "hand") == (AutoCategoryOf(r.type) == AutoCategory::Arms && r.type != AutoType::Elbow));
        // Story 10-9a: a sideless kind has one colour and no role key (no foot, no hand).
        if (AutoTypeRow(r.type).sideless) CHECK(r.color[0] == r.color[1] && std::string(r.role_key).empty());
        for (int j = 0; j < kAutoKindCount; ++j) {
            const AutoKindInfo& o = AutoKindRow(static_cast<AutoKind>(j));
            if (j == k || o.type != r.type || o.scuff != r.scuff) continue;
            const AutoKind rs = StartKindOf(r.kind), os = StartKindOf(o.kind);
            const bool hand_pivot_pair = (rs == AutoKind::HandPivot && os == AutoKind::HandPivotAny) ||
                                         (rs == AutoKind::HandPivotAny && os == AutoKind::HandPivot);
            // Story 10-9o: Punch at its stop and at its peak (a mode) read their own source.
            const bool punch_pair = (r.kind == AutoKind::Punch && o.kind == AutoKind::PunchPeak) ||
                                    (r.kind == AutoKind::PunchPeak && o.kind == AutoKind::Punch);
            // Story 10-9c: a Peak kind and its Start twin read their own source.
            if (hand_pivot_pair || punch_pair || r.at_start != o.at_start) CHECK(r.sigs[0].source != o.sigs[0].source);
            else CHECK(r.sigs[0].source == o.sigs[0].source);
        }
        if (r.scuff != ScuffParts::Combined) {
            AutoKind ck = r.type == AutoType::Slide ? AutoKind::Slide : AutoKind::Pivot;
            if (StartKindOf(r.kind) != r.kind) ck = r.type == AutoType::Slide ? AutoKind::SlidePeak : AutoKind::PivotPeak;
            const AutoKindInfo& combined = AutoKindRow(ck);
            CHECK(combined.scuff == ScuffParts::Combined && combined.type == r.type);
            if (r.scuff == ScuffParts::EachPart) CHECK(r.sigs[0].source != combined.sigs[0].source);
            // Story 10-9j: a Longest part Peak twin times the part that holds longest (its own source).
            else CHECK((r.sigs[0].source == combined.sigs[0].source) == (StartKindOf(r.kind) == r.kind));
        }
    }
    // Story 10-10a: every type's confidence probes at least one of its sensitivity fields (its
    // `conf_probe` names only fields it has); Clap and Hand on body leave their reach out.
    for (int t = 0; t < kAutoTypeCount; ++t) {
        const AutoTypeInfo& r = AutoTypeRow(static_cast<AutoType>(t));
        int                 probed = 0;
        for (int i = 0; i < kAutoSensFields; ++i) {
            const bool named = r.conf_probe == 0 || ((r.conf_probe >> i) & 1u) != 0;
            if (named && r.conf_probe != 0) CHECK(r.sens[i].field != nullptr);
            probed += named && r.sens[i].field != nullptr;
        }
        CHECK(probed >= 1);
        CHECK(r.conf_probe < (1u << kAutoSensFields));
        CHECK(r.conf_scale > 0.0 && r.conf_scale <= 1.0);  // story 10-9h
    }
    CHECK(AutoTypeRow(AutoType::Punch).conf_scale == 0.5);
    CHECK(AutoTypeRow(AutoType::Swing).conf_scale == 1.0);
    CHECK(AutoTypeRow(AutoType::Clap).conf_probe == 1u && AutoTypeRow(AutoType::Clap).sens[1].field == &PhysicsParams::clap_reach);
    CHECK(AutoTypeRow(AutoType::HandOnBody).conf_probe == 1u &&
          AutoTypeRow(AutoType::HandOnBody).sens[1].field == &PhysicsParams::body_reach);
}

// Story 10-10b: every type's strength levels (the row follows the type: a static_assert checks
// the order and 0 < medium < hard).
void TestStrengthLevels()
{
    for (int t = 0; t < kAutoTypeCount; ++t) {
        const AutoType          ty = static_cast<AutoType>(t);
        const AutoStrengthCuts& c = AutoStrengthRow(ty);
        CHECK(c.type == ty);
        CHECK(AutoStrengthLevel(ty, c.medium * 0.99) == kStrengthSoft);
        CHECK(AutoStrengthLevel(ty, c.medium) == kStrengthMedium);
        CHECK(AutoStrengthLevel(ty, c.hard * 0.999) == kStrengthMedium);
        CHECK(AutoStrengthLevel(ty, c.hard) == kStrengthHard);
        CHECK(AutoStrengthLevel(ty, 0.0) == 0 && AutoStrengthLevel(ty, -1.0) == 0 && AutoStrengthLevel(ty, std::nan("")) == 0);
        CHECK(AutoStrengthLevel(ty, std::numeric_limits<double>::infinity()) == 0);
    }
    // Steps, for scale (measured on the fixtures): a climb Soft, a walk Medium, a run Hard.
    CHECK(AutoStrengthLevel(AutoType::Step, 0.9) == kStrengthSoft);
    CHECK(AutoStrengthLevel(AutoType::Step, 2.6) == kStrengthMedium);
    CHECK(AutoStrengthLevel(AutoType::Step, 8.0) == kStrengthHard);
    CHECK(std::string(StrengthLevelName(kStrengthSoft)) == "Soft" && std::string(StrengthLevelName(kStrengthMedium)) == "Medium" &&
          std::string(StrengthLevelName(kStrengthHard)) == "Hard" && std::string(StrengthLevelName(0)) == "" &&
          std::string(StrengthLevelName(4)) == "");
}

}  // namespace

int main()
{
    TestOrder();
    TestStrengthLevels();
    TestUniqueWords();
    TestRowsComplete();
    TestCategoryTypes();
    TestKnownValues();
    TestTypeSeparate();
    TestRoles();
    TestRowInvariants();
    if (g_fails) {
        std::printf("auto_types_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("auto_types_test: all passed\n");
    return 0;
}
