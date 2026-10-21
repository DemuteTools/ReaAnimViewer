// SPDX-License-Identifier: MIT
//
// Story 10-9l: slaps. A slap is a flat hand hitting something (the floor, a wall, an object, the
// body): the palm open, the fingers extended through the impact and after it. A grab closes the
// fingers after the contact. The Arms type "Slap" (auto_types.cpp row, auto_types_slap.cpp) reads
// the hand's contacts that Grab and Hand on body find, and keeps those whose fingers stay open.
//
// The finger curl, per analysed sample (degrees): the angle between the back of the hand (wrist ->
// knuckle, the hand bone's origin to the finger's base) and the finger (knuckle -> fingertip), from
// the positions PhysicsAnalysis::role_pos keeps (roles LeftKnuckle / LeftFingertip...: the middle
// finger, else the index, else the ring finger; bone_roles.h). 0 = the finger straight in line with
// the hand, about 90 = bent at a right angle, 150-180 = a fist. A contact is a slap when
//   the hand's fingers move in the clip     (the curl's range over the analysed track is at least
//                                            kSlapMovingDeg: a rig whose fingers are not animated,
//                                            frozen open or closed, cannot tell a slap from a grab)
//   the fingers are open at the contact     (curl <= kSlapOpenMaxDeg)
//   and they do not close after it          (over the kSlapAfterS after the contact the curl never
//                                            rises more than kSlapCloseRiseDeg above its value at
//                                            the contact; a window running past the end of a clip
//                                            that does not loop is not seen: no slap)
// A fist is closed before the contact (a punch, a guard: curl 130-180), so it is never a slap.
//
// Why these, measured 2026-10-21 on the fixtures' hand contacts (Grab and Hand on body at the
// default sensitivity; 240 Hz). Open hands on the floor: Crawl_Low Crawl R 15 deg (+0 after),
// Crawl_Running Crawl L 30 (+5), Crawling_On hands and knees R 8 / L 3 (+0), the AirPunch's
// landing on the hands L 17 / R 18 (+0 / +3). Grabs: Climbing Ladder's rungs 36-46 at the contact
// then +22 to +30 within 0.25 s (closing), Climbing Up Wall's holds 103-115, Pistol Equip's grip
// 58 and 104, a fist (Fist Fight 155-163, the punches 130-178). Hence open <= 45 deg, a close =
// a rise over 15 deg in 0.25 s (the rungs rise 18-30 by 0.15 s). Fingers not animated: Hip Hop
// Dancing (0 deg throughout), Climbing_Armored (26 / 12: a climb whose grabs would all read as
// slaps), Climbing_Large_Normal, Jumping, the punches; a 10 deg range sets them apart from the
// least moving animated hand (Crawl_Low Crawl R, 16 deg).
//
// Pure C++17, deterministic. Host-tested (tests/hand_slap_test.cpp).

#pragma once

#include <string>
#include <vector>

#include "motion_physics.h"

namespace rav {

constexpr double kSlapOpenMaxDeg = 45.0;
constexpr double kSlapCloseRiseDeg = 15.0;
constexpr double kSlapAfterS = 0.25;
constexpr double kSlapMovingDeg = 10.0;

// The finger curl of hand `side` ('L' / 'R') per analysed sample (degrees), as above. Empty when
// the hand, its knuckle or its fingertip has no bone (or the analysis failed).
std::vector<double> FingerCurlDeg(const PhysicsAnalysis& a, char side);

// Whether a contact of hand `side` at clip time `t` is a slap (as above). False without the finger
// bones; `why` (optional) says why it is not one ("" for a slap).
bool SlapAt(const PhysicsAnalysis& a, char side, double t, std::string* why = nullptr);

// The slaps among Grab's contacts (HandEvents' Grab events whose fingers stay open: Kind Grab) and
// among the touches on the body (HandOnBodyEvents: Kind HandOnBody), each event as its source
// gave it but for its span (end_s = start_s = time_s: a slap replaces only what lands with it).
std::vector<PhysicsEvent> SlapContactEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});
std::vector<PhysicsEvent> SlapBodyEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});

}  // namespace rav
