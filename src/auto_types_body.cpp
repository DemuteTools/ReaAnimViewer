// SPDX-License-Identifier: MIT
//
// Story 10-9b: the Body category's types (sit, fall) as the rows of auto_types.cpp read them:
// their event sources, matches, required parts, lanes and roles; and the body contacts' event
// source, shared with Knee (Legs) and Elbow (Arms).

#include <algorithm>
#include <cstdio>

#include "auto_types.h"

namespace rav {
namespace {

bool HasAnyFoot(const std::vector<int>& role_to_bone)
{
    for (char s : {'L', 'R'})
        for (int q = 0; q < kFootPartCount; ++q)
            if (AutoHasRole(role_to_bone, FootPartRole(s, q))) return true;
    return false;
}

bool HasChest(const std::vector<int>& role_to_bone)
{
    return AutoHasRole(role_to_bone, Role::Chest) || AutoHasRole(role_to_bone, Role::Spine);
}

void PushFloor(const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    if (!HasAnyFoot(role_to_bone))
        for (char s : {'L', 'R'}) AutoPushOnce(out, RoleName(FootPartRole(s, 0)));
}

}  // namespace

// ---- Event source ------------------------------------------------------------------------------

// Knees, elbows, sits and falls (BodyContactEvents).
std::vector<PhysicsEvent> AutoSourceBody(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return BodyContactEvents(a, p);
}

// ---- Type parts (AutoMissingParts) -------------------------------------------------------------

// Sit: the hips and the chest (or spine); a foot for the floor (each foot's heel listed when no
// foot has a part).
void AutoPartsSit(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    if (!AutoHasRole(role_to_bone, Role::Hips)) AutoPushOnce(out, RoleName(Role::Hips));
    if (!HasChest(role_to_bone)) AutoPushOnce(out, RoleName(Role::Chest));
    PushFloor(role_to_bone, out);
}

// Fall: the hips or the chest (or spine); a foot for the floor.
void AutoPartsFall(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    if (!AutoHasRole(role_to_bone, Role::Hips) && !HasChest(role_to_bone)) AutoPushOnce(out, RoleName(Role::Hips));
    PushFloor(role_to_bone, out);
}

// ---- Matches -----------------------------------------------------------------------------------

bool AutoMatchSit(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Sit;
}

bool AutoMatchFall(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Fall;
}

// ---- Part missing (AutoBlockPartMissing) -------------------------------------------------------

bool AutoMissingSit(char, const std::vector<int>& role_to_bone)
{
    return !AutoHasRole(role_to_bone, Role::Hips) || !HasChest(role_to_bone) || !HasAnyFoot(role_to_bone);
}

bool AutoMissingFall(char, const std::vector<int>& role_to_bone)
{
    return (!AutoHasRole(role_to_bone, Role::Hips) && !HasChest(role_to_bone)) || !HasAnyFoot(role_to_bone);
}

// ---- Lanes -------------------------------------------------------------------------------------

// Sit: the hips' speed vs sit_contact_speed, shaded where they rest low (their contact).
void AutoLanesSit(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const BodyTrack& b = args.a->body;
    const PartTrack& pt = b.hips;
    if (!b.has_floor) {
        reason = b.floor_missing;
        return;
    }
    if (!pt.present) {
        reason = pt.missing;
        return;
    }
    AutoLane l;
    l.label = "Hips  speed";
    l.unit = "leg/s";
    l.curve = AutoLaneWindow(pt.speed, args.first, args.n, false);
    l.contact.assign(args.n, 0);
    for (size_t i = 0; i < args.n && args.first + i < pt.contact.size(); ++i) l.contact[i] = pt.contact[args.first + i];
    l.threshold = args.p->sit_contact_speed;
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: the hips rest within %.2f leg of the floor. A sit also needs the back upright,\n"
                  "the hips away from the feet and no knee down.",
                  args.p->sit_max_height);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

// Fall: the trunk's downward speed (the lower of hips and chest) vs fall_min_speed, shaded where
// it is within fall_height of the floor.
void AutoLanesFall(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const BodyTrack& b = args.a->body;
    if (!b.has_floor) {
        reason = b.floor_missing;
        return;
    }
    if (!b.hips.present && !b.chest.present) {
        reason = b.hips.missing;
        return;
    }
    const double leg = args.a->leg_length;
    AutoLane     l;
    l.label = "Trunk  downward speed";
    l.unit = "leg/s";
    l.curve.assign(args.n, 0.0);
    l.contact.assign(args.n, 0);
    for (size_t i = 0; i < args.n; ++i) {
        const size_t j = args.first + i;
        double       h = 1e9, down = 0.0;
        for (const PartTrack* pt : {&b.hips, &b.chest})
            if (pt->present && j < pt->y.size()) {
                const double y = (pt->y[j] - b.floor_y) / leg;
                if (y < h) {
                    h = y;
                    down = std::max(0.0, -pt->vy[j]);
                }
            }
        l.curve[i] = down;
        l.contact[i] = h <= args.p->fall_height ? 1 : 0;
    }
    l.threshold = args.p->fall_min_speed;
    l.above = true;
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: the hips or the back within %.2f leg of the floor. A fall lands there\n"
                  "after falling faster than the line.",
                  args.p->fall_height);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

// ---- Roles -------------------------------------------------------------------------------------

std::vector<Role> AutoRolesSit(char)
{
    return {Role::Hips, Role::Chest};
}

std::vector<Role> AutoRolesFall(char)
{
    return {Role::Hips, Role::Spine, Role::Chest};
}

}  // namespace rav
