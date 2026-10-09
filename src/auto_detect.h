// SPDX-License-Identifier: MIT
//
// Auto detection (Epic 10, story 10-8b; hands: 10-8c): foot and hand events from the motion
// physics (motion_physics.h), as blocks of the item next to its rules. The model only: no ImGui,
// no REAPER.
//
// The user ticks event types in the Tagging view's "Auto detection" section and presses Detect.
// Each type belongs to one category (AutoCategory), a foldable group of the section.
//   Feet
//   step       heel and toe separate (one marker per part contact: Heel L, Toe L...) or combined
//              (one per landing, at the foot's first part to touch: FS L / FS R). The toe is the
//              ball, or the toe tip on a rig without a ball bone.
//   lift-off   the foot leaves the ground (Lift L / R).
//   slide      a planted part slides (Slide L / R).
//   pivot      the planted foot turns (Pivot L / R).
//   Hands
//   grab       the hand lands on something (Grab L / R).
//   release    the hand leaves its support (Release L / R).
// Each ticked type x side is one block of the item (Block::auto_type, auto_side, sens), with no
// condition: the rule engine never fires it. Its marker, colour, offset_ms and on/off are the
// block's own, so a preset, pooled copies, the Commit snapshot and Legacy carry it as any block,
// and its events join the item's event list (edits, Commit / Cancel, markers, mirror).
//
// Sensitivity: 0..100 %, 50 = the spike's constants (PhysicsParams defaults). It moves one
// constant per type by f = 2^((s - 50) / 50): step and lift-off contact_speed x f (more
// sensitive = more contacts), slide slide_speed / f, pivot pivot_min_deg and pivot_rate_dps / f,
// grab and release hand_contact_speed x f.
// The block's offset (ms) is added on top of the physics' own per-part step offsets.
//
// Values: strength is the physics strength (leg/s for steps, lift-offs, slides, grabs and
// releases, degrees for pivots); speed is strength x the leg length (m/s), 0 for pivots.
//
// Pure C++17, deterministic. Host-tested (tests/auto_detect_test.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bone_events.h"
#include "motion_physics.h"

namespace rav {

// ---- Types and block kinds ---------------------------------------------------------------------

// The types the section ticks.
enum class AutoType : int { Step = 0, LiftOff, Slide, Pivot, Grab, Release };
constexpr int kAutoTypeCount = 6;
// "Step", "Lift-off", "Slide scuff", "Pivot scuff", "Grab", "Release"
const char* AutoTypeLabel(AutoType t);

// The section's categories (foldable groups), each holding some types.
enum class AutoCategory : int { Feet = 0, Hands };
constexpr int kAutoCategoryCount = 2;
AutoCategory AutoCategoryOf(AutoType t);              // step, lift-off, slide, pivot: Feet; grab, release: Hands
const char*  AutoCategoryLabel(AutoCategory c);       // "Feet", "Hands"

// The block kinds (Block::auto_type words): a combined step, a separate heel or toe step, a
// lift-off, a slide, a pivot, a grab, a release.
enum class AutoKind : int { Step = 0, Heel, Toe, Lift, Slide, Pivot, Grab, Release };
constexpr int kAutoKindCount = 8;
// "step", "heel", "toe", "lift", "slide", "pivot", "grab", "release"
const char* AutoKindWord(AutoKind k);
bool AutoKindFromWord(const std::string& w, AutoKind* out);
AutoType AutoTypeOfKind(AutoKind k);
// The marker name: "FS L", "Heel L", "Toe L", "Lift L", "Slide L", "Pivot L", "Grab L", "Release L"
// (and R).
std::string AutoMarkerName(AutoKind k, char side);
// The default colour (Block::color form: 0x1000000 | 0xRRGGBB), one per kind and side.
uint32_t AutoDefaultColor(AutoKind k, char side);

// A known auto block: its kind and side. False for a rule, or an auto type this version does not know.
bool AutoBlockKind(const Block& b, AutoKind* kind, char* side);

// A fresh auto block of that kind and side (default marker and colour, on, no condition).
Block MakeAutoBlock(AutoKind k, char side, double sens, double offset_ms);

// ---- Settings ----------------------------------------------------------------------------------

constexpr double kAutoDefaultSens = 50.0;

struct AutoTypeSettings {
    bool   on = false;
    double sens = kAutoDefaultSens;  // %
    double offset_ms = 0.0;
};

struct AutoSettings {
    AutoTypeSettings type[kAutoTypeCount];
    bool             separate_steps = false;  // steps: heel and toe separate, else combined
};
bool operator==(const AutoTypeSettings& a, const AutoTypeSettings& b);
bool operator==(const AutoSettings& a, const AutoSettings& b);
inline bool operator!=(const AutoSettings& a, const AutoSettings& b)
{
    return !(a == b);
}

// Categories. A category is shown when the item has one of its known auto blocks, or when the
// user added it (+) in this session; its x unticks its types (they wait for Detect).
bool AutoCategoryInBlocks(const std::vector<Block>& blocks, AutoCategory c);
int  AutoCategoryOnCount(const AutoSettings& s, AutoCategory c);  // its ticked types
void UntickAutoCategory(AutoSettings& s, AutoCategory c);

// What Detect would change: a type ticked or unticked, or steps switched between separate and
// combined (sensitivity and offset never wait for Detect on a detected type).
bool AutoPending(const AutoSettings& ui, const AutoSettings& item);

// The blocks of these settings, per ticked type x side, in type order (steps: combined FS, or heel
// then toe), left then right.
std::vector<Block> AutoBlocks(const AutoSettings& s);
// The settings the item's known auto blocks say: a type is on when one of its blocks is there,
// its sensitivity and offset are its first block's; steps are separate when heel or toe blocks
// are there and no combined one.
AutoSettings ReadAutoSettings(const std::vector<Block>& blocks);

// Detect: the blocks after these settings. A wanted kind x side already there keeps its block
// (marker, colour, on; sensitivity and offset set from the settings), the others' blocks go,
// new ones are added at the end (AutoBlocks order). Rules and auto blocks of unknown types stay.
// Returns the old -> new block map (-1 = gone) for RemapEventBlocks: switching steps between
// separate and combined keeps the user's edits on that foot (combined -> the heel, heel and toe ->
// combined); an unticked type's go with its blocks. `changed` (optional): the blocks changed.
std::vector<int> ApplyAutoSettings(std::vector<Block>& blocks, const AutoSettings& s, bool* changed = nullptr);

// ---- Physics parameters ------------------------------------------------------------------------

// 2^((s - 50) / 50), s clamped to 0..100 (a non-finite s reads 50).
double SensitivityFactor(double sens);
// The physics constants of one type at a sensitivity (the defaults otherwise).
PhysicsParams AutoPhysicsParams(AutoType t, double sens);
// True when two parameter sets give the same analysis (the constants sensitivity moves).
bool SameAutoAnalysis(const PhysicsParams& a, const PhysicsParams& b);

// ---- Detection ---------------------------------------------------------------------------------

struct AutoAnalysis {
    PhysicsParams   params;
    PhysicsAnalysis analysis;
};

// The parameter sets the item's auto blocks need (known types, on), each once, in block order.
std::vector<PhysicsParams> AutoParamSets(const std::vector<Block>& blocks);

// The events of the auto blocks (Event::block = the block's index, marker = its marker), from the
// analysis of each block's parameter set (a block whose set is not in `analyses`, or whose
// analysis failed, has none). Sorted by time (then block).
std::vector<Event> AutoEvents(const std::vector<AutoAnalysis>& analyses, const std::vector<Block>& blocks);

// The whole run on role tracks (indexed by Role, as AnalyseMotion): one analysis per parameter
// set. `cache` (optional, the same role tracks): analyses already run are reused, new ones added.
// No auto block: no analysis, no event.
std::vector<Event> DetectAutoEvents(const std::vector<Block>& blocks, const std::vector<BoneTrack>& role_tracks,
                                    std::vector<AutoAnalysis>* cache = nullptr);

// True when the blocks hold an auto block of a known type that is on (detection needs the
// physics role tracks).
bool HasActiveAutoBlocks(const std::vector<Block>& blocks);

// ---- Missing parts -----------------------------------------------------------------------------

// The roles a type needs that have no bone (role_to_bone indexed by Role, -1 = none), by name,
// each once: the body scale first (heel, knee and up leg, or the hips, of one side at least, or
// else the knee and up leg of one side, twice the thigh; each side's missing heel, knee and up
// leg when neither works), then the type's parts, per side:
//   step combined, lift-off, slide   the heel, toe or toe end (all listed when none has a bone)
//   step separate                    the heel; the toe (or the toe end)
//   pivot                            the heel and the toe (or the toe end)
//   grab, release                    the hand
std::vector<std::string> AutoMissingParts(AutoType t, bool separate, const std::vector<int>& role_to_bone);

}  // namespace rav
