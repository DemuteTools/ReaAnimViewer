// SPDX-License-Identifier: MIT
//
// Roles (Epic 10): presets name roles (left heel, pelvis...), never bones. A role is bound
// to a bone of the loaded skeleton by its name, guessed here (Mixamo, Unreal; editable and
// remembered per skeleton from 10-2 on).
//
// A role is a point: the origin of its bone, where the skeleton view draws the joint. So a
// role is named for that joint: the knee is the shin bone (LeftLeg, calf_l), the hip the thigh
// (LeftUpLeg, thigh_l), the shoulder the upper arm (LeftArm, upperarm_l), the elbow the forearm
// (LeftForeArm, lowerarm_l). Mixamo's LeftShoulder (Unreal clavicle_l) starts at the base of
// the neck: it is the clavicle.
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

// Role values are ids within one run (BoneRefId), never written: records, presets and
// roles.txt write keys. New roles are appended; the lists show them in RolesInListOrder.
enum class Role : int {
    LeftHeel = 0, LeftToe, RightHeel, RightToe, LeftKnee, RightKnee, LeftUpLeg, RightUpLeg, Hips,
    // The toe tips, the arms, the spine and the head.
    LeftToeEnd, RightToeEnd, LeftClavicle, RightClavicle, LeftShoulder, RightShoulder, LeftElbow, RightElbow,
    LeftHand, RightHand, Spine, Chest, Neck, Head,
    Count
};

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
    case Role::LeftToeEnd: return "left toe end";
    case Role::RightToeEnd: return "right toe end";
    case Role::LeftClavicle: return "left clavicle";
    case Role::RightClavicle: return "right clavicle";
    case Role::LeftShoulder: return "left shoulder (upper arm)";
    case Role::RightShoulder: return "right shoulder (upper arm)";
    case Role::LeftElbow: return "left elbow";
    case Role::RightElbow: return "right elbow";
    case Role::LeftHand: return "left hand";
    case Role::RightHand: return "right hand";
    case Role::Spine: return "spine";
    case Role::Chest: return "chest";
    case Role::Neck: return "neck";
    case Role::Head: return "head";
    default: return "?";
    }
}

// The stable key a record writes for a role (snake_case, never an index): "left_heel"...
// Written as "role:<key>". New roles get new keys; a key never changes meaning. A user's own
// role (10-3e) whose key became built-in keeps its map lines: they are read for the built-in.
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
    case Role::LeftToeEnd: return "left_toe_end";
    case Role::RightToeEnd: return "right_toe_end";
    case Role::LeftClavicle: return "left_clavicle";
    case Role::RightClavicle: return "right_clavicle";
    case Role::LeftShoulder: return "left_shoulder";
    case Role::RightShoulder: return "right_shoulder";
    case Role::LeftElbow: return "left_elbow";
    case Role::RightElbow: return "right_elbow";
    case Role::LeftHand: return "left_hand";
    case Role::RightHand: return "right_hand";
    case Role::Spine: return "spine";
    case Role::Chest: return "chest";
    case Role::Neck: return "neck";
    case Role::Head: return "head";
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

// Story 10-3e -- custom roles. The key of a role the user creates from its name: lower-case
// ASCII snake_case ([a-z0-9_]): letters and digits kept (lower-cased), every run of other
// bytes (spaces, punctuation, accents) becomes one '_', trimmed at both ends. "Sword Tip"
// -> "sword_tip". "" when nothing is left (the name is refused).
inline std::string RoleKeyFromName(const std::string& name)
{
    std::string key;
    bool        pending = false;  // a run of other bytes since the last kept one
    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool          alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum) {
            pending = true;
            continue;
        }
        if (pending && !key.empty()) key += '_';
        pending = false;
        key += static_cast<char>(std::tolower(c));
    }
    return key;
}

// A role key read as a name when no definition gives one (a colleague's role): '_' read as
// a space. "tail_end" -> "tail end".
inline std::string RoleNameFromKey(const std::string& key)
{
    std::string s = key;
    for (char& c : s)
        if (c == '_') c = ' ';
    return s;
}

// The side of a role: 'L', 'R', or 0 (none, e.g. hips).
inline char RoleSide(Role r)
{
    switch (r) {
    case Role::LeftHeel: case Role::LeftToe: case Role::LeftKnee: case Role::LeftUpLeg: case Role::LeftToeEnd:
    case Role::LeftClavicle: case Role::LeftShoulder: case Role::LeftElbow: case Role::LeftHand: return 'L';
    case Role::RightHeel: case Role::RightToe: case Role::RightKnee: case Role::RightUpLeg: case Role::RightToeEnd:
    case Role::RightClavicle: case Role::RightShoulder: case Role::RightElbow: case Role::RightHand: return 'R';
    default: return 0;
    }
}

// The role without its side, for signal names: "heel", "toe", "knee", "hip", "hips", "hand"...
inline const char* RolePart(Role r)
{
    switch (r) {
    case Role::LeftHeel: case Role::RightHeel: return "heel";
    case Role::LeftToe: case Role::RightToe: return "toe";
    case Role::LeftKnee: case Role::RightKnee: return "knee";
    case Role::LeftUpLeg: case Role::RightUpLeg: return "hip";
    case Role::Hips: return "hips";
    case Role::LeftToeEnd: case Role::RightToeEnd: return "toe end";
    case Role::LeftClavicle: case Role::RightClavicle: return "clavicle";
    case Role::LeftShoulder: case Role::RightShoulder: return "shoulder";
    case Role::LeftElbow: case Role::RightElbow: return "elbow";
    case Role::LeftHand: case Role::RightHand: return "hand";
    case Role::Spine: return "spine";
    case Role::Chest: return "chest";
    case Role::Neck: return "neck";
    case Role::Head: return "head";
    default: return "?";
    }
}

// The group a role is listed under in the roles window and the Bone menu.
enum class RoleGroup { Legs, Arms, Body };

inline RoleGroup RoleGroupOf(Role r)
{
    switch (r) {
    case Role::LeftClavicle: case Role::RightClavicle: case Role::LeftShoulder: case Role::RightShoulder:
    case Role::LeftElbow: case Role::RightElbow: case Role::LeftHand: case Role::RightHand: return RoleGroup::Arms;
    case Role::Hips: case Role::Spine: case Role::Chest: case Role::Neck: case Role::Head: return RoleGroup::Body;
    default: return RoleGroup::Legs;
    }
}

// The group's caption: "Legs", "Arms", "Body".
inline const char* RoleGroupName(RoleGroup g)
{
    switch (g) {
    case RoleGroup::Legs: return "Legs";
    case RoleGroup::Arms: return "Arms";
    default: return "Body";
    }
}

// Every built-in role once, in the order the lists show them: by group (legs, arms, body),
// from the end of a limb inwards, left before right; the body from the hips up.
inline const std::vector<Role>& RolesInListOrder()
{
    static const std::vector<Role> kOrder = {
        Role::LeftHeel, Role::LeftToe, Role::LeftToeEnd, Role::RightHeel, Role::RightToe, Role::RightToeEnd,
        Role::LeftKnee, Role::RightKnee, Role::LeftUpLeg, Role::RightUpLeg,
        Role::LeftHand, Role::RightHand, Role::LeftElbow, Role::RightElbow, Role::LeftShoulder, Role::RightShoulder,
        Role::LeftClavicle, Role::RightClavicle,
        Role::Hips, Role::Spine, Role::Chest, Role::Neck, Role::Head,
    };
    return kOrder;
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

// One guess candidate: a bone name (normalized) and whether it is the role's own bone. A
// stand-in (own = false) is the nearest bone of the same chain, taken when the rig lacks the
// role's own: the tool still finds a bone on a rig that has fewer.
struct RoleCandidate {
    const char* name;
    bool        own;
};

// The candidates of each role (indexed by Role), in order of preference: Mixamo, then Unreal
// (the UE4 and UE5 mannequins), then the stand-ins. The toe keeps its 10-2 order (its tip before
// ball_l). A stand-in never leaves the role's limb or the spine.
inline const std::vector<std::vector<RoleCandidate>>& RoleCandidates()
{
    constexpr bool kOwn = true, kNear = false;
    static const std::vector<std::vector<RoleCandidate>> kTable = {
        {{"leftfoot", kOwn}, {"foot_l", kOwn}},                              // LeftHeel
        {{"lefttoebase", kOwn}, {"lefttoe_end", kNear}, {"ball_l", kOwn}},    // LeftToe (its tip without a base)
        {{"rightfoot", kOwn}, {"foot_r", kOwn}},                             // RightHeel
        {{"righttoebase", kOwn}, {"righttoe_end", kNear}, {"ball_r", kOwn}},  // RightToe
        {{"leftleg", kOwn}, {"calf_l", kOwn}},        // LeftKnee (the shin joint sits at the knee)
        {{"rightleg", kOwn}, {"calf_r", kOwn}},       // RightKnee
        {{"leftupleg", kOwn}, {"thigh_l", kOwn}},     // LeftUpLeg (the thigh joint sits at the hip)
        {{"rightupleg", kOwn}, {"thigh_r", kOwn}},    // RightUpLeg
        {{"hips", kOwn}, {"pelvis", kOwn}},           // Hips
        // LeftToeEnd: Unreal has no toe tip, its toe (ball_l) stands in.
        {{"lefttoe_end", kOwn}, {"lefttoebase", kNear}, {"ball_l", kNear}},
        {{"righttoe_end", kOwn}, {"righttoebase", kNear}, {"ball_r", kNear}},  // RightToeEnd
        {{"leftshoulder", kOwn}, {"clavicle_l", kOwn}},   // LeftClavicle
        {{"rightshoulder", kOwn}, {"clavicle_r", kOwn}},  // RightClavicle
        {{"leftarm", kOwn}, {"upperarm_l", kOwn}},        // LeftShoulder (the upper arm joint sits at the shoulder)
        {{"rightarm", kOwn}, {"upperarm_r", kOwn}},       // RightShoulder
        {{"leftforearm", kOwn}, {"lowerarm_l", kOwn}},    // LeftElbow (the forearm joint sits at the elbow)
        {{"rightforearm", kOwn}, {"lowerarm_r", kOwn}},   // RightElbow
        {{"lefthand", kOwn}, {"hand_l", kOwn}},           // LeftHand (the wrist)
        {{"righthand", kOwn}, {"hand_r", kOwn}},          // RightHand
        {{"spine", kOwn}, {"spine_01", kOwn}},            // Spine (the first bone above the hips)
        // Chest: the bone the clavicles and the neck hang from (Mixamo, UE5, UE4); else the
        // highest spine bone the rig has.
        {{"spine2", kOwn}, {"spine_05", kOwn}, {"spine_03", kOwn}, {"spine1", kNear}, {"spine", kNear},
         {"spine_04", kNear}, {"spine_02", kNear}, {"spine_01", kNear}},
        {{"neck", kOwn}, {"neck_01", kOwn}},              // Neck
        {{"head", kOwn}},                                 // Head
    };
    return kTable;
}

// Bone index per role (-1 = not found), indexed by Role: the first candidate the rig has.
inline std::vector<int> GuessRoleMapping(const std::vector<std::string>& bone_names)
{
    const std::vector<std::vector<RoleCandidate>>& table = RoleCandidates();
    std::vector<std::string> norm;
    norm.reserve(bone_names.size());
    for (const std::string& n : bone_names) norm.push_back(NormalizeBoneName(n));

    std::vector<int> map(static_cast<size_t>(Role::Count), -1);
    for (size_t r = 0; r < table.size() && r < map.size(); ++r)
        for (const RoleCandidate& cand : table[r]) {
            for (size_t b = 0; b < norm.size() && map[r] < 0; ++b)
                if (norm[b] == cand.name) map[r] = static_cast<int>(b);
            if (map[r] >= 0) break;
        }
    return map;
}

// True when `bone_name` (any namespace) is one of the role's own bones, not a stand-in: a bone
// picked in the 3D view that plays several roles gives the one it is named for (ball_l: the
// toe, not the toe end it stands in for).
inline bool IsOwnBoneOfRole(Role r, const std::string& bone_name)
{
    const std::vector<std::vector<RoleCandidate>>& table = RoleCandidates();
    const size_t                                   i = static_cast<size_t>(r);
    if (i >= table.size()) return false;
    const std::string n = NormalizeBoneName(bone_name);
    for (const RoleCandidate& cand : table[i])
        if (cand.own && n == cand.name) return true;
    return false;
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
