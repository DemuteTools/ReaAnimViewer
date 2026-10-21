// SPDX-License-Identifier: MIT
//
// Story 10-9j: Peak for the scuffs (Legs > Slide scuff, Legs > Pivot scuff, Arms > Pivot scuff).
// Their markers sit at the scuff's start (today's, the default: Start); Peak puts them where the
// rub is fastest: the horizontal speed peak, within the scuff's span [start, end) and while the
// part is down (the whole span when it never is), of the part that rubs the floor:
//   - a foot slide / pivot: the event's own part (the planted part a pivot turns on, the sliding
//     part; Each part: the heel or the forefoot event's part; Longest part: the part that holds
//     longest), else the forefoot (the ball, else the toe tip; the heel on a rig with neither);
//   - a hand pivot: the hand.
// Measured on Hip Hop Dancing (story 10-9j, tests/trigger_bench.cpp's Scuff Peak block): Antho's
// seven scuff times by ear sit 1-32 ms (mean 12) from the Peak of the ball pivot that holds them,
// 150-312 ms (mean 225) after its Start.
// Only the marker's time moves (PhysicsEvent::time_s); the span, the part and the strength stay,
// so the same scuffs are found in both modes.
//
// Pure C++17, deterministic. Host-tested (tests/scuff_peak_test.cpp).

#pragma once

#include <vector>

#include "motion_physics.h"

namespace rav {

// The event's time moved to its rub's speed peak (clip time, as the event's): Slide and Pivot
// events read the foot (`by_longest`: the part that holds longest, PhysicsEvent::longest, when
// set), HandPivot events the hand; any other event is returned unchanged.
PhysicsEvent ScuffPeakEvent(const PhysicsAnalysis& a, const PhysicsEvent& e, bool by_longest = false);

// Every event of the list through ScuffPeakEvent, sorted again by time (then side, kind, part).
std::vector<PhysicsEvent> ScuffPeakEvents(const PhysicsAnalysis& a, std::vector<PhysicsEvent> ev, bool by_longest = false);

}  // namespace rav
