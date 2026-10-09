// SPDX-License-Identifier: MIT
//
// Events from bone motion (Epic 10, spike 10-0): the rule engine behind auto-markers.
//
// A block is a set of conditions combined with AND, plus timing (min hold, cooldown, signed
// offset, all in ms) and a marker name. A condition is a signal, a direction (below /
// above), a threshold and a hysteresis margin. A signal is Bone x Reference x Measure x Axis:
//   bones      one or more tracks, combined (single / average / lowest / highest)
//   reference  the floor (a level), or one or more tracks combined the same way
//   measure    position, speed or acceleration of (bones - reference), or (spike 10-7a)
//              stillness or relative drop:
//                stillness      at each sample, the radius the point stays within over the
//                               window that starts there (SignalSpec::window_ms, default 150
//                               ms, at least one sample; near the clip end, the last full
//                               window), on the axis: the largest distance from the window's
//                               mid-range point (one component: half its range). Raw
//                               positions, no smoothing. Metres. "Below th" comes true as a
//                               still stretch starts (sub-frame), ahead of it by the time the
//                               approach takes to cover 2 th.
//                relative drop  the speed on the axis (a magnitude, smoothed as speed) over
//                               its peak in the approach window that ends at this sample
//                               (window_ms, default 300 ms, clamped to the clip). The peak is
//                               floored at 10 % of the clip's 95th-percentile speed, so a bone
//                               resting after a move reads near 0 (a peak under 1e-9 per
//                               second, an exact hold's rounding, reads 0); a bone that only
//                               jitters for the whole clip still compares noise with noise.
//                               0..1: "below 0.2" = the speed fell to 20 % of the approach.
//                               On one component (vertical...) the speed also drops where the
//                               motion turns back (the top of a lift).
//              Both are magnitudes (keep_sign does not apply).
//   axis       vertical (= Y), horizontal (XZ), total (3D), X, Y or Z
// or an angle signal (degrees, scale-free; reference and axis do not apply):
//   joint angle  the flexion at bone b from bones (a, b, c): 180 - the angle a-b-c, so 0 =
//                straight and it rises as the joint bends
//   yaw          the heading of the segment a -> b about the vertical axis (atan2 on X/Z),
//                unwrapped (no jump at +-180)
//   interior     (10-4 follow-up) the interior angle at bone b from bones (a, b, c): the
//                angle a-b-c itself, 180 = straight, smaller = more bent. A record holds
//                the joint b only; binding adds its parent a and its first child c.
//   rotation     (10-4 follow-up) one bone's orientation (BoneTrack::rot_world /
//                rot_parent), reference = Parent (its parent) or Floor (the world). Value
//                = the XYZ Euler angle on axis X / Y / Z (rotate order XYZ, R = Rz Ry Rx,
//                like a 3D tool's rotate channels), unwrapped. Speed / acceleration on
//                X / Y / Z = the derivatives of those; on total = the magnitude of the
//                angular velocity (its derivative for acceleration). Position on total
//                (or horizontal) reads as X; vertical reads as Y. Stillness and relative
//                drop have no total: on total (or horizontal) the signal does not fit.
// with the same measures (position = the angle, speed, acceleration, stillness = half the
// angle's range over the window, relative drop of the angle speed).
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

#include <cstdint>
#include <string>
#include <vector>

namespace rav {

struct Vec3d {
    double x = 0.0;
    double y = 0.0;  // vertical (model Y up)
    double z = 0.0;
};

// A rotation as a unit quaternion (w, x, y, z).
struct Quatd {
    double w = 1.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// One bone's position per sample, in metres, at a fixed rate. Every track handed to one
// call has the same rate and length. Orientations (10-4 follow-up) are optional: empty
// when not sampled (an offline CSV dump holds positions only), else one per sample.
struct BoneTrack {
    double             rate_hz = 240.0;
    std::vector<Vec3d> pos;
    std::vector<Quatd> rot_world;   // the bone's orientation in model space
    std::vector<Quatd> rot_parent;  // relative to its parent: its local rotation (a root's own)
};

// Text a newer RAV wrote on an object's record line that this version does not read (story
// 10-2, rule_record.h). The object carries it, so it moves with the object and is dropped
// with it. Detection and the equality checks ignore it.
struct KeptField {
    std::string key;                 // "" = a bare token
    std::string raw;                 // the value as written
    bool        known = false;       // a key this version reads...
    bool        read = false;        // ...and its value read
    // A known key whose value did not read: the model's value for it right after the read
    // (as written, `parsed_present` = the field was written at all). Its raw text is kept
    // only while the model still holds that value: an edit wins.
    bool        parsed_present = false;
    std::string parsed;
};

struct KeptText {
    std::vector<KeptField>   fields;  // the line's fields in order, when one was not understood
    std::vector<std::string> lines;   // unknown lines read right after this object's line
    bool empty() const { return fields.empty() && lines.empty(); }
};

enum class Combine { Single, Average, Lowest, Highest };
// Parent (10-4 follow-up): a rotation read relative to the bone's parent. Floor on a
// rotation = the world.
enum class Reference { Floor, Bones, Parent };
// Stillness and RelativeDrop: spike 10-7a, step 1 (see the header comment).
enum class Measure { Position, Speed, Acceleration, Stillness, RelativeDrop };
enum class Axis { Vertical, Horizontal, Total, X, Y, Z };
enum class Quantity { Point, JointAngle, Yaw, InteriorAngle, Rotation };

// The windows of the 10-7a measures when SignalSpec::window_ms is 0, and the relative drop's
// peak floor (a share of the clip's 95th-percentile speed).
constexpr double kStillnessWindowMs = 150.0;
constexpr double kRelativeDropWindowMs = 300.0;
constexpr double kRelativeDropPeakFloor = 0.10;

struct SignalSpec {
    // Point: the bones' (combined) position. JointAngle: bones = {a, b, c}, degrees of
    // flexion at b. Yaw: bones = {a, b}, degrees, unwrapped. InteriorAngle: bones = {a, b,
    // c} once bound ({b} in a record), the angle a-b-c in degrees. Rotation: bones = {b},
    // degrees, reference Parent or Floor (world), axis X / Y / Z / total. The angles
    // ignore combine; all but Rotation ignore reference and axis.
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
    // Stillness / relative drop: the window in ms, 0 = the measure's default
    // (kStillnessWindowMs / kRelativeDropWindowMs). The other measures ignore it.
    double           window_ms = 0.0;
};

// The window a signal's measure uses (ms): window_ms when > 0, else the measure's default;
// 0 for a measure without a window (position, speed, acceleration).
double MeasureWindowMs(const SignalSpec& spec);

enum class Direction { Below, Above };

struct Condition {
    SignalSpec signal;
    Direction  dir = Direction::Below;
    double     threshold = 0.0;
    double     margin = 0.0;  // hysteresis, >= 0 (negative reads as 0)
    bool       auto_threshold = true;  // false: Analyse keeps this threshold and margin (a fixed limit)
    KeptText   kept;                   // its `cond` line (rule_record.h)
};

enum class Landing { Crossing, PeakOf };

struct Block {
    // The rule's on/off switch (story 10-3): an off rule never fires (Detect skips it).
    bool                   enabled = true;
    std::string            marker;
    // The rule's colour (notify row, project markers): 0 = none (neutral), else
    // 0x1000000 | 0xRRGGBB (REAPER's "custom colour" flag, so black stays a colour).
    // Detection ignores it.
    uint32_t               color = 0;
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
    KeptText               kept;           // its `block` line (rule_record.h)
    KeptText               kept_strength;  // its `strength` line
    // Story 10-8b -- an auto-detection block (auto_detect.h): `auto_type` names its event type
    // ("step", "heel", "toe", "lift", "slide", "pivot"; "" = a rule), `auto_side` its foot
    // ('L' / 'R') and `sens` its sensitivity (0..100 %, 50 = the physics defaults). It has no
    // condition and the rule engine never fires it (DetectTrace skips it): its events come from
    // the motion physics (motion_physics.h). Marker, colour, offset_ms and enabled are its own.
    std::string            auto_type;
    char                   auto_side = 'L';
    double                 sens = 50.0;
};

// True for an auto-detection block (Block::auto_type set).
inline bool IsAutoBlock(const Block& b)
{
    return !b.auto_type.empty();
}

struct DetectOptions {
    double sensitivity = 0.0;     // 0..1: drop events weaker than this share of the block's strongest
    double edge_margin_ms = 0.0;  // drop crossings this close to the clip start / end
    double smooth_ms = 8.0;       // zero-phase Gaussian sigma on positions before derivatives (0 = none)
    KeptText kept;                // its `options` line (rule_record.h)
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

// One block's detection state, sample by sample (story 10-3: what the Tagging view draws).
struct BlockTrace {
    // False: the block is off, has no condition, is an auto block (story 10-8b), or a signal
    // does not fit the tracks. The vectors are then empty and it has no events.
    bool                              ran = false;
    std::vector<std::vector<double>>  curves;  // per condition: its signal, one value per sample
    std::vector<std::vector<char>>    holds;   // per condition: in (1) / out (0), with hysteresis
    std::vector<char>                 active;  // the whole rule (the AND) holds
    std::vector<double>               rearm;   // per condition: the re-arm level (threshold -/+ margin)
    std::vector<Event>                events;  // this block's events (after sensitivity), by time
};

struct DetectionTrace {
    double                  rate_hz = 0.0;
    size_t                  samples = 0;    // 0 when the tracks do not fit
    std::vector<BlockTrace> blocks;         // one per block, in block order
    std::vector<Event>      events;         // every block's events, sorted by time (= Detect)
};

// Detect, keeping the per-sample state. No-throw on bad input.
DetectionTrace DetectTrace(const std::vector<Block>& blocks, const std::vector<BoneTrack>& tracks,
                           const DetectOptions& opts = {});

// Story 10-4 -- an event's values (strength, speed) at event time t (clip seconds, the
// block's offset included), read from the same series detection uses: for a detected event,
// EventValuesAt(its block, ..., e.time_s) gives e.strength and e.speed. A user event's values
// are measured this way at its time. False (values 0) when the block or the tracks do not fit,
// and for an auto block (story 10-8b: a user event on its row has values 0).
bool EventValuesAt(const Block& blk, const std::vector<BoneTrack>& tracks, const DetectOptions& opts, double t,
                   double* strength, double* speed);

// Every block over the whole clip, sorted by time. No-throw on bad input (no events).
// = DetectTrace(...).events.
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
    KeptText kept;                    // its `analyse` line (rule_record.h)
};

// Proposes thresholds from the clip, per condition:
//   conditions with auto_threshold = false are left as they are.
//   any point signal with reference = floor (whatever the measure): floor_y (and bone_floors when
//     per_bone_floor) = the floor percentile of the raw level.
//   position (point with floor or bones reference, or an angle), stillness and relative drop
//     (the same level rule, spike 10-7a): on the values, low = their floor percentile,
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
    // Spike 10-7a: det - ref of each match (s), in reference time order (SumMatches: the
    // items' lists one after the other).
    std::vector<double> match_err_s;
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
