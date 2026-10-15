// SPDX-License-Identifier: MIT
//
// Story 10-9-0: the Hands category's types (grab, release, hand pivot) as the rows of
// auto_types.cpp read them: their event sources, matches, required parts, lanes and roles.

#include "auto_types.h"

namespace rav {
namespace {

Role HandRole(char side)
{
    return side == 'R' ? Role::RightHand : Role::LeftHand;
}

}  // namespace

// ---- Event sources -----------------------------------------------------------------------------

// Grabs, releases and the pivots of a hand that came to rest with a grab (story 10-8e).
std::vector<PhysicsEvent> AutoSourceHand(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return HandEvents(a, p);
}

// The same with the pivots of any resting hand.
std::vector<PhysicsEvent> AutoSourceHandAny(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    PhysicsParams any = p;
    any.hand_pivot_after_grab = false;
    return HandEvents(a, any);
}

// ---- Type parts (AutoMissingParts) -------------------------------------------------------------

// Grab, release, hand pivot: the hand.
void AutoPartsHands(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    for (Role hand : {Role::LeftHand, Role::RightHand})
        if (!AutoHasRole(role_to_bone, hand)) AutoPushOnce(out, RoleName(hand));
}

// ---- Matches -----------------------------------------------------------------------------------

bool AutoMatchGrab(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Grab;
}

bool AutoMatchRelease(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Release;
}

// Both hand pivot kinds (their sources differ).
bool AutoMatchHandPivot(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::HandPivot;
}

// ---- Part missing (AutoBlockPartMissing) -------------------------------------------------------

bool AutoMissingHand(char side, const std::vector<int>& role_to_bone)
{
    return !AutoHasRole(role_to_bone, HandRole(side));
}

// ---- Lanes -------------------------------------------------------------------------------------

// Grab and release: the hand's speed vs hand_contact_speed.
void AutoLanesHandContact(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const HandTrack& ht = args.Hand();
    if (ht.part.present) AutoPartLane(args, out, ht.part, "Hand", false, args.p->hand_contact_speed);
    else reason = ht.part.missing;
}

// Hand pivot (both kinds): |heading rate| vs hand_pivot_rate_dps.
void AutoLanesHandPivot(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const HandTrack& ht = args.Hand();
    if (ht.part.present && !ht.has_heading) {
        reason = "no usable hand rotation";
    } else if (ht.part.present) {
        AutoLane l;
        l.label = "Hand " + std::string(1, args.side) + "  heading rate";
        l.unit = "deg/s";
        l.curve = AutoLaneWindow(ht.heading_rate_dps, args.first, args.n, true);
        l.threshold = args.p->hand_pivot_rate_dps;
        l.above = true;
        out.push_back(std::move(l));
    } else {
        reason = ht.part.missing;
    }
}

// ---- Roles -------------------------------------------------------------------------------------

// Story 10-8c: a hand block shows its hand.
std::vector<Role> AutoRolesHand(char side)
{
    return {HandRole(side)};
}

}  // namespace rav
