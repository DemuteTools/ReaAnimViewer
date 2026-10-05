// SPDX-License-Identifier: MIT
//
// Factory presets (Epic 10), written in roles (bone_roles.h): a preset's SignalSpec bones
// hold Role values until BindRoles turns them into track indices.
//
// FootstepsPreset, the offline reference (the knee / yaw design of 2026-10-04, not the shipped
// factory preset): per foot, one block, AND of
//   [0] height: P = the lowest of {heel, toe}, height above the floor, below h (the trigger)
//   [1] knee:   knee flexion speed (joint angle up leg - knee - ankle, signed, rising as the
//               knee bends) above the onset threshold (the weight is being taken)
//   [2] yaw:    foot yaw speed (heel -> toe about the vertical, magnitude) below a fixed
//               limit (a pivot on a turn is not a step)
// and the event lands at the peak of the knee flexion speed while the AND holds.
// Strength = the peak downward vertical speed of P in the 100 ms before the event.
// h, the knee onset and the floor come from Analyse; the yaw limit is fixed (deg/s).
// Every default lives in FootstepsParams (current guesses, to be tuned offline on Antho's
// tagged clips with tests/detection_eval).
//
// This preset is the offline measuring tools' (footstep_measure, tests/detection_eval); it no
// longer ships. The factory presets (presets/factory/*.ravpreset, compiled into the DLL) are
// Footsteps Heel and Footsteps Toe since the 10-4 follow-up (Antho 2026-10-05): one rule per
// foot, the heel's (toe's) height below a threshold, the marker at the start of the match.
//
// Pure C++17, header-only.

#pragma once

#include <string>
#include <vector>

#include "bone_events.h"
#include "bone_roles.h"

namespace rav {

struct FootstepsParams {
    double height_fraction = 0.40;   // h = floor + this share of the floor-to-swing gap
    double knee_fraction = 0.30;     // knee onset = this share of the p95 of knee flexion speed
    double yaw_limit_dps = 800.0;    // foot yaw speed limit (deg/s)
    double yaw_margin_dps = 0.0;     // its hysteresis
    double min_hold_ms = 0.0;
    double cooldown_ms = 250.0;
    double offset_ms = 0.0;
    bool   per_bone_floor = true;    // heel and toe each measured above their own floor
    double floor_percentile = 2.0;
    double margin_ratio = 0.5;       // hysteresis of the analysed thresholds
    double smooth_ms = 8.0;          // before derivatives
    double sensitivity = 0.0;        // 0 = off
    double edge_margin_ms = 0.0;
    double strength_window_ms = 100.0;
};

inline Block FootstepBlock(Role heel, Role toe, Role knee, Role up_leg, const FootstepsParams& prm,
                           const char* marker, uint32_t color = 0)
{
    SignalSpec p;
    p.bones = {static_cast<int>(heel), static_cast<int>(toe)};
    p.combine = Combine::Lowest;
    p.reference = Reference::Floor;
    p.measure = Measure::Position;
    p.axis = Axis::Vertical;

    Condition low;
    low.signal = p;
    low.dir = Direction::Below;
    low.threshold = 0.05;  // replaced by Analyse
    low.margin = 0.025;

    Condition bend;
    bend.signal.quantity = Quantity::JointAngle;
    bend.signal.bones = {static_cast<int>(up_leg), static_cast<int>(knee), static_cast<int>(heel)};
    bend.signal.measure = Measure::Speed;
    bend.signal.keep_sign = true;
    bend.dir = Direction::Above;
    bend.threshold = 50.0;  // replaced by Analyse (deg/s)
    bend.margin = 25.0;

    Condition yaw;
    yaw.signal.quantity = Quantity::Yaw;
    yaw.signal.bones = {static_cast<int>(heel), static_cast<int>(toe)};
    yaw.signal.measure = Measure::Speed;
    yaw.dir = Direction::Below;
    yaw.threshold = prm.yaw_limit_dps;
    yaw.margin = prm.yaw_margin_dps;
    yaw.auto_threshold = false;

    Block b;
    b.marker = marker;
    b.color = color;
    b.conditions = {low, bend, yaw};
    b.min_hold_ms = prm.min_hold_ms;
    b.cooldown_ms = prm.cooldown_ms;
    b.offset_ms = prm.offset_ms;
    b.strength_signal = p;
    b.strength_signal.measure = Measure::Speed;
    b.strength_signal.keep_sign = true;
    b.strength_sign = -1.0;  // downward
    b.strength_window_ms = prm.strength_window_ms;
    b.landing = Landing::PeakOf;
    b.peak_condition = 1;
    b.peak_max = true;
    return b;
}

struct Preset {
    std::string        name;
    std::vector<Block> blocks;  // bones = Role values
};

inline Preset FootstepsPreset(const FootstepsParams& prm = {})
{
    Preset p;
    p.name = "Footsteps";
    p.blocks = {FootstepBlock(Role::LeftHeel, Role::LeftToe, Role::LeftKnee, Role::LeftUpLeg, prm, "Footstep L",
                              0x1000000u | 0x5F9EDDu),
                FootstepBlock(Role::RightHeel, Role::RightToe, Role::RightKnee, Role::RightUpLeg, prm, "Footstep R",
                              0x1000000u | 0xDD9E5Fu)};
    return p;
}

// The Analyse / Detect options that go with the params.
inline AnalyseOptions FootstepsAnalyseOptions(const FootstepsParams& prm)
{
    AnalyseOptions o;
    o.floor_percentile = prm.floor_percentile;
    o.position_fraction = prm.height_fraction;
    o.margin_ratio = prm.margin_ratio;
    o.onset_fraction = prm.knee_fraction;
    o.per_bone_floor = prm.per_bone_floor;
    o.smooth_ms = prm.smooth_ms;
    return o;
}

inline DetectOptions FootstepsDetectOptions(const FootstepsParams& prm)
{
    DetectOptions o;
    o.sensitivity = prm.sensitivity;
    o.edge_margin_ms = prm.edge_margin_ms;
    o.smooth_ms = prm.smooth_ms;
    return o;
}

}  // namespace rav
