// SPDX-License-Identifier: MIT
//
// Story 10-9-0: the Legs category's types (step, lift-off, slide, pivot) as the rows of
// auto_types.cpp read them: their event sources, matches, required parts, lanes and roles.

#include <cmath>

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

// Step and slide: separate as a pivot, combined as a lift-off.
void AutoPartsFootSplit(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    if (separate) AutoPartsFootBoth(separate, role_to_bone, out);
    else AutoPartsFootAny(separate, role_to_bone, out);
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

// ---- Roles -------------------------------------------------------------------------------------

// Story 10-8b: an auto block shows its foot's parts (heel, toe, toe end).
std::vector<Role> AutoRolesFoot(char side)
{
    std::vector<Role> out;
    for (int q = 0; q < kFootPartCount; ++q) out.push_back(FootPartRole(side, q));
    return out;
}

}  // namespace rav
