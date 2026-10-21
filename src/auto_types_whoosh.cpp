// SPDX-License-Identifier: MIT
//
// Story 10-9c: the whoosh types (kick in Legs; swing and punch in Arms) as the rows of
// auto_types.cpp read them: their event sources, matches, required parts, lanes and roles. The
// detector is WhooshEvents (motion_whoosh.cpp).

#include "auto_types.h"

namespace rav {

// Shared with auto_types_legs.cpp / auto_types_arms.cpp (declared in auto_types.cpp).
void AutoPartsFootSplit(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMissingFootAny(char side, const std::vector<int>& role_to_bone);
bool AutoMissingHeel(char side, const std::vector<int>& role_to_bone);
bool AutoMissingToe(char side, const std::vector<int>& role_to_bone);
bool AutoMissingHand(char side, const std::vector<int>& role_to_bone);
std::vector<Role> AutoRolesFoot(char side);
std::vector<Role> AutoRolesHand(char side);

namespace {

constexpr int kHeelPart = static_cast<int>(FootPart::Heel);

// The torso the speeds are read against: the chest, else the spine, else the hips.
bool HasTorso(const std::vector<int>& role_to_bone)
{
    return AutoHasRole(role_to_bone, Role::Chest) || AutoHasRole(role_to_bone, Role::Spine) ||
           AutoHasRole(role_to_bone, Role::Hips);
}

void PushTorso(const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    if (!HasTorso(role_to_bone)) AutoPushOnce(out, RoleName(Role::Chest));
}

const WhooshTrack& Foot(const AutoLaneArgs& args)
{
    return args.a->whoosh_foot[args.side == 'R' ? 1 : 0];
}

const WhooshTrack& Hand(const AutoLaneArgs& args)
{
    return args.a->whoosh_hand[args.side == 'R' ? 1 : 0];
}

// A whoosh lane: the limb's speed relative to the torso vs the type's speed (fires above).
void SpeedLane(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason, const WhooshTrack& w,
               const char* name, double thr)
{
    if (!w.present) {
        reason = w.missing;
        return;
    }
    AutoLane l;
    l.label = std::string(name) + " " + std::string(1, args.side) + "  speed vs torso";
    l.unit = "leg/s";
    l.curve = AutoLaneWindow(w.speed, args.first, args.n, false);
    l.threshold = thr;
    l.above = true;
    out.push_back(std::move(l));
}

}  // namespace

// ---- Event sources -----------------------------------------------------------------------------

std::vector<PhysicsEvent> AutoSourceWhoosh(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return WhooshEvents(a, p, false);
}

std::vector<PhysicsEvent> AutoSourceWhooshStart(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return WhooshEvents(a, p, true);
}

// ---- Type parts (AutoMissingParts) -------------------------------------------------------------

// Kick: the torso, and each foot as a lift-off (separate: the heel and the toe or toe end).
void AutoPartsKick(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    PushTorso(role_to_bone, out);
    AutoPartsFootSplit(separate, role_to_bone, out);
}

// Swing, punch: the torso and the hands.
void AutoPartsWhooshHand(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    PushTorso(role_to_bone, out);
    for (Role hand : {Role::LeftHand, Role::RightHand})
        if (!AutoHasRole(role_to_bone, hand)) AutoPushOnce(out, RoleName(hand));
}

// ---- Matches -----------------------------------------------------------------------------------

bool AutoMatchKick(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Kick;
}

bool AutoMatchKickHeel(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Kick && e.part == kHeelPart;
}

// The toe: the ball or the toe tip.
bool AutoMatchKickToe(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Kick && e.part >= 0 && e.part != kHeelPart;
}

bool AutoMatchSwing(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Swing;
}

bool AutoMatchPunch(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Punch;
}

// ---- Part missing (AutoBlockPartMissing) -------------------------------------------------------

bool AutoMissingKick(char side, const std::vector<int>& role_to_bone)
{
    return !HasTorso(role_to_bone) || AutoMissingFootAny(side, role_to_bone);
}

bool AutoMissingKickHeel(char side, const std::vector<int>& role_to_bone)
{
    return !HasTorso(role_to_bone) || AutoMissingHeel(side, role_to_bone);
}

bool AutoMissingKickToe(char side, const std::vector<int>& role_to_bone)
{
    return !HasTorso(role_to_bone) || AutoMissingToe(side, role_to_bone);
}

bool AutoMissingWhooshHand(char side, const std::vector<int>& role_to_bone)
{
    return !HasTorso(role_to_bone) || AutoMissingHand(side, role_to_bone);
}

// ---- Lanes -------------------------------------------------------------------------------------

void AutoLanesKick(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    SpeedLane(args, out, reason, Foot(args), "Foot", args.p->kick_speed);
}

void AutoLanesSwing(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    SpeedLane(args, out, reason, Hand(args), "Hand", args.p->swing_speed);
}

void AutoLanesPunch(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    SpeedLane(args, out, reason, Hand(args), "Hand", args.p->punch_speed);
}

// ---- Roles -------------------------------------------------------------------------------------

std::vector<Role> AutoRolesKick(char side)
{
    return AutoRolesFoot(side);
}

std::vector<Role> AutoRolesWhooshHand(char side)
{
    return AutoRolesHand(side);
}

}  // namespace rav
