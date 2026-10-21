// SPDX-License-Identifier: MIT
//
// Story 10-9d: touches. Claps (the two hands meeting) and hand on body (a hand landing on the
// trunk, the head or a thigh), read from the positions PhysicsAnalysis::role_pos keeps, in body
// units (leg lengths, leg/s), so one setting works on every clip. The Arms types "Clap" and
// "Hand on body" (auto_types.cpp rows) read them.
//
// A touch is a hand (the hand bone's origin: the wrist) and a target: the other hand (clap), or
// the nearest point of the body's segments (hand on body): hips -> spine -> chest -> neck -> head
// (the trunk's present bones in that order), hips -> each up leg (the pelvis) and each up leg ->
// its knee (the thighs). Per sample: the distance d (leg), the closing speed c = -dd/dt and the
// relative speed s = |v hand - v target| (the target point's velocity: its segment's ends',
// interpolated where the hand is nearest). A touch fires at the first sample where
//   d <= reach                              (clap_reach / body_reach)
//   the peak of c over the touch_window_s before is >= min speed (clap_min_speed / body_min_speed)
//   s <= touch_stop_frac x that peak        (the hand has stopped against the target)
//   and the braking took stop_s at most     (clap_stop_s / body_stop_s: the stop is abrupt; the
//                                            braking starts at the last sample where c is still
//                                            0.9 x the peak, so a held approach speed brakes
//                                            from its end)
// never at the clip's first sample; then not again until d rises above 1.3 x reach. Strength =
// the peak closing speed (leg/s). A clap's side is the hand moving faster (the ground-frame hand
// speed's peak over the approach; L on a tie): one marker for both hands. Hand on body: the hand's.
//
// Why these, measured 2026-10-21 on the fixtures (57 clips, 240 Hz; no clip holds a clap or a
// tagged body touch: the values are settled from the motions): wrists with the palms together
// sit 0.08-0.11 leg apart (Pistol Equip's two-handed grip at 0.97 s), a two-handed sword grip
// 0.23 leg or more (Great Sword Idle), so clap_reach 0.15. A hanging hand's wrist sits 0.11-0.20
// leg from the thigh's axis (Walk With Briefcase, Right Turn), as close as a palm on the body, so
// the reach does not tell a touch from a hand at rest: the stop does. An arm swing passes the
// body at full speed (no stop); a swing's turn near the body brakes over 100-300 ms (Two Cycle
// Sprint, Treadmill Running, the arms falling back after Jump_Small / Standing Jump, Jump
// Attack's sword stroke ending by the thigh, a crawl's hand passing the knee: 100-120 ms), a hand
// landing on the body in 60-100 ms (Hip Hop Dancing's left hand on the thigh at 1.18 s, Pistol
// Equip's left hand dropping to the thigh at 0.48 s, Climbing_Large's left arm falling to the
// side at 2.03 s). Hence body_stop_s 0.10. Two hands braking together take longer: Pistol
// Equip's hands joining on the grip 100-120 ms, so clap_stop_s 0.15. Min speeds 1.5 leg/s (the
// slow drifts to rest, 1.0-1.3, are no touch), touch_stop_frac 0.25. With these, no other clip
// gives a touch. Limits: a slow placing of the hand (under 1.5 leg/s at the default
// sensitivity) is none; a touch on the far side of a thick body part (the belly, the buttocks)
// can sit beyond body_reach from the bone's axis; a hand grabbing gear on the body (Pistol
// Equip's right hand at the holster, 0.50 s, which only slows to 0.9 leg/s) is no touch.
//
// Pure C++17, deterministic. Host-tested (tests/auto_detect_test.cpp).

#pragma once

#include <string>
#include <vector>

#include "motion_physics.h"

namespace rav {

// The claps (Kind Clap, part -1, side = the faster hand), by time (clip time).
std::vector<PhysicsEvent> ClapEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});
// The hands on body (Kind HandOnBody, part -1, side = the hand), by time, then side.
std::vector<PhysicsEvent> HandOnBodyEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});

// Per analysed sample, in leg lengths: the distance between the hands (empty without both hands)
// and from a hand ('L' / 'R') to the nearest body segment (empty without that hand or a segment).
std::vector<double> HandsDistance(const PhysicsAnalysis& a);
std::vector<double> HandBodyDistance(const PhysicsAnalysis& a, char side);

// Whether the analysis' bones give at least one body segment (roles indexed by Role, -1 = none).
bool HasBodySegment(const std::vector<int>& role_to_bone);

// "touches: claps 1, hand on body 2" ("touches: skipped: <error>" when the analysis failed).
std::string TouchSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev);

}  // namespace rav
