// SPDX-License-Identifier: MIT
//
// Auto detection (Epic 10, story 10-8b; hands: 10-8c, 10-8e): foot and hand events from the motion
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
//   pivot      the planted hand turns (Hand Pivot L / R): only after a grab (a hand that came to
//              rest with a grab's approach, or at rest from the clip start; kind hand_pivot, the
//              default), or any resting hand (kind hand_pivot_any).
// Each ticked type x side is one block of the item (Block::auto_type, auto_side, sens), with no
// condition: the rule engine never fires it. Its marker, colour, offset_ms and on/off are the
// block's own, so a preset, pooled copies, the Commit snapshot and Legacy carry it as any block,
// and its events join the item's event list (edits, Commit / Cancel, markers, mirror).
//
// Sensitivity: 0..100 %, 50 = the spike's constants (PhysicsParams defaults). It moves one
// constant per type by f = 2^((s - 50) / 50): step and lift-off contact_speed x f (more
// sensitive = more contacts), slide slide_speed / f, pivot pivot_min_deg and pivot_rate_dps / f,
// grab and release hand_contact_speed x f, hand pivot hand_pivot_min_deg and hand_pivot_rate_dps / f.
// The block's offset (ms) is added on top of the physics' own per-part step offsets.
//
// Values: strength is the physics strength (leg/s for steps, lift-offs, slides, grabs and
// releases, degrees for pivots and hand pivots); speed is strength x the leg length (m/s), 0 for
// pivots and hand pivots.
//
// Pure C++17, deterministic. Host-tested (tests/auto_detect_test.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bone_events.h"
#include "motion_physics.h"
#include "rule_record.h"  // EventEntry

namespace rav {

// ---- Types and block kinds ---------------------------------------------------------------------

// The types the section ticks.
enum class AutoType : int { Step = 0, LiftOff, Slide, Pivot, Grab, Release, HandPivot };
constexpr int kAutoTypeCount = 7;
// "Step", "Lift-off", "Slide scuff", "Pivot scuff", "Grab", "Release", "Pivot scuff" (the hand's)
const char* AutoTypeLabel(AutoType t);

// The section's categories (foldable groups), each holding some types.
enum class AutoCategory : int { Feet = 0, Hands };
constexpr int kAutoCategoryCount = 2;
AutoCategory AutoCategoryOf(AutoType t);              // step, lift-off, slide, pivot: Feet; grab, release, hand pivot: Hands
const char*  AutoCategoryLabel(AutoCategory c);       // "Feet", "Hands"

// The block kinds (Block::auto_type words): a combined step, a separate heel or toe step, a
// lift-off, a slide, a pivot, a grab, a release, a hand pivot after a grab or of any resting hand.
enum class AutoKind : int { Step = 0, Heel, Toe, Lift, Slide, Pivot, Grab, Release, HandPivot, HandPivotAny };
constexpr int kAutoKindCount = 10;
// "step", "heel", "toe", "lift", "slide", "pivot", "grab", "release", "hand_pivot", "hand_pivot_any"
const char* AutoKindWord(AutoKind k);
bool AutoKindFromWord(const std::string& w, AutoKind* out);
AutoType AutoTypeOfKind(AutoKind k);
// The marker name: "FS L", "Heel L", "Toe L", "Lift L", "Slide L", "Pivot L", "Grab L", "Release L",
// "Hand Pivot L" (both hand pivot kinds) (and R).
std::string AutoMarkerName(AutoKind k, char side);
// The default colour (Block::color form: 0x1000000 | 0xRRGGBB), one per kind and side.
uint32_t AutoDefaultColor(AutoKind k, char side);

// A known auto block: its kind and side. False for a rule, or an auto type this version does not know.
bool AutoBlockKind(const Block& b, AutoKind* kind, char* side);

// A fresh auto block of that kind and side (default marker and colour, on, no condition).
Block MakeAutoBlock(AutoKind k, char side, double sens, double offset_ms);

// ---- Settings ----------------------------------------------------------------------------------

constexpr double kAutoDefaultSens = 50.0;

// Story 10-8h: one row's own values before Detect (a block not on the item yet), by its kind
// word (Block::auto_type) and side.
struct AutoRowValue {
    std::string kind;
    char        side = 'L';
    double      sens = kAutoDefaultSens;
    double      offset_ms = 0.0;
};

struct AutoTypeSettings {
    bool   on = false;
    double sens = kAutoDefaultSens;  // %
    double offset_ms = 0.0;
    // Story 10-8h: per-row values set in the section's per-row view (a row not listed takes
    // sens / offset_ms). AutoBlocks gives them to new blocks. Not compared by == (never pending).
    std::vector<AutoRowValue> rows;
};
// Story 10-8h: the row of that kind word and side (null = none).
const AutoRowValue* FindAutoRow(const AutoTypeSettings& t, const std::string& kind, char side);
// Sets one row's sensitivity or offset (the row added, from the type's values, when absent).
void SetAutoRowValue(AutoTypeSettings& t, const std::string& kind, char side, bool sens, double v);

struct AutoSettings {
    AutoTypeSettings type[kAutoTypeCount];
    bool             separate_steps = false;  // steps: heel and toe separate, else combined
    bool             hand_pivot_any = false;  // hand pivots: any resting hand, else only after a grab
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

// What Detect would change: a type ticked or unticked, steps switched between separate and
// combined, or hand pivots between after a grab and any (sensitivity and offset never wait for
// Detect on a detected type).
bool AutoPending(const AutoSettings& ui, const AutoSettings& item);

// The blocks of these settings, per ticked type x side (story 10-8h: a row's own values when the
// type has one for that kind and side), in type order (steps: combined FS, or heel
// then toe; hand pivots: hand_pivot, or hand_pivot_any), left then right.
std::vector<Block> AutoBlocks(const AutoSettings& s);
// The settings the item's known auto blocks say: a type is on when one of its blocks is there,
// its sensitivity and offset are its first block's; steps are separate when heel or toe blocks
// are there and no combined one; hand pivots are any when hand_pivot_any blocks are there and no
// hand_pivot one.
AutoSettings ReadAutoSettings(const std::vector<Block>& blocks);

// Detect: the blocks after these settings. A wanted kind x side already there keeps its block
// (marker, colour, on, and its own sensitivity and offset: story 10-8h, a side keeps its value,
// perhaps raised by Detect; a kind switched drops the raise), the others' blocks go; a new block
// replacing a dropped one of the same type and side takes its start value and offset, not raised,
// new ones are added at the end (AutoBlocks order). Rules and auto blocks of unknown types stay.
// Returns the old -> new block map (-1 = gone) for RemapEventBlocks: switching steps between
// separate and combined keeps the user's edits on that foot (combined -> the heel, heel and toe ->
// combined), and a hand pivot switched between after a grab and any keeps its block (place,
// marker, colour, on/off, edits; only its kind changes); an unticked type's go with its blocks. `changed` (optional): the blocks changed.
std::vector<int> ApplyAutoSettings(std::vector<Block>& blocks, const AutoSettings& s, bool* changed = nullptr);

// ---- Physics parameters ------------------------------------------------------------------------

// 2^((s - 50) / 50), s clamped to 0..100 (a non-finite s reads 50).
double SensitivityFactor(double sens);
// The physics constants of one type at a sensitivity (the defaults otherwise). `looping`: the
// item's "Is looping" (story 10-8i, PhysicsParams::looping).
PhysicsParams AutoPhysicsParams(AutoType t, double sens, bool looping);
// True when two parameter sets give the same analysis (the constants sensitivity moves, and
// looping).
bool SameAutoAnalysis(const PhysicsParams& a, const PhysicsParams& b);

// ---- Detection ---------------------------------------------------------------------------------

struct AutoAnalysis {
    PhysicsParams   params;
    PhysicsAnalysis analysis;
};

// The parameter sets the item's auto blocks need (known types, on), each once, in block order.
// `looping` (story 10-8i, the item's ClipSettings::loop) goes into every set; the functions below
// take it the same way (required: a caller cannot leave the item's setting out). Looping, a
// block's offset wraps round the cycle: its events stay in [0, T).
std::vector<PhysicsParams> AutoParamSets(const std::vector<Block>& blocks, bool looping);

// The events of the auto blocks (Event::block = the block's index, marker = its marker), from the
// analysis of each block's parameter set (a block whose set is not in `analyses`, or whose
// analysis failed, has none). Sorted by time (then block).
std::vector<Event> AutoEvents(const std::vector<AutoAnalysis>& analyses, const std::vector<Block>& blocks,
                              bool looping);

// The whole run on role tracks (indexed by Role, as AnalyseMotion): one analysis per parameter
// set. `cache` (optional, the same role tracks): analyses already run are reused, new ones added.
// No auto block: no analysis, no event.
std::vector<Event> DetectAutoEvents(const std::vector<Block>& blocks, const std::vector<BoneTrack>& role_tracks,
                                    std::vector<AutoAnalysis>* cache, bool looping);

// ---- Sensitivity search (story 10-8h) ---------------------------------------------------------

// One block's search result.
struct AutoSensResult {
    bool   searched = false;  // the block was searched (on, known, its part has a bone, 0 events, analysis ok)
    bool   none = false;      // searched, and no event up to 100 % (sens unchanged)
    bool   raised = false;    // searched, and an event found at `sens`
    double sens = 0.0;        // the block's sensitivity after the search (its own when not raised)
    double sens_from = -1.0;  // raised: the value it was raised from (the block's own sens_from if it had one)
};

// Detect's search, per block: an auto block that is on, of a known type, whose part has a bone
// (AutoBlockPartMissing) and with no detected event (event_counts[i] == 0, the block's detections
// before the user's Suppress edits) is tested at 100 %. Nothing there: `none`, its value
// unchanged. Else a bisection between its value and 100 % finds the lowest whole % with at least
// one event (non-monotonic counts may give a higher boundary: the goal is >= 1). Rules, blocks
// with events, off or unknown blocks, a missing part or a failed analysis: not searched. Uses a
// local analysis cache. One result per block.
std::vector<AutoSensResult> SearchAutoSensitivity(const std::vector<Block>& blocks, const std::vector<BoneTrack>& role_tracks,
                                                  const std::vector<int>& event_counts, const std::vector<int>& role_to_bone,
                                                  bool looping);

// Detect's apply step: each block's detected events counted (before Suppress edits; `cache`
// optional, as DetectAutoEvents), SearchAutoSensitivity run, and each raised block's sens and
// sens_from written. Returns whether a block changed; `results` (optional) gets the search's.
bool ApplyAutoSensSearch(std::vector<Block>& blocks, const std::vector<BoneTrack>& role_tracks,
                         const std::vector<int>& role_to_bone, std::vector<AutoSensResult>* results,
                         std::vector<AutoAnalysis>* cache, bool looping);

// An auto block of a known type and side that is on (it runs; SkippedBlocks' `auto_runs`).
bool IsActiveAutoBlock(const Block& b);
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
//   grab, release, hand pivot        the hand
std::vector<std::string> AutoMissingParts(AutoType t, bool separate, const std::vector<int>& role_to_bone);

// 10-8b fb-1 -- true when a known auto block that is on cannot find its events on this mapping:
// no body scale, or its own part on its side has no bone (combined step, lift-off, slide: no heel,
// toe nor toe end; heel: no heel; toe: no toe nor toe end; pivot: no heel, or no toe nor toe end;
// grab, release, hand pivot: no hand). Its earlier markers are then kept (KeepBlockEvents, event_list.h).
bool AutoBlockPartMissing(const Block& b, const std::vector<int>& role_to_bone);
// Per block: AutoBlockPartMissing (rules: 0).
std::vector<char> AutoBlocksPartMissing(const std::vector<Block>& blocks, const std::vector<int>& role_to_bone);
// The keep step on one item's detections (live view and Commit alike): the blocks that cannot run
// here, the rules `skips` skips (SkippedBlocks, rule_record.h) and the auto blocks whose part has
// no bone on `role_to_bone`, get their last Commit's events back from `entries` (KeepBlockEvents,
// event_list.h). Returns the kept mask (per block).
std::vector<char> KeepUnrunnableEvents(std::vector<Event>& detections, const std::vector<EventEntry>& entries,
                                       const std::vector<Block>& blocks, const std::vector<char>& skipped,
                                       const std::vector<int>& role_to_bone);

}  // namespace rav
