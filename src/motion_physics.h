// SPDX-License-Identifier: MIT
//
// Motion physics (Epic 10, spike 10-8a; hands: stories 10-8c, 10-8e): foot and hand events read from
// bone motion as physics, in body units, so one setting works on every clip. No clip
// statistics, no examples, no Analyse.
//
// Roles in, events out. The analysis reads, per side, the heel (the foot bone), the ball (the
// toe base), the toe tip (the toe end), the knee, the up leg and the hand, plus the hips
// (bone_roles.h). Positions, and the ball's and the hands' rotations when the track has them:
//   body scale    the leg length: up leg -> knee -> heel (the median over the clip, the mean of
//                 the sides; the hips stand in for a missing up leg). When no side gives it (no
//                 heel), 2 x (up leg -> knee), noted "thigh x2". Speeds are in leg lengths
//                 per second (leg/s), heights in leg lengths.
//   ground frame  per sample (story 10-8t). Only an in-place clip (root motion removed) has a
//                 moving ground. It is read per stance: where the lowest foot part holds a
//                 steady velocity (100 ms), that velocity is the ground's, unless the hips
//                 travel over the clip ((end - start) / duration) at half that speed or more (then
//                 the ground is the world, 0); each stance value is the median of 3 consecutive
//                 stances; held before the first / after the last stance, interpolated between
//                 (a run that speeds up is followed); 0 without a stance or a foot part. An
//                 in-place climb (story 10-8g) replaces it with one constant frame. Every speed
//                 is measured in this frame; a foot's contact and slide thresholds widen by a
//                 slack of 15 % of the ground's horizontal speed (0 in a world frame and under
//                 a climb; none on the hands).
//   contact       per foot part: a two-state segmentation (free / contact), the most probable
//                 sequence (Viterbi) on the ground-frame speed, with a cost per switch. The
//                 evidence for contact at a sample is (contact_speed - speed) / contact_speed,
//                 clamped to [free_evidence_floor, 1]; free costs 0 (on an in-place clip the
//                 threshold is contact_speed + slack and, below it, the evidence is counted
//                 against contact_speed + contact_slack_rest x slack). One sample of clear rest
//                 is worth 1, so a switch costs switch_cost samples of evidence: no hysteresis,
//                 immune to jitter.
//                 Two contacts of a part whose gap stays at the first one's support height
//                 (within slide_height) are one contact that slid (no step in between).
//   support       per contact, where the part comes to rest: the median height over 100 ms
//                 from the first sample, in the contact's first 500 ms, whose speed is below
//                 settle_speed (from the contact start when it never rests). Local, so stairs,
//                 holds and ledges work; a heel lowered slowly after the toe lands rests at the
//                 bottom, not where the slow lowering started.
// Events (FootEvents), each with a time, a side, a part and a strength:
//   step          (separate) a contact start of one part, never at the clip start. Strength =
//                 its approach speed (the peak speed over the 100 ms before, leg/s).
//   foot step     (combined) a start of the foot's contact (any part down): the earliest
//                 step of the parts that touch during that contact (the foot's first part to
//                 touch, by the step timing). Strength = that part's approach speed.
//                 Also (story 10-8k) a part that lands while the foot is already down,
//                 within pivot_planted_s of the end of a pivot on the other end of the foot
//                 (a heel pivot takes a ball / tip landing, a ball / tip pivot a heel one):
//                 the earliest such landing per pivot (its raw landing, so the part's
//                 timing and offset move the step but never add or remove it), named by
//                 its part (the slap that ends a turn). Never at the time of another foot step of that foot; a
//                 whole-foot pivot (no part) adds none.
//   lift-off      an end of the foot's contact, never at the clip end: the last of the heel
//                 and ball to leave (story 10-8p; the tip on a rig without a ball, or when it
//                 alone touched), plus lift_offset_s. Strength = the departure speed (the peak
//                 speed over the 100 ms after).
//   slide scuff   a planted part moves horizontally in the ground frame (>= slide_speed for >=
//                 slide_min_s, over >= slide_min_dist) while it stays at its support height.
//                 One per foot for overlapping parts (its part = the fastest). Strength = the
//                 peak horizontal speed (leg/s). A slide during a pivot of that foot is the
//                 pivot's.
//   pivot scuff   the planted foot turns: the yaw of the heel -> toe vector seen from above, at
//                 >= pivot_rate_dps for >= pivot_min_s and >= pivot_min_deg swept, while a
//                 part of the foot is in contact. Its part is the one it turns on: the part
//                 still in contact (the slower one when both are), or none (-1, the whole foot
//                 turns). Strength = the angle swept (degrees). Planted means planted: that
//                 part (the foot for none) is in contact from pivot_planted_s before the turn
//                 to pivot_planted_s after it, and the foot stays flat enough for a heading
//                 over pivot_planted_s after the turn. So a foot that lands turning, or twists
//                 as it rolls off the toes and leaves (a heel whip at toe-off), makes no pivot.
//                 Except a spin turn (story 10-8l): a turn of >= pivot_spin_factor x
//                 pivot_min_deg whose foot leaves (not at the clip end) within pivot_planted_s
//                 after it; its flat and planted checks after the turn stop at the lift-off.
//                 A steep foot (story 10-8d): where the heel -> toe yaw does not hold (a foot
//                 on its ball with the heel up, or on its heel with the toes up), the heading
//                 comes from a second source (FootTrack::heading_source):
//                   toe end    the ball -> toe tip vector seen from above, when the rig has a
//                              toe end (on tiptoe the toes stay flat). It holds while that
//                              vector is at least half its length (the median over the clip),
//                              and turns on the ball: a fallback sample needs the ball in
//                              contact.
//                   rotation   else the toe bone's own rotation (BoneTrack::rot_world of the
//                              ball): its across-the-foot axis stays level whether the foot
//                              stands on its ball or its heel. That axis, in bone space, is
//                              calibrated as the mean of R^-1 . (up x heel -> ball from above)
//                              over the samples where the heel -> toe yaw holds and the ball is
//                              in contact (none: no source). It holds while the axis is within
//                              pivot_level_deg (60) of level, and a fallback sample needs the
//                              ball or the heel in contact.
//                 A fallback sample is one where the heel -> toe yaw gives no rate (the sample
//                 or a neighbour does not hold, so a turn that crosses into the steep part is
//                 one turn), the source holds and its part is in contact: there, the rate is
//                 the source's. A turn with a fallback sample sweeps the integral of the rate
//                 used; one without keeps the heel -> toe yaw's difference. After the turn,
//                 flat = the heel -> toe yaw holds or a fallback sample. So a foot that rolls
//                 onto its toes as it turns and stays there (a dance swivel) pivots on the
//                 ball.
//   A scuff also has a start and an end; its time is its start.
// Step timing: each contact carries the times of several definitions (StepTiming). FootEvents
// places each part's steps by the definition and offset PhysicsParams gives that part (see
// PhysicsParams::step_timing for how the defaults were chosen and their limits).
//
// Hands (HandEvents): a hand contact is the hand coming to rest in the ground frame, segmented
// as a foot part with its own contact speed and switch cost (PhysicsParams::hand_*), its slid
// gaps joined with the feet's slide constants (slide_gap_max_s, slide_height, slide_max_speed:
// a hand that re-grabs at the same height within 1 s without lifting is one contact). A contact
// counts when it lasts hand_min_contact_s at least, so a hand stopping briefly between two
// moves is no contact; one the clip start or end cuts short counts. Its part is -1.
//   grab          a counted contact's start, never at the clip start, approached at
//                 hand_min_approach at least (a still hand drifting to rest is no grab).
//                 Timed by hand_timing + hand_offset_s; Speed (the default) is, since story
//                 10-8q, the first fall below max(hand_reach_floor, hand_reach_frac x the reach's
//                 peak speed) after that peak (the peak within hand_reach_window_s before the
//                 contact, after the previous one). Strength = the approach speed (leg/s).
//   release       a counted contact's end (its lift-off time), never at the clip end, left at
//                 hand_min_approach at least. Timed (10-8q) at the last rise above the same
//                 fraction of the departure's peak before it. Strength = the departure speed.
//                 No crossing (the hand never gets that slow): the contact speed's crossing.
//                 The hand point is the hand bone: a finger point (the middle finger's first
//                 bone) was measured and found worse (spec 10-8q).
//   hand pivot    (story 10-8e) the planted hand turns: the foot pivot's rules with the hand's
//                 own thresholds: the heading turns at >= hand_pivot_rate_dps for >= pivot_min_s
//                 and sweeps >= hand_pivot_min_deg (the unwrapped heading's difference), the hand
//                 is in contact from pivot_planted_s before the turn to pivot_planted_s after it,
//                 and its heading holds over pivot_planted_s after the turn. So a hand that lands
//                 turning, or leaves at once, makes none. Strength = the angle swept (degrees).
//                 The heading comes from the hand bone's own rotation (BoneTrack::rot_world of
//                 the hand; no finger role, no elbow -> hand vector: a vault where the palm stays
//                 put turns nothing): a bone-space axis perpendicular to the hand's mean up (the
//                 mean of R^-1 . up over the hand's contact samples; none: no source), seen from
//                 above. It holds while that axis is within pivot_level_deg of level. By default
//                 (PhysicsParams::hand_pivot_after_grab) only a hand that came to rest with a
//                 grab's approach (>= hand_min_approach), or was at rest from the clip start,
//                 pivots; off, any resting hand does.
//
// Looping clips (story 10-8i, PhysicsParams::looping, a user setting): the motion is read as a
// cycle, the end continuing into the start, so a contact, step, lift-off, grab, release, slide or
// pivot that crosses the loop seam is read whole. The clip is unrolled into three cycles of period
// n - 1 samples (the last sample is the seam, the next cycle's first) and analysed as one track;
// the middle cycle is the clip (PhysicsAnalysis::window_first, window_samples). Each bone's
// positions repeat shifted by its own end - start offset per cycle (an in-place climb ends one
// rung higher; 0 on a closed loop), its rotations unshifted, so velocities wrap and no height
// assumes the end pose equals the start pose. Events are mapped back to clip time and kept in
// [0, T) (T = the seam): an event exactly on the seam is counted once, at 0. A contact in the
// middle cycle is never cut by the clip start or end, so the "cut short still counts" waiver never
// applies there, and a part in contact the whole cycle gives no event. Clip-wide statistics
// (medians) over three identical cycles are the clip's. Limits: a clip without the duplicate end
// frame loses one source frame of motion at the seam; a turning loop (a net yaw per cycle, a
// circle walk) is unrolled without rotating the next cycle, so the seam can show a heading jump
// (a possible extra pivot there). Off: the analysis is the clip as it is.
//
// Arrival at the clip end (story 10-8j, not looping): a part that brakes onto a support in the clip's
// last moments has too few slow samples to pay the switch cost, so it would get no contact. A foot
// part or hand free at the last sample, whose speed falls through the arrival speed
// (arrival_speed_factor x its contact speed) within arrival_window_s of the end, approached at the
// type's min approach at least (arrival_min_approach for the feet, hand_min_approach for the
// hands), and stays below it to the end, gets a contact from that crossing (the braking point) to
// the end: a step / foot step / grab there (each part's step offset still applies), no lift-off or
// release, no scuff. The smoothing holds the ends, which lowers the last ~3 sigma of speeds, so the
// crossing must lie before them (ceil(3 sigma x rate) samples: a brake within the last 25 ms at
// 240 Hz, 33 ms at 30 Hz, is not seen). Limit: a hand that
// stops in the air at the end after a fast move (a dance's last pose) reads as a grab.
//
// Jumps (JumpEvents, story 10-9a): a flight is a run of samples where no part of either foot is in
// contact (every present part of both feet; a foot with no part, or no hips bone, gives no jump)
// lasting jump_min_flight_s at least, while the hips fall freely: their mean vertical acceleration
// over the flight (the change of PhysicsAnalysis::hips_vy over its samples, in m/s^2 through the
// leg length; read 3 smoothing sigmas inside each end, which the smoothing blurs with the push and
// the landing, when the flight is long enough) is at most -jump_fall_g x 9.81. So a flight the
// hands hold up (hanging, climbing: the hips do not fall) is no jump, and a run's short flights
// are none at the default.
//   jump          the take-off: the last foot leaving before the flight (the latest foot
//                 lift-off, story 10-8p, without lift_offset_s, of the feet whose contact ends
//                 where it starts; take-off = the last foot leaving the
//                 ground, Linthorne 2001), never for a flight already under way at sample 0 (no
//                 contact ends where it starts; see "Clip start" below). Feet that leave together
//                 give one event. Strength = the peak |hips vertical speed| over approach_window_s
//                 after it (leg/s).
//   land          the landing: the first foot touching after the flight (a contact of it starts
//                 where the flight ends), timed as that foot's FootEvents foot step: the earliest
//                 timed part (step_timing / step_offset_s) to touch during that contact of the
//                 foot (initial contact), never at the clip end. Strength = the peak |hips
//                 vertical speed| over approach_window_s before it (leg/s).
// Story 10-9a-3: both carry the foot (PhysicsEvent::side L / R) and the part (FootPart) that
// leaves last / touches first. Feet within jump_tie_s of each other (a two-footed take-off or
// landing: the parts' 8 ms smoothing does not order them) tie, and a tie keeps the first foot
// (L), at its own last part to leave / its own step: one event. Looping clips: as the other
// events.
//
// Running strides (story 10-9a-4): a run's flights (both feet off each stride, longer as it
// speeds up: Treadmill Running's sprint flies 154-162 ms at -1 g, which Detect's raise of the
// minimum flight to 150 ms read as jumps) are no jumps. A both-feet-off run is a stride's flight
// when the flight before or after it lies one single-foot stance away (only one foot touches in
// between, jump_stride_stance_s at most: running alternates single supports and flights) and lasts
// at least this one / jump_stride_ratio: it does not stand out from the clip's own cadence. Each
// jump source (flight, free fall, push-off) skips them, so no Jump and no Land there. A jump taken
// while running flies 3.4x its neighbours or more; a lone flight (no neighbour) is judged as
// before. Limit: a jump among strides as long as itself (a run of long bounds) is not found.
//
// Clip start (story 10-9a-3, not looping, feet only): a take-off whose push the clip starts in
// has a contact too short for the Viterbi. A foot part free at sample 0 that stays within
// slide_height x leg of its lowest height over the clip from sample 0, and comes to rest within
// that run (speed under contact_speed + its slack), is in contact from sample 0 to its last
// resting sample of that run (no step there; its lift-off is read as any). Measured: Running Jump
// Over Hole R ball 0.009 L up, 0.09 leg/s; Running Jump_Long R ball 0.34 leg/s at the floor;
// Jump_WhileRunning R ball arrives at sample 2. A swing skimming the floor never comes to rest:
// no contact. No floor-height contact anywhere else.
//
// Free-fall jumps (JumpFreeFallEvents, story 10-9a-3): a third way to find a take-off, for a
// flight that does not end on the feet (a fall on the back: the flight rule averages the fall
// over the lying time too). Both feet off for jump_min_flight_s at least, starting after the clip
// start; the hips' mean vertical acceleration over the first jump_freefall_window_s of the run
// (read from 3 smoothing sigmas inside its start; a shorter head is read right after them)
// is at most -jump_fall_g x 9.81, whatever ends the run. Measured: real jumps -0.87 to -1.71 g, every other run -0.11 g or more (the ballistic
// phase is -1 g). Take-off only (no Land: a body landing is 10-9b's), as JumpEvents', with its
// side and part.
//
// Push-off jumps (JumpPushOffEvents, story 10-9a-2): a second way to find a take-off, for jumps
// whose flight is too short or does not fall (a leap onto a ledge, one foot to the other). The
// ground pushes the body up well above its weight (F = m (a + g), the hips standing for the
// centre of mass): the hips' upward acceleration (HipsUpAccelerationG) peaks at jump_impulse_g at
// least while a foot is on the ground, a both-feet-off run (2 samples at least, no minimum
// flight) starts within jump_impulse_window_s after that peak, and no hand is in contact from the
// peak to that take-off (a push the hands share, climbing, is no jump), and the body is not
// running: its mean horizontal speed over the window before the take-off (hips_vh) stays under
// jump_impulse_max_speed (a run's stride pushes as hard as a jump; a jump taken while running is
// left to its flight).
//   jump         the take-off as JumpEvents' (the latest foot lift-off of the feet whose
//                 contact ends where the run starts, its side and part), never at the clip start. Strength as
//                 JumpEvents'.
//
// A missing role drops only that part's events (PhysicsAnalysis::missing says why for the feet,
// hand_missing for the hands, hips_missing for the jumps). Two parts on the same bone (an Unreal
// toe end standing in on ball_l) keep the first.
//
// Pure C++17, deterministic: no REAPER, no GL. Host-tested (tests/motion_physics_test.cpp).

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "bone_events.h"
#include "bone_roles.h"

namespace rav {

// The parts of a foot. Marker words: "heel", "toe" (the ball), "tip".
enum class FootPart : int { Heel = 0, Ball, Tip };
constexpr int kFootPartCount = 3;
const char* FootPartWord(int part);
// The role of a foot part: side 'L' / 'R', part in FootPart order (LeftHeel, LeftToe, LeftToeEnd...).
Role FootPartRole(char side, int part);

// Where a step marker lands, per definition (searched from search_back_s before the contact
// start, never before the previous contact of that part ends):
//   Speed    the speed falls through the contact speed (the segmentation's own edge, sub-sample).
//   Height   the height falls through the support + height_eps: the first crossing after
//            the part was last height_drop above that, else the last crossing before it rests.
//   Descent  the main descent ends: after the fastest descent of the approach, the downward
//            speed falls below descent_speed and stays below for descent_hold_s.
//   Settle   the speed falls below settle_speed: the part has come to rest.
// Speed searches up to search_fwd_s after the contact start; the others up to where the part
// comes to rest (a heel lowered slowly can rest 300 ms after its speed contact starts).
// A definition that finds no crossing falls back to Speed, and so does Height or Descent when
// the part still moves horizontally faster than a swing (slide_max_speed, the ground frame's
// hspeed) at its crossing: a foot that ends its descent long before it stops and glides into
// place lands where it stops. Settle crossings are already slow, so the rule never touches them.
enum class StepTiming : int { Speed = 0, Height, Descent, Settle };
constexpr int kStepTimingCount = 4;
const char* StepTimingName(StepTiming t);  // "speed", "height", "descent", "settle"
const char* StepTimingHint(StepTiming t);  // one line

// The constants: body units (leg lengths L), seconds, degrees. Nothing comes from the clip.
struct PhysicsParams {
    double smooth_ms = 8.0;              // zero-phase Gaussian sigma on positions before velocities
    double contact_speed = 0.4;          // leg/s: contact evidence 0 (below = contact)
    double switch_cost = 10.0;           // per switch, in samples of clear evidence (240 Hz)
    double free_evidence_floor = -3.0;   // the clamp of the evidence for free
    double support_window_s = 0.100;     // support = the median height over 100 ms from where the part rests...
    double rest_search_s = 0.500;        // ...the first sample this far into the contact below settle_speed
    double height_eps = 0.01;            // leg: Height's level above the support
    double height_drop = 0.05;           // leg: Height starts from the last time the part was this much higher
    double descent_speed = 0.1;          // leg/s: Descent ends below this downward speed...
    double descent_hold_s = 0.050;       // ...for this long
    double settle_speed = 0.1;           // leg/s: Settle
    double search_back_s = 0.300;        // timing searches this far before the contact start...
    double search_fwd_s = 0.150;         // ...and this far after
    double approach_window_s = 0.100;    // step / lift-off strength window
    // Story 10-8p, the foot's lift-off, measured on the all-trigger bench's 106 confirmed lift-off
    // references (10-8o): the toe tip leaves 42 ms after the reference on rigs with a ball (it
    // rolls off last), so the lift-off is the last of the heel and ball to leave (the tip only
    // without a ball, or when it alone touched; lift_from_tip = the old rule). Then lift_offset_s
    // (added): the median signed error of that anchor, -8.4 ms, the same left-one-clip-out.
    // Anchors measured and dropped: the part's speed peak (+160 ms raw; with its offset median
    // |err| 12.5 ms against 10.7 for the crossing), the vertical speed peak (34 ms), peak knee
    // extension (Fellin et al. 2010, running toe-off: 47-58 ms here, SD 94 ms).
    bool   lift_from_tip = false;
    double lift_offset_s = -0.008;
    double slide_speed = 0.25;           // leg/s horizontal
    double slide_max_speed = 2.0;        // leg/s: faster is a swing, not a slide
    double slide_height = 0.015;         // leg: a slide stays this close to the support height
    double slide_min_s = 0.05;
    double slide_min_dist = 0.05;        // leg
    double slide_gap_max_s = 1.0;        // two contacts further apart are never one slid contact
    // leg/s: past this vertical speed shared by two limbs, the ground frame is theirs (an in-place
    // climb, story 10-8g: -0.67 leg/s on Climbing Up Wall_InPlace, -0.72 on Climbing Ladder; at
    // most 0.09 on the other fixtures, the real climbs included, where planted parts hold still).
    double ground_vertical_min = 0.3;
    // The stance ground frame (story 10-8t), set on the all-trigger bench (10-8o).
    // A stance candidate: over a centred ground_stance_s window, |vy| < ground_stance_tol and the
    // horizontal velocity within max(ground_stance_tol, ground_stance_frac x its window mean) of
    // that mean. 0.4 leg/s is the default contact speed. Candidate runs on the fast in-place
    // runs are short (4 - 165 ms on Treadmill Running, Run Forward_Exhausted), so the window is short too; a gap of at
    // most half the window does not end a stance (a glide's ramps would split it). Source:
    // short gaps are filled in contact labels (UnderPressure, Mourot et al. 2022: an 11-frame
    // majority filter).
    double ground_stance_s = 0.100;     // s
    double ground_stance_tol = 0.4;     // leg/s; also the climb-frame limbs' steadiness (3D)
    // Also the slack, on the feet only: an in-place clip follows its ground only roughly (Two
    // Cycle Sprint: the planted toe moves -5.6 to -8.1 leg/s around a -7.3 ground), so a foot's
    // contact and slide speeds widen by this fraction of the ground's horizontal speed (without
    // it the fast in-place runs lose 26 steps / lift-offs on the bench). Source: a speed-scaled
    // contact threshold (foot thresholds as a fraction of walking speed: Bruening & Ridge 2014,
    // Bonci et al. 2022; Hendershot et al.: speed-adjusted foot-velocity thresholds are the most
    // consistent on a treadmill); a real belt's speed varies 4-9 % within a step, a keyframed
    // in-place clip follows its ground within about 15 % (Two Cycle Sprint).
    double ground_stance_frac = 0.15;
    // Story 10-9a-5: the share of that slack a foot part's contact evidence is counted against
    // (evidence = (contact speed + slack - speed) / (contact speed + this x slack), below the
    // widened threshold; 1 = the plain ratio of 10-8t). A keyframed in-place foot drifts along its
    // belt within the slack, so a speed inside it is mostly rest: Treadmill Running's
    // sprint stance at 5.86 s (137 ms, the planted toe going -4.65 -> -5.97 m/s on a -5.5 belt)
    // earned 19.8 samples of evidence against the 20 two switches cost and was no step. Any value
    // 0.25 - 0.75 gives the same default matrix on the bench; 0 also takes a heel the reference
    // judge rejects (Two Cycle Sprint L 0.159 s). No effect in a world frame (no slack) or on hands.
    double contact_slack_rest = 0.5;
    // A stance (or the climb frame) is a moving ground only when the hips' travel speed over the
    // clip is below this fraction of its speed: a crawl with root motion drags its feet at the
    // body's speed (hips +0.59, feet +0.73 leg/s), a strafe jump carries its hands (hips -3.3).
    // Source: in place = the hips do not travel (root motion removed; Zeni's pelvis-relative
    // method exists because a treadmill's stance foot moves at the belt speed).
    double ground_inplace_frac = 0.5;
    double pivot_smooth_ms = 20.0;       // Gaussian sigma on positions for the yaw
    double pivot_rate_dps = 90.0;
    double pivot_min_s = 0.05;
    double pivot_min_deg = 15.0;
    double pivot_planted_s = 0.100;      // the pivot part is in contact this long before and after
    double pivot_spin_factor = 2.0;      // a turn of this x pivot_min_deg may end at lift-off (spin turn); > 1
    double pivot_level_deg = 60.0;       // the toe rotation's across axis holds within this of level
    // The step timing of each part (FootPart order) and its offset (s, added). Chosen by the
    // spike on its 6 tagged reference clips (19 foot contacts; one clip's left / right tags
    // swapped back): the definition with the fewest edits per part, each offset minus that
    // part's median signed error over all of them. Fitted on those clips only; the tip's comes
    // from one clip (4 contacts). Limits: a foot that glides into place (Catwalk, high heels,
    // 3-4 leg/s after its descent ends) is timed by Speed, where it stops (story 10-8f); the
    // offsets were not refitted for it.
    // Story 10-8r refit the heel and toe offsets (they were +0.007 / -0.031) on the all-trigger
    // bench's confirmed step references (10-8o, 185 part steps, 103 foot steps, walks and runs):
    // Descent's median signed error is +14.7 ms on the toe (runs +15.9, others +14.3), +14.3 on the
    // heel. The toe takes -0.015 (its median); the heel -0.005 only, so that the heel stays 10 ms
    // behind the toe and Combined's first part (and a Land's part) does not flip on a flat landing
    // (Jump Attack: heel -0.010 / toe -0.015 already names its landing by the heel). Foot steps:
    // runs mean -16.7 -> -5.9 ms, median |err| 18.2 -> 12.1; others -6.1 -> +0.3, 18.9 -> 11.7.
    // Not kept, measured on the same references (left one clip out): the peak of the part's
    // upward vertical acceleration (Handsaker et al. 2016, running touchdown; sigma 0-20 ms) and
    // Speed are no better than Descent on runs (median |err| toe 16.6 / 16.5 vs 12.1 ms, heel 21.2
    // / 17.9 vs 18.8, foot 19.3 / 23.4 vs 15.8) and worse on the other clips. The tip keeps +0.020
    // (no tip reference).
    StepTiming step_timing[kFootPartCount] = {StepTiming::Descent, StepTiming::Descent, StepTiming::Descent};
    double     step_offset_s[kFootPartCount] = {-0.005, -0.015, 0.020};
    // Hands (story 10-8c), segmented as a foot part with these instead of contact_speed and
    // switch_cost. Chosen with detection_eval --physics on the 11 fixture clips, which hold one
    // hand REF (Climbing, right hand, 3.65 s):
    //   contact speed and switch cost: the feet's. Contact 0.3-0.5 finds the REF grab; 0.4 with
    //     a cost of 10 adds no other hand event on the clips without hand contact (0.5 adds 4,
    //     a cost of 5 adds 1, 20 drops Climbing's left release).
    //   min approach: the hands that come to rest in the air (the back of an arm swing, the
    //     end of a jump) approach at 0.81 leg/s at most; the REF grab at 1.54. 1.0 sits between
    //     (0.8 adds 3 events, 1.4 still keeps the grab).
    //   min contact: 150 ms, for a stop inside the clip; a contact the clip start or end cuts
    //     short always counts (the REF grab's lasts 175 ms, to the clip end).
    //   timing: each definition on the REF: speed +8 ms, descent -55, height -93, settle +81.
    //     Speed is chosen. The offset stays 0: one REF cannot fit an offset.
    // Limits: one hand REF; a hand frozen in the air after a fast move (a long dance hit) reads
    // as a grab.
    double     hand_contact_speed = 0.4;     // leg/s
    double     hand_switch_cost = 10.0;      // samples of clear evidence (240 Hz)
    double     hand_min_approach = 1.0;      // leg/s: a grab's approach, a release's departure
    double     hand_min_contact_s = 0.150;   // a shorter contact is a stop between two moves
    StepTiming hand_timing = StepTiming::Speed;
    double     hand_offset_s = 0.0;
    // Story 10-8q: grab and release timed on the reach's own speed (10 % of its peak, the
    // reach onset / offset of Schnorenberg et al. 2015), with an absolute floor against jitter;
    // which contacts exist stays the absolute rule above. 0 = the absolute crossing (the old
    // timing). Measured on the all-trigger bench (confirmed references, spec 10-8q): grab mean
    // -14.3 -> +5.2 ms, median |err| 12.6 -> 4.1; release mean +56.0 -> -8.6, median 28.6 -> 28.2;
    // no count change. 5 % / 15 % / 20 % / 30 % and a per-type release fraction were worse; the
    // floor (0.02-0.10) moves nothing; a 0.3 s window drags grabs late, 1.0 s widens them.
    // Under a +-0.05 leg/s change of hand_contact_speed the matched hand events move by 0 ms
    // median (p90 5 ms) instead of 10.9 (p90 21.9).
    double     hand_reach_frac = 0.10;
    double     hand_reach_floor = 0.05;      // leg/s
    double     hand_reach_window_s = 0.5;    // the reach's peak is searched this far from the contact
    // Hand pivots (story 10-8e): the foot's rate and angle (no hand pivot REF exists to choose
    // others); pivot_min_s, pivot_planted_s, pivot_smooth_ms and pivot_level_deg are shared.
    double     hand_pivot_rate_dps = 90.0;
    double     hand_pivot_min_deg = 15.0;
    // HandEvents only: a pivot needs the hand's contact to start with a grab's approach (or at the
    // clip start). Off: any resting hand pivots. The analysis does not read it.
    bool       hand_pivot_after_grab = true;
    // Story 10-8j: arrival at the clip end (see "Arrival at the clip end" above). The arrival
    // speed is this factor x the part's contact speed (feet 0.8, hands 0.8 leg/s at the default
    // sensitivity, which moves it with the contact speed): Climbing Ladder's
    // left hand brakes from 2.8 to 0.6-0.78 leg/s 70 ms before the end. Half the approach peak was
    // rejected (Ascending Stairs' left heel "brakes" from 4.2 to 2 leg/s, still swinging). The
    // feet's min approach has nothing to fit: the fixture feet arriving at the end approach at
    // 2.7 leg/s at least; the hands use hand_min_approach.
    double     arrival_window_s = 0.150;
    double     arrival_speed_factor = 2.0;
    double     arrival_min_approach = 1.0;   // leg/s, feet
    // Story 10-8i: read the clip as a loop (see "Looping clips" above). A user setting, off by
    // default; no constant changes with it.
    bool       looping = false;
    // Story 10-9a: jumps (see "Jumps" above). The minimum flight, both feet off, chosen on the
    // fixtures (default analysis): Jumping Down's jump flies 675 ms (-0.89 g); the longest
    // both-feet-off run that falls at -0.5 g or more elsewhere is Climbing Ladder's, 129 ms
    // (-0.67 g, the hands holding); Left Strafe (221, 258 ms) and Climbing (621 ms) do not fall
    // (> -0.25 g). 300 ms sits between, and the sensitivity (/ f) keeps it there: 150 ms at 100 %
    // (so Detect's raise never reaches the ladder; a sprint's 150-160 ms flights it reaches are left
    // to the stride rule, story 10-9a-4), 600 ms at 0 %.
    // Limits: a hop under 300 ms (11 cm of hips rise) needs a higher sensitivity; Jumping (a leap
    // onto a ledge, one foot to the other) is both feet off for 58 ms only, under a run's flight:
    // no jump there.
    double     jump_min_flight_s = 0.300;
    double     jump_fall_g = 0.5;
    // Story 10-9a-3: the free-fall jumps (see "Free-fall jumps" above): the head of the
    // both-feet-off run whose mean hips acceleration is read (s). Not moved by the sensitivity.
    double     jump_freefall_window_s = 0.300;
    // Story 10-9a-3: feet leaving / landing within this of each other do it together (s): the
    // first foot (L) names the jump / land. Measured: two-footed take-offs 4-7 ms apart (Jump
    // Attack, Jump_Small, Standing Jump), staggered ones 60 ms or more (Standing Jump02, Jump
    // Away); landings 13 ms or more apart. Not moved by the sensitivity.
    double     jump_tie_s = 0.010;
    // Story 10-9a-4: a running stride's flight is no jump (see "Running strides" above): a
    // neighbouring flight one single-foot stance away (the stance this long at most, s) lasting at
    // least this flight / jump_stride_ratio. Measured (240 Hz dumps): consecutive stride flights
    // differ by 1.8x at most (Unarmed Run Back 142 / 79 ms; Treadmill Running 92-162 ms as it
    // speeds up, 1.3x between neighbours), stances 117-296 ms, always one foot; a jump's flight is
    // 3.4x its neighbours at least (JumpingOverObstacle 588 / 175 ms; Run Strafing Jump 488 / 100,
    // Running Jump Over Hole 400 / 46). Not moved by the sensitivity.
    double     jump_stride_ratio = 2.5;
    double     jump_stride_stance_s = 0.400;
    // Story 10-9a-2: the push-off jumps (see "Push-off jumps" above). The hips' upward
    // acceleration peak (g) a push must reach while a foot is on the ground (the sensitivity
    // divides it), and how soon after that peak both feet must leave (s). Chosen on the fixtures
    // (peak with a foot down in the window before a both-feet-off run, 40 ms smoothing): Jumping's
    // push-off 1.12 g (no hand); the highest elsewhere without a hand is Climbing's 0.52 g (a 58 ms
    // hop at 2.72 s), Left Strafe 0.48 g, Walk With Briefcase 0.18 g; Climbing Ladder's 0.74 g
    // has the hands on. 0.9 sits between: Jumping is found from 35 % sensitivity up, Climbing's
    // first false Jump comes at 90 %. Jumping Down steps off a ledge (0.26 g): its flight finds it.
    double     jump_impulse_g = 0.9;
    double     jump_impulse_window_s = 0.25;
    // The body's horizontal speed (leg/s, ground frame) at or above which a push is a running
    // stride, not a jump. Measured (mean over the window before the take-off): the push-off jumps
    // Jump_Small 0.45, Standing Jump 0.59 / 0.68, Jump Attack 1.06, Jumping 1.23; the strides
    // Slow Run 3.3-3.5 leg/s (pushes 1.04 / 1.55 g), Running Jump Over Hole's stride after the
    // landing 4.91 (1.03 g), Left Strafe 5.2 (in place: the ground's speed). 2.2 sits between; the
    // running jumps (Running Jump Over Hole's 4.37) keep their flight.
    double     jump_impulse_max_speed = 2.2;
    double     jump_impulse_smooth_s = 0.040;  // the acceleration's smoothing (Gaussian sigma, s)
    // Story 10-9g: the gear jolts (see JoltEvents). The centre of mass's acceleration peak (g) an
    // impact must reach (the sensitivity divides it), the window around the impact it is read in
    // (s, before / after the contact), its smoothing (Gaussian sigma, s), and the impacts closer
    // than jolt_merge_s that make one jolt (the earliest's time, the highest peak). 1 g = the
    // ground pushing with twice the body's weight: running peaks at 2-3 body weights, walking at
    // 1.1-1.3 (vertical ground reaction force). Measured on the fixtures (JoltAccelerationG peak
    // per impact, 20 ms smoothing): jump landings 1.33-6.26 g (Running Jump Over Hole 1.33,
    // Jumping Down 1.51, Standing Jump 2.10, Jump Attack 6.26); running steps 0.65-3.2 g (Slow Run
    // 1.46 / 1.89, Treadmill Running 1.6-3.2, Two Cycle Sprint 1.66-2.43, Great Sword Run 0.93 /
    // 1.11, the strafes 0.65-1.12); walks 0.05-0.54 g (Walking 0.15 / 0.20, Catwalk 0.11-0.36,
    // Walk With Briefcase 0.39 / 0.43, Ascending Stairs 0.51 / 0.54), climbs 0.07-1.00. At 1 g
    // every landing and most running steps jolt, no walk; the sensitivity (/ f) takes it to 0.5 g
    // at 100 % (brisk steps, stairs) and 2 g at 0 % (hard landings, sprints). The smoothing moves
    // the peaks (Slow Run 1.46 g at 20 ms, 1.62 at 10, 1.04 at 40): 20 ms sits between the
    // parts' 8 ms and the push-off's 40. Window: the step time lags the hips' deceleration by up
    // to 30 ms; the deceleration lasts 50-150 ms after it.
    double     jolt_min_g = 1.0;
    double     jolt_before_s = 0.030;
    double     jolt_after_s = 0.150;
    double     jolt_smooth_s = 0.020;
    double     jolt_merge_s = 0.150;
    // Story 10-9f: the body spins (auto_types_spin.cpp). The turn segmentation of gait studies
    // (R[1][12]): the hip line's yaw rate above spin_start_dps starts a spin, it runs while the
    // rate stays at spin_end_dps or more in the same direction, and it is kept when it sweeps
    // spin_min_deg at least (the sensitivity divides the sweep only; Detect never raises it). The yaw is
    // smoothed over spin_smooth_s (Gaussian sigma) first: measured on the fixtures, 50 ms keeps
    // the timing of Hip Hop Dancing's four turns (98-111 deg) and drops Climbing Up Wall's 46 deg
    // pelvis twist (no smoothing: 46 deg); a walk's hip sway sweeps 21 deg at most.
    double     spin_start_dps = 15.0;
    double     spin_end_dps = 5.0;
    double     spin_min_deg = 45.0;
    double     spin_smooth_s = 0.050;
    // Story 10-9e: foot taps and drags (FootTapEvents, FootDragEvents, foot_taps.h). A tap is a
    // toe contact (ball or tip) of tap_max_s at most, the heel up (higher above that part than in
    // the foot's flat pose by tap_heel_rise, leg) and the other foot down the whole time (it
    // carries the body), its same part at the same support height (within tap_floor_height,
    // leg: the floor the body stands on). The sensitivity multiplies tap_max_s. Measured on the
    // fixture dumps (default analysis): no foot contact of 300 ms or less has the other foot down
    // at any sample (the sprints' toe-only stances, 117-167 ms on Treadmill Running and Two Cycle
    // Sprint, are single supports), so a walk or a run gives no tap.
    double     tap_max_s = 0.150;
    double     tap_heel_rise = 0.03;
    double     tap_floor_height = 0.03;
    // A drag is the toe (ball or tip) sliding on its support during a swing: the heel off the
    // ground, the toe part not in contact (it moves), within drag_height (leg) above or below the
    // support of its latest contact, at drag_speed to drag_max_speed (leg/s, horizontal, ground
    // frame), for drag_min_s at least over drag_min_dist (leg) at least; runs drag_gap_s apart or
    // less (the heel still off) are one. The sensitivity divides drag_min_s. Measured on the
    // fixture dumps (spec 10-9e): the knee crawl (246 ms), the one-knee kneel (375 ms, its two
    // runs 100 ms apart joined) and its stand-up (179 ms) drag; no walk or run drags at the
    // default, the longest such run on one is 79 ms (Walking, Catwalk: found from 67 %). Within
    // 0.8 cm of the support (drag_height) a swing skims the floor only at toe-off and landing;
    // past 2 leg/s the toe is swinging.
    double     drag_height = 0.008;
    double     drag_speed = 0.5;
    double     drag_max_speed = 2.0;
    double     drag_min_s = 0.100;
    double     drag_min_dist = 0.08;
    double     drag_gap_s = 0.100;
    // Story 10-9b: body contacts (see BodyContactEvents). A knee, elbow or the hips touch when they
    // come to rest (segmented as a hand: contact speed, switch cost, min contact) while low over
    // the floor ("stationary and low", HuMoR's knee / hand rule, research d3); heights in leg
    // lengths above the floor, of the joint centres. Measured on the fixtures (the knee joint):
    // kneeling rests at 0.12-0.14 (Kneel_1knee), 0.09 (Kneeling Down To Crouch), crawling -0.05 to
    // 0.06; the lowest knee elsewhere 0.15 moving at 0.4-0.8 leg/s (Jump Attack's lunge), 0.17 at
    // 1+ leg/s (Running Crawl, never at rest), 0.28 (Standing Jump02), 0.32 (Crouched Walking).
    // HuMoR's 0.08 m (0.09 leg) misses the Kneel clip's knee: 0.20. An elbow's centre sits closer
    // to its skin (a forearm flat on the floor: about 0.05 leg): 0.12; no fixture rests on its
    // elbows (Jump Away's lie on the back: 0.18-0.24, crawling 0.26+). The descent: the kneels
    // and crawls come down at 0.5-1.0 leg/s, Jump Away's legs stop sliding at 0.1: 0.3.
    double     knee_contact_speed = 0.4;     // leg/s (the Knee sensitivity multiplies it)
    double     elbow_contact_speed = 0.4;    // leg/s (the Elbow sensitivity multiplies it)
    double     sit_contact_speed = 0.4;      // leg/s, the hips (the Sit sensitivity multiplies it)
    double     body_switch_cost = 10.0;      // samples of clear evidence (240 Hz), as the hands'
    double     body_min_contact_s = 0.150;   // a shorter rest is no contact
    double     body_min_descent = 0.3;       // leg/s: a knee / elbow contact comes down at this speed at least
    double     body_stop_frac = 0.2;         // a knee / elbow is timed where its downward speed falls to this x the descent's peak
    double     knee_ground_height = 0.20;    // leg
    double     elbow_ground_height = 0.12;   // leg
    double     body_floor_pct = 5.0;         // the floor: this percentile of the lowest foot part's height
    // Sit: the hips at rest below sit_max_height, the chest at least sit_min_upright above them,
    // at least sit_min_offset (horizontal) from the feet, no knee down, for sit_min_s. A squat
    // must keep the hips over the feet to balance (Kneeling Down To Crouch 0.04, Kneel_1knee
    // 0.05-0.08 leg), a chair holds them a thigh behind (about 0.5 leg); a lying or crawling
    // trunk has its chest 0.1-0.2 leg over the hips, a standing or kneeling one 0.35+. No fixture
    // sits: chosen from these, checked on synthetic clips only.
    double     sit_max_height = 0.75;        // leg
    double     sit_min_upright = 0.30;       // leg
    double     sit_min_offset = 0.25;        // leg
    double     sit_min_s = 0.300;
    // Fall: the trunk (the lower of the hips and the chest) comes within fall_height of the floor
    // falling at fall_min_speed at least over the fall_window_s before its descent stops (the
    // Fall sensitivity divides fall_min_speed). Measured: Jump Away lands on its back with the
    // hips at 0.11 leg falling at 3.4 leg/s; the lowest trunk elsewhere is 0.44 (Kneeling Down To
    // Crouch's hips), 0.47 (crawling), 0.48 (Jumping Down's landing crouch).
    double     fall_height = 0.35;           // leg
    double     fall_min_speed = 1.0;         // leg/s, downwards
    double     fall_window_s = 0.300;
    double     fall_stop_speed = 0.25;       // leg/s: the descent has stopped (vertical speed above -this)
    // Story 10-9d: claps and hand on body (hand_touch.h, "Touches"). A touch is a hand coming
    // within reach of the other hand (clap) or of a body bone's segment (hand on body) at speed,
    // then stopping against it. Reach (leg lengths, from the hand bone's origin, the wrist, to the
    // other wrist / the segment's axis), the approach (the peak closing speed, leg/s, the
    // sensitivity divides it), the stop (the relative speed falls to touch_stop_frac x that peak)
    // and its abruptness (the stop within this long of the braking's start, s). See hand_touch.h
    // for the values measured on the fixtures. Not moved by the sensitivity: the stop and the
    // window.
    double     clap_reach = 0.15;
    double     clap_min_speed = 1.5;
    double     clap_stop_s = 0.150;
    double     body_reach = 0.22;
    double     body_min_speed = 1.5;
    double     body_stop_s = 0.100;
    double     touch_stop_frac = 0.25;
    double     touch_window_s = 0.300;  // the approach's peak is searched this far before the stop
};

// One contact of a part, in samples [start, end).
struct PartContact {
    int    start = 0;
    int    end = 0;
    double support_y = 0.0;                  // m
    double step_s[kStepTimingCount] = {};    // each definition's step time (clip s, no offset); start > 0 only
    double approach = 0.0;                   // leg/s
    double lift_s = 0.0;                     // the speed rises through the contact speed; end < samples only
    double departure = 0.0;                  // leg/s
};

struct PartTrack {
    bool                     present = false;
    std::string              missing;   // why not present ("no bone for left toe end"...)
    std::vector<double>      y;         // height (m, smoothed)
    std::vector<double>      speed;     // ground-frame speed (leg/s)
    std::vector<double>      hspeed;    // ground-frame horizontal speed (leg/s)
    std::vector<double>      vy;        // vertical velocity (leg/s, up > 0)
    std::vector<double>      slack;     // per sample, added to contact / slide speeds (leg/s); empty = 0 (hands)
    std::vector<char>        contact;   // 1 = contact (after slid gaps are joined)
    std::vector<PartContact> contacts;
};

// A slide or pivot: samples [start, end), its part (-1 = none), its strength (leg/s or deg).
struct Scuff {
    int    start = 0;
    int    end = 0;
    int    part = -1;
    double strength = 0.0;
    // Story 10-8m: the part that holds longest, Heel or the forefoot's part (ball, or tip), -1 =
    // none. A slide: the most sliding samples in the merged scuff (heel against the forefoot's
    // busier part); a pivot: its part, else the part with more contact samples over the turn. A
    // tie goes to the forefoot.
    int    longest = -1;
};

// Where a steep foot's heading comes from (story 10-8d, see the pivot scuff above).
enum class HeadingSource : int { None = 0, ToeEnd, Rotation };

struct FootTrack {
    char                side = 'L';
    PartTrack           part[kFootPartCount];
    bool                has_yaw = false;  // heel and toe (ball or tip) present
    std::vector<double> yaw_deg;          // unwrapped
    std::vector<double> yaw_rate_dps;     // 0 where the vector is too short from above
    std::vector<char>   yaw_valid;        // seen from above, the vector is at least half the foot's length
    // The fallback heading of a steep foot (None: no toe end, and no usable toe rotation).
    HeadingSource       heading_source = HeadingSource::None;
    std::vector<double> heading_rate_dps;   // the source's yaw rate; 0 where it does not hold
    std::vector<char>   heading_valid;      // the source holds (whatever the contact)
    std::vector<Scuff>  slides;
    std::vector<Scuff>  pivots;
    // Story 10-8m: the heel's slides and the forefoot's slides (ball and tip merged), found
    // separately, pivot-filtered as `slides`; each part = Heel or the forefoot's fastest part.
    std::vector<Scuff>  part_slides;
};

// A hand: its contacts as a part (PartTrack, segmented with the hand constants), and its
// heading for the pivots (story 10-8e; none without a usable rotation).
struct HandTrack {
    char                side = 'L';
    PartTrack           part;
    bool                has_heading = false;  // the hand's rotation gives a heading
    std::vector<double> heading_rate_dps;     // 0 where it does not hold
    std::vector<char>   heading_valid;        // the heading holds (whatever the contact)
    std::vector<Scuff>  pivots;               // part -1; every resting hand's (HandEvents filters)
};

// Story 10-9b: the body parts that touch the ground. knee / elbow / hips: segmented as a hand
// (their own contact speed, PhysicsParams::body_switch_cost) where they are low enough (knee /
// elbow: knee_ground_height / elbow_ground_height; hips: sit_max_height), free above; chest: its
// heights and speeds only (the chest role, else the spine). Heights (PartTrack::y) in the ground
// frame, metres; `floor_y` the floor (body_floor_pct of the lowest foot part's height).
struct BodyTrack {
    bool                has_floor = false;
    double              floor_y = 0.0;      // m
    std::string         floor_missing;      // !has_floor: why ("no foot bone")
    PartTrack           knee[2];            // L, R
    PartTrack           elbow[2];           // L, R
    PartTrack           hips;
    PartTrack           chest;
    std::vector<double> hips_feet;          // per sample: the hips' horizontal distance to the feet's middle (leg); empty without
};

struct PhysicsAnalysis {
    bool                     ok = false;
    std::string              error;          // !ok: why
    double                   rate_hz = 0.0;
    size_t                   samples = 0;
    double                   leg_length = 0.0;  // m
    std::string              scale_note;        // "" or how the scale was found when not both legs
    Vec3d                    ground_velocity;   // m/s, clip summary (median per axis of ground_vel);
                                                // y = 0 unless the climb frame applies
    std::vector<Vec3d>       ground_vel;        // m/s per sample, the frame every speed uses
    std::vector<double>      ground_slack;      // leg/s per sample, the feet's (0 in a world frame / climb)
    FootTrack                foot[2];           // L, R
    std::vector<std::string> missing;           // one line per dropped foot part / event type
    HandTrack                hand[2];           // L, R
    std::vector<std::string> hand_missing;      // one line per dropped hand ("L hand: no bone for left hand")
    // Story 10-9a: the hips' vertical velocity in the ground frame (smoothed as the parts, leg/s,
    // up > 0), for the jumps. Empty without a hips bone (hips_missing says why).
    std::vector<double>      hips_vy;
    // Story 10-9a-2: the hips' horizontal speed in the ground frame (leg/s), same smoothing; the
    // push-off jumps' running check. Empty with hips_vy.
    std::vector<double>      hips_vh;
    std::string              hips_missing;
    // Story 10-9f: the hip line's yaw (left up leg -> right up leg seen from above, degrees,
    // unwrapped, not smoothed; a sample where that line is under half its length from above holds
    // the previous yaw and is flagged invalid), for the spins. Empty without both up legs
    // (spin_missing says why).
    std::vector<double>      hips_yaw_deg;
    std::vector<char>        hips_yaw_valid;
    std::string              spin_missing;
    BodyTrack                body;           // story 10-9b
    // Story 10-9d: every physics role's position (m, smoothed as the parts; indexed by Role, empty =
    // no bone), for the touches (hand_touch.h). Analysed track samples.
    std::vector<std::vector<Vec3d>> role_pos;
    // The clip inside the analysed track (story 10-8i): looping, the track is three cycles and
    // the clip is its samples [window_first, window_first + window_samples); otherwise 0 and
    // `samples`. Per-sample data (PartTrack...) is indexed in the analysed track; the event
    // functions return clip times.
    bool                     looping = false;
    size_t                   window_first = 0;
    size_t                   window_samples = 0;
};

// The roles the analysis reads (Role order).
const std::vector<Role>& PhysicsRoles();

// One track per Role (indexed by Role; empty = no bone) from a role -> bone mapping (indexed by
// Role, -1 = none) and a sampler of bone indices (one track per index, in order). Only
// PhysicsRoles() are sampled. Empty when the sampler fails.
std::vector<BoneTrack> PhysicsRoleTracks(const std::vector<int>& bone_of_role,
                                         const std::function<std::vector<BoneTrack>(const std::vector<int>&)>& sample);

// The analysis (p.looping: of the unrolled cycle, see above). role_tracks: indexed by Role, an empty track = the role has no bone; every
// non-empty track has the same rate and length. Rotations are optional: only the ball's and the
// hands' rot_world are read (one per sample, else ignored).
PhysicsAnalysis AnalyseMotion(const std::vector<BoneTrack>& role_tracks, const PhysicsParams& p = {});

// Story 10-9e appends Tap and Drag (foot_taps.h); story 10-9b Knee, Elbow (side L / R), Sit, Fall
// (side 'B': the body); story 10-9d Clap, HandOnBody (hand_touch.h).
enum class PhysicsKind { Step, FootStep, LiftOff, Slide, Pivot, Grab, Release, HandPivot, Jump, Land, Jolt, Spin, SpinEnd, Tap, Drag,
                         Knee, Elbow, Sit, Fall, Clap, HandOnBody };
// Story 10-9f: Spin / SpinEnd, a spin's start / end (side B, start_s / end_s its span, strength =
// its sweep in degrees; auto_types_spin.cpp).
constexpr int kPhysicsKindCount = 21;  // story 10-9-0: PhysicsMarkerName's rows follow the enum

struct PhysicsEvent {
    PhysicsKind kind = PhysicsKind::Step;
    char        side = 'L';      // story 10-9a-3: a jump / land's foot (the one leaving last / touching first)
    int         part = 0;        // FootPart; -1 = none (a pivot on neither, a hand event)
    double      time_s = 0.0;    // clip time (a scuff: its start)
    double      start_s = 0.0;   // a scuff's span (= time_s for the others)
    double      end_s = 0.0;
    double      strength = 0.0;  // leg/s (steps, lift-offs, slides, grabs, releases) or degrees (pivots, hand pivots)
    int         longest = -1;    // a scuff's Scuff::longest (story 10-8m); never printed
};

// Every event, by time (then side, kind, part). Steps use p.step_timing / p.step_offset_s.
std::vector<PhysicsEvent> FootEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});

// Story 10-8m: the slide and pivot events by part (Kind Slide / Pivot, part Heel or the forefoot's
// part): FootTrack::part_slides, and each pivot on its part (a whole-foot pivot gives a heel event
// and a forefoot event). Clip time, sorted as FootEvents.
std::vector<PhysicsEvent> FootPartScuffEvents(const PhysicsAnalysis& a);

// Every hand event (grabs, releases and pivots), by time (then side, kind). Uses p.hand_*
// (p.hand_pivot_after_grab picks the pivots; their thresholds are the analysis').
std::vector<PhysicsEvent> HandEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});

// Story 10-9a: the jumps (take-offs and landings), by time. Uses p.jump_min_flight_s,
// p.jump_fall_g and the steps' p.step_timing / p.step_offset_s (a landing is timed as a step).
std::vector<PhysicsEvent> JumpEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});
// Story 10-9a-2: the hips' upward acceleration per analysed sample, in g (the change of
// PhysicsAnalysis::hips_vy through the leg length, smoothed over p.jump_impulse_smooth_s).
// Empty without hips.
std::vector<double> HipsUpAccelerationG(const PhysicsAnalysis& a, const PhysicsParams& p = {});
// Story 10-9a-2: the push-off take-offs (Kind Jump), by time. Uses p.jump_impulse_g,
// p.jump_impulse_window_s and p.jump_impulse_smooth_s.
std::vector<PhysicsEvent> JumpPushOffEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});
// Story 10-9a-3: the free-fall take-offs (Kind Jump), by time. Uses p.jump_min_flight_s,
// p.jump_fall_g and p.jump_freefall_window_s.
std::vector<PhysicsEvent> JumpFreeFallEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});
// Story 10-9g (defined in auto_types_jolt.cpp, the Jolt type's file): the centre of mass's upward
// acceleration per analysed sample, in g: HipsUpAccelerationG smoothed over p.jolt_smooth_s
// instead, downward read as 0 (gravity not counted: 0 at a steady speed, 1 g = the ground pushing
// with twice the body's weight). Vertical only: the horizontal speed's change (hips_vh) peaks at
// 4-10 g away from any impact on in-place clips (Running Jump_Long 9.9 g, its vertical 0.8 g).
// Empty without hips.
std::vector<double> JoltAccelerationG(const PhysicsAnalysis& a, const PhysicsParams& p = {});
// Story 10-9g: the gear jolts (Kind Jolt, side 'B', part -1), by time: an impact (a foot step or a
// jump's landing; body contacts once 10-9b lands) whose JoltAccelerationG peak within
// [-p.jolt_before_s, +p.jolt_after_s] of it reaches p.jolt_min_g, at the impact's time; impacts
// within p.jolt_merge_s of each other make one (on a loop, through the seam too). Strength = the
// peak (g).
std::vector<PhysicsEvent> JoltEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});

// Story 10-9b: body contacts (src/body_contacts.cpp), by time:
//   knee / elbow  (side L / R) a counted contact (BodyTrack: at rest and low), never at the clip
//                 start, lasting body_min_contact_s at least (one the clip end cuts short
//                 counts), that came down at body_min_descent at least over the fall_window_s
//                 before it (legs that stop sliding along the floor give none). Timed where the
//                 part's downward speed falls to body_stop_frac x that peak (it has landed).
//                 Strength = the peak downward speed (leg/s).
//   sit           (side B) a counted hips contact (at rest under sit_max_height) lasting sit_min_s,
//                 its chest sit_min_upright above the hips (not lying), its hips sit_min_offset
//                 from the feet's middle (something else carries the body: a squat keeps the hips
//                 over the feet) and no knee in contact over it (a kneel). Never at the clip start.
//                 Strength = the hips' peak speed before.
//   fall          (side B) the trunk (the lower of hips and chest) enters fall_height of the floor
//                 and stops descending (vertical speed above -fall_stop_speed), having fallen at
//                 fall_min_speed at least within fall_window_s before: a body landing (a fall on
//                 the back after a free fall). One per stay low (re-armed above 1.5 x fall_height).
//                 A squat is none: where the descent stops, the chest sit_min_upright above the
//                 hips with the hips within sit_min_offset of the feet's middle.
//                 Strength = that peak downward speed.
// No floor (no foot bone) or no bone for a part: none of its events (BodyContactSummary says why).
std::vector<PhysicsEvent> BodyContactEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});
// "body: knees 2, elbows 0, sits 0, falls 1" plus "; missing: ..." ("body: skipped: <error>").
std::string BodyContactSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev);
// Analysed times (time_s, start_s, end_s) as clip times (a loop: its middle cycle), dropping the
// events outside the clip (story 10-9b: for the event functions outside motion_physics.cpp).
void PhysicsEventsToClipTime(const PhysicsAnalysis& a, std::vector<PhysicsEvent>& ev);

// The separate step times of one part (side 'L' / 'R'), by `timing` plus `offset_s`.
std::vector<double> PartStepTimes(const PhysicsAnalysis& a, char side, int part, StepTiming timing, double offset_s);

// "PHY L heel 1.8", "PHY R step toe 2.1", "PHY L lift tip 1.2", "PHY L slide toe 0.6 210ms",
// "PHY R pivot ball 87deg 250ms", "PHY L grab 1.4", "PHY R release 0.9",
// "PHY L hand pivot 87deg 250ms", "PHY R jump 1.2", "PHY L land 1.5" (strengths in leg/s or
// degrees).
std::string PhysicsMarkerName(const PhysicsEvent& e);
// True for a marker name this module writes ("PHY " prefix).
bool IsPhysicsMarkerName(const char* name);

// The report: "leg 0.93 m, ground (+0.00, -0.41) m/s; steps heel 2, toe 2, tip 2; foot steps 2,
// lift-offs 1, slides 0, pivots 0" plus "; missing: ..." when a part is dropped.
std::string PhysicsSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev);
// The hands' report: "hands: grabs 2, releases 1, pivots 0" plus "; missing: ..." when a hand is
// dropped ("hands: skipped: <error>" when the analysis failed). Counts the hand events of ev.
std::string HandSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev);
// Story 10-9a: the jumps' report: "jumps: take-offs 1, landings 1" plus "; missing: ..." when
// the hips have no bone ("jumps: skipped: <error>" when the analysis failed).
std::string JumpSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev);
// Story 10-9a-2: the push-off jumps' report: "jumps (push-off): 1" plus "; missing: hips: ..."
// ("jumps (push-off): skipped: <error>" when the analysis failed).
std::string JumpPushOffSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev);
// Story 10-9a-3: the free-fall jumps' report: "jumps (free fall): 1", as JumpPushOffSummary.
std::string JumpFreeFallSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev);
// One line per event ("   1.234 s  PHY L heel 1.8", a scuff or hand pivot "... (to 1.456 s)"), each with
// `indent` in front and '\n' after.
std::string PhysicsEventLines(const std::vector<PhysicsEvent>& ev, const std::string& indent);
// "heel speed +0 ms, toe height -12 ms, tip speed +0 ms"
std::string StepTimingText(const PhysicsParams& p);

}  // namespace rav
