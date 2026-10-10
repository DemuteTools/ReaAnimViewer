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
//   ground frame  the median horizontal velocity of the lowest foot part (0 without a foot
//                 part). An in-place clip (a treadmill walk) slides; a normal clip reads about
//                 0. Every speed is measured in this frame.
//   contact       per foot part: a two-state segmentation (free / contact), the most probable
//                 sequence (Viterbi) on the ground-frame speed, with a cost per switch. The
//                 evidence for contact at a sample is (contact_speed - speed) / contact_speed,
//                 clamped to [free_evidence_floor, 1]; free costs 0. One sample of clear rest
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
//   lift-off      an end of the foot's contact (the last part leaves), never at the clip end.
//                 Strength = the departure speed (the peak speed over the 100 ms after).
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
//                 Timed by hand_timing + hand_offset_s. Strength = the approach speed (leg/s).
//   release       a counted contact's end (its lift-off time), never at the clip end, left at
//                 hand_min_approach at least. Strength = the departure speed (leg/s).
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
// A missing role drops only that part's events (PhysicsAnalysis::missing says why for the feet,
// hand_missing for the hands). Two parts on the same bone (an Unreal toe end standing in on
// ball_l) keep the first.
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
    double pivot_smooth_ms = 20.0;       // Gaussian sigma on positions for the yaw
    double pivot_rate_dps = 90.0;
    double pivot_min_s = 0.05;
    double pivot_min_deg = 15.0;
    double pivot_planted_s = 0.100;      // the pivot part is in contact this long before and after
    double pivot_level_deg = 60.0;       // the toe rotation's across axis holds within this of level
    // The step timing of each part (FootPart order) and its offset (s, added). Chosen by the
    // spike on its 6 tagged reference clips (19 foot contacts; one clip's left / right tags
    // swapped back): the definition with the fewest edits per part, each offset minus that
    // part's median signed error over all of them. Fitted on those clips only; the tip's comes
    // from one clip (4 contacts). Limits: a foot that glides into place (Catwalk, high heels,
    // 3-4 leg/s after its descent ends) is timed by Speed, where it stops (story 10-8f); the
    // offsets were not refitted for it.
    StepTiming step_timing[kFootPartCount] = {StepTiming::Descent, StepTiming::Descent, StepTiming::Descent};
    double     step_offset_s[kFootPartCount] = {0.007, -0.031, 0.020};
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
    // Hand pivots (story 10-8e): the foot's rate and angle (no hand pivot REF exists to choose
    // others); pivot_min_s, pivot_planted_s, pivot_smooth_ms and pivot_level_deg are shared.
    double     hand_pivot_rate_dps = 90.0;
    double     hand_pivot_min_deg = 15.0;
    // HandEvents only: a pivot needs the hand's contact to start with a grab's approach (or at the
    // clip start). Off: any resting hand pivots. The analysis does not read it.
    bool       hand_pivot_after_grab = true;
    // Story 10-8i: read the clip as a loop (see "Looping clips" above). A user setting, off by
    // default; no constant changes with it.
    bool       looping = false;
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
    std::vector<char>        contact;   // 1 = contact (after slid gaps are joined)
    std::vector<PartContact> contacts;
};

// A slide or pivot: samples [start, end), its part (-1 = none), its strength (leg/s or deg).
struct Scuff {
    int    start = 0;
    int    end = 0;
    int    part = -1;
    double strength = 0.0;
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

struct PhysicsAnalysis {
    bool                     ok = false;
    std::string              error;          // !ok: why
    double                   rate_hz = 0.0;
    size_t                   samples = 0;
    double                   leg_length = 0.0;  // m
    std::string              scale_note;        // "" or how the scale was found when not both legs
    Vec3d                    ground_velocity;   // m/s, y = 0 unless past ground_vertical_min
    FootTrack                foot[2];           // L, R
    std::vector<std::string> missing;           // one line per dropped foot part / event type
    HandTrack                hand[2];           // L, R
    std::vector<std::string> hand_missing;      // one line per dropped hand ("L hand: no bone for left hand")
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

enum class PhysicsKind { Step, FootStep, LiftOff, Slide, Pivot, Grab, Release, HandPivot };

struct PhysicsEvent {
    PhysicsKind kind = PhysicsKind::Step;
    char        side = 'L';
    int         part = 0;        // FootPart; -1 = none (a pivot on neither, a hand event)
    double      time_s = 0.0;    // clip time (a scuff: its start)
    double      start_s = 0.0;   // a scuff's span (= time_s for the others)
    double      end_s = 0.0;
    double      strength = 0.0;  // leg/s (steps, lift-offs, slides, grabs, releases) or degrees (pivots, hand pivots)
};

// Every event, by time (then side, kind, part). Steps use p.step_timing / p.step_offset_s.
std::vector<PhysicsEvent> FootEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});

// Every hand event (grabs, releases and pivots), by time (then side, kind). Uses p.hand_*
// (p.hand_pivot_after_grab picks the pivots; their thresholds are the analysis').
std::vector<PhysicsEvent> HandEvents(const PhysicsAnalysis& a, const PhysicsParams& p = {});

// The separate step times of one part (side 'L' / 'R'), by `timing` plus `offset_s`.
std::vector<double> PartStepTimes(const PhysicsAnalysis& a, char side, int part, StepTiming timing, double offset_s);

// "PHY L heel 1.8", "PHY R step toe 2.1", "PHY L lift tip 1.2", "PHY L slide toe 0.6 210ms",
// "PHY R pivot ball 87deg 250ms", "PHY L grab 1.4", "PHY R release 0.9",
// "PHY L hand pivot 87deg 250ms" (strengths in leg/s or degrees).
std::string PhysicsMarkerName(const PhysicsEvent& e);
// True for a marker name this module writes ("PHY " prefix).
bool IsPhysicsMarkerName(const char* name);

// The report: "leg 0.93 m, ground (+0.00, -0.41) m/s; steps heel 2, toe 2, tip 2; foot steps 2,
// lift-offs 1, slides 0, pivots 0" plus "; missing: ..." when a part is dropped.
std::string PhysicsSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev);
// The hands' report: "hands: grabs 2, releases 1, pivots 0" plus "; missing: ..." when a hand is
// dropped ("hands: skipped: <error>" when the analysis failed). Counts the hand events of ev.
std::string HandSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev);
// One line per event ("   1.234 s  PHY L heel 1.8", a scuff or hand pivot "... (to 1.456 s)"), each with
// `indent` in front and '\n' after.
std::string PhysicsEventLines(const std::vector<PhysicsEvent>& ev, const std::string& indent);
// "heel speed +0 ms, toe height -12 ms, tip speed +0 ms"
std::string StepTimingText(const PhysicsParams& p);

}  // namespace rav
