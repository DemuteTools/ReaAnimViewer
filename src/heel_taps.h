// SPDX-License-Identifier: MIT
//
// Story 10-9i: heel taps, read from the motion physics (motion_physics.h), as the Legs type
// Heel Tap (auto_types.cpp). Body units, the constants in PhysicsParams (heel_tap_*).
//   heel tap  the heel drops back onto the floor while the ball of the foot stays down (a dancer's
//             heel drop: the foot rolls onto its ball, then the heel comes down). Per foot: a rise
//             is a run of samples where the heel is more than heel_tap_land above its floor (the
//             support of its contact), between two samples where it is back within it (never cut
//             by the clip start or end). It is a heel tap when, over the whole run:
//               - the heel stays in contact (the segmentation's: it rises slowly). A heel that
//                 leaves the ground lands as a Step (Heel), which marks it already;
//               - the forefoot (the ball, else the tip) stays in contact, within
//                 heel_tap_ball_height of its own floor (the ball stays down), and that floor is
//                 the body's (within heel_tap_floor of BodyTrack::floor_y: not a climbing hold);
//             the heel rises above the forefoot by heel_tap_min_rise at least (both over their
//             floors: the foot tilts onto its ball, a foot lifted flat is no heel tap), and comes
//             down at heel_tap_min_speed at least (the fastest descent after that peak, leg/s).
//             It then rests there: within heel_tap_land of its floor (above or below) for
//             heel_tap_hold_s (a heel still sinking past the floor it was read on is not down).
//             Time = where the heel gets back within heel_tap_land of its floor (interpolated);
//             strength = that fastest descent (leg/s).
//             Its physics kind is PhysicsKind::Tap on the heel (part FootPart::Heel): a toe tap
//             (foot_taps.h) is a Tap on the ball or the tip, so the two never mix.
// Needs the heel and a forefoot part (ball or tip) of the foot. Looping clips: as the other
// events (the middle cycle, clip time in [0, T)).
//
// Pure C++17, deterministic. Host-tested (tests/heel_taps_test.cpp).

#pragma once

#include <vector>

#include "motion_physics.h"

namespace rav {

// Every heel tap (PhysicsKind::Tap, part Heel), by time (then side).
std::vector<PhysicsEvent> HeelTapEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});
// Per sample of the analysed track, foot `side`'s heel height over its floor (leg; 0 where the
// heel is not in contact), and in `down` (optional) whether the forefoot is down there (in
// contact, within heel_tap_ball_height of its floor) with the heel in contact. Empty when the foot
// has no heel or no forefoot part, or the analysis failed.
std::vector<double> HeelTapRise(const PhysicsAnalysis& a, const PhysicsParams& p, char side, std::vector<char>* down = nullptr);

}  // namespace rav
