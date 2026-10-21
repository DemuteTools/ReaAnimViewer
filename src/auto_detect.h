// SPDX-License-Identifier: MIT
//
// Smart Detection (Epic 10, story 10-8b; hands: 10-8c, 10-8e): foot and hand events from the motion
// physics (motion_physics.h), as blocks of the item next to its rules. The model only: no ImGui,
// no REAPER.
//
// The user ticks event types in the Tagging view's "Smart Detection" section and presses Detect.
// Each type belongs to one category (AutoCategory), a foldable group of the section.
//   Legs
//   step       heel and toe separate (one marker per part contact: Heel L, Toe L...) or combined
//              (one per landing, at the foot's first part to touch: FS L / FS R; also the part
//              that slaps down at the end of a pivot on the foot's other end). The toe is the
//              ball, or the toe tip on a rig without a ball bone.
//   lift-off   the foot leaves the ground (Lift L / R).
//   slide      a planted part slides (Slide L / R).
//   pivot      the planted foot turns (Pivot L / R).
//   jump       (story 10-9a) the take-off of a jump: the last foot part leaving before both feet
//              are off long enough while the hips fall freely (or after a hard push, or into a
//              free fall: its signatures). Story 10-9a-3: on the foot that leaves last
//              (Jump L / R).
//   land       (story 10-9a) its landing: the first foot touching after it, timed as that foot's
//              step (Land L / R). With Step ticked too, Land replaces the steps (FS / Heel / Toe,
//              either foot) within +-30 ms of it; with Lift-off ticked too, Jump replaces the
//              lift-offs (either foot, any part) within +-30 ms of it (their physics times, before
//              the blocks' offsets). Story 10-8r: a Separate one (Land Heel / Land Toe, Jump
//              Heel / Jump Toe) replaces only its own part's (and the whole-foot kinds'), so a
//              toe landing keeps the heel slap after it. Strength = the hips' vertical speed (leg/s).
//   tap        (story 10-9e) a light toe touch while the other foot carries the body: a foot
//              contact under 150 ms with no heel, the other foot down throughout (Tap L / R),
//              at that contact's step. With Step ticked too, a Tap replaces that foot's steps
//              (FS, Toe) within +-30 ms of it; with Lift-off, its lift-offs over the touch.
//   drag       (story 10-9e) the toe sliding on the ground during a swing, the heel up
//              (Drag L / R, with its end). See foot_taps.h.
//   Story 10-9a-3: lift-off, jump and land each have Combined / Separate
//   (AutoSettings::separate_parts, the types whose row says `part_split`). Separate puts each
//   event on its part's block: Heel = the heel, Toe = the ball or the toe tip (kinds lift_heel,
//   lift_toe, jump_heel, jump_toe, land_heel, land_toe; markers "Lift Heel L", "Jump Toe R",
//   "Land Heel L").
//   Story 10-8m: slide and pivot each have a "Heel / toe separate" mode (AutoSettings::slide_parts,
//   pivot_parts). Combined (the default): one block per side, as above. Separate: a heel block and
//   a toe block per side (Slide Heel L, Slide Toe L, Pivot Heel R, Pivot Toe R...; the toe is the
//   forefoot: the ball, or the toe tip), and either
//     Each part     every part that scuffs gets its own event (kinds slide_heel, slide_toe,
//                   pivot_heel, pivot_toe): the heel's slides and the forefoot's are found
//                   separately (a slide during a pivot of that foot is still the pivot's); a pivot
//                   on one part gives that part's event, a whole-foot pivot both.
//     Longest part  one event per scuff (the combined ones), on the block of the part that holds
//                   longest (PhysicsEvent::longest; kinds slide_heel_longest, slide_toe_longest,
//                   pivot_heel_longest, pivot_toe_longest). A tie goes to the toe.
//   The heel block takes the type's colour, the toe block a shade lighter. Sensitivity and offset
//   work as for the combined type (same AutoType, same constants).
//   Arms
//   grab       the hand lands on something (Grab L / R).
//   release    the hand leaves its support (Release L / R).
//   pivot      the planted hand turns (Hand Pivot L / R): only after a grab (a hand that came to
//              rest with a grab's approach, or at rest from the clip start; kind hand_pivot, the
//              default), or any resting hand (kind hand_pivot_any).
//   clap       (story 10-9d) the two hands meet at speed and stop against each other
//              (hand_touch.h): one marker, on the faster hand (Clap L / R).
//   hand on body (story 10-9d) a hand lands at speed on the trunk, the head or a thigh, on that
//              hand (Hand on body L / R). Strength = the closing speed (leg/s), both.
//   Body
//   jolt       (story 10-9g) an impact that shakes the gear and cloth: a foot step, a jump's
//              landing or a body contact (a knee, a sit, a fall: 10-9b) where the centre of
//              mass's acceleration peaks above the sensitivity's threshold (motion_physics.h
//              JoltEvents). Sideless ("Jolt", side B). Strength = the peak in g.
//   spin       (story 10-9f) the body turns about the vertical: the hip line's yaw rate above
//              15 deg/s starts it, below 5 deg/s (or turning back) ends it, kept when it sweeps
//              45 deg at least. Sideless: two blocks, "Spin" at its start (kind spin) and
//              "Spin End" at its end (kind spin_end). Strength = the sweep (degrees).
// Each ticked type x side is one block of the item (Block::auto_type, auto_side, sens), with no
// condition: the rule engine never fires it. Story 10-9a: a sideless type (its type row's
// `sideless`; none since story 10-9a-3, kept for the Body types) has one block per kind, saved
// side=B, its marker without " L/R" and one colour. A block's marker, colour, offset_ms and
// on/off are the block's own, so a preset, pooled copies, the Commit snapshot and Legacy carry it
// as any block, and its events join the item's event list (edits, Commit / Cancel, markers,
// mirror).
//
// Sensitivity: 0..100 %, 50 = the spike's constants (PhysicsParams defaults). It moves one
// constant per type by f = 2^((s - 50) / 50): step and lift-off contact_speed x f (more
// sensitive = more contacts), slide slide_speed / f, pivot pivot_min_deg and pivot_rate_dps / f,
// grab and release hand_contact_speed x f, hand pivot hand_pivot_min_deg and hand_pivot_rate_dps / f,
// jump and land jump_min_flight_s / f (story 10-9a: higher sensitivity = shorter flights count),
// jolt jolt_min_g / f (story 10-9g: higher sensitivity = softer impacts count).
// tap tap_max_s x f and drag drag_min_s / f (story 10-9e).
// Story 10-9d: clap clap_min_speed / f and clap_reach x f, hand on body body_min_speed / f and
// body_reach x f.
// The arrival speed at the clip end (story 10-8j, 2 x the contact speed) follows contact_speed
// and hand_contact_speed; the min approaches do not move.
// The block's offset (ms) is added on top of the physics' own per-part step offsets.
//
// Values: strength is the physics strength (leg/s for steps, lift-offs, slides, grabs and
// releases, the hips' vertical speed for jumps and lands, degrees for pivots and hand pivots, g for
// jolts); speed is strength x the leg length (m/s), 0 for pivots, hand pivots and jolts (a kind
// row's `strength_unit`).
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

// The types the section ticks (story 10-9e appends Tap and Drag, story 10-9b Knee, Elbow, Sit, Fall,
// story 10-9d Clap and Hand on body).
enum class AutoType : int { Step = 0, LiftOff, Slide, Pivot, Grab, Release, HandPivot, Jump, Land, Jolt, Spin, Tap, Drag,
                            Knee, Elbow, Sit, Fall, Clap, HandOnBody };
constexpr int kAutoTypeCount = 19;  // story 10-9g: Jolt; 10-9f: Spin (Body, sideless); 10-9e: Tap, Drag; 10-9b; 10-9d
// "Step", "Lift-off", "Slide scuff", "Pivot scuff", "Grab", "Release", "Pivot scuff" (the hand's),
// "Jump", "Land", "Jolt" (story 10-9g, Body), "Spin" (story 10-9f, Body), "Tap", "Drag";
// story 10-9b: "Knee", "Elbow", "Sit", "Fall"; story 10-9d: "Clap", "Hand on body"
const char* AutoTypeLabel(AutoType t);

// Story 10-9-0: every type, category and kind has one descriptor row in auto_types.cpp (labels,
// words, marker names, colours, sensitivity constants, events, parts, lanes); the functions below
// read it.

// The section's categories (foldable groups), each holding some types: the body part that makes
// the sound (story 10-9-1). Body (trunk, head): Jolt (story 10-9g), Spin (10-9f), sit and fall (story 10-9b).
enum class AutoCategory : int { Legs = 0, Arms, Body };
constexpr int kAutoCategoryCount = 3;
AutoCategory AutoCategoryOf(AutoType t);              // step, lift-off, slide, pivot, jump, land, knee: Legs; grab, release, hand pivot, elbow: Arms; sit, fall: Body
const char*  AutoCategoryLabel(AutoCategory c);       // "Legs", "Arms", "Body"

// The block kinds (Block::auto_type words): a combined step, a separate heel or toe step, a
// lift-off, a slide, a pivot, a grab, a release, a hand pivot after a grab or of any resting hand.
// Story 10-8m appends the slides and pivots by part (Each part, then Longest part); story 10-9a
// jump and land; story 10-9a-3 the lift-off, jump and land parts (Separate); story 10-9b knee,
// elbow, sit, fall.
enum class AutoKind : int {
    Step = 0, Heel, Toe, Lift, Slide, Pivot, Grab, Release, HandPivot, HandPivotAny,
    SlideHeel, SlideToe, PivotHeel, PivotToe, SlideHeelLongest, SlideToeLongest, PivotHeelLongest, PivotToeLongest,
    Jump, Land, LiftHeel, LiftToe, JumpHeel, JumpToe, LandHeel, LandToe, Jolt,
    Spin, SpinEnd,  // story 10-9f: a spin's start ("Spin") and end ("Spin End")
    Tap, Drag,      // story 10-9e
    Knee, Elbow, Sit, Fall,  // story 10-9b
    Clap, HandOnBody         // story 10-9d
};
constexpr int kAutoKindCount = 37;
// "step", "heel", "toe", "lift", "slide", "pivot", "grab", "release", "hand_pivot", "hand_pivot_any",
// "slide_heel", "slide_toe", "pivot_heel", "pivot_toe", "slide_heel_longest", "slide_toe_longest",
// "pivot_heel_longest", "pivot_toe_longest", "jump", "land", "lift_heel", "lift_toe", "jump_heel",
// "jump_toe", "land_heel", "land_toe", "jolt" (story 10-9g), "spin", "spin_end" (10-9f), "tap", "drag",
// "knee", "elbow", "sit", "fall" (10-9b), "clap", "hand_body" (10-9d)
const char* AutoKindWord(AutoKind k);
bool AutoKindFromWord(const std::string& w, AutoKind* out);
AutoType AutoTypeOfKind(AutoKind k);
// The marker name: "FS L", "Heel L", "Toe L", "Lift L", "Slide L", "Pivot L", "Grab L", "Release L",
// "Hand Pivot L" (both hand pivot kinds), "Slide Heel L", "Slide Toe L", "Pivot Heel L", "Pivot Toe L"
// (both part modes), "Jump L", "Land L", "Lift Heel L", "Jump Toe L", "Land Heel L"... (and R),
// "Knee L", "Elbow L"; a sideless kind's has no side (any `side`): "Sit", "Fall".
std::string AutoMarkerName(AutoKind k, char side);
// The default colour (Block::color form: 0x1000000 | 0xRRGGBB), one per kind and side (a sideless
// kind: one).
uint32_t AutoDefaultColor(AutoKind k, char side);

// Story 10-9a: a kind of a sideless type (its type row's `sideless`; story 10-9g: jolt).
bool AutoKindSideless(AutoKind k);
// The sides of a kind's blocks: "LR", or "B" for a sideless kind.
const char* AutoKindSides(AutoKind k);

// A known auto block: its kind and side. False for a rule, or an auto type this version does not
// know, or a side its kind does not have (L / R, or B for a sideless kind: a jump / land block
// saved side=B before story 10-9a-3 is no known block, and Detect drops it).
bool AutoBlockKind(const Block& b, AutoKind* kind, char* side);

// A fresh auto block of that kind and side (default marker and colour, on, no condition). A
// sideless kind's block takes side B whatever `side` says.
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

// Story 10-8m: a foot scuff type's mode (see the header).
enum class ScuffParts : int { Combined = 0, EachPart, Longest };

struct AutoSettings {
    AutoTypeSettings type[kAutoTypeCount];
    bool             separate_steps = false;  // steps: heel and toe separate, else combined
    bool             hand_pivot_any = false;  // hand pivots: any resting hand, else only after a grab
    ScuffParts       slide_parts = ScuffParts::Combined;
    ScuffParts       pivot_parts = ScuffParts::Combined;
    // Story 10-9a-3: Separate (heel / toe blocks) for the types whose row says `part_split`
    // (lift-off, jump, land), by AutoType; ignored for the others.
    bool             separate_parts[kAutoTypeCount] = {};
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
// combined, slides or pivots between their part modes, a `part_split` type (story 10-9a-3)
// between Combined and Separate, or hand pivots between after a grab and any (sensitivity and
// offset never wait for Detect on a detected type).
bool AutoPending(const AutoSettings& ui, const AutoSettings& item);

// A type's own "separate" mode (AutoMissingParts' `separate`): steps, slides and pivots (steps
// separate_steps, slides and pivots a part mode other than Combined) and the `part_split` types
// (story 10-9a-3: separate_parts); false for every other type. Pivots' required parts ignore it
// (a pivot always needs the heel and the toe).
bool AutoTypeSeparate(AutoType t, const AutoSettings& s);

// The blocks of these settings, per ticked type x side (a sideless kind: one, side B; story
// 10-8h: a row's own values when the type has one for that kind and side), in type order
// (steps: combined FS, or heel then toe; a `part_split` type: its combined kind, or heel then
// toe; hand pivots: hand_pivot, or hand_pivot_any), left then right.
std::vector<Block> AutoBlocks(const AutoSettings& s);
// The settings the item's known auto blocks say: a type is on when one of its blocks is there,
// its sensitivity and offset are its first block's; steps are separate when heel or toe blocks
// are there and no combined one; hand pivots are any when hand_pivot_any blocks are there and no
// hand_pivot one; a slide's / pivot's mode is Each part or Longest part when only blocks of that
// mode are there (else combined); a `part_split` type is separate when its heel or toe blocks are
// there and no combined one (story 10-9a-3).
AutoSettings ReadAutoSettings(const std::vector<Block>& blocks);

// Detect: the blocks after these settings. A wanted kind x side already there keeps its block
// (marker, colour, on, and its own sensitivity and offset: story 10-8h, a side keeps its value,
// perhaps raised by Detect; a kind switched drops the raise), the others' blocks go; a new block
// replacing a dropped one of the same type and side takes its start value and offset, not raised,
// new ones are added at the end (AutoBlocks order). Rules and auto blocks of unknown types stay.
// Returns the old -> new block map (-1 = gone) for RemapEventBlocks: switching steps between
// separate and combined keeps the user's edits on that foot (combined -> the heel, heel and toe ->
// combined), and a hand pivot switched between after a grab and any keeps its block (place,
// marker, colour, on/off, edits; only its kind changes), and so does a slide / pivot part block
// switched between Each part and Longest part (story 10-8m); a slide / pivot, or a `part_split`
// type (story 10-9a-3: lift-off, jump, land), switched between combined and separate maps the
// edits as steps do (combined -> heel, heel and toe -> combined); an unticked type's go with its
// blocks. Story 10-9a-3: an auto block of a known kind on a side that kind does not have (a jump /
// land block saved side=B in development) is dropped. `changed` (optional): the blocks changed.
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
// analysis failed, has none). Sorted by time (then block). Story 10-9a: a Land event replaces the
// step blocks' events (combined, heel, toe) within kAutoReplaceS of it, a Jump event the lift-off
// blocks' events, both on their physics times (before the blocks' offsets); story 10-8r: a part
// kind (role key heel / toe) only those of a kind with the same role key or a whole-foot kind.
// Story 10-9a-2: every signature of a block's kind runs (AutoKindInfo::sigs), merged per block
// by MergeSignatureEvents; Event::signature = the signature that found it.
constexpr double kAutoReplaceS = 0.030;
// Story 10-9a-2: the merge window of a kind's signatures (wider than kAutoReplaceS: two
// signatures time the same take-off from different signals).
constexpr double kAutoMergeS = 0.060;
// One kept event of MergeSignatureEvents: its signature and its index in that signature's list.
struct AutoSigPick {
    int    signature = 0;
    size_t index = 0;
};
// Story 10-9a-2: the merge of one block's signatures. `times[s]` = the physics times (before the
// block's offset) of signature s's events; signature by signature, an event within kAutoMergeS
// of one kept from an EARLIER signature is dropped (through the seam when `period` > 0, a loop's
// period), so a signature never loses its own events and the earlier one gives the time (an
// appended signature never moves an existing event; its new events can still remove steps /
// lift-offs through the Land / Jump replacement). The kept events, signature by signature, each
// list in its order.
std::vector<AutoSigPick> MergeSignatureEvents(const std::vector<std::vector<double>>& times, double period);
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
// with events, off or unknown blocks, a missing part or a failed analysis: not searched. Story
// 10-9a-3: nor a block whose group holds the event: another active block of the same `part_split`
// type and side has events (Lift Heel L is silent when the toe leaves last), or, for a type whose
// row says `shared_sides` (jump, land: one event per jump for both feet), another active block of
// that type on either side. Story 10-9g: a type whose row says `no_sens_raise` (jolt) is never
// raised: `searched`, not `none`, its value unchanged. Uses a
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
//   slide separate (story 10-8m)     the heel; the toe (or the toe end)
// (`separate` is the type's own mode: separate_steps for steps, slide_parts != Combined for slides.)
//   grab, release, hand pivot        the hand
//   lift-off separate (10-9a-3)      the heel; the toe (or the toe end)
//   jump, land (story 10-9a)         the hips; per side, the heel, toe or toe end (as a lift-off;
//                                    separate: the heel and the toe or toe end)
//   tap, drag (story 10-9e)          the heel and the toe (or the toe end), as a pivot
std::vector<std::string> AutoMissingParts(AutoType t, bool separate, const std::vector<int>& role_to_bone);

// 10-8b fb-1 -- true when a known auto block that is on cannot find its events on this mapping:
// no body scale, or its own part on its side has no bone (combined step, lift-off, slide: no heel,
// toe nor toe end; heel, slide heel: no heel; toe, slide toe: no toe nor toe end; every pivot kind:
// no heel, or no toe nor toe end;
// grab, release, hand pivot: no hand; jump, land: no hips, or a foot with no heel, toe nor toe
// end; story 10-9a-3: lift heel / toe as heel / toe; jump / land heel / toe as jump, or that
// side's heel / toe missing). Its earlier markers are then kept (KeepBlockEvents, event_list.h).
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

// ---- Signal lanes (story 10-8s) ---------------------------------------------------------------

// One read-only lane of the Tagging strip's "Signals" view: a signal the block reads and its
// threshold (the analysis params' value, so the block's sensitivity), over the clip window
// (window_first .. + window_samples, at the analysis' rate_hz).
struct AutoLane {
    std::string         label;     // "Heel L  speed"
    std::string         unit;      // "leg/s" or "deg/s"
    std::vector<double> curve;     // per window sample
    std::vector<char>   contact;   // per window sample (empty for pivot lanes)
    double              threshold = 0.0;
    bool                above = false;  // the block fires above the line (slide, pivots), else below
    double              rate_hz = 0.0;  // the analysis' sample rate
    // Story 10-9a: a lane with no threshold (the jumps' hips vertical speed, signed: no line, the
    // range from the curve), and what its shading means ("" = the part's contact).
    bool                no_threshold = false;
    std::string         shade_note;
};

// The lanes of one auto block from its analysis: combined step and lift-off, each present foot
// part's speed vs contact_speed; heel, the heel; toe, the ball (else the tip); slide, each present
// part's hspeed vs slide_speed; pivot, |yaw rate| (yaw where valid, else heading where valid, else
// 0) vs pivot_rate_dps; grab and release, the hand's speed vs hand_contact_speed; hand pivot (both
// kinds), |heading rate| vs hand_pivot_rate_dps; jump and land (story 10-9a), the hips' vertical
// speed (no threshold), shaded over the flights both feet are off; tap (story 10-9e) as toe, drag
// the ball's (else the tip's) hspeed vs drag_speed, shaded where it drags. Empty for a rule, an unknown
// kind, a failed analysis or an absent part; `why` (optional) then gets the reason ("" for a
// rule or unknown kind).
std::vector<AutoLane> AutoBlockLanes(const AutoAnalysis& aa, const Block& b, std::string* why = nullptr);

}  // namespace rav
