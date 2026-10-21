// SPDX-License-Identifier: MIT
//
// Story 10-9-0: the Arms category's types (grab, release, hand pivot) as the rows of
// auto_types.cpp read them: their event sources, matches, required parts, lanes and roles.
// Story 10-9b: the elbow touching the ground.

#include <cstdio>

#include "auto_types.h"

namespace rav {
namespace {

Role HandRole(char side)
{
    return side == 'R' ? Role::RightHand : Role::LeftHand;
}

// Story 10-9b: a part of either foot has a bone (the body contacts' floor).
bool AnyFoot(const std::vector<int>& role_to_bone)
{
    for (char s : {'L', 'R'})
        for (int q = 0; q < kFootPartCount; ++q)
            if (AutoHasRole(role_to_bone, FootPartRole(s, q))) return true;
    return false;
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

// ---- Story 10-9b: the elbow touching the ground -------------------------------------------------

// Elbow: the elbows, and a foot for the floor (each foot's heel listed when no foot has a part).
void AutoPartsElbow(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    for (Role elbow : {Role::LeftElbow, Role::RightElbow})
        if (!AutoHasRole(role_to_bone, elbow)) AutoPushOnce(out, RoleName(elbow));
    if (!AnyFoot(role_to_bone))
        for (char side : {'L', 'R'}) AutoPushOnce(out, RoleName(FootPartRole(side, 0)));
}

bool AutoMatchElbow(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Elbow;
}

// The elbow, and a foot for the floor.
bool AutoMissingElbow(char side, const std::vector<int>& role_to_bone)
{
    return !AutoHasRole(role_to_bone, side == 'R' ? Role::RightElbow : Role::LeftElbow) || !AnyFoot(role_to_bone);
}

// The elbow's speed vs elbow_contact_speed, shaded where it rests low (its contact).
void AutoLanesElbow(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const BodyTrack& b = args.a->body;
    const PartTrack& pt = b.elbow[args.side == 'R' ? 1 : 0];
    const size_t     before = out.size();
    if (!b.has_floor) reason = b.floor_missing;
    else if (!pt.present) reason = pt.missing;
    else AutoPartLane(args, out, pt, "Elbow", false, args.p->elbow_contact_speed);
    if (out.size() == before) return;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Shaded: the elbow rests within %.2f leg of the floor (a contact).",
                  args.p->elbow_ground_height);
    out.back().shade_note = buf;
}

std::vector<Role> AutoRolesElbow(char side)
{
    return {side == 'R' ? Role::RightElbow : Role::LeftElbow};
}

}  // namespace rav
