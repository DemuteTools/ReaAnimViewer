// SPDX-License-Identifier: MIT
//
// Story 10-9-0: the Legs category's types (step, lift-off, slide, pivot; story 10-9a: jump, land;
// story 10-9b: knee)
// as the rows of auto_types.cpp read them: their event sources, matches, required parts, lanes and
// roles.

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "auto_types.h"

namespace rav {
namespace {

constexpr char kSides[2] = {'L', 'R'};
constexpr int  kHeel = static_cast<int>(FootPart::Heel);

const PartTrack& Part(const AutoLaneArgs& args, FootPart p)
{
    return args.Foot().part[static_cast<int>(p)];
}

// A scuff part's events: Each part reads the part events (FootPartScuffEvents), Longest part the
// combined scuffs' `longest`; the toe is any part but the heel (story 10-8m).
bool PartHeel(const PhysicsEvent& e, PhysicsKind scuff)
{
    return e.kind == scuff && e.part == kHeel;
}
bool PartToe(const PhysicsEvent& e, PhysicsKind scuff)
{
    return e.kind == scuff && e.part >= 0 && e.part != kHeel;
}
bool LongestHeel(const PhysicsEvent& e, PhysicsKind scuff)
{
    return e.kind == scuff && e.longest == kHeel;
}
bool LongestToe(const PhysicsEvent& e, PhysicsKind scuff)
{
    return e.kind == scuff && e.longest >= 0 && e.longest != kHeel;
}

}  // namespace

// ---- Event sources -----------------------------------------------------------------------------

std::vector<PhysicsEvent> AutoSourceFoot(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return FootEvents(a, p);
}

std::vector<PhysicsEvent> AutoSourceFootParts(const PhysicsAnalysis& a, const PhysicsParams&)
{
    return FootPartScuffEvents(a);
}

// Story 10-9a: take-offs and landings.
std::vector<PhysicsEvent> AutoSourceJump(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return JumpEvents(a, p);
}

// Story 10-9a-2: push-off take-offs (Jump's second signature).
std::vector<PhysicsEvent> AutoSourceJumpPushOff(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return JumpPushOffEvents(a, p);
}

// Story 10-9a-3: free-fall take-offs (Jump's third signature).
std::vector<PhysicsEvent> AutoSourceJumpFreeFall(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return JumpFreeFallEvents(a, p);
}

// ---- Type parts (AutoMissingParts) -------------------------------------------------------------

// Combined step, lift-off, slide: the heel, toe or toe end (all listed when none has a bone).
void AutoPartsFootAny(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    for (char side : kSides) {
        const Role heel = FootPartRole(side, 0), ball = FootPartRole(side, 1), tip = FootPartRole(side, 2);
        if (AutoHasRole(role_to_bone, heel) || AutoHasRole(role_to_bone, ball) || AutoHasRole(role_to_bone, tip)) continue;
        AutoPushOnce(out, RoleName(heel));
        AutoPushOnce(out, RoleName(ball));
        AutoPushOnce(out, RoleName(tip));
    }
}

// Pivot: the heel and the toe (or the toe end).
void AutoPartsFootBoth(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    for (char side : kSides) {
        const Role heel = FootPartRole(side, 0), ball = FootPartRole(side, 1), tip = FootPartRole(side, 2);
        if (!AutoHasRole(role_to_bone, heel)) AutoPushOnce(out, RoleName(heel));
        if (!AutoHasRole(role_to_bone, ball) && !AutoHasRole(role_to_bone, tip)) AutoPushOnce(out, RoleName(ball));
    }
}

// Step and slide (story 10-9a-3: and lift-off): separate as a pivot, combined as a lift-off.
void AutoPartsFootSplit(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    if (separate) AutoPartsFootBoth(separate, role_to_bone, out);
    else AutoPartsFootAny(separate, role_to_bone, out);
}

// Story 10-9a: jump and land: the hips, and each foot as a lift-off (story 10-9a-3: separate,
// the heel and the toe).
void AutoPartsJump(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    if (!AutoHasRole(role_to_bone, Role::Hips)) AutoPushOnce(out, RoleName(Role::Hips));
    AutoPartsFootSplit(separate, role_to_bone, out);
}

// ---- Matches -----------------------------------------------------------------------------------

bool AutoMatchStep(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::FootStep;
}

bool AutoMatchHeel(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Step && e.part == kHeel;
}

// Separate toe steps: the ball, or the tip on a rig without a ball bone.
bool AutoMatchToe(const PhysicsEvent& e, const PhysicsAnalysis& a, char side)
{
    const FootTrack& ft = a.foot[side == 'R' ? 1 : 0];
    const int toe = ft.part[static_cast<int>(FootPart::Ball)].present ? static_cast<int>(FootPart::Ball)
                                                                       : static_cast<int>(FootPart::Tip);
    return e.kind == PhysicsKind::Step && e.part == toe;
}

bool AutoMatchLift(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::LiftOff;
}

bool AutoMatchSlide(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Slide;
}

bool AutoMatchPivot(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Pivot;
}

bool AutoMatchSlideHeel(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return PartHeel(e, PhysicsKind::Slide);
}

bool AutoMatchSlideToe(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return PartToe(e, PhysicsKind::Slide);
}

bool AutoMatchPivotHeel(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return PartHeel(e, PhysicsKind::Pivot);
}

bool AutoMatchPivotToe(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return PartToe(e, PhysicsKind::Pivot);
}

bool AutoMatchSlideHeelLongest(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return LongestHeel(e, PhysicsKind::Slide);
}

bool AutoMatchSlideToeLongest(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return LongestToe(e, PhysicsKind::Slide);
}

bool AutoMatchPivotHeelLongest(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return LongestHeel(e, PhysicsKind::Pivot);
}

bool AutoMatchPivotToeLongest(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return LongestToe(e, PhysicsKind::Pivot);
}

bool AutoMatchJump(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Jump;
}

bool AutoMatchLand(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Land;
}

// Story 10-9a-3: lift-off, jump and land by part: Heel = the heel, Toe = the ball or the tip (the
// part that leaves last / touches first).
bool AutoMatchLiftHeel(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return PartHeel(e, PhysicsKind::LiftOff);
}

bool AutoMatchLiftToe(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return PartToe(e, PhysicsKind::LiftOff);
}

bool AutoMatchJumpHeel(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return PartHeel(e, PhysicsKind::Jump);
}

bool AutoMatchJumpToe(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return PartToe(e, PhysicsKind::Jump);
}

bool AutoMatchLandHeel(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return PartHeel(e, PhysicsKind::Land);
}

bool AutoMatchLandToe(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return PartToe(e, PhysicsKind::Land);
}

// ---- Part missing (AutoBlockPartMissing) -------------------------------------------------------

// Heel, slide heel: no heel.
bool AutoMissingHeel(char side, const std::vector<int>& role_to_bone)
{
    return !AutoHasRole(role_to_bone, FootPartRole(side, 0));
}

// Toe, slide toe: no toe nor toe end.
bool AutoMissingToe(char side, const std::vector<int>& role_to_bone)
{
    return !AutoHasRole(role_to_bone, FootPartRole(side, 1)) && !AutoHasRole(role_to_bone, FootPartRole(side, 2));
}

// Combined step, lift-off, slide: no heel, toe nor toe end.
bool AutoMissingFootAny(char side, const std::vector<int>& role_to_bone)
{
    return AutoMissingHeel(side, role_to_bone) && AutoMissingToe(side, role_to_bone);
}

// Every pivot kind: no heel, or no toe nor toe end.
bool AutoMissingHeelOrToe(char side, const std::vector<int>& role_to_bone)
{
    return AutoMissingHeel(side, role_to_bone) || AutoMissingToe(side, role_to_bone);
}

// Story 10-9a: jump and land (either side: a jump reads both feet): no hips, or a foot with no
// heel, toe nor toe end.
bool AutoMissingJump(char, const std::vector<int>& role_to_bone)
{
    return !AutoHasRole(role_to_bone, Role::Hips) || AutoMissingFootAny('L', role_to_bone) ||
           AutoMissingFootAny('R', role_to_bone);
}

// Story 10-9a-3: a jump / land part block: as a jump, or that side's part missing.
bool AutoMissingJumpHeel(char side, const std::vector<int>& role_to_bone)
{
    return AutoMissingJump(side, role_to_bone) || AutoMissingHeel(side, role_to_bone);
}

bool AutoMissingJumpToe(char side, const std::vector<int>& role_to_bone)
{
    return AutoMissingJump(side, role_to_bone) || AutoMissingToe(side, role_to_bone);
}

// ---- Lanes -------------------------------------------------------------------------------------

namespace {

const char* const kLanePartName[kFootPartCount] = {"Heel", "Ball", "Tip"};

// Each present foot part's speed (or horizontal speed).
void FootParts(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason, bool horizontal, double thr)
{
    const FootTrack& ft = args.Foot();
    for (int i = 0; i < kFootPartCount; ++i)
        if (ft.part[i].present) AutoPartLane(args, out, ft.part[i], kLanePartName[i], horizontal, thr);
    if (out.empty()) reason = ft.part[0].missing;
}

}  // namespace

// Combined step and lift-off: each present foot part's speed vs contact_speed.
void AutoLanesFootContact(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    FootParts(args, out, reason, false, args.p->contact_speed);
}

// Slide: each present part's hspeed vs slide_speed.
void AutoLanesFootSlide(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    FootParts(args, out, reason, true, args.p->slide_speed);
}

void AutoLanesHeel(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const PartTrack& heel = Part(args, FootPart::Heel);
    if (heel.present) AutoPartLane(args, out, heel, "Heel", false, args.p->contact_speed);
    else reason = heel.missing;
}

// The ball, else the tip.
void AutoLanesToe(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const PartTrack& ball = Part(args, FootPart::Ball);
    const PartTrack& tip = Part(args, FootPart::Tip);
    if (ball.present) AutoPartLane(args, out, ball, "Ball", false, args.p->contact_speed);
    else if (tip.present) AutoPartLane(args, out, tip, "Tip", false, args.p->contact_speed);
    else reason = ball.missing;
}

// 10-8m: the heel part of a slide.
void AutoLanesSlideHeel(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const PartTrack& heel = Part(args, FootPart::Heel);
    if (heel.present) AutoPartLane(args, out, heel, "Heel", true, args.p->slide_speed);
    else reason = heel.missing;
}

// 10-8m: the toe part of a slide (ball and tip).
void AutoLanesSlideToe(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const PartTrack& ball = Part(args, FootPart::Ball);
    const PartTrack& tip = Part(args, FootPart::Tip);
    if (ball.present) AutoPartLane(args, out, ball, "Ball", true, args.p->slide_speed);
    if (tip.present) AutoPartLane(args, out, tip, "Tip", true, args.p->slide_speed);
    if (out.empty()) reason = ball.missing;
}

// Every pivot kind (10-8m: a pivot part reads the same yaw rate): |yaw rate| (yaw where valid,
// else heading where valid, else 0) vs pivot_rate_dps.
void AutoLanesPivot(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const FootTrack& ft = args.Foot();
    if (!ft.has_yaw) {  // FindPivots needs the heel -> toe yaw
        reason = "pivot needs the heel and the toe";
        return;
    }
    AutoLane l;
    l.label = "Pivot " + std::string(1, args.side) + "  yaw rate";
    l.unit = "deg/s";
    l.curve.assign(args.n, 0.0);
    for (size_t i = 0; i < args.n; ++i) {
        const size_t j = args.first + i;
        if (j < ft.yaw_valid.size() && ft.yaw_valid[j] && j < ft.yaw_rate_dps.size())
            l.curve[i] = std::fabs(ft.yaw_rate_dps[j]);
        else if (j < ft.heading_valid.size() && ft.heading_valid[j] && j < ft.heading_rate_dps.size())
            l.curve[i] = std::fabs(ft.heading_rate_dps[j]);
    }
    l.threshold = args.p->pivot_rate_dps;
    l.above = true;
    out.push_back(std::move(l));
}

// Story 10-9a: land (and jump's first lane): the hips' vertical speed (signed, no threshold),
// shaded where both feet are off (every present part of both feet free).
void AutoLanesLand(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const PhysicsAnalysis& a = *args.a;
    if (a.hips_vy.empty()) {
        reason = a.hips_missing.empty() ? std::string("jump needs the hips") : a.hips_missing;
        return;
    }
    bool feet[2] = {false, false};
    for (int s = 0; s < 2; ++s)
        for (const PartTrack& pt : a.foot[s].part) feet[s] = feet[s] || pt.present;
    if (!feet[0] || !feet[1]) {
        reason = "jump needs a heel, toe or toe end on each foot";
        return;
    }
    AutoLane l;
    l.label = "Hips  vertical speed";
    l.unit = "leg/s";
    l.curve = AutoLaneWindow(a.hips_vy, args.first, args.n, false);
    l.contact.assign(args.n, 0);
    for (size_t i = 0; i < args.n; ++i) {
        const size_t j = args.first + i;
        bool         on = false;
        for (const FootTrack& f : a.foot)
            for (const PartTrack& pt : f.part) on = on || (pt.present && j < pt.contact.size() && pt.contact[j]);
        l.contact[i] = on ? 0 : 1;
    }
    l.no_threshold = true;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: both feet are off. A jump is a shaded run of %.0f ms at least\n"
                  "(this sensitivity's minimum flight) where the hips fall freely\n"
                  "(their vertical speed drops steadily).",
                  args.p->jump_min_flight_s * 1000.0);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

// Story 10-9a-2: jump: land's lane, then the hips' upward acceleration (g, downward shown as 0)
// against the push-off threshold, shaded where a foot is on the ground.
void AutoLanesJump(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const size_t before = out.size();
    AutoLanesLand(args, out, reason);
    if (out.size() == before) return;
    const PhysicsAnalysis&    a = *args.a;
    const std::vector<double> acc = HipsUpAccelerationG(a, *args.p);
    if (acc.empty()) return;
    AutoLane l;
    l.label = "Hips  upward acceleration";
    l.unit = "g";
    l.curve = AutoLaneWindow(acc, args.first, args.n, false);
    for (double& v : l.curve) v = std::max(0.0, v);
    l.contact.assign(args.n, 0);
    for (size_t i = 0; i < args.n; ++i) {
        const size_t j = args.first + i;
        bool         on = false;
        for (const FootTrack& f : a.foot)
            for (const PartTrack& pt : f.part) on = on || (pt.present && j < pt.contact.size() && pt.contact[j]);
        l.contact[i] = on ? 1 : 0;
    }
    l.threshold = args.p->jump_impulse_g;
    l.above = true;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: a foot is on the ground. A push-off is a peak above the line while a foot\n"
                  "is on, with both feet leaving within %.0f ms after it and no hand holding on.",
                  args.p->jump_impulse_window_s * 1000.0);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

// ---- Roles -------------------------------------------------------------------------------------

// Story 10-8b: an auto block shows its foot's parts (heel, toe, toe end).
std::vector<Role> AutoRolesFoot(char side)
{
    std::vector<Role> out;
    for (int q = 0; q < kFootPartCount; ++q) out.push_back(FootPartRole(side, q));
    return out;
}

// Story 10-9a: a jump or land block shows the hips and both feet's parts.
std::vector<Role> AutoRolesJump(char)
{
    std::vector<Role> out = {Role::Hips};
    for (char side : kSides)
        for (int q = 0; q < kFootPartCount; ++q) out.push_back(FootPartRole(side, q));
    return out;
}

// ---- Story 10-9b: the knee touching the ground --------------------------------------------------

// Knee: the knees, and a foot for the floor (each foot's heel listed when no foot has a part).
void AutoPartsKnee(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    for (Role knee : {Role::LeftKnee, Role::RightKnee})
        if (!AutoHasRole(role_to_bone, knee)) AutoPushOnce(out, RoleName(knee));
    if (AutoMissingFootAny('L', role_to_bone) && AutoMissingFootAny('R', role_to_bone))
        for (char side : kSides) AutoPushOnce(out, RoleName(FootPartRole(side, 0)));
}

bool AutoMatchKnee(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Knee;
}

// The knee, and a foot for the floor.
bool AutoMissingKnee(char side, const std::vector<int>& role_to_bone)
{
    return !AutoHasRole(role_to_bone, side == 'R' ? Role::RightKnee : Role::LeftKnee) ||
           (AutoMissingFootAny('L', role_to_bone) && AutoMissingFootAny('R', role_to_bone));
}

// The knee's speed vs knee_contact_speed, shaded where it rests low (its contact).
void AutoLanesKnee(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const BodyTrack& b = args.a->body;
    const PartTrack& pt = b.knee[args.side == 'R' ? 1 : 0];
    const size_t     before = out.size();
    if (!b.has_floor) reason = b.floor_missing;
    else if (!pt.present) reason = pt.missing;
    else AutoPartLane(args, out, pt, "Knee", false, args.p->knee_contact_speed);
    if (out.size() == before) return;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Shaded: the knee rests within %.2f leg of the floor (a contact).",
                  args.p->knee_ground_height);
    out.back().shade_note = buf;
}

std::vector<Role> AutoRolesKnee(char side)
{
    return {side == 'R' ? Role::RightKnee : Role::LeftKnee};
}

}  // namespace rav
