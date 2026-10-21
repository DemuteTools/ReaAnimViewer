// SPDX-License-Identifier: MIT
//
// Story 10-9e: foot taps and drags, read from the motion physics (motion_physics.h), as the Legs
// types Tap and Drag (auto_types.cpp). Body units, the constants in PhysicsParams (tap_*, drag_*).
//   tap    a light toe touch while the other foot carries the body: a toe contact (the ball's or
//          the tip's, the segmentation's, merged) of tap_max_s at most, never cut by the clip
//          start or end, where at every sample the heel is up (above that part by tap_heel_rise
//          more than in the foot's flat pose: the 10th percentile while both are in contact) and the other
//          foot is down, its same part resting at the same support height (tap_floor_height) when
//          the toe touches. Timed and named as a step of its first forefoot part to touch (step
//          timing and offsets), so a Tap sits on the foot step it is, its end = the touch's end;
//          with Step / Lift-off ticked it replaces that foot's steps within kAutoReplaceS and
//          its lift-offs over the touch (auto_detect.h). Strength = its approach speed (leg/s).
//   drag   the toe sliding on its support during a swing: per sample, the heel is off the ground
//          and a forefoot part (ball or tip) is not in contact, within drag_height of the support
//          of its latest contact (the first one before it), moving at drag_speed to
//          drag_max_speed horizontally. A run of drag_min_s at least over drag_min_dist at least
//          (gaps of drag_gap_s or less with the heel off joined) is one drag, never from the clip
//          start. Part = the forefoot part with more such samples (a tie: the ball). Time = its
//          start, with its end; strength = the peak horizontal speed (leg/s).
// Both need the heel and a forefoot part (ball or tip) of the foot; a tap also the other foot.
// Looping clips: as the other events (the middle cycle, clip time in [0, T)).
//
// Pure C++17, deterministic. Host-tested (tests/foot_taps_test.cpp).

#pragma once

#include <vector>

#include "motion_physics.h"

namespace rav {

// Every tap (PhysicsKind::Tap), by time (then side).
std::vector<PhysicsEvent> FootTapEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});
// Every drag (PhysicsKind::Drag), by time (then side).
std::vector<PhysicsEvent> FootDragEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});
// The samples of the analysed track where foot `side` ('L' / 'R') has a forefoot part dragging
// (the per-sample rule above, before the minimum length and distance). Empty when the foot has no
// heel or no forefoot part, or the analysis failed.
std::vector<char> FootDragMask(const PhysicsAnalysis& a, const PhysicsParams& p, char side);

}  // namespace rav
