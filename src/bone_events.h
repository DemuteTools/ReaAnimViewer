// SPDX-License-Identifier: MIT
//
// Events from bone motion (Epic 10, spike 10-0): the rule engine behind auto-markers.
//
// A block is a set of conditions combined with AND, plus timing (min hold, cooldown, signed
// offset, all in ms) and a marker name. A condition is a signal, a direction (below /
// above), a threshold and a hysteresis margin. A signal is Bone x Reference x Measure x Axis:
//   bones      one or more tracks, combined (single / average / lowest / highest)
//   reference  the floor (a level), or one or more tracks combined the same way
//   measure    position, speed or acceleration of (bones - reference)
//   axis       vertical (= Y), horizontal (XZ), total (3D), X, Y or Z
// or an angle signal (degrees, scale-free; reference and axis do not apply):
//   joint angle  the flexion at bone b from bones (a, b, c): 180 - the angle a-b-c, so 0 =
//                straight and it rises as the joint bends
//   yaw          the heading of the segment a -> b about the vertical axis (atan2 on X/Z),
//                unwrapped (no jump at +-180)
// with the same measures (position = the angle, speed, acceleration).
// Detection runs offline over the whole clip (it may look ahead). An event fires when the
// AND comes true and lands at the interpolated sub-frame crossing of the condition that
// completed it, plus the block's offset. The first condition is the trigger: one entry of
// it gives at most one event (another condition flickering while the trigger stays in
// never re-fires). Every event keeps its values (strength, speed).
//
// Landing: by default the event lands at that crossing. A block can instead land at the
// peak (max or min, sub-sample, parabolic) of one condition's signal over the span where
// the AND holds that started the event; offset, cooldown, edge margin and strength then
// use that time.
//
// Hysteresis is a margin relative to the threshold: a "below th" condition goes in under
// th and goes out only above th + margin. Re-arming needs both: the trigger must go out
// (past its re-arm level) and come back in, and the next event must come after the
// cooldown. The cooldown alone never re-arms (no marker spam on an idle).
//
// Pure C++17: no GL, no glm, no REAPER, no Windows. Host-tested (tests/bone_events_test.cpp).

#pragma once

#include <string>
#include <vector>

namespace rav {

struct Vec3d {
    double x = 0.0;
    double y = 0.0;  // vertical (model Y up)
    double z = 0.0;
};

// One bone's position per sample, in metres, at a fixed rate. Every track handed to one
// call has the same rate and length.
struct BoneTrack {
    double             rate_hz = 240.0;
    std::vector<Vec3d> pos;
};

enum class Combine { Single, Average, Lowest, Highest };
enum class Reference { Floor, Bones };
enum class Measure { Position, Speed, Acceleration };
enum class Axis { Vertical, Horizontal, Total, X, Y, Z };
enum class Quantity { Point, JointAngle, Yaw };

struct SignalSpec {
    // Point: the bones' (combined) position. JointAngle: bones = {a, b, c}, degrees of
    // flexion at b. Yaw: bones = {a, b}, degrees, unwrapped. Angles ignore combine,
    // reference and axis.
    Quantity         quantity = Quantity::Point;
    std::vector<int> bones;                // indices into the track list (a preset holds roles)
    Combine          combine = Combine::Single;
    Reference        reference = Reference::Floor;
    std::vector<int> ref_bones;            // Reference::Bones
    Combine          ref_combine = Combine::Average;
    double           floor_y = 0.0;        // Reference::Floor: the floor level (m)
    // Reference::Floor, optional: one floor per bone (same order as `bones`). Each bone is
    // measured above its own floor before they are combined (then floor_y adds on top).
    std::vector<double> bone_floors;
    Measure          measure = Measure::Position;
    Axis             axis = Axis::Vertical;
    // Speed / acceleration on a single axis (vertical, X, Y, Z) or of an angle: keep the
    // sign instead of the magnitude. Position components and angles are always signed;
    // horizontal / total are magnitudes.
    bool             keep_sign = false;
};

enum class Direction { Below, Above };

struct Condition {
    SignalSpec signal;
    Direction  dir = Direction::Below;
    double     threshold = 0.0;
    double     margin = 0.0;  // hysteresis, >= 0 (negative reads as 0)
    bool       auto_threshold = true;  // false: Analyse keeps this threshold and margin (a fixed limit)
};

enum class Landing { Crossing, PeakOf };

struct Block {
    std::string            marker;
    std::vector<Condition> conditions;  // AND; an empty block never fires
    double                 min_hold_ms = 0.0;
    double                 cooldown_ms = 0.0;
    double                 offset_ms = 0.0;  // signed: moves the event, not the gate
    // Strength = the peak of strength_sign * strength_signal over the strength window
    // before the crossing. No bones: the total speed of the first condition's signal.
    SignalSpec             strength_signal;
    double                 strength_sign = 1.0;
    double                 strength_window_ms = 100.0;
    // Where the event lands: the AND crossing (default), or the peak of condition
    // peak_condition's signal (its max when peak_max, else its min) while the AND holds.
    Landing                landing = Landing::Crossing;
    int                    peak_condition = 0;
    bool                   peak_max = true;
};

struct DetectOptions {
    double sensitivity = 0.0;     // 0..1: drop events weaker than this share of the block's strongest
    double edge_margin_ms = 0.0;  // drop crossings this close to the clip start / end
    double smooth_ms = 8.0;       // zero-phase Gaussian sigma on positions before derivatives (0 = none)
};

struct Event {
    double      time_s = 0.0;  // clip time: landing (crossing or peak) + offset
    int         block = 0;
    std::string marker;
    double      strength = 0.0;
    double      speed = 0.0;   // total speed of the first condition's signal at the landing (m/s, or deg/s for an angle)
    // When each condition last came true (clip time, before the offset), in condition order.
    // The latest one is the crossing that completed the AND.
    std::vector<double> cond_entry_s;
};

// The signal, one value per sample. Empty when the spec does not fit the tracks.
std::vector<double> EvaluateSignal(const SignalSpec& spec, const std::vector<BoneTrack>& tracks,
                                   double smooth_ms = 8.0);

// Every block over the whole clip, sorted by time. No-throw on bad input (no events).
std::vector<Event> Detect(const std::vector<Block>& blocks, const std::vector<BoneTrack>& tracks,
                          const DetectOptions& opts = {});

struct AnalyseOptions {
    double floor_percentile = 2.0;    // the floor = this percentile of the lowest point
    double position_fraction = 0.25;  // position threshold = this share of the low-high gap from its side
    double speed_percentile = 30.0;   // speed / acceleration "below" threshold = this percentile
    double margin_ratio = 0.5;        // position: margin = ratio * fraction * gap; speed: ratio * |threshold|
    double onset_fraction = 0.1;      // signed speed / acceleration: threshold = this share of its 95th percentile
    bool   per_bone_floor = false;    // Reference::Floor: one floor per bone (bone_floors)
    double smooth_ms = 8.0;
};

// Proposes thresholds from the clip, per condition:
//   conditions with auto_threshold = false are left as they are.
//   any point signal with reference = floor (whatever the measure): floor_y (and bone_floors when
//     per_bone_floor) = the floor percentile of the raw level.
//   position (point with floor or bones reference, or an angle): on the values, low = their floor percentile,
//     high = the median of Otsu's upper cluster, gap = high - low.
//     Below: th = low + fraction * gap. Above: th = high - fraction * gap.
//     Margin = margin_ratio * fraction * gap.
//   speed / acceleration (on magnitudes): below = the speed percentile (at least 1 % of
//     the 95th percentile, so an exactly held pose still counts as still), above = the
//     (100 - speed percentile); margin = margin_ratio * th.
//   signed speed / acceleration (keep_sign, an onset: "starts dropping"): below =
//     -onset_fraction * the 95th percentile of the magnitude, above = +the same;
//     margin = margin_ratio * |th|. Scale-free: it follows the clip's own motion.
// Returns the blocks with those values; everything else is kept.
std::vector<Block> Analyse(const std::vector<Block>& blocks, const std::vector<BoneTrack>& tracks,
                           const AnalyseOptions& opts = {});

// Reference vs detected times (seconds), both counted only inside [lo_s, hi_s]. One-to-one,
// nearest pair first, within +-window_s.
struct MatchResult {
    int    n_ref = 0;
    int    n_det = 0;
    int    n_match = 0;
    double recall = 1.0;     // n_match / n_ref (1 when n_ref = 0)
    double precision = 1.0;  // n_match / n_det (1 when n_det = 0)
    double mean_abs_err_s = 0.0;
    double max_abs_err_s = 0.0;
    double mean_signed_err_s = 0.0;  // det - ref: a steady lead / lag (what an offset fixes)
    std::vector<double> unmatched_ref;
    std::vector<double> unmatched_det;
};

MatchResult MatchEvents(const std::vector<double>& ref, const std::vector<double>& det, double window_s,
                        double lo_s, double hi_s);

// The total over several items (counts summed, errors weighted by matches, max of max).
MatchResult SumMatches(const std::vector<MatchResult>& items);

// NFR-AT1: recall >= 98 %, precision >= 98 %, every matched event within +-16 ms.
constexpr double kGateRecall = 0.98;
constexpr double kGatePrecision = 0.98;
constexpr double kGateMaxErrS = 0.016;
bool PassesAccuracyGate(const MatchResult& m);

}  // namespace rav
