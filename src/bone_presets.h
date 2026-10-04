// SPDX-License-Identifier: MIT
//
// Factory presets (Epic 10), written in roles (bone_roles.h): a preset's SignalSpec bones
// hold Role values until BindRoles turns them into track indices.
//
// Footsteps (v1): the event is the foot's first ground contact, heel or toe, whichever
// lands first (a design that wants it later uses the block's signed offset). Per foot,
// one block: P = the lowest of {heel, toe}, as height above the floor, AND of
//   P vertical position below h (hysteresis m)
//   P vertical speed below v ("nearly still"; vertical only, so in-place clips work)
//   optional (knee bend, Antho 2026-10-04): the knee's vertical speed relative to the
//   heel below a small negative threshold (the knee starts dropping: the weight is taken)
// min hold 30 ms, cooldown 250 ms, offset 0. Strength = the peak downward vertical speed
// in the 100 ms before the event. h, m, v, the knee threshold and the floor come from
// Analyse. The event lands at the crossing that completes the AND: with the knee
// condition, the knee onset when it comes after the contact.
//
// Pure C++17, header-only.

#pragma once

#include <string>
#include <vector>

#include "bone_events.h"
#include "bone_roles.h"

namespace rav {

inline Block FootstepBlock(Role heel, Role toe, Role knee, bool knee_bend, const char* marker)
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

    Condition still;
    still.signal = p;
    still.signal.measure = Measure::Speed;
    still.dir = Direction::Below;
    still.threshold = 0.2;  // replaced by Analyse
    still.margin = 0.1;

    Block b;
    b.marker = marker;
    b.conditions = {low, still};
    if (knee_bend) {
        Condition bend;
        bend.signal.bones = {static_cast<int>(knee)};
        bend.signal.reference = Reference::Bones;
        bend.signal.ref_bones = {static_cast<int>(heel)};
        bend.signal.ref_combine = Combine::Single;
        bend.signal.measure = Measure::Speed;
        bend.signal.axis = Axis::Vertical;
        bend.signal.keep_sign = true;
        bend.dir = Direction::Below;
        bend.threshold = -0.05;  // replaced by Analyse
        bend.margin = 0.025;
        b.conditions.push_back(bend);
    }
    b.min_hold_ms = 30.0;
    b.cooldown_ms = 250.0;
    b.offset_ms = 0.0;
    b.strength_signal = p;
    b.strength_signal.measure = Measure::Speed;
    b.strength_signal.keep_sign = true;
    b.strength_sign = -1.0;  // downward
    b.strength_window_ms = 100.0;
    return b;
}

struct Preset {
    std::string        name;
    std::vector<Block> blocks;  // bones = Role values
};

// knee_bend: add the knee condition (default on; the measure dialog can switch it off).
inline Preset FootstepsPreset(bool knee_bend = true)
{
    Preset p;
    p.name = "Footsteps";
    p.blocks = {FootstepBlock(Role::LeftHeel, Role::LeftToe, Role::LeftKnee, knee_bend, "Footstep L"),
                FootstepBlock(Role::RightHeel, Role::RightToe, Role::RightKnee, knee_bend, "Footstep R")};
    return p;
}

}  // namespace rav
