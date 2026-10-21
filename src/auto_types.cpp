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
//      below. Story 10-10b: also one kStrengthRows row (its Soft / Medium / Hard cuts in its
//      strength unit), measured with tools/bench (the bench's STRENGTH block) on the fixtures.
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
//   - a type whose events stand for another type's near them (Land the steps, Jump the lift-offs,
//     story 10-9e Tap its foot's steps): `replaces` (and `replaces_own_side`) on its row.
//   - a Peak / Start choice (story 10-9c):
//     `start_option` on the type row and, per kind, a twin appended with the other `at_start` (same
//     marker, colour, role key and sound key), in the order of the kinds it pairs with; the type's
//     first kind's flavour is the default (story 10-9j: the scuffs' kinds say `at_start`, their
//     Peak twins do not, so Start stays their default); the widget, the settings and the block
//     kept across a switch follow the row. `no_sens_raise` keeps Detect from raising a silent block
//     of a type most clips do not have.
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
// Confidence (story 10-10a): a new type gets its events' confidence (amber when doubtful) from its
// row's `sens` fields, with no code (auto_detect.h RateAutoEvents: the block run again with them
// stricter); a field that is a tolerance rather than a margin (a reach) is left out with
// `conf_probe` (Clap, Hand on body).
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
// Story 10-9b: the knee touching the ground.
std::vector<PhysicsEvent> AutoSourceBody(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoPartsKnee(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMatchKnee(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingKnee(char side, const std::vector<int>& role_to_bone);
void AutoLanesKnee(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
std::vector<Role> AutoRolesKnee(char side);

// foot_taps.cpp (story 10-9e)
std::vector<PhysicsEvent> AutoSourceTap(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceDrag(const PhysicsAnalysis& a, const PhysicsParams& p);
bool AutoMatchTap(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchDrag(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
void AutoLanesDrag(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);

// heel_taps.cpp (story 10-9i)
std::vector<PhysicsEvent> AutoSourceHeelTap(const PhysicsAnalysis& a, const PhysicsParams& p);
bool AutoMatchHeelTap(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
void AutoLanesHeelTap(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);

// scuff_peak.cpp (story 10-9j: the scuffs' Peak twins)
std::vector<PhysicsEvent> AutoSourceFootPeak(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceFootLongestPeak(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceFootPartsPeak(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceHandPeak(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceHandAnyPeak(const PhysicsAnalysis& a, const PhysicsParams& p);

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
// Story 10-9b: the elbow touching the ground (its source: AutoSourceBody).
void AutoPartsElbow(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMatchElbow(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingElbow(char side, const std::vector<int>& role_to_bone);
void AutoLanesElbow(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
std::vector<Role> AutoRolesElbow(char side);

// auto_types_body.cpp (story 10-9b): sit and fall (their source: AutoSourceBody).
void AutoPartsSit(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
void AutoPartsFall(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMatchSit(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchFall(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingSit(char side, const std::vector<int>& role_to_bone);
bool AutoMissingFall(char side, const std::vector<int>& role_to_bone);
void AutoLanesSit(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesFall(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
std::vector<Role> AutoRolesSit(char side);
std::vector<Role> AutoRolesFall(char side);

// auto_types_jolt.cpp (story 10-9g)
std::vector<PhysicsEvent> AutoSourceJolt(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoPartsJolt(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMatchJolt(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingJolt(char side, const std::vector<int>& role_to_bone);
void AutoLanesJolt(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
std::vector<Role> AutoRolesJolt(char side);
// auto_types_spin.cpp (story 10-9f)
std::vector<PhysicsEvent> AutoSourceSpin(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoPartsSpin(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMatchSpin(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchSpinEnd(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingSpin(char side, const std::vector<int>& role_to_bone);
void AutoLanesSpin(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
std::vector<Role> AutoRolesSpin(char side);

// hand_touch.cpp (story 10-9d: clap and hand on body)
std::vector<PhysicsEvent> AutoSourceClap(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceHandOnBody(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoPartsClap(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
void AutoPartsHandOnBody(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMatchClap(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchHandOnBody(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingClap(char side, const std::vector<int>& role_to_bone);
bool AutoMissingHandOnBody(char side, const std::vector<int>& role_to_bone);
void AutoLanesClap(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesHandOnBody(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
std::vector<Role> AutoRolesClap(char side);
std::vector<Role> AutoRolesHandOnBody(char side);

// auto_types_slap.cpp (story 10-9l)
std::vector<PhysicsEvent> AutoSourceSlapContact(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceSlapBody(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoPartsSlap(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMatchSlapContact(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchSlapBody(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingSlap(char side, const std::vector<int>& role_to_bone);
void AutoLanesSlap(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
std::vector<Role> AutoRolesSlap(char side);

// auto_types_whoosh.cpp (story 10-9c: kick, swing, punch)
std::vector<PhysicsEvent> AutoSourceWhoosh(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceWhooshStart(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoPartsKick(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
void AutoPartsWhooshHand(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMatchKick(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchKickHeel(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchKickToe(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchSwing(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMatchPunch(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
bool AutoMissingKick(char side, const std::vector<int>& role_to_bone);
bool AutoMissingKickHeel(char side, const std::vector<int>& role_to_bone);
bool AutoMissingKickToe(char side, const std::vector<int>& role_to_bone);
bool AutoMissingWhooshHand(char side, const std::vector<int>& role_to_bone);
void AutoLanesKick(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesSwing(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesPunch(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
std::vector<Role> AutoRolesKick(char side);
std::vector<Role> AutoRolesWhooshHand(char side);

// auto_types_arm_jump.cpp (story 10-9k: the arms of a jump)
std::vector<PhysicsEvent> AutoSourceArmJump(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceArmLand(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoPartsArmJump(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMissingArmJump(char side, const std::vector<int>& role_to_bone);
void AutoLanesArmJump(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
void AutoLanesArmLand(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);

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
     "or Slide Heel L / Slide Toe L... (separate). At the slide's start, or where it rubs\n"
     "fastest (Peak).",
     "slide", {{&P::slide_speed, S::Divide}, {}}, &AutoPartsFootSplit, false, false, false, false, true},
    {T::Pivot, "Pivot scuff", "Pivot scuff", C::Legs,
     "A marker where a planted foot turns on its ball or heel: Pivot L / Pivot R (combined),\n"
     "or Pivot Heel L / Pivot Toe L... (separate). At the turn's start, or where the planted\n"
     "part rubs fastest (Peak).",
     "pivot", {{&P::pivot_min_deg, S::Divide}, {&P::pivot_rate_dps, S::Divide}}, &AutoPartsFootBoth, false, false, false,
     false, true},
    {T::Grab, "Grab", "Grab", C::Arms, "A marker where the hand lands on something: Grab L / Grab R.", "grab",
     {{&P::hand_contact_speed, S::Times}, {}}, &AutoPartsHands},
    {T::Release, "Release", "Release", C::Arms, "A marker where the hand leaves its support: Release L / Release R.",
     "release", {{&P::hand_contact_speed, S::Times}, {}}, &AutoPartsHands},
    // Story 10-8e: the hand's Pivot scuff is named with its category in undo names.
    {T::HandPivot, "Pivot scuff", "Arms Pivot scuff", C::Arms,
     "A marker where a planted hand turns: Hand Pivot L / Hand Pivot R. At the turn's start,\n"
     "or where the hand rubs fastest (Peak).", "hand_pivot",
     {{&P::hand_pivot_min_deg, S::Divide}, {&P::hand_pivot_rate_dps, S::Divide}}, &AutoPartsHands, false, false, false,
     false, true},
    // Story 10-9a: higher sensitivity = shorter flights count; story 10-9a-2: and a softer
    // push-off (jump_impulse_g) counts. Story 10-9a-3: on the foot that leaves last / touches
    // first (L / R), Combined / Separate.
    {T::Jump, "Jump", "Jump", C::Legs,
     "A marker where a jump takes off, on the last foot to leave (Jump L / Jump R):\n"
     "both feet off while the body falls freely, right after a hard push off the ground,\n"
     "or into a free fall (a fall on the back). Separate: Jump Heel L / Jump Toe L...\n"
     "With Lift-off ticked, it replaces the lifts there.",
     "jump", {{&P::jump_min_flight_s, S::Divide}, {&P::jump_impulse_g, S::Divide}}, &AutoPartsJump, false, true, true, false, false,
     static_cast<int>(T::LiftOff)},
    {T::Land, "Land", "Land", C::Legs,
     "A marker where a jump lands, on the first foot to touch, at its step (Land L / Land R).\n"
     "Separate: Land Heel L / Land Toe L...\n"
     "With Step ticked, it replaces the steps there.",
     "land", {{&P::jump_min_flight_s, S::Divide}, {}}, &AutoPartsJump, false, true, true, false, false, static_cast<int>(T::Step)},
    // Story 10-9g: the gear jolt, Body, sideless; higher sensitivity = softer impacts count.
    {T::Jolt, "Jolt", "Jolt", C::Body,
     "A marker where an impact shakes the gear and cloth (Jolt): a step, a jump's landing,\n"
     "a knee coming down, a sit or a fall, where the body's acceleration peaks above the\n"
     "sensitivity's threshold.\n"
     "Raise the sensitivity for softer steps, lower it for only the hard impacts.",
     "jolt", {{&P::jolt_min_g, S::Divide}, {}}, &AutoPartsJolt, true, false, false, true},
    // Story 10-9f: the Body's first type, sideless (a turn of the whole body has no foot).
    // Higher sensitivity = a smaller turn counts (the sweep / f). Never raised by Detect.
    {T::Spin, "Spin", "Spin", C::Body,
     "A spin of the whole body (the hips turning about the vertical):\n"
     "Spin where it starts turning, Spin End where it stops (45 deg or more).",
     "spin", {{&P::spin_min_deg, S::Divide}, {}}, &AutoPartsSpin, true, false, false, true},
    // Story 10-9e: higher sensitivity = longer toe contacts count as taps, and shorter toe
    // slides (a lower minimum length) as drags.
    {T::Tap, "Tap", "Tap", C::Legs,
     "A marker where the toes touch the ground lightly while the other foot carries the body\n"
     "(a toe contact under 150 ms, the heel up): Tap L / Tap R.\n"
     "With Step / Lift-off ticked, it replaces that foot's step / lift-off there.",
     "tap", {{&P::tap_max_s, S::Times}, {}}, &AutoPartsFootBoth, false, false, false, true, false, static_cast<int>(T::Step),
     static_cast<int>(T::LiftOff), true},
    {T::Drag, "Drag", "Drag", C::Legs,
     "A marker where the toes slide on the ground during a swing, the heel up\n"
     "(a foot dragged behind, a crawl, a kneel): Drag L / Drag R.",
     "drag", {{&P::drag_min_s, S::Divide}, {}}, &AutoPartsFootBoth, false, false, false, true},
    // Story 10-9b: body contacts, segmented as the hands (higher sensitivity = a faster part
    // still counts as at rest); a fall: a slower descent counts. Sit and Fall are the body's
    // (sideless).
    {T::Knee, "Knee", "Knee", C::Legs, "A marker where a knee touches the ground (kneeling, crawling): Knee L / Knee R.",
     "knee", {{&P::knee_contact_speed, S::Times}, {}}, &AutoPartsKnee},
    {T::Elbow, "Elbow", "Elbow", C::Arms, "A marker where an elbow touches the ground (lying, crawling on the elbows):\n"
     "Elbow L / Elbow R.",
     "elbow", {{&P::elbow_contact_speed, S::Times}, {}}, &AutoPartsElbow},
    {T::Sit, "Sit", "Sit", C::Body,
     "A marker where the body sits down (on a seat or the floor): the hips come to rest, low,\n"
     "the back upright and away from the feet, no knee down.",
     "sit", {{&P::sit_contact_speed, S::Times}, {}}, &AutoPartsSit, true},
    {T::Fall, "Fall", "Fall", C::Body,
     "A marker where the body lands on the ground (hips or back) after a fall,\n"
     "such as a fall on the back after a free fall.",
     "fall", {{&P::fall_min_speed, S::Divide}, {}}, &AutoPartsFall, true},
    // Story 10-9d: touches (hand_touch.h). Higher sensitivity = slower touches count and the reach
    // grows. Clap: one event for both hands, on the faster one (shared_sides). Neither is raised by
    // Detect (no_sens_raise): most clips hold no touch, a raise would make a swing's turn one.
    {T::Clap, "Clap", "Clap", C::Arms,
     "A marker where the two hands meet at speed and stop against each other,\n"
     "on the faster hand (Clap L / Clap R).",
     "clap", {{&P::clap_min_speed, S::Divide}, {&P::clap_reach, S::Times}}, &AutoPartsClap, false, false, true, true,
     false, -1, -1, false, 0b01u},  // story 10-10a: the confidence probes the approach speed, not the reach
    {T::HandOnBody, "Hand on body", "Hand on body", C::Arms,
     "A marker where a hand lands at speed on the body (trunk, head or thigh) and stops there:\n"
     "Hand on body L / Hand on body R.",
     "hand_body", {{&P::body_min_speed, S::Divide}, {&P::body_reach, S::Times}}, &AutoPartsHandOnBody, false, false,
     false, true, false, -1, -1, false, 0b01u},  // story 10-10a: the speed only, as Clap
    // Story 10-9c: the whooshes; higher sensitivity = slower motions count (their speed / f).
    {T::Kick, "Kick", "Kick", C::Legs,
     "A marker where a foot whips through the air, raised off the ground (Kick L / Kick R),\n"
     "at its fastest, or where the motion starts (Start). Separate: on the part fastest\n"
     "at the peak (Kick Heel L / Kick Toe L...).",
     "kick", {{&P::kick_speed, S::Divide}, {}}, &AutoPartsKick, false, true, false, true, true},
    {T::Swing, "Swing", "Swing", C::Arms,
     "A marker where a hand whips through the air (Swing L / Swing R), at its fastest,\n"
     "or where the motion starts (Start). A straight thrust is a Punch.",
     "swing", {{&P::swing_speed, S::Divide}, {}}, &AutoPartsWhooshHand, false, false, false, true, true},
    {T::Punch, "Punch", "Punch", C::Arms,
     "A marker where a hand thrusts straight out from the shoulder (Punch L / Punch R),\n"
     "at its fastest, or where the motion starts (Start).",
     "punch", {{&P::punch_speed, S::Divide}, {}}, &AutoPartsWhooshHand, false, false, false, true, true, -1, -1,
     false, 0u, 0.5},  // story 10-9h: half steps for the confidence (conf_scale)
    // Story 10-9i: higher sensitivity = a smaller heel rise counts (heel_tap_min_rise / f).
    {T::HeelTap, "Heel Tap", "Heel Tap", C::Legs,
     "A marker where the heel drops back onto the floor while the ball of the foot stays down\n"
     "(the foot rolls onto its ball, then the heel comes down): Heel Tap L / Heel Tap R.\n"
     "With Step ticked, it replaces that foot's step there.",
     "heel_tap", {{&P::heel_tap_min_rise, S::Divide}, {}}, &AutoPartsFootBoth, false, false, false, true, false,
     static_cast<int>(T::Step), -1, true, 0u, 0.5},  // conf_scale: Hip Hop's softest drops rise 0.017 over 0.013
    // Story 10-9k: the arms of a jump (auto_types_arm_jump.cpp); higher sensitivity = slower arms
    // count (their speed / f). With Swing ticked, each replaces the Swing of that hand within its
    // run (one motion, one sound). Not raised by Detect (no_sens_raise): a clip with no jump has
    // none, and a raise would turn an arm's sway at a step into one.
    {T::ArmJump, "Jump", "Arms Jump", C::Arms,
     "A marker where an arm is thrown up into a jump's take-off (Arms Jump L / Arms Jump R),\n"
     "at the hand's fastest. It follows the take-offs Legs > Jump finds (ticked or not).\n"
     "With Swing ticked, it replaces that hand's Swing there.",
     "arm_jump", {{&P::arm_jump_speed, S::Divide}, {}}, &AutoPartsArmJump, false, false, false, true, false, -1,
     static_cast<int>(T::Swing), true, 0u, 0.5},  // conf_scale: Jump Attack's left arm 4.76 over 4.0
    {T::ArmLand, "Land", "Arms Land", C::Arms,
     "A marker where an arm comes down at a jump's landing, reaching for the floor or the knees\n"
     "(Arms Land L / Arms Land R), at the hand's fastest. It follows the landings Legs > Land\n"
     "finds (ticked or not). With Swing ticked, it replaces that hand's Swing there.",
     "arm_land", {{&P::arm_land_speed, S::Divide}, {}}, &AutoPartsArmJump, false, false, false, true, false, -1,
     static_cast<int>(T::Swing), true, 0u, 0.5},  // conf_scale: the hand to the floor 3.7-3.9 over 3.0
    // Story 10-9l: a hand contact (Grab's) or a touch on the body (Hand on body's) whose fingers
    // stay extended (hand_slap.h). Higher sensitivity = the slower contacts and touches count, as
    // Grab's and Hand on body's (so a slap and the grab / touch it replaces line up). It replaces
    // that hand's Grab (`replaces`) and Hand on body (`replaces_in_span`: its span is its time) at
    // the same contact. Never raised by Detect: most clips hold no slap, a raise would turn a
    // slow resting hand into one.
    {T::Slap, "Slap", "Slap", C::Arms,
     "A marker where a flat hand hits something (the floor, a wall, the body) and the fingers\n"
     "stay extended: Slap L / Slap R. A hand whose fingers close after the contact is a Grab.\n"
     "With Grab or Hand on body ticked, it replaces that hand's grab or hand on body there.\n"
     "Needs finger bones that move in the clip (a knuckle and a fingertip per hand):\n"
     "without them, a slap cannot be told apart and the contacts stay Grabs.",
     "slap", {{&P::hand_contact_speed, S::Times}, {&P::body_min_speed, S::Divide}}, &AutoPartsSlap, false, false, false,
     true, false, static_cast<int>(T::Grab), static_cast<int>(T::HandOnBody), true},
};

// ---- Strength levels (story 10-10b) ------------------------------------------------------------
// Per type: Medium from the first cut, Hard from the second, in the strength unit of its kinds
// (leg/s; degrees for the pivots and the spin; g for the jolt). Measured 2026-10-21 on the 61
// fixture clips' default matrix (trigger_bench, its STRENGTH block): a type with events on several
// clips takes the clip-weighted 33rd and 67th percentiles (each clip weighs the same, so the
// 26 steps of a treadmill run do not set the levels alone), rounded. A type with few or no
// events borrows the cuts of the type that measures the same thing (named on its row).
// Steps, for scale: climbing and crawling Soft, walking Medium, running Hard; claps: the 4 claps
// of Clap_Normal Medium, the 7 of Clap_Hard Hard.

constexpr AutoStrengthCuts kStrengthRows[] = {
    {T::Step, 1.2, 4.0},        // 121 steps on 46 clips: wp33 1.23, wp67 4.14 leg/s (the foot's approach)
    {T::LiftOff, 2.0, 4.5},     // 118 on 46 clips: wp33 2.15, wp67 4.57 leg/s
    {T::Slide, 0.5, 0.7},       // 13 on 8 clips: wp33 0.49, wp67 0.71 leg/s (horizontal)
    {T::Pivot, 30.0, 85.0},     // 19 on 8 clips: wp33 31, wp67 86 deg
    {T::Grab, 1.3, 1.6},        // 22 on 14 clips: wp33 1.35, wp67 1.54 leg/s
    {T::Release, 1.5, 2.3},     // 17 on 14 clips: wp33 1.50, wp67 2.26 leg/s
    {T::HandPivot, 30.0, 85.0}, // 1 event (26 deg): the foot pivot's cuts (the same sweep)
    {T::Jump, 2.0, 3.3},        // 13 on 13 clips: wp33 2.08, wp67 3.34 leg/s (the hips' vertical speed)
    {T::Land, 2.7, 4.4},        // 10 on 10 clips: wp33 2.69, wp67 4.38 leg/s (the same)
    {T::Jolt, 1.35, 2.1},       // 61 on 24 clips: wp33 1.35, wp67 2.11 g
    // 30 (start and end) on 12 clips, a punch's hip twist no longer counted (story 10-9h): Climbing
    // 49, Rope Climb 51, Roue 56, AirPunch 62, Jump Attack 75, Right Turn 91, Jump Away 97,
    // Hip Hop 98-111, Catwalk 178, Roue 220, Armada 375 deg (the sweep; wp33 75, wp67 111); the
    // second cut sits between a quarter and a half turn (135), so a 90 or 180 deg turn never
    // falls on a cut.
    {T::Spin, 75.0, 135.0},
    {T::Tap, 1.2, 4.0},         // no event: the step's cuts (the toe's approach)
    {T::Drag, 1.5, 1.8},        // 5 on 4 clips: wp33 1.54, wp67 1.85 leg/s (the toe's peak slide)
    // 10 on 6 clips (the crawls and kneels): wp33 0.54, wp67 0.76 leg/s (the descent); the
    // AirPunch's drop onto the knees 3.4-3.8 (Hard).
    {T::Knee, 0.55, 0.75},
    {T::Elbow, 0.55, 0.75},     // no event: the knee's cuts (the same contact segmentation)
    // 2 events (1.0, 1.3 leg/s, both on the clap clips: doubtful sits), no real sit clip yet:
    // a sit at about 1 leg/s Medium, a drop onto the seat from 1.6 Hard. Unmeasured.
    {T::Sit, 0.8, 1.6},
    {T::Fall, 2.7, 4.4},        // 1 event (3.4): the land's cuts (the hips coming down)
    // 15 on 4 clips: Clap_Normal 2.0 to 2.95 (6, the light claps), Clap_Hard 8.1 to 11.1 (7),
    // Pistol Equip 2.8, ElbowPunch 5.05 leg/s (the closing speed): Hard from 5 (between the two
    // clap clips), Medium from 2.
    {T::Clap, 2.0, 5.0},
    {T::HandOnBody, 2.0, 5.0},  // 2 events (1.5, 2.8): the clap's cuts (the same closing speed)
    {T::Kick, 9.0, 15.0},       // 2 events (10.5, 20, the dance clips): unmeasured beyond them
    {T::Swing, 7.4, 8.0},       // 10 on 6 clips: wp33 7.37, wp67 8.03 leg/s (the hand's peak speed)
    // 9 on 4 clips (story 10-9h; the radial extending speed, not the hand's): the jabs 2.34-2.44
    // Soft, the crosses 2.66-2.79 Medium, the wound-up punch 4.69 Hard.
    {T::Punch, 2.6, 3.5},
    // 11 on 2 clips: wp33 0.259, wp67 0.308 leg/s (the heel's fastest descent); Hip Hop Dancing's
    // 9 from 0.12 (slow drops) to 0.34, Clap_Normal's seated heel pops 0.26 and 0.31.
    {T::HeelTap, 0.26, 0.31},
    // Story 10-9k: 18 on 10 clips: wp33 5.36, wp67 6.58 leg/s (the hand's peak speed); Jumping's
    // and Jump Attack's slower arms (4.8-5.7) Soft, Standing Jump's 7.4-7.6 Hard.
    {T::ArmJump, 5.4, 6.6},
    // 15 on 9 clips: wp33 4.0, wp67 5.07 leg/s; Standing Jump02's arms coming down 3.1-3.3 Soft,
    // Jump Attack's strike on landing 8.0 and the vault's 9.2 Hard.
    {T::ArmLand, 4.0, 5.1},
    // Story 10-9l: 8 on 5 clips (the crawls' hands 1.04-1.44, Standing Jump02 1.29 and 1.9, the
    // AirPunch's landing on the hands 2.97 and 8.21 leg/s; the hand's approach, as Grab): wp33 1.42,
    // wp67 2.61.
    {T::Slap, 1.4, 2.6},
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
     {{"slide", &AutoSourceFoot, &AutoMatchSlide}}, &AutoMissingFootAny, &AutoLanesFootSlide, &AutoRolesFoot,
     nullptr, true},
    {K::Pivot, "pivot", T::Pivot, "Pivot", {0x9E5FDD, 0xDD5F9E}, kNo, false, true, "", K::Pivot,
     {{"pivot", &AutoSourceFoot, &AutoMatchPivot}}, &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot,
     nullptr, true},
    {K::Grab, "grab", T::Grab, "Grab", {0x5FDD9E, 0xDDB45F}, kNo, false, false, "hand", K::Grab,
     {{"contact", &AutoSourceHand, &AutoMatchGrab}}, &AutoMissingHand, &AutoLanesHandContact, &AutoRolesHand},
    {K::Release, "release", T::Release, "Release", {0x9EB4DD, 0xDD9E9E}, kNo, false, false, "hand", K::Release,
     {{"contact", &AutoSourceHand, &AutoMatchRelease}}, &AutoMissingHand, &AutoLanesHandContact, &AutoRolesHand},
    {K::HandPivot, "hand_pivot", T::HandPivot, "Hand Pivot", {0xA8DD3C, 0xB4643C}, kNo, false, true, "hand",
     K::HandPivot,
     {{"pivot", &AutoSourceHand, &AutoMatchHandPivot}}, &AutoMissingHand, &AutoLanesHandPivot, &AutoRolesHand,
     nullptr, true},
    {K::HandPivotAny, "hand_pivot_any", T::HandPivot, "Hand Pivot", {0xA8DD3C, 0xB4643C}, kNo, false, true, "hand",
     K::HandPivot,
     {{"pivot", &AutoSourceHandAny, &AutoMatchHandPivot}}, &AutoMissingHand, &AutoLanesHandPivot, &AutoRolesHand,
     nullptr, true},
    {K::SlideHeel, "slide_heel", T::Slide, "Slide Heel", {0x5FDD5F, 0xDD5FDD}, kEach, true, false, "heel", K::SlideHeel,
     {{"slide", &AutoSourceFootParts, &AutoMatchSlideHeel}}, &AutoMissingHeel, &AutoLanesSlideHeel, &AutoRolesFoot,
     nullptr, true},
    {K::SlideToe, "slide_toe", T::Slide, "Slide Toe", {0x9FEE9F, 0xEE9FEE}, kEach, false, false, "toe", K::SlideToe,
     {{"slide", &AutoSourceFootParts, &AutoMatchSlideToe}}, &AutoMissingToe, &AutoLanesSlideToe, &AutoRolesFoot,
     nullptr, true},
    {K::PivotHeel, "pivot_heel", T::Pivot, "Pivot Heel", {0x9E5FDD, 0xDD5F9E}, kEach, true, true, "heel", K::PivotHeel,
     {{"pivot", &AutoSourceFootParts, &AutoMatchPivotHeel}}, &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot,
     nullptr, true},
    {K::PivotToe, "pivot_toe", T::Pivot, "Pivot Toe", {0xC49FEE, 0xEE9FC4}, kEach, false, true, "toe", K::PivotToe,
     {{"pivot", &AutoSourceFootParts, &AutoMatchPivotToe}}, &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot,
     nullptr, true},
    {K::SlideHeelLongest, "slide_heel_longest", T::Slide, "Slide Heel", {0x5FDD5F, 0xDD5FDD}, kLong, true, false, "heel",
     K::SlideHeel,
     {{"slide", &AutoSourceFoot, &AutoMatchSlideHeelLongest}}, &AutoMissingHeel, &AutoLanesSlideHeel, &AutoRolesFoot,
     nullptr, true},
    {K::SlideToeLongest, "slide_toe_longest", T::Slide, "Slide Toe", {0x9FEE9F, 0xEE9FEE}, kLong, false, false, "toe",
     K::SlideToe,
     {{"slide", &AutoSourceFoot, &AutoMatchSlideToeLongest}}, &AutoMissingToe, &AutoLanesSlideToe, &AutoRolesFoot,
     nullptr, true},
    {K::PivotHeelLongest, "pivot_heel_longest", T::Pivot, "Pivot Heel", {0x9E5FDD, 0xDD5F9E}, kLong, true, true, "heel",
     K::PivotHeel,
     {{"pivot", &AutoSourceFoot, &AutoMatchPivotHeelLongest}}, &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot,
     nullptr, true},
    {K::PivotToeLongest, "pivot_toe_longest", T::Pivot, "Pivot Toe", {0xC49FEE, 0xEE9FC4}, kLong, false, true, "toe",
     K::PivotToe,
     {{"pivot", &AutoSourceFoot, &AutoMatchPivotToeLongest}}, &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot,
     nullptr, true},
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
    // Story 10-9f: a spin's start and its end (sideless: color[0]; the end a shade lighter).
    {K::Spin, "spin", T::Spin, "Spin", {0xB45FDD, 0xB45FDD}, kNo, false, true, "", K::Spin,
     {{"hip turn", &AutoSourceSpin, &AutoMatchSpin}}, &AutoMissingSpin, &AutoLanesSpin, &AutoRolesSpin},
    {K::SpinEnd, "spin_end", T::Spin, "Spin End", {0xD29FEE, 0xD29FEE}, kNo, false, true, "", K::SpinEnd,
     {{"hip turn", &AutoSourceSpin, &AutoMatchSpinEnd}}, &AutoMissingSpin, &AutoLanesSpin, &AutoRolesSpin},
    // Story 10-9e: tap and drag, on the toe (the ball or the tip).
    {K::Tap, "tap", T::Tap, "Tap", {0x78B4F0, 0xF0B478}, kNo, false, false, "toe", K::Tap,
     {{"contact", &AutoSourceTap, &AutoMatchTap}}, &AutoMissingHeelOrToe, &AutoLanesToe, &AutoRolesFoot},
    {K::Drag, "drag", T::Drag, "Drag", {0x8C96B4, 0xB4968C}, kNo, false, false, "toe", K::Drag,
     {{"drag", &AutoSourceDrag, &AutoMatchDrag}}, &AutoMissingHeelOrToe, &AutoLanesDrag, &AutoRolesFoot},
    // Story 10-9b: body contacts.
    {K::Knee, "knee", T::Knee, "Knee", {0x7F7FB4, 0xB47F7F}, kNo, false, false, "knee", K::Knee,
     {{"contact", &AutoSourceBody, &AutoMatchKnee}}, &AutoMissingKnee, &AutoLanesKnee, &AutoRolesKnee},
    {K::Elbow, "elbow", T::Elbow, "Elbow", {0x7FB4B4, 0xB4B47F}, kNo, false, false, "elbow", K::Elbow,
     {{"contact", &AutoSourceBody, &AutoMatchElbow}}, &AutoMissingElbow, &AutoLanesElbow, &AutoRolesElbow},
    {K::Sit, "sit", T::Sit, "Sit", {0xB48CDC, 0xB48CDC}, kNo, false, false, "", K::Sit,
     {{"contact", &AutoSourceBody, &AutoMatchSit}}, &AutoMissingSit, &AutoLanesSit, &AutoRolesSit},
    {K::Fall, "fall", T::Fall, "Fall", {0xDC5A8C, 0xDC5A8C}, kNo, false, false, "", K::Fall,
     {{"landing", &AutoSourceBody, &AutoMatchFall}}, &AutoMissingFall, &AutoLanesFall, &AutoRolesFall},
    // Story 10-9d: clap and hand on body.
    {K::Clap, "clap", T::Clap, "Clap", {0x3C8CDD, 0xDD8C3C}, kNo, false, false, "hand", K::Clap,
     {{"touch", &AutoSourceClap, &AutoMatchClap}}, &AutoMissingClap, &AutoLanesClap, &AutoRolesClap},
    {K::HandOnBody, "hand_body", T::HandOnBody, "Hand on body", {0x8C8CDD, 0xDDA08C}, kNo, false, false, "hand",
     K::HandOnBody,
     {{"touch", &AutoSourceHandOnBody, &AutoMatchHandOnBody}}, &AutoMissingHandOnBody, &AutoLanesHandOnBody,
     &AutoRolesHandOnBody},
    // Story 10-9c: kick (combined, heel, toe), swing and punch, at the peak; then each at the
    // start (`at_start`: the same marker and colour, the peak kind's sound key).
    {K::Kick, "kick", T::Kick, "Kick", {0x7850DD, 0xDD7850}, kNo, false, false, "", K::Kick,
     {{"speed peak", &AutoSourceWhoosh, &AutoMatchKick}}, &AutoMissingKick, &AutoLanesKick, &AutoRolesKick},
    {K::KickHeel, "kick_heel", T::Kick, "Kick Heel", {0x7850DD, 0xDD7850}, kNo, false, false, "heel", K::KickHeel,
     {{"speed peak", &AutoSourceWhoosh, &AutoMatchKickHeel}}, &AutoMissingKickHeel, &AutoLanesKick, &AutoRolesKick},
    {K::KickToe, "kick_toe", T::Kick, "Kick Toe", {0xA896EE, 0xEEA896}, kNo, false, false, "toe", K::KickToe,
     {{"speed peak", &AutoSourceWhoosh, &AutoMatchKickToe}}, &AutoMissingKickToe, &AutoLanesKick, &AutoRolesKick},
    {K::KickStart, "kick_start", T::Kick, "Kick", {0x7850DD, 0xDD7850}, kNo, false, false, "", K::Kick,
     {{"start", &AutoSourceWhooshStart, &AutoMatchKick}}, &AutoMissingKick, &AutoLanesKick, &AutoRolesKick,
     nullptr, true},
    {K::KickHeelStart, "kick_heel_start", T::Kick, "Kick Heel", {0x7850DD, 0xDD7850}, kNo, false, false, "heel",
     K::KickHeel,
     {{"start", &AutoSourceWhooshStart, &AutoMatchKickHeel}}, &AutoMissingKickHeel, &AutoLanesKick, &AutoRolesKick,
     nullptr, true},
    {K::KickToeStart, "kick_toe_start", T::Kick, "Kick Toe", {0xA896EE, 0xEEA896}, kNo, false, false, "toe",
     K::KickToe,
     {{"start", &AutoSourceWhooshStart, &AutoMatchKickToe}}, &AutoMissingKickToe, &AutoLanesKick, &AutoRolesKick,
     nullptr, true},
    {K::Swing, "swing", T::Swing, "Swing", {0x3CA0DD, 0xDD3CA0}, kNo, false, false, "hand", K::Swing,
     {{"speed peak", &AutoSourceWhoosh, &AutoMatchSwing}}, &AutoMissingWhooshHand, &AutoLanesSwing,
     &AutoRolesWhooshHand},
    {K::SwingStart, "swing_start", T::Swing, "Swing", {0x3CA0DD, 0xDD3CA0}, kNo, false, false, "hand", K::Swing,
     {{"start", &AutoSourceWhooshStart, &AutoMatchSwing}}, &AutoMissingWhooshHand, &AutoLanesSwing,
     &AutoRolesWhooshHand, nullptr, true},
    {K::Punch, "punch", T::Punch, "Punch", {0xDD503C, 0x3CDD8C}, kNo, false, false, "hand", K::Punch,
     {{"speed peak", &AutoSourceWhoosh, &AutoMatchPunch}}, &AutoMissingWhooshHand, &AutoLanesPunch,
     &AutoRolesWhooshHand},
    {K::PunchStart, "punch_start", T::Punch, "Punch", {0xDD503C, 0x3CDD8C}, kNo, false, false, "hand", K::Punch,
     {{"start", &AutoSourceWhooshStart, &AutoMatchPunch}}, &AutoMissingWhooshHand, &AutoLanesPunch,
     &AutoRolesWhooshHand, nullptr, true},
    // Story 10-9i: the heel tap, on the heel (dark blue / brown: away from Scuff's teal / sand, 10-9j).
    {K::HeelTap, "heel_tap", T::HeelTap, "Heel Tap", {0x1E5A8C, 0x8C5A1E}, kNo, false, false, "heel", K::HeelTap,
     {{"heel drop", &AutoSourceHeelTap, &AutoMatchHeelTap}}, &AutoMissingHeelOrToe, &AutoLanesHeelTap, &AutoRolesFoot},
    // Story 10-9j: the scuffs' Peak twins (the rows above mark the start, `at_start`), in the order
    // of their Start kinds: the same marker, colour, role key and sound key, timed at the rub's
    // speed peak (scuff_peak.h).
    {K::SlidePeak, "slide_peak", T::Slide, "Slide", {0x5FDD5F, 0xDD5FDD}, kNo, false, false, "", K::Slide,
     {{"rub peak", &AutoSourceFootPeak, &AutoMatchSlide}}, &AutoMissingFootAny, &AutoLanesFootSlide, &AutoRolesFoot},
    {K::SlideHeelPeak, "slide_heel_peak", T::Slide, "Slide Heel", {0x5FDD5F, 0xDD5FDD}, kEach, true, false, "heel", K::SlideHeel,
     {{"rub peak", &AutoSourceFootPartsPeak, &AutoMatchSlideHeel}},
     &AutoMissingHeel, &AutoLanesSlideHeel, &AutoRolesFoot},
    {K::SlideToePeak, "slide_toe_peak", T::Slide, "Slide Toe", {0x9FEE9F, 0xEE9FEE}, kEach, false, false, "toe", K::SlideToe,
     {{"rub peak", &AutoSourceFootPartsPeak, &AutoMatchSlideToe}}, &AutoMissingToe, &AutoLanesSlideToe, &AutoRolesFoot},
    {K::SlideHeelLongestPeak, "slide_heel_longest_peak", T::Slide, "Slide Heel", {0x5FDD5F, 0xDD5FDD}, kLong, true, false, "heel",
     K::SlideHeel,
     {{"rub peak", &AutoSourceFootLongestPeak, &AutoMatchSlideHeelLongest}},
     &AutoMissingHeel, &AutoLanesSlideHeel, &AutoRolesFoot},
    {K::SlideToeLongestPeak, "slide_toe_longest_peak", T::Slide, "Slide Toe", {0x9FEE9F, 0xEE9FEE}, kLong, false, false, "toe",
     K::SlideToe,
     {{"rub peak", &AutoSourceFootLongestPeak, &AutoMatchSlideToeLongest}},
     &AutoMissingToe, &AutoLanesSlideToe, &AutoRolesFoot},
    {K::PivotPeak, "pivot_peak", T::Pivot, "Pivot", {0x9E5FDD, 0xDD5F9E}, kNo, false, true, "", K::Pivot,
     {{"rub peak", &AutoSourceFootPeak, &AutoMatchPivot}}, &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot},
    {K::PivotHeelPeak, "pivot_heel_peak", T::Pivot, "Pivot Heel", {0x9E5FDD, 0xDD5F9E}, kEach, true, true, "heel", K::PivotHeel,
     {{"rub peak", &AutoSourceFootPartsPeak, &AutoMatchPivotHeel}},
     &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot},
    {K::PivotToePeak, "pivot_toe_peak", T::Pivot, "Pivot Toe", {0xC49FEE, 0xEE9FC4}, kEach, false, true, "toe", K::PivotToe,
     {{"rub peak", &AutoSourceFootPartsPeak, &AutoMatchPivotToe}},
     &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot},
    {K::PivotHeelLongestPeak, "pivot_heel_longest_peak", T::Pivot, "Pivot Heel", {0x9E5FDD, 0xDD5F9E}, kLong, true, true, "heel",
     K::PivotHeel,
     {{"rub peak", &AutoSourceFootLongestPeak, &AutoMatchPivotHeelLongest}},
     &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot},
    {K::PivotToeLongestPeak, "pivot_toe_longest_peak", T::Pivot, "Pivot Toe", {0xC49FEE, 0xEE9FC4}, kLong, false, true, "toe",
     K::PivotToe,
     {{"rub peak", &AutoSourceFootLongestPeak, &AutoMatchPivotToeLongest}},
     &AutoMissingHeelOrToe, &AutoLanesPivot, &AutoRolesFoot},
    {K::HandPivotPeak, "hand_pivot_peak", T::HandPivot, "Hand Pivot", {0xA8DD3C, 0xB4643C}, kNo, false, true, "hand",
     K::HandPivot,
     {{"rub peak", &AutoSourceHandPeak, &AutoMatchHandPivot}}, &AutoMissingHand, &AutoLanesHandPivot, &AutoRolesHand},
    {K::HandPivotAnyPeak, "hand_pivot_any_peak", T::HandPivot, "Hand Pivot", {0xA8DD3C, 0xB4643C}, kNo, false, true, "hand",
     K::HandPivot,
     {{"rub peak", &AutoSourceHandAnyPeak, &AutoMatchHandPivot}},
     &AutoMissingHand, &AutoLanesHandPivot, &AutoRolesHand},
    // Story 10-9k: the arms of a jump, on the hand (a cyan / amber and a deep blue / bronze, away
    // from Swing's blue / magenta and Legs Jump's teal).
    {K::ArmJump, "arm_jump", T::ArmJump, "Arms Jump", {0x46C8DC, 0xDC9646}, kNo, false, false, "hand", K::ArmJump,
     {{"take-off arm", &AutoSourceArmJump, &AutoMatchSwing}}, &AutoMissingArmJump, &AutoLanesArmJump,
     &AutoRolesWhooshHand},
    {K::ArmLand, "arm_land", T::ArmLand, "Arms Land", {0x2E6EA0, 0xA06E2E}, kNo, false, false, "hand", K::ArmLand,
     {{"landing arm", &AutoSourceArmLand, &AutoMatchSwing}}, &AutoMissingArmJump, &AutoLanesArmLand,
     &AutoRolesWhooshHand},
    // Story 10-9l: the slap, on the hand (sky blue / copper).
    {K::Slap, "slap", T::Slap, "Slap", {0x46B4DC, 0xDC7846}, kNo, false, false, "hand", K::Slap,
     {{"contact", &AutoSourceSlapContact, &AutoMatchSlapContact}, {"on body", &AutoSourceSlapBody, &AutoMatchSlapBody}},
     &AutoMissingSlap, &AutoLanesSlap, &AutoRolesSlap},
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
constexpr bool StrengthRowsValid()
{
    for (int i = 0; i < kAutoTypeCount; ++i)
        if (static_cast<int>(kStrengthRows[i].type) != i || !(kStrengthRows[i].medium > 0.0) ||
            !(kStrengthRows[i].hard > kStrengthRows[i].medium))
            return false;
    return true;
}
static_assert(sizeof(kStrengthRows) / sizeof(kStrengthRows[0]) == kAutoTypeCount, "one strength row per AutoType");
static_assert(StrengthRowsValid(), "strength rows follow the AutoType order, 0 < medium < hard");
static_assert(CategoryRowsInOrder(), "category rows follow the AutoCategory order");
static_assert(KindSignaturesValid(), "every kind: a first signature, used slots contiguous and named");
static_assert(TypeRowsInOrder(), "type rows follow the AutoType order");
static_assert(KindRowsInOrder(), "kind rows follow the AutoKind order");
// Story 10-9j (review): a `start_option` type's twins pair by position (StartTwin, auto_detect.cpp):
// the n-th kind of each flavour. Each pair must be one kind in both flavours: one word the other's
// plus "_start" or "_peak", the same marker, sound key and part mode.
constexpr bool SameText(const char* a, const char* b)
{
    for (; *a && *a == *b; ++a, ++b) {}
    return *a == *b;
}
constexpr bool TwinWords(const char* a, const char* b)
{
    for (; *a && *a == *b; ++a, ++b) {}
    const char* tail = *a ? a : b;  // the longer word's rest
    if (*a && *b) return false;
    return SameText(tail, "_start") || SameText(tail, "_peak");
}
constexpr const AutoKindInfo* NthOfFlavour(AutoType t, bool at_start, int n)
{
    for (const AutoKindInfo& r : kKindRows)
        if (r.type == t && r.at_start == at_start && n-- == 0) return &r;
    return nullptr;
}
constexpr bool StartTwinsPaired()
{
    for (const AutoTypeInfo& t : kTypeRows) {
        if (!t.start_option) continue;
        for (int n = 0;; ++n) {
            const AutoKindInfo* a = NthOfFlavour(t.type, false, n);
            const AutoKindInfo* b = NthOfFlavour(t.type, true, n);
            if (!a && !b) break;
            if (!a || !b || !TwinWords(a->word, b->word) || !SameText(a->marker, b->marker) ||
                a->sound_key != b->sound_key || a->scuff != b->scuff || a->scuff_heel != b->scuff_heel)
                return false;
        }
    }
    return true;
}
static_assert(StartTwinsPaired(), "a start_option type's Peak and Start kinds pair in the same order");

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

const AutoStrengthCuts& AutoStrengthRow(AutoType t)
{
    return kStrengthRows[RowIndex(static_cast<int>(t), kAutoTypeCount)];
}

int AutoStrengthLevel(AutoType t, double strength)
{
    if (!std::isfinite(strength) || !(strength > 0.0)) return 0;
    const AutoStrengthCuts& c = AutoStrengthRow(t);
    return strength >= c.hard ? kStrengthHard : strength >= c.medium ? kStrengthMedium : kStrengthSoft;
}

const char* StrengthLevelName(int level)
{
    switch (level) {
    case kStrengthSoft: return "Soft";
    case kStrengthMedium: return "Medium";
    case kStrengthHard: return "Hard";
    default: return "";
    }
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
