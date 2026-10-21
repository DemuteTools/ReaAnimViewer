// SPDX-License-Identifier: MIT
//
// Story 10-9-0: the auto event types as one table. Every category (AutoCategory), type
// (AutoType) and block kind (AutoKind) of auto_detect.h has one descriptor row here, in enum
// order; the code that used to switch on them reads the rows. A type's own logic (the physics
// events it takes, its lanes, its required parts) is reached through the function pointers of its
// rows, in one file per category (auto_types_legs.cpp, auto_types_arms.cpp) or per type
// (auto_types_jolt.cpp, story 10-9g). How to add a type:
// see the note at the top of auto_types.cpp.
//
// The modes of the existing types (steps separate, slide / pivot parts, hand pivot any) stay as
// code in auto_detect.cpp, used only by their own types; a type with no mode needs none.
//
// Pure C++17, no ImGui, no REAPER. Host-tested (tests/auto_types_test.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "auto_detect.h"
#include "bone_roles.h"
#include "motion_physics.h"

namespace rav {

// ---- Rows --------------------------------------------------------------------------------------

struct AutoCategoryInfo {
    AutoCategory category;
    const char*  label;       // "Legs" (the section's fold row)
    const char*  sound_part;  // "legs" (Epic 13: the body part its sounds are assigned under)
};

// One constant a type's sensitivity moves (AutoPhysicsParams): x f (more sensitive = more
// contacts) or / f (a lower threshold), f = SensitivityFactor(sens).
enum class AutoSensDir : int { Times = 0, Divide };
struct AutoSensField {
    double PhysicsParams::*field = nullptr;  // null = unused
    AutoSensDir            dir = AutoSensDir::Times;
};
constexpr int kAutoSensFields = 2;

// The parts a type needs that have no bone (AutoMissingParts, after the body scale), by role
// name, pushed each once (AutoPushOnce). `separate`: the type's own mode (AutoTypeSeparate).
using AutoTypePartsFn = void (*)(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);

struct AutoTypeInfo {
    AutoType        type;
    const char*     label;       // the section's checkbox ("Slide scuff")
    const char*     undo_label;  // in undo names ("Arms Pivot scuff")
    AutoCategory    category;
    const char*     hint;        // the checkbox's tooltip
    const char*     sound_word;  // Epic 13: the stable lower-case word ("step", "hand_pivot")
    AutoSensField   sens[kAutoSensFields];
    AutoTypePartsFn parts;
    // Story 10-9a: one block per kind, not per side: saved side=B, its marker without " L/R", one
    // colour (color[0]), one row in the Tagging view. No type since story 10-9a-3 (jump and land
    // take the foot); kept for the Body types.
    bool            sideless = false;
    // Story 10-9a-3: Combined / Separate (AutoSettings::separate_parts): its kinds are one combined
    // kind (no role key) and a heel and a toe kind (role keys "heel", "toe"; the toe = the ball or
    // the toe tip), Separate making the part kinds (lift-off, jump, land).
    bool            part_split = false;
    // Story 10-9a-3: one event for both feet (jump, land: one take-off / landing per jump, on the
    // foot that names it): Detect's sensitivity search skips a block when a block of the type on
    // either side has the event.
    bool            shared_sides = false;
    // Story 10-9f: Detect's sensitivity search never raises this type's silent blocks (a motion
    // most clips never hold, a spin: a raise would turn a clip's sway or twist into one). The
    // user's own sensitivity still applies. Story 10-9g: Jolt too (a walk or a climb has no jolt;
    // a raise would make its softest step one); the block counts as searched (Detect not lit).
    // Story 10-9e: Tap and Drag too (a walk's toe skims drag from 67 %). Story 10-9d: Clap and
    // Hand on body too. Story 10-9c: Kick, Swing and Punch too.
    bool            no_sens_raise = false;
    // Story 10-9c: Peak / Start (AutoSettings::at_start): each kind has a twin whose row says
    // `at_start` (same type, marker, colour, role key; its sound key the peak kind), Start making
    // the twins (kick, swing, punch). The Peak / Start widget, the settings and the block kept
    // across a switch follow the row.
    bool            start_option = false;
    // Story 10-9e: the type whose events this type's events replace within kAutoReplaceS when both
    // are ticked (AutoEvents; story 10-9a: Land the steps, Jump the lift-offs), as an AutoType
    // value; -1 = none. `replaces_in_span`: another type replaced anywhere over the event's span
    // (its physics start to end, +-kAutoReplaceS: a Tap the lift-off that ends its touch); -1 =
    // none. `replaces_own_side`: only those of the same foot (Tap), else either foot.
    int             replaces = -1;
    int             replaces_in_span = -1;
    bool            replaces_own_side = false;
};

// An event source: the physics events of one analysis a kind reads (FootEvents, HandEvents...).
// AutoEvents computes each source once per analysis.
using AutoEventSourceFn = std::vector<PhysicsEvent> (*)(const PhysicsAnalysis& a, const PhysicsParams& p);
// Whether a source's event (already of the block's side) is one of the kind's.
using AutoMatchFn = bool (*)(const PhysicsEvent& e, const PhysicsAnalysis& a, char side);
// Whether the kind's own part on that side has no bone (AutoBlockPartMissing, after the body scale).
using AutoPartMissingFn = bool (*)(char side, const std::vector<int>& role_to_bone);

// The lanes of one block (AutoBlockLanes): the analysis, its params, the block's side (B for a
// sideless kind) and the clip window; a lanes function appends to `out`, and sets `reason` when
// it adds none.
struct AutoLaneArgs {
    const PhysicsAnalysis* a = nullptr;
    const PhysicsParams*   p = nullptr;
    char                   side = 'L';
    size_t                 first = 0;
    size_t                 n = 0;
    const FootTrack&       Foot() const { return a->foot[side == 'R' ? 1 : 0]; }
    const HandTrack&       Hand() const { return a->hand[side == 'R' ? 1 : 0]; }
};
using AutoLanesFn = void (*)(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);
// The roles the Tagging view highlights for a block of that side (B: a sideless kind's).
using AutoRolesFn = std::vector<Role> (*)(char side);

// Story 10-9a-2: one way to recognise a kind's events: its event source and match. A kind holds
// an ordered list of them (AutoKindInfo::sigs); all run, their events merged per block (an event
// within kAutoMergeS of one an earlier signature kept is dropped). Not saved data.
struct AutoSignature {
    const char*       name = nullptr;  // "flight", "push-off" (the inspector's "Found by")
    AutoEventSourceFn source = nullptr;
    AutoMatchFn       match = nullptr;
};
constexpr int kAutoMaxSignatures = 4;

struct AutoKindInfo {
    AutoKind          kind;
    const char*       word;  // the save word (Block::auto_type): never renamed
    AutoType          type;
    const char*       marker;    // the marker name's word ("FS": "FS L")
    uint32_t          color[2];  // the default colour (0xRRGGBB), left then right (a sideless kind: color[0])
    ScuffParts        scuff;     // story 10-8m: a slide / pivot part kind's mode (Combined otherwise)
    bool              scuff_heel;  // a slide / pivot part kind: the heel's (false for the Heel step)
    bool              degrees;   // strength in degrees (a pivot): no speed
    const char*       role_key;  // Epic 13: the event's role key ("heel", "toe", "hand", "")
    AutoKind          sound_key; // Epic 13: the kind whose word names its sound key (kinds of one marker share it)
    // Story 10-9a-2: its signatures, in order (the first gives an event's time when two find it);
    // unused slots null (name, source and match), at least one, contiguous.
    AutoSignature     sigs[kAutoMaxSignatures];
    AutoPartMissingFn part_missing;
    AutoLanesFn       lanes;
    AutoRolesFn       roles;
    // Story 10-9g: the strength's unit when it is neither leg/s nor degrees ("g": a jolt); its
    // speed is then 0 (no m/s). Null for the others.
    const char*       strength_unit = nullptr;
    // Story 10-9c: a `start_option` type's kind that marks the motion's start (its sound key is
    // its peak twin).
    bool              at_start = false;
};

// The rows, by enum (a value out of range reads the first row).
const AutoCategoryInfo& AutoCategoryRow(AutoCategory c);
const AutoTypeInfo&     AutoTypeRow(AutoType t);
const AutoKindInfo&     AutoKindRow(AutoKind k);
// Story 10-9a-2: how many signatures a kind holds (its leading non-null slots).
int AutoSignatureCount(const AutoKindInfo& row);

// The types of a category, in enum order (the section's rows and the sound parts' types).
std::vector<AutoType> AutoTypesOf(AutoCategory c);
// The kinds of a type, in enum order.
std::vector<AutoKind> AutoKindsOf(AutoType t);

// ---- Helpers for the per-category files --------------------------------------------------------

bool AutoHasRole(const std::vector<int>& role_to_bone, Role r);
void AutoPushOnce(std::vector<std::string>& out, const std::string& s);
// One part's lane: its speed (or horizontal speed) vs `thr`, with its contact mask.
void AutoPartLane(const AutoLaneArgs& args, std::vector<AutoLane>& out, const PartTrack& pt, const char* name,
                  bool horizontal, double thr);
// The window of a per-sample curve (absolute values when `absval`).
std::vector<double> AutoLaneWindow(const std::vector<double>& v, size_t first, size_t n, bool absval);

}  // namespace rav
