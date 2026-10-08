// SPDX-License-Identifier: MIT
//
// The Tagging view's words and units for a condition (Story 10-3): what the inspector's
// sentence menus offer, how a signal's value reads on screen, and how an item maps its
// clip time onto project time (the strip's axis).
//
// Units (display = engine value * scale):
//   position of a point   cm     (the engine works in metres)
//   speed                 m/s
//   acceleration          m/s2
//   angle                 deg    (bend / turn / joint angle / rotation)
//   angle speed           deg/s
//   angle acceleration    deg/s2
//   stillness             cm     (deg on an angle; spike 10-7a)
//   relative drop         %      (the engine works in 0..1)
//
// Pure C++17: no REAPER, no ImGui, no Windows. Host-tested (tests/tagging_signal_test.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bone_events.h"

namespace rav {

// ---- Units -------------------------------------------------------------------------------
enum class SignalUnit {
    Centimetre,
    MetrePerSecond,
    MetrePerSecond2,
    Degree,
    DegreePerSecond,
    DegreePerSecond2,
    Percent,  // spike 10-7a: relative drop
};

SignalUnit UnitOf(const SignalSpec& spec);
const char* UnitLabel(SignalUnit u);  // UTF-8: "cm", "m/s", "m/s\xC2\xB2", "\xC2\xB0", "%"...
double UnitScale(SignalUnit u);       // display = engine value * scale
int UnitDecimals(SignalUnit u);       // how many decimals a value shows
double UnitDragStep(SignalUnit u);    // display units per pixel of a sideways drag

double ToDisplay(const SignalSpec& spec, double engine_value);
double FromDisplay(const SignalSpec& spec, double display_value);
// The number in display units, its decimals, no unit ("5.0", "-0.25", "120").
std::string FormatDisplayNumber(const SignalSpec& spec, double engine_value);
// The same with its unit ("5.0 cm", "120 \xC2\xB0/s").
std::string FormatDisplay(const SignalSpec& spec, double engine_value);

// ---- Sentence menus ----------------------------------------------------------------------
// Stillness and Relative drop (spike 10-7a) are offered too, with their default windows.
constexpr Measure kMeasureChoices[5] = {Measure::Position, Measure::Speed, Measure::Acceleration, Measure::Stillness,
                                        Measure::RelativeDrop};
constexpr Axis kAxisChoices[6] = {Axis::Vertical, Axis::Horizontal, Axis::Total, Axis::X, Axis::Y, Axis::Z};
constexpr Direction kDirectionChoices[2] = {Direction::Below, Direction::Above};

const char* MeasureLabel(Measure m);      // "Position", "Speed", "Acceleration", "Stillness", "Relative drop"
// What a measure reads, for the menu's tooltip, with its default window ("" for position,
// speed, acceleration).
std::string MeasureHint(Measure m);
const char* AxisLabel(Axis a);            // "vertical", "horizontal", "total", "X", "Y", "Z"
const char* DirectionLabel(Direction d);  // "below", "above"
bool IsAngleSignal(const SignalSpec& spec);  // in degrees: bend, turn, joint angle, rotation

// ---- Condition kinds (10-4 follow-up) ------------------------------------------------------
// The kind chip at the head of a condition's sentence: Bone (a point: position, speed,
// acceleration), Joint angle (the interior angle at a joint, 180 = straight), Rotation (a
// bone's rotation on an axis, from its parent or the world). Bend / Turn are the older
// flexion (q=joint) and yaw (q=yaw) conditions: shown as they are, never offered.
enum class ConditionKind { Bone, JointAngle, Rotation, Bend, Turn };
constexpr ConditionKind kConditionKindChoices[3] = {ConditionKind::Bone, ConditionKind::JointAngle,
                                                    ConditionKind::Rotation};
ConditionKind ConditionKindOf(const SignalSpec& spec);
const char* ConditionKindLabel(ConditionKind k);  // "Bone", "Joint angle", "Rotation", "Bend", "Turn"
// Resets `c` to the kind's defaults (one gesture). The bone is kept when there is one to
// keep (the condition's first bone; a joint or yaw keeps its middle / first bone), else the
// kind's default bone. Bone: as DefaultCondition. Joint angle: angle below 90 deg, margin
// 5 deg (default joint: the left knee). Rotation: total speed from its parent above 300
// deg/s, margin 50 deg/s (default bone: the left toe). Bend / Turn read as Bone.
void SetConditionKind(Condition& c, ConditionKind k);
// The Measure chip's label for this signal: a joint angle reads "angle", "angle speed",
// "angle acceleration", "angle stillness", "angle relative drop"; a rotation "Angle",
// "Speed", "Acceleration", "Stillness", "Relative drop"; else MeasureLabel.
const char* MeasureLabelFor(const SignalSpec& spec, Measure m);
// A rotation's axis choices: X, Y, Z, and total (speed / acceleration only: an angle,
// stillness and relative drop have no total).
constexpr Axis kRotationAxisChoices[4] = {Axis::X, Axis::Y, Axis::Z, Axis::Total};
bool RotationAxisOffered(Measure m, Axis a);
// The axis a rotation reads (an older or odd value mapped: total / horizontal on a position
// = X, vertical = Y), as the engine reads it (bone_events.h): stillness or relative drop on
// total / horizontal reads total, which does not fit.
Axis RotationAxisOf(const SignalSpec& spec);
// The Reference chip of a rotation: "its parent" / "the world".
const char* RotationReferenceLabel(Reference r);

// Where the marker lands ("Place the marker at [the start | the highest point | the lowest
// point] of [signal]").
enum class Placement { Start, Highest, Lowest };
constexpr Placement kPlacementChoices[3] = {Placement::Start, Placement::Highest, Placement::Lowest};
const char* PlacementLabel(Placement p);  // "the start", "the highest point", "the lowest point"
Placement PlacementOf(const Block& b);
// Start = the crossing; Highest / Lowest = the peak of condition `of` (kept in range).
void SetPlacement(Block& b, Placement p, int of);

// ---- New rules ---------------------------------------------------------------------------
// The bone reference id a new condition reads (the left heel role, rule_record.h ids).
constexpr int kDefaultConditionBone = 0;
// A new condition: the left heel's height, below 5 cm, margin 2 cm (Analyse may change it).
Condition DefaultCondition();
// A new rule: one default condition, crossing landing, cooldown 250 ms.
Block DefaultBlock(const std::string& marker, uint32_t color);
// Removing condition `index` keeps the peak landing on the same signal (or the first one).
void RemoveCondition(Block& b, size_t index);

// ---- Binding to bone tracks --------------------------------------------------------------
// After BindBoneRefs (rule_record.h) the blocks hold skeleton bone indices. This turns them
// into indices of the tracks to sample: `out_bones` gets each skeleton bone once, in first-use
// order (conditions, references, strength), and every index becomes its place in that list.
// An index < 0 stays as it is (the blocks then do not bind).
void RemapToTracks(std::vector<Block>& bound, std::vector<int>* out_bones);

// Analyse ran on `analysed` (bound copies of `record`'s blocks, same shape): copies back what
// it proposes (each condition's threshold, margin and floors, the strength signal's floors).
// Bone references, Fixed conditions (Analyse leaves them alone) and everything else stay.
void CopyAnalysedValues(const std::vector<Block>& analysed, std::vector<Block>& record);

// ---- Clip time on the project timeline ---------------------------------------------------
// An item: its position and length (project time), the take's start offset and play rate,
// and the clip's length; a looped item repeats the clip.
struct ItemClipMap {
    double item_pos = 0.0;
    double item_len = 0.0;
    double start_offs = 0.0;
    double rate = 1.0;
    double clip_len = 0.0;
    bool   loop = false;
};

// One pass of the clip over the item: project [p0, p1) shows clip time clip0 at p0, and
// clip0 + (p - p0) * rate at p.
struct ClipPass {
    double p0 = 0.0;
    double p1 = 0.0;
    double clip0 = 0.0;
};

// The passes, in time order (one when the item does not loop, none when it shows nothing of
// the clip). At most 4096 passes.
std::vector<ClipPass> ClipPasses(const ItemClipMap& m);
// The project times where clip time t shows (one per pass), ascending.
std::vector<double> ClipToProjectTimes(const ItemClipMap& m, double clip_t);
// The clip time at project time p. False when the item shows no clip there.
bool ProjectToClipTime(const ItemClipMap& m, double p, double* out_clip_t);

// The sample position (fractional index into a signal sampled at rate_hz from clip time 0)
// of project time t within pass `p`, `rate` = the take's play rate (<= 0 reads as 1). Equals
// ProjectToClipTime(t) * rate_hz inside the pass.
double SamplePosInPass(const ClipPass& p, double t, double rate, double rate_hz);
// A signal's value at a fractional sample position (linear, held at both ends; 0 when empty).
double InterpSample(const std::vector<double>& s, double pos);

}  // namespace rav
