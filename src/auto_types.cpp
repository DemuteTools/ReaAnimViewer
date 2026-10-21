// SPDX-License-Identifier: MIT
//
// See auto_types.h. The table of the auto event types.
//
// How to add an auto event type (story 10-9-0). Scope: a type with no mode, in an existing
// category, reading existing PhysicsKinds:
//   1. auto_detect.h: append its AutoType and its AutoKind values at the end of their enums, and
//      raise kAutoTypeCount / kAutoKindCount.
//   2. Here: append one row per enum value to kTypeRows / kKindRows (the rows follow enum order:
//      static_assert below; every type row needs a hint), and declare its functions in the block
//      below.
//   3. Its functions (event source, match, part missing, lanes, roles, type parts) in its
//      category's file (auto_types_<category>.cpp), reusing the shared ones where they fit.
//   4. CMakeLists.txt: a new .cpp next to auto_types.cpp; tests/CMakeLists.txt: add it to
//      RAV_AUTO_SRCS (one edit); tests/auto_types_test.cpp: its known values.
// Within that scope no switch is extended: the Tagging view, the sounds and the detection read
// the rows. Beyond it:
//   - a new category: also its AutoCategory value, kAutoCategoryCount, a kCategoryRows row and its
//     BodyPart in sound_assign.cpp BuildParts. A category may start with no type (Body, story
//     10-9-1): the Tagging view's + does not offer it until it has one (cat_offered). Its first
//     type also brings its auto_types_<category>.cpp (CMakeLists.txt + RAV_AUTO_SRCS) and updates
//     the Body-is-empty checks in tests/auto_types_test.cpp and tests/sound_assign_test.cpp;
//   - a new PhysicsKind: kPhysicsKindCount and a kPhysicsKindRows row (motion_physics.cpp);
//   - a type with a mode: code in KindsOf, AutoTypeSeparate, AutoPending, ReadAutoSettings /
//     ApplyAutoSettings (auto_detect.cpp) and its widget in tagging_view_ui.cpp. Except a heel /
//     toe split (story 10-9a-3): `part_split` on the type row, a combined kind (no role key) and a
//     heel and a toe kind (role keys "heel", "toe") appended; the Combined / Separate widget, the
//     settings and the remap of edits follow the row.
// Adding a signature to a kind (story 10-9a-2): a new way to recognise the same event, for an
// edge case the kind's other signatures miss. Append {name, event source, match} to the kind's
// `sigs` (at most kAutoMaxSignatures, contiguous); its source and match live in the category's
// file, its constants in PhysicsParams (body units, seconds, g), and a constant the type's
// sensitivity moves goes in the type row's `sens` (SameAutoAnalysis then compares it). All
// signatures run; per block an event within kAutoMergeS of one an earlier signature kept is
// dropped, so a signature appended at the end never moves an existing marker's time; its new
// events can still remove existing markers through the Land / Jump replacement of steps and
// lift-offs (kAutoReplaceS). Signatures are
// not saved: no save word, no RAV_RULES change. The inspector names the one that found an event
// ("Found by") when a kind has more than one; tests/detection_eval.cpp and
// src/detection_measure.cpp print its events on a line of their own.
// A type may also live in a file of its own (auto_types_jolt.cpp, story 10-9g: one type, its
// physics and its row functions); a strength in a unit other than leg/s or degrees takes the kind
// row's `strength_unit` ("g"), which the inspector shows (and its speed is 0).
// The save words (kind `word`, type `sound_word`) are saved data: never rename one.

#include "auto_types.h"

#include <algorithm>
#include <cmath>

namespace rav {

// ---- The rows' functions, by file --------------------------------------------------------------

// auto_types_legs.cpp
std::vector<PhysicsEvent> AutoSourceFoot(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceFootParts(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoPartsFootAny(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
void AutoPartsFootSplit(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
void AutoPartsFootBoth(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMatchStep(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchHeel(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchToe(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchLift(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchSlide(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchPivot(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchSlideHeel(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchSlideToe(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchPivotHeel(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchPivotToe(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchSlideHeelLongest(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchSlideToeLongest(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchPivotHeelLongest(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchPivotToeLongest(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingFootAny(char side, const std::vector<int>& role_to_bone);
bool AutoMissingHeel(char side, const std::vector<int>& role_to_bone);
bool AutoMissingToe(char side, const std::vector<int>& role_to_bone);
bool AutoMissingHeelOrToe(char side, const std::vector<int>& role_to_bone);
void AutoLanesFootContact(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesFootSlide(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesHeel(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesToe(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesSlideHeel(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesSlideToe(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesPivot(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
std::vector<Role> AutoRolesFoot(char side);
// Story 10-9a: jump and land.
std::vector<PhysicsEvent> AutoSourceJump(const PhysicsAnalysis& a, const PhysicsParams& p);
// Story 10-9a-2: the push-off take-offs (Jump's second signature).
std::vector<PhysicsEvent> AutoSourceJumpPushOff(const PhysicsAnalysis& a, const PhysicsParams& p);
// Story 10-9a-3: the free-fall take-offs (Jump's third signature).
std::vector<PhysicsEvent> AutoSourceJumpFreeFall(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoPartsJump(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMatchJump(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchLand(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingJump(char side, const std::vector<int>& role_to_bone);
// Story 10-9a-3: lift-off, jump and land by part (the heel; the toe = the ball or the tip).
bool AutoMatchLiftHeel(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchLiftToe(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchJumpHeel(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchJumpToe(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchLandHeel(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchLandToe(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingJumpHeel(char side, const std::vector<int>& role_to_bone);
bool AutoMissingJumpToe(char side, const std::vector<int>& role_to_bone);
void AutoLanesJump(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesLand(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
std::vector<Role> AutoRolesJump(char side);

// auto_types_arms.cpp
std::vector<PhysicsEvent> AutoSourceHand(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceHandAny(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoPartsHands(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMatchGrab(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchRelease(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchHandPivot(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingHand(char side, const std::vector<int>& role_to_bone);
void AutoLanesHandContact(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesHandPivot(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
std::vector<Role> AutoRolesHand(char side);

// auto_types_jolt.cpp (story 10-9g)
std::vector<PhysicsEvent> AutoSourceJolt(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoPartsJolt(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMatchJolt(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingJolt(char side, const std::vector<int>& role_to_bone);
void AutoLanesJolt(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
std::vector<Role> AutoRolesJolt(char side);

namespace {

using C = AutoCategory;
using T = AutoType;
using K = AutoKind;
using S = AutoSensDir;
using P = PhysicsParams;

// ---- Categories --------------------------------------------------------------------------------

constexpr AutoCategoryInfo kCategoryRows[] = {
    {C::Legs, "Legs", "legs"},
    {C::Arms, "Arms", "arms"},
    {C::Body, "Body", "body"},
};

// ---- Types -------------------------------------------------------------------------------------

constexpr AutoTypeInfo kTypeRows[] = {
    {T::Step, "Step", "Step", C::Legs,
     "A marker where the foot lands: FS L / FS R (combined: the first part to touch),\n"
     "or Heel and Toe separately (one marker per part).",
     "step", {{&P::contact_speed, S::Times}, {}}, &AutoPartsFootSplit},
    {T::LiftOff, "Lift-off", "Lift-off", C::Legs,
     "A marker where the foot leaves the ground: Lift L / Lift R (combined: the last part to leave),\n"
     "or Lift Heel L / Lift Toe L... (separate: on the part that leaves last).",
     "lift", {{&P::contact_speed, S::Times}, {}}, &AutoPartsFootSplit, false, true},
    {T::Slide, "Slide scuff", "Slide scuff", C::Legs,
     "A marker where a planted foot slides: Slide L / Slide R (combined),\n"
     "or Slide Heel L / Slide Toe L... (separate).",
     "slide", {{&P::slide_speed, S::Divide}, {}}, &AutoPartsFootSplit},
    {T::Pivot, "Pivot scuff", "Pivot scuff", C::Legs,
     "A marker where a planted foot turns on its ball or heel: Pivot L / Pivot R (combined),\n"
     "or Pivot Heel L / Pivot Toe L... (separate).",
     "pivot", {{&P::pivot_min_deg, S::Divide}, {&P::pivot_rate_dps, S::Divide}}, &AutoPartsFootBoth},
    {T::Grab, "Grab", "Grab", C::Arms, "A marker where the hand lands on something: Grab L / Grab R.", "grab",
     {{&P::hand_contact_speed, S::Times}, {}}, &AutoPartsHands},
    {T::Release, "Release", "Release", C::Arms, "A marker where the hand leaves its support: Release L / Release R.",
     "release", {{&P::hand_contact_speed, S::Times}, {}}, &AutoPartsHands},
    // Story 10-8e: the hand's Pivot scuff is named with its category in undo names.
    {T::HandPivot, "Pivot scuff", "Arms Pivot scuff", C::Arms,
     "A marker where a planted hand turns: Hand Pivot L / Hand Pivot R.", "hand_pivot",
     {{&P::hand_pivot_min_deg, S::Divide}, {&P::hand_pivot_rate_dps, S::Divide}}, &AutoPartsHands},
    // Story 10-9a: higher sensitivity = shorter flights count; story 10-9a-2: and a softer
    // push-off (jump_impulse_g) counts. Story 10-9a-3: on the foot that leaves last / touches
    // first (L / R), Combined / Separate.
    {T::Jump, "Jump", "Jump", C::Legs,
     "A marker where a jump takes off, on the last foot to leave (Jump L / Jump R):\n"
     "both feet off while the body falls freely, right after a hard push off the ground,\n"
     "or into a free fall (a fall on the back). Separate: Jump Heel L / Jump Toe L...\n"
     "With Lift-off ticked, it replaces the lifts there.",
     "jump", {{&P::jump_min_flight_s, S::Divide}, {&P::jump_impulse_g, S::Divide}}, &AutoPartsJump, false, true, true},
    {T::Land, "Land", "Land", C::Legs,
     "A marker where a jump lands, on the first foot to touch, at its step (Land L / Land R).\n"
     "Separate: Land Heel L / Land Toe L...\n"
     "With Step ticked, it replaces the steps there.",
     "land", {{&P::jump_min_flight_s, S::Divide}, {}}, &AutoPartsJump, false, true, true},
    // Story 10-9g: the gear jolt, Body, sideless; higher sensitivity = softer impacts count.
    {T::Jolt, "Jolt", "Jolt", C::Body,
     "A marker where an impact shakes the gear and cloth (Jolt): a step or a jump's landing\n"
     "where the body's acceleration peaks above the sensitivity's threshold.\n"
     "Raise the sensitivity for softer steps, lower it for only the hard impacts.",
     "jolt", {{&P::jolt_min_g, S::Divide}, {}}, &AutoPartsJolt, true, false, false, true},
};

// ---- Kinds -------------------------------------------------------------------------------------
// Default colours: left in a cool hue, right in a warm one (the Footsteps presets' blue and orange
// for steps); a separate toe block a shade lighter than its heel's.

constexpr ScuffParts kNo = ScuffParts::Combined, kEach = ScuffParts::EachPart, kLong = ScuffParts::Longest;

constexpr AutoKindInfo kKindRows[] = {
    // kind, word, type, marker, colours L/R, scuff mode, scuff heel, degrees, role key, sound key,
    // signatures ({name, event source, match}...), part missing, lanes, roles
    {K::Step, "step", T::Step, "FS", {0x5F9EDD, 0xDD9E5F}, kNo, false, false, "", K::Step,
     {{"contact", &AutoSourceFoot, &AutoMatchStep}}, &AutoMissingFootAny, &AutoLanesFootContact, &AutoRolesFoot},
    {K::Heel, "heel", T::Step, "Heel", {0x5F9EDD, 0xDD9E5F}, kNo, false, false, "heel", K::Heel,
     {{"contact", &AutoSourceFoot, &AutoMatchHeel}}, &AutoMissingHeel, &AutoLanesHeel, &AutoRolesFoot},
    {K::Toe, "toe", T::Step, "Toe", {0x5FDDDD, 0xDDDD5F}, kNo, false, false, "toe", K::Toe,
     {{"contact", &AutoSourceFoot, &AutoMatchToe}}, &AutoMissingToe, &AutoLanesToe, &AutoRolesFoot},
    {K::Lift, "lift", T::LiftOff, "Lift", {0x5F5FDD, 0xDD5F5F}, kNo, false, false, "", K::Lift,
     {{"contact", &AutoSourceFoot, &AutoMatchLift}}, &AutoMissingFootAny, &AutoLanesFootContact, &AutoRolesFoot},
    {K::Slide, "slide", T::Slide, "Slide", {0x5FDD5F, 0xDD5FDD}, kNo, false, false, "", K::Slide,
     {{"slide", &AutoSourceFoot, &AutoMatchSlide}}, &AutoMissingFootAny, &AutoLanesFootSlide, &AutoRolesFoot},
    {K::Pivot, "pivot", T::Pivot, "Pivot", {0x9E5FDD, 0xDD5F9E}, kNo, false, true, "", K::Pivot,
     {{"pivot", &AutoSourceFoot, &AutoMatchPivot}}, &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot},
    {K::Grab, "grab", T::Grab, "Grab", {0x5FDD9E, 0xDDB45F}, kNo, false, false, "hand", K::Grab,
     {{"contact", &AutoSourceHand, &AutoMatchGrab}}, &AutoMissingHand, &AutoLanesHandContact, &AutoRolesHand},
    {K::Release, "release", T::Release, "Release", {0x9EB4DD, 0xDD9E9E}, kNo, false, false, "hand", K::Release,
     {{"contact", &AutoSourceHand, &AutoMatchRelease}}, &AutoMissingHand, &AutoLanesHandContact, &AutoRolesHand},
    {K::HandPivot, "hand_pivot", T::HandPivot, "Hand Pivot", {0xA8DD3C, 0xB4643C}, kNo, false, true, "hand",
     K::HandPivot,
     {{"pivot", &AutoSourceHand, &AutoMatchHandPivot}}, &AutoMissingHand, &AutoLanesHandPivot, &AutoRolesHand},
    {K::HandPivotAny, "hand_pivot_any", T::HandPivot, "Hand Pivot", {0xA8DD3C, 0xB4643C}, kNo, false, true, "hand",
     K::HandPivot,
     {{"pivot", &AutoSourceHandAny, &AutoMatchHandPivot}}, &AutoMissingHand, &AutoLanesHandPivot, &AutoRolesHand},
    {K::SlideHeel, "slide_heel", T::Slide, "Slide Heel", {0x5FDD5F, 0xDD5FDD}, kEach, true, false, "heel", K::SlideHeel,
     {{"slide", &AutoSourceFootParts, &AutoMatchSlideHeel}}, &AutoMissingHeel, &AutoLanesSlideHeel, &AutoRolesFoot},
    {K::SlideToe, "slide_toe", T::Slide, "Slide Toe", {0x9FEE9F, 0xEE9FEE}, kEach, false, false, "toe", K::SlideToe,
     {{"slide", &AutoSourceFootParts, &AutoMatchSlideToe}}, &AutoMissingToe, &AutoLanesSlideToe, &AutoRolesFoot},
    {K::PivotHeel, "pivot_heel", T::Pivot, "Pivot Heel", {0x9E5FDD, 0xDD5F9E}, kEach, true, true, "heel", K::PivotHeel,
     {{"pivot", &AutoSourceFootParts, &AutoMatchPivotHeel}}, &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot},
    {K::PivotToe, "pivot_toe", T::Pivot, "Pivot Toe", {0xC49FEE, 0xEE9FC4}, kEach, false, true, "toe", K::PivotToe,
     {{"pivot", &AutoSourceFootParts, &AutoMatchPivotToe}}, &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot},
    {K::SlideHeelLongest, "slide_heel_longest", T::Slide, "Slide Heel", {0x5FDD5F, 0xDD5FDD}, kLong, true, false, "heel",
     K::SlideHeel,
     {{"slide", &AutoSourceFoot, &AutoMatchSlideHeelLongest}}, &AutoMissingHeel, &AutoLanesSlideHeel, &AutoRolesFoot},
    {K::SlideToeLongest, "slide_toe_longest", T::Slide, "Slide Toe", {0x9FEE9F, 0xEE9FEE}, kLong, false, false, "toe",
     K::SlideToe,
     {{"slide", &AutoSourceFoot, &AutoMatchSlideToeLongest}}, &AutoMissingToe, &AutoLanesSlideToe, &AutoRolesFoot},
    {K::PivotHeelLongest, "pivot_heel_longest", T::Pivot, "Pivot Heel", {0x9E5FDD, 0xDD5F9E}, kLong, true, true, "heel",
     K::PivotHeel,
     {{"pivot", &AutoSourceFoot, &AutoMatchPivotHeelLongest}}, &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot},
    {K::PivotToeLongest, "pivot_toe_longest", T::Pivot, "Pivot Toe", {0xC49FEE, 0xEE9FC4}, kLong, false, true, "toe",
     K::PivotToe,
     {{"pivot", &AutoSourceFoot, &AutoMatchPivotToeLongest}}, &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot},
    // Story 10-9a: jump and land; story 10-9a-3: L / R colours, and the free-fall signature
    // (appended: it never moves an existing marker).
    {K::Jump, "jump", T::Jump, "Jump", {0x3CC8B4, 0xC8B43C}, kNo, false, false, "", K::Jump,
     {{"flight", &AutoSourceJump, &AutoMatchJump}, {"push-off", &AutoSourceJumpPushOff, &AutoMatchJump},
      {"free fall", &AutoSourceJumpFreeFall, &AutoMatchJump}},
     &AutoMissingJump, &AutoLanesJump, &AutoRolesJump},
    {K::Land, "land", T::Land, "Land", {0x1E7850, 0x8C1E50}, kNo, false, false, "", K::Land,
     {{"flight", &AutoSourceJump, &AutoMatchLand}}, &AutoMissingJump, &AutoLanesLand, &AutoRolesJump},
    // Story 10-9a-3: lift-off, jump and land by part (Separate): the heel in the type's colour, the
    // toe (ball or tip) a shade lighter.
    {K::LiftHeel, "lift_heel", T::LiftOff, "Lift Heel", {0x5F5FDD, 0xDD5F5F}, kNo, false, false, "heel", K::LiftHeel,
     {{"contact", &AutoSourceFoot, &AutoMatchLiftHeel}}, &AutoMissingHeel, &AutoLanesHeel, &AutoRolesFoot},
    {K::LiftToe, "lift_toe", T::LiftOff, "Lift Toe", {0x9F9FEE, 0xEE9F9F}, kNo, false, false, "toe", K::LiftToe,
     {{"contact", &AutoSourceFoot, &AutoMatchLiftToe}}, &AutoMissingToe, &AutoLanesToe, &AutoRolesFoot},
    {K::JumpHeel, "jump_heel", T::Jump, "Jump Heel", {0x3CC8B4, 0xC8B43C}, kNo, false, false, "heel", K::JumpHeel,
     {{"flight", &AutoSourceJump, &AutoMatchJumpHeel}, {"push-off", &AutoSourceJumpPushOff, &AutoMatchJumpHeel},
      {"free fall", &AutoSourceJumpFreeFall, &AutoMatchJumpHeel}},
     &AutoMissingJumpHeel, &AutoLanesJump, &AutoRolesJump},
    {K::JumpToe, "jump_toe", T::Jump, "Jump Toe", {0x8ADED2, 0xDED28A}, kNo, false, false, "toe", K::JumpToe,
     {{"flight", &AutoSourceJump, &AutoMatchJumpToe}, {"push-off", &AutoSourceJumpPushOff, &AutoMatchJumpToe},
      {"free fall", &AutoSourceJumpFreeFall, &AutoMatchJumpToe}},
     &AutoMissingJumpToe, &AutoLanesJump, &AutoRolesJump},
    {K::LandHeel, "land_heel", T::Land, "Land Heel", {0x1E7850, 0x8C1E50}, kNo, false, false, "heel", K::LandHeel,
     {{"flight", &AutoSourceJump, &AutoMatchLandHeel}}, &AutoMissingJumpHeel, &AutoLanesLand, &AutoRolesJump},
    {K::LandToe, "land_toe", T::Land, "Land Toe", {0x50A07D, 0xB45078}, kNo, false, false, "toe", K::LandToe,
     {{"flight", &AutoSourceJump, &AutoMatchLandToe}}, &AutoMissingJumpToe, &AutoLanesLand, &AutoRolesJump},
    // Story 10-9g: the gear jolt (one colour, no role key), strength in g.
    {K::Jolt, "jolt", T::Jolt, "Jolt", {0xC88C3C, 0xC88C3C}, kNo, false, false, "", K::Jolt,
     {{"impact", &AutoSourceJolt, &AutoMatchJolt}}, &AutoMissingJolt, &AutoLanesJolt, &AutoRolesJolt, "g"},
};

// One row per enum value, in enum order.
constexpr bool CategoryRowsInOrder()
{
    for (int i = 0; i < kAutoCategoryCount; ++i)
        if (static_cast<int>(kCategoryRows[i].category) != i) return false;
    return true;
}
constexpr bool TypeRowsInOrder()
{
    for (int i = 0; i < kAutoTypeCount; ++i)
        if (static_cast<int>(kTypeRows[i].type) != i) return false;
    return true;
}
constexpr bool KindRowsInOrder()
{
    for (int i = 0; i < kAutoKindCount; ++i)
        if (static_cast<int>(kKindRows[i].kind) != i) return false;
    return true;
}
static_assert(sizeof(kCategoryRows) / sizeof(kCategoryRows[0]) == kAutoCategoryCount, "one row per AutoCategory");
static_assert(sizeof(kTypeRows) / sizeof(kTypeRows[0]) == kAutoTypeCount, "one row per AutoType");
static_assert(sizeof(kKindRows) / sizeof(kKindRows[0]) == kAutoKindCount, "one row per AutoKind");
// Story 10-9a-2: every kind has a first signature; the used slots are contiguous and named.
constexpr bool KindSignaturesValid()
{
    for (const AutoKindInfo& r : kKindRows) {
        if (!r.sigs[0].source) return false;
        bool ended = false;
        for (const AutoSignature& g : r.sigs) {
            const bool used = g.source != nullptr;
            if (used != (g.match != nullptr) || used != (g.name != nullptr) || (used && ended)) return false;
            ended = ended || !used;
        }
    }
    return true;
}
static_assert(CategoryRowsInOrder(), "category rows follow the AutoCategory order");
static_assert(KindSignaturesValid(), "every kind: a first signature, used slots contiguous and named");
static_assert(TypeRowsInOrder(), "type rows follow the AutoType order");
static_assert(KindRowsInOrder(), "kind rows follow the AutoKind order");

int RowIndex(int i, int count)
{
    return (i >= 0 && i < count) ? i : 0;
}

}  // namespace

const AutoCategoryInfo& AutoCategoryRow(AutoCategory c)
{
    return kCategoryRows[RowIndex(static_cast<int>(c), kAutoCategoryCount)];
}

const AutoTypeInfo& AutoTypeRow(AutoType t)
{
    return kTypeRows[RowIndex(static_cast<int>(t), kAutoTypeCount)];
}

const AutoKindInfo& AutoKindRow(AutoKind k)
{
    return kKindRows[RowIndex(static_cast<int>(k), kAutoKindCount)];
}

int AutoSignatureCount(const AutoKindInfo& row)
{
    int n = 0;
    while (n < kAutoMaxSignatures && row.sigs[n].source && row.sigs[n].match) ++n;
    return n;
}

std::vector<AutoType> AutoTypesOf(AutoCategory c)
{
    std::vector<AutoType> out;
    for (const AutoTypeInfo& r : kTypeRows)
        if (r.category == c) out.push_back(r.type);
    return out;
}

std::vector<AutoKind> AutoKindsOf(AutoType t)
{
    std::vector<AutoKind> out;
    for (const AutoKindInfo& r : kKindRows)
        if (r.type == t) out.push_back(r.kind);
    return out;
}

// ---- Helpers for the per-category files --------------------------------------------------------

bool AutoHasRole(const std::vector<int>& role_to_bone, Role r)
{
    const size_t i = static_cast<size_t>(r);
    return i < role_to_bone.size() && role_to_bone[i] >= 0;
}

void AutoPushOnce(std::vector<std::string>& out, const std::string& s)
{
    if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
}

std::vector<double> AutoLaneWindow(const std::vector<double>& v, size_t first, size_t n, bool absval)
{
    std::vector<double> out(n, 0.0);
    for (size_t i = 0; i < n && first + i < v.size(); ++i) out[i] = absval ? std::fabs(v[first + i]) : v[first + i];
    return out;
}

void AutoPartLane(const AutoLaneArgs& args, std::vector<AutoLane>& out, const PartTrack& pt, const char* name,
                  bool horizontal, double thr)
{
    AutoLane l;
    l.label = std::string(name) + " " + std::string(1, args.side) + "  " + (horizontal ? "hspeed" : "speed");
    l.unit = "leg/s";
    l.curve = AutoLaneWindow(horizontal ? pt.hspeed : pt.speed, args.first, args.n, false);
    l.contact.assign(args.n, 0);
    for (size_t i = 0; i < args.n && args.first + i < pt.contact.size(); ++i) l.contact[i] = pt.contact[args.first + i];
    l.threshold = thr;
    l.above = horizontal;  // a slide fires above slide_speed, a contact below contact_speed
    out.push_back(std::move(l));
}

}  // namespace rav
