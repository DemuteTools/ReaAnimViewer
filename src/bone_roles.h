// SPDX-License-Identifier: MIT
//
// Roles (Epic 10): presets name roles (left heel, pelvis...), never bones. A role is bound
// to a bone of the loaded skeleton by its name, guessed here (Mixamo, Unreal; editable and
// remembered per skeleton from 10-2 on).
//
// Names are compared ASCII case-insensitively after the namespace is stripped: anything up
// to the last ':' ("mixamorig:LeftFoot"), or a "mixamorig<N>_" prefix
// ("mixamorig1_LeftFoot").
//
// Pure C++17, header-only. Host-tested (tests/bone_roles_test.cpp).

#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "bone_events.h"

namespace rav {

enum class Role : int { LeftHeel = 0, LeftToe, RightHeel, RightToe, LeftKnee, RightKnee, LeftUpLeg, RightUpLeg, Hips, Count };

inline const char* RoleName(Role r)
{
    switch (r) {
    case Role::LeftHeel: return "left heel";
    case Role::LeftToe: return "left toe";
    case Role::RightHeel: return "right heel";
    case Role::RightToe: return "right toe";
    case Role::LeftKnee: return "left knee";
    case Role::RightKnee: return "right knee";
    case Role::LeftUpLeg: return "left hip (up leg)";
    case Role::RightUpLeg: return "right hip (up leg)";
    case Role::Hips: return "hips";
    default: return "?";
    }
}

// The stable key a record writes for a role (snake_case, never an index): "left_heel"...
// Written as "role:<key>". New roles get new keys; a key never changes meaning.
inline const char* RoleKey(Role r)
{
    switch (r) {
    case Role::LeftHeel: return "left_heel";
    case Role::LeftToe: return "left_toe";
    case Role::RightHeel: return "right_heel";
    case Role::RightToe: return "right_toe";
    case Role::LeftKnee: return "left_knee";
    case Role::RightKnee: return "right_knee";
    case Role::LeftUpLeg: return "left_up_leg";
    case Role::RightUpLeg: return "right_up_leg";
    case Role::Hips: return "hips";
    default: return "";
    }
}

// The role of a key (without the "role:" prefix). False when the key is not a known role.
inline bool RoleFromKey(const std::string& key, Role* out)
{
    for (int r = 0; r < static_cast<int>(Role::Count); ++r)
        if (key == RoleKey(static_cast<Role>(r))) {
            if (out) *out = static_cast<Role>(r);
            return true;
        }
    return false;
}

// The side of a role: 'L', 'R', or 0 (none, e.g. hips).
inline char RoleSide(Role r)
{
    switch (r) {
    case Role::LeftHeel: case Role::LeftToe: case Role::LeftKnee: case Role::LeftUpLeg: return 'L';
    case Role::RightHeel: case Role::RightToe: case Role::RightKnee: case Role::RightUpLeg: return 'R';
    default: return 0;
    }
}

// The role without its side, for signal names: "heel", "toe", "knee", "hip", "hips".
inline const char* RolePart(Role r)
{
    switch (r) {
    case Role::LeftHeel: case Role::RightHeel: return "heel";
    case Role::LeftToe: case Role::RightToe: return "toe";
    case Role::LeftKnee: case Role::RightKnee: return "knee";
    case Role::LeftUpLeg: case Role::RightUpLeg: return "hip";
    case Role::Hips: return "hips";
    default: return "?";
    }
}

// The short label of a role for signal names: "L heel", "R knee", "hips".
inline std::string RoleShortLabel(Role r)
{
    const char side = RoleSide(r);
    std::string s;
    if (side) {
        s += side;
        s += ' ';
    }
    return s + RolePart(r);
}

// The bone name without its namespace, lower-cased (ASCII).
inline std::string NormalizeBoneName(const std::string& name)
{
    std::string s = name;
    const size_t colon = s.rfind(':');
    if (colon != std::string::npos) s = s.substr(colon + 1);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string pre = "mixamorig";
    if (s.compare(0, pre.size(), pre) == 0) {
        size_t i = pre.size();
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (i < s.size() && s[i] == '_') s = s.substr(i + 1);
    }
    return s;
}

// Bone index per role (-1 = not found), indexed by Role. Candidates in order of preference.
inline std::vector<int> GuessRoleMapping(const std::vector<std::string>& bone_names)
{
    static const std::vector<std::vector<const char*>> kCandidates = {
        {"leftfoot", "foot_l"},                      // LeftHeel
        {"lefttoebase", "lefttoe_end", "ball_l"},    // LeftToe
        {"rightfoot", "foot_r"},                     // RightHeel
        {"righttoebase", "righttoe_end", "ball_r"},  // RightToe
        {"leftleg", "calf_l"},                       // LeftKnee (the shin joint sits at the knee)
        {"rightleg", "calf_r"},                      // RightKnee
        {"leftupleg", "thigh_l"},                    // LeftUpLeg (the thigh joint sits at the hip)
        {"rightupleg", "thigh_r"},                   // RightUpLeg
        {"hips", "pelvis"},                          // Hips
    };
    std::vector<std::string> norm;
    norm.reserve(bone_names.size());
    for (const std::string& n : bone_names) norm.push_back(NormalizeBoneName(n));

    std::vector<int> map(static_cast<size_t>(Role::Count), -1);
    for (size_t r = 0; r < kCandidates.size(); ++r)
        for (const char* cand : kCandidates[r]) {
            for (size_t b = 0; b < norm.size() && map[r] < 0; ++b)
                if (norm[b] == cand) map[r] = static_cast<int>(b);
            if (map[r] >= 0) break;
        }
    return map;
}

// The roles a preset's blocks use (conditions, references, strength), each once, in Role order.
inline std::vector<Role> RolesUsed(const std::vector<Block>& blocks)
{
    std::vector<char> used(static_cast<size_t>(Role::Count), 0);
    auto mark = [&](const SignalSpec& s) {
        for (int r : s.bones)
            if (r >= 0 && r < static_cast<int>(Role::Count)) used[r] = 1;
        if (s.reference == Reference::Bones)
            for (int r : s.ref_bones)
                if (r >= 0 && r < static_cast<int>(Role::Count)) used[r] = 1;
    };
    for (const Block& b : blocks) {
        for (const Condition& c : b.conditions) mark(c.signal);
        mark(b.strength_signal);
    }
    std::vector<Role> out;
    for (int r = 0; r < static_cast<int>(Role::Count); ++r)
        if (used[r]) out.push_back(static_cast<Role>(r));
    return out;
}

// Rewrites a role preset's bones into track indices: role_to_track[role] = the track (-1 =
// no track). False when a role used has no track (`missing` lists them, ", "-separated).
inline bool BindRoles(std::vector<Block>& blocks, const std::vector<int>& role_to_track, std::string* missing)
{
    std::vector<std::string> names;  // the missing roles, each once
    auto bind = [&](std::vector<int>& idx) {
        for (int& r : idx) {
            const int t = (r >= 0 && r < static_cast<int>(role_to_track.size())) ? role_to_track[r] : -1;
            if (t < 0) {
                const std::string nm = (r >= 0 && r < static_cast<int>(Role::Count)) ? RoleName(static_cast<Role>(r))
                                                                                     : "?";
                if (std::find(names.begin(), names.end(), nm) == names.end()) names.push_back(nm);
            }
            r = t;
        }
    };
    for (Block& b : blocks) {
        for (Condition& c : b.conditions) {
            bind(c.signal.bones);
            if (c.signal.reference == Reference::Bones) bind(c.signal.ref_bones);
        }
        bind(b.strength_signal.bones);
        if (b.strength_signal.reference == Reference::Bones) bind(b.strength_signal.ref_bones);
    }
    std::string miss;
    for (const std::string& nm : names) miss += (miss.empty() ? "" : ", ") + nm;
    if (missing) *missing = miss;
    return names.empty();
}

}  // namespace rav
