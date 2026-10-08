// SPDX-License-Identifier: MIT
//
// Host test of the role mapping (src/bone_roles.h, Epic 10, spike 10-0): Mixamo,
// mixamorig<N>_ and Unreal (UE4, UE5) names, the keys and the list order. No REAPER, no
// Windows: any C++17 compiler.

#include "bone_presets.h"
#include "bone_roles.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

int R(Role r)
{
    return static_cast<int>(r);
}

}  // namespace

int main()
{
    // Namespace stripping.
    CHECK(NormalizeBoneName("mixamorig:LeftFoot") == "leftfoot");
    CHECK(NormalizeBoneName("Character1:mixamorig:LeftFoot") == "leftfoot");
    CHECK(NormalizeBoneName("mixamorig1_LeftFoot") == "leftfoot");
    CHECK(NormalizeBoneName("mixamorig_LeftFoot") == "leftfoot");
    CHECK(NormalizeBoneName("mixamorig12_LeftFoot") == "leftfoot");
    CHECK(NormalizeBoneName("foot_l") == "foot_l");
    CHECK(NormalizeBoneName("mixamorigLeftFoot") == "mixamorigleftfoot");  // no separator: kept

    // Mixamo (colon namespace).
    {
        std::vector<std::string> names = {"mixamorig:Hips", "mixamorig:LeftUpLeg", "mixamorig:LeftFoot",
                                          "mixamorig:LeftToeBase", "mixamorig:LeftToe_End", "mixamorig:RightFoot",
                                          "mixamorig:RightToeBase"};
        auto m = GuessRoleMapping(names);
        CHECK(m[R(Role::Hips)] == 0);
        CHECK(m[R(Role::LeftHeel)] == 2);
        CHECK(m[R(Role::LeftToe)] == 3);  // ToeBase preferred over Toe_End
        CHECK(m[R(Role::RightHeel)] == 5);
        CHECK(m[R(Role::RightToe)] == 6);
    }
    // mixamorig1_ prefix, ToeBase missing -> Toe_End; case-insensitive.
    {
        std::vector<std::string> names = {"mixamorig1_RightToe_End", "MIXAMORIG1_LEFTFOOT", "mixamorig1_lefttoe_end",
                                          "mixamorig1_RightFoot"};
        auto m = GuessRoleMapping(names);
        CHECK(m[R(Role::LeftHeel)] == 1);
        CHECK(m[R(Role::LeftToe)] == 2);
        CHECK(m[R(Role::RightHeel)] == 3);
        CHECK(m[R(Role::RightToe)] == 0);
        CHECK(m[R(Role::Hips)] == -1);
    }
    // Unreal.
    {
        std::vector<std::string> names = {"root", "pelvis", "foot_l", "ball_l", "foot_r", "ball_r"};
        auto m = GuessRoleMapping(names);
        CHECK(m[R(Role::Hips)] == 1);
        CHECK(m[R(Role::LeftHeel)] == 2);
        CHECK(m[R(Role::LeftToe)] == 3);
        CHECK(m[R(Role::RightHeel)] == 4);
        CHECK(m[R(Role::RightToe)] == 5);
    }

    // Knee roles: Mixamo LeftLeg / RightLeg (any namespace), Unreal calf_l / calf_r.
    {
        auto m = GuessRoleMapping({"mixamorig:LeftUpLeg", "mixamorig:LeftLeg", "mixamorig1_RightLeg"});
        CHECK(m[R(Role::LeftKnee)] == 1);  // not LeftUpLeg
        CHECK(m[R(Role::RightKnee)] == 2);
        m = GuessRoleMapping({"thigh_l", "calf_l", "calf_twist_01_l", "CALF_R"});
        CHECK(m[R(Role::LeftKnee)] == 1);
        CHECK(m[R(Role::RightKnee)] == 3);
    }

    // Up legs: Mixamo LeftUpLeg / RightUpLeg (any namespace), Unreal thigh_l / thigh_r.
    {
        auto m = GuessRoleMapping({"mixamorig:LeftUpLeg", "mixamorig:LeftLeg", "mixamorig1_RightUpLeg"});
        CHECK(m[R(Role::LeftUpLeg)] == 0);
        CHECK(m[R(Role::RightUpLeg)] == 2);
        CHECK(m[R(Role::LeftKnee)] == 1);
        m = GuessRoleMapping({"pelvis", "THIGH_L", "calf_l", "thigh_r", "thigh_twist_01_r"});
        CHECK(m[R(Role::LeftUpLeg)] == 1);
        CHECK(m[R(Role::RightUpLeg)] == 3);
    }

    // Toe tips, arms, spine and head on a whole Mixamo rig (the bones of fixtures/Climbing.fbx);
    // the leg roles keep their guesses (the toe still prefers ToeBase).
    {
        const std::vector<std::string> names = {
            "mixamorig:Hips",          "mixamorig:Spine",         "mixamorig:Spine1",        "mixamorig:Spine2",
            "mixamorig:Neck",          "mixamorig:Head",          "mixamorig:HeadTop_End",   "mixamorig:LeftEye",
            "mixamorig:LeftShoulder",  "mixamorig:LeftArm",       "mixamorig:LeftForeArm",   "mixamorig:LeftHand",
            "mixamorig:LeftHandIndex1", "mixamorig:LeftHandThumb1", "mixamorig:RightShoulder", "mixamorig:RightArm",
            "mixamorig:RightForeArm",  "mixamorig:RightHand",     "mixamorig:RightHandIndex1", "mixamorig:LeftUpLeg",
            "mixamorig:LeftLeg",       "mixamorig:LeftFoot",      "mixamorig:LeftToeBase",   "mixamorig:LeftToe_End",
            "mixamorig:RightUpLeg",    "mixamorig:RightLeg",      "mixamorig:RightFoot",     "mixamorig:RightToeBase",
            "mixamorig:RightToe_End"};
        auto m = GuessRoleMapping(names);
        CHECK(m.size() == static_cast<size_t>(Role::Count));
        CHECK(m[R(Role::Hips)] == 0);
        CHECK(m[R(Role::Spine)] == 1);
        CHECK(m[R(Role::Chest)] == 3);  // Spine2: the clavicles and the neck hang from it
        CHECK(m[R(Role::Neck)] == 4);
        CHECK(m[R(Role::Head)] == 5);   // not HeadTop_End
        CHECK(m[R(Role::LeftClavicle)] == 8);
        CHECK(m[R(Role::LeftShoulder)] == 9);
        CHECK(m[R(Role::LeftElbow)] == 10);
        CHECK(m[R(Role::LeftHand)] == 11);  // not a finger
        CHECK(m[R(Role::RightClavicle)] == 14);
        CHECK(m[R(Role::RightShoulder)] == 15);
        CHECK(m[R(Role::RightElbow)] == 16);
        CHECK(m[R(Role::RightHand)] == 17);
        CHECK(m[R(Role::LeftUpLeg)] == 19 && m[R(Role::LeftKnee)] == 20 && m[R(Role::LeftHeel)] == 21);
        CHECK(m[R(Role::LeftToe)] == 22);
        CHECK(m[R(Role::LeftToeEnd)] == 23);
        CHECK(m[R(Role::RightUpLeg)] == 24 && m[R(Role::RightKnee)] == 25 && m[R(Role::RightHeel)] == 26);
        CHECK(m[R(Role::RightToe)] == 27);
        CHECK(m[R(Role::RightToeEnd)] == 28);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) CHECK(m[r] >= 0);  // every role found
    }
    // The same on mixamorig1_ names; a rig without ToeBase plays its Toe_End as both toe roles.
    {
        auto m = GuessRoleMapping({"mixamorig1_Spine2", "mixamorig1_LeftHand", "mixamorig1_RightForeArm",
                                   "MIXAMORIG1_RIGHTARM", "mixamorig1_LeftShoulder", "mixamorig1_Neck",
                                   "mixamorig1_LeftToe_End", "mixamorig1_Head"});
        CHECK(m[R(Role::Chest)] == 0);
        CHECK(m[R(Role::LeftHand)] == 1);
        CHECK(m[R(Role::RightElbow)] == 2);
        CHECK(m[R(Role::RightShoulder)] == 3);
        CHECK(m[R(Role::LeftClavicle)] == 4);
        CHECK(m[R(Role::Neck)] == 5);
        CHECK(m[R(Role::LeftToeEnd)] == 6 && m[R(Role::LeftToe)] == 6);
        CHECK(m[R(Role::Head)] == 7);
        CHECK(m[R(Role::Spine)] == -1 && m[R(Role::RightHand)] == -1 && m[R(Role::RightToeEnd)] == -1);
    }
    // Unreal, UE5 mannequin (fixtures/SKM_Manny_Simple.glb): spine_05 is the chest; the twist,
    // IK and finger bones are never guessed; no toe tip: the toe (ball_l) stands in.
    {
        const std::vector<std::string> names = {
            "root",        "pelvis",      "spine_01",           "spine_02",    "spine_03",
            "spine_04",    "spine_05",    "neck_01",            "neck_02",     "head",
            "clavicle_l",  "upperarm_l",  "lowerarm_l",         "lowerarm_twist_01_l", "hand_l",
            "index_01_l",  "upperarm_twist_01_l", "clavicle_r", "upperarm_r",  "lowerarm_r",
            "hand_r",      "thigh_r",     "calf_r",             "foot_r",      "ball_r",
            "thigh_l",     "calf_l",      "foot_l",             "ball_l",      "ik_hand_root",
            "ik_hand_gun", "ik_hand_l",   "ik_hand_r",          "ik_foot_l"};
        auto m = GuessRoleMapping(names);
        CHECK(m[R(Role::Hips)] == 1);
        CHECK(m[R(Role::Spine)] == 2);
        CHECK(m[R(Role::Chest)] == 6);
        CHECK(m[R(Role::Neck)] == 7);
        CHECK(m[R(Role::Head)] == 9);
        CHECK(m[R(Role::LeftClavicle)] == 10);
        CHECK(m[R(Role::LeftShoulder)] == 11);
        CHECK(m[R(Role::LeftElbow)] == 12);
        CHECK(m[R(Role::LeftHand)] == 14);
        CHECK(m[R(Role::RightClavicle)] == 17);
        CHECK(m[R(Role::RightShoulder)] == 18);
        CHECK(m[R(Role::RightElbow)] == 19);
        CHECK(m[R(Role::RightHand)] == 20);
        CHECK(m[R(Role::RightToe)] == 24 && m[R(Role::LeftToe)] == 28);
        CHECK(m[R(Role::LeftToeEnd)] == 28 && m[R(Role::RightToeEnd)] == 24);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) CHECK(m[r] >= 0);  // "all found"
    }
    // Unreal, UE4 mannequin: three spine bones, spine_03 is the chest.
    {
        auto m = GuessRoleMapping({"root", "pelvis", "spine_01", "spine_02", "spine_03", "neck_01", "head"});
        CHECK(m[R(Role::Spine)] == 2);
        CHECK(m[R(Role::Chest)] == 4);
        CHECK(m[R(Role::Neck)] == 5);
        CHECK(m[R(Role::Head)] == 6);
    }

    // Stand-ins: a rig without the role's own bone gets the nearest bone of the same chain.
    {
        // Mixamo without Toe_End: the toe base; the left toe end never takes a right toe.
        auto m = GuessRoleMapping({"mixamorig:LeftToeBase", "mixamorig:RightToeBase"});
        CHECK(m[R(Role::LeftToeEnd)] == 0 && m[R(Role::RightToeEnd)] == 1);
        m = GuessRoleMapping({"RightToeBase", "ball_r", "LeftFoot"});
        CHECK(m[R(Role::LeftToeEnd)] == -1 && m[R(Role::RightToeEnd)] == 0);  // no foot stand-in
        // The chest: the highest spine bone the rig has.
        m = GuessRoleMapping({"mixamorig:Hips", "mixamorig:Spine", "mixamorig:Spine1", "mixamorig:Neck"});
        CHECK(m[R(Role::Chest)] == 2 && m[R(Role::Spine)] == 1);
        m = GuessRoleMapping({"Hips", "Spine", "Neck"});
        CHECK(m[R(Role::Chest)] == 1 && m[R(Role::Spine)] == 1);
        m = GuessRoleMapping({"pelvis", "spine_01", "spine_02", "neck_01"});
        CHECK(m[R(Role::Chest)] == 2);
        m = GuessRoleMapping({"pelvis", "spine_01", "spine_02", "spine_03", "spine_04"});
        CHECK(m[R(Role::Chest)] == 3);  // UE4's own chest before UE5's spine_04 stand-in
        m = GuessRoleMapping({"Hips", "Neck", "Head"});
        CHECK(m[R(Role::Chest)] == -1 && m[R(Role::Spine)] == -1);  // never the neck or the hips
        // The leg roles keep their order: a toe's tip still before ball_l.
        m = GuessRoleMapping({"ball_l", "LeftToe_End"});
        CHECK(m[R(Role::LeftToe)] == 1 && m[R(Role::LeftToeEnd)] == 1);
        // Own bones vs stand-ins (any namespace): what a click in the 3D view prefers.
        CHECK(IsOwnBoneOfRole(Role::LeftToe, "ball_l") && !IsOwnBoneOfRole(Role::LeftToeEnd, "ball_l"));
        CHECK(IsOwnBoneOfRole(Role::LeftToeEnd, "mixamorig:LeftToe_End"));
        CHECK(!IsOwnBoneOfRole(Role::LeftToe, "mixamorig1_LeftToe_End"));
        CHECK(IsOwnBoneOfRole(Role::Spine, "Spine") && !IsOwnBoneOfRole(Role::Chest, "Spine"));
        CHECK(IsOwnBoneOfRole(Role::Chest, "spine_05") && !IsOwnBoneOfRole(Role::Chest, "spine_04"));
        CHECK(!IsOwnBoneOfRole(Role::LeftHand, "hand_r") && !IsOwnBoneOfRole(Role::Count, "head"));
        // Every role has an own bone; the table covers every role.
        CHECK(RoleCandidates().size() == static_cast<size_t>(Role::Count));
        for (const std::vector<RoleCandidate>& c : RoleCandidates()) CHECK(!c.empty() && c[0].own);
        // A bone name is the own bone of one role at most (a click never ties on two).
        std::vector<std::string> own;
        for (const std::vector<RoleCandidate>& c : RoleCandidates())
            for (const RoleCandidate& k : c)
                if (k.own) own.push_back(k.name);
        for (size_t i = 0; i < own.size(); ++i)
            for (size_t j = i + 1; j < own.size(); ++j) CHECK(own[i] != own[j]);
    }

    // Keys, names and labels: every role has its own, a key reads back as its role and is a
    // valid key (what + Role and REF labels give); the 9 leg roles keep theirs.
    {
        std::vector<std::string> keys, names, labels;
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) {
            const Role role = static_cast<Role>(r);
            Role       back = Role::Count;
            CHECK(RoleFromKey(RoleKey(role), &back) && back == role);
            CHECK(RoleKeyFromName(RoleKey(role)) == RoleKey(role));
            CHECK(std::string(RoleName(role)) != "?" && std::string(RolePart(role)) != "?");
            keys.push_back(RoleKey(role));
            names.push_back(RoleName(role));
            labels.push_back(RoleShortLabel(role));
        }
        for (size_t i = 0; i < keys.size(); ++i)
            for (size_t j = i + 1; j < keys.size(); ++j) {
                CHECK(keys[i] != keys[j]);
                CHECK(names[i] != names[j]);
                CHECK(labels[i] != labels[j]);
            }
        CHECK(!RoleFromKey("left_foot", nullptr) && !RoleFromKey("", nullptr));
        const char* const kLegKeys[] = {"left_heel", "left_toe", "right_heel", "right_toe", "left_knee",
                                        "right_knee", "left_up_leg", "right_up_leg", "hips"};
        for (int r = 0; r < 9; ++r) CHECK(std::string(RoleKey(static_cast<Role>(r))) == kLegKeys[r]);
        CHECK(std::string(RoleKey(Role::LeftHand)) == "left_hand" && RoleShortLabel(Role::LeftHand) == "L hand");
        CHECK(std::string(RoleKey(Role::LeftShoulder)) == "left_shoulder" &&
              std::string(RoleName(Role::LeftShoulder)) == "left shoulder (upper arm)");
        CHECK(RoleShortLabel(Role::RightElbow) == "R elbow" && RoleShortLabel(Role::LeftToeEnd) == "L toe end");
        CHECK(RoleShortLabel(Role::Chest) == "chest" && RoleSide(Role::Head) == 0);
    }

    // The list order: every role once, grouped (legs, arms, body), left before right.
    {
        const std::vector<Role>& order = RolesInListOrder();
        CHECK(order.size() == static_cast<size_t>(Role::Count));
        std::vector<int> seen(static_cast<size_t>(Role::Count), 0);
        for (Role r : order) ++seen[static_cast<size_t>(R(r))];
        for (int n : seen) CHECK(n == 1);
        for (size_t i = 1; i < order.size(); ++i)
            CHECK(static_cast<int>(RoleGroupOf(order[i - 1])) <= static_cast<int>(RoleGroupOf(order[i])));
        CHECK(order.front() == Role::LeftHeel && order.back() == Role::Head);
        CHECK(RoleGroupOf(Role::LeftToeEnd) == RoleGroup::Legs && RoleGroupOf(Role::LeftClavicle) == RoleGroup::Arms);
        CHECK(RoleGroupOf(Role::Hips) == RoleGroup::Body);
        CHECK(std::string(RoleGroupName(RoleGroup::Arms)) == "Arms");
    }

    // The Footsteps preset (v2): heel, toe, knee and up leg per foot; conditions height /
    // knee flexion speed / foot yaw speed; landing on the knee peak.
    {
        Preset p = FootstepsPreset();
        CHECK(p.blocks.size() == 2);
        CHECK(p.blocks[0].conditions.size() == 3);
        CHECK(p.blocks[0].landing == Landing::PeakOf && p.blocks[0].peak_condition == 1 && p.blocks[0].peak_max);
        CHECK(p.blocks[0].conditions[1].signal.quantity == Quantity::JointAngle);
        CHECK(p.blocks[0].conditions[2].signal.quantity == Quantity::Yaw);
        CHECK(!p.blocks[0].conditions[2].auto_threshold);
        CHECK(p.blocks[0].conditions[2].threshold == FootstepsParams{}.yaw_limit_dps);
        auto used = RolesUsed(p.blocks);
        CHECK(used.size() == 8);
        // Tracks in RolesUsed (= Role) order: L heel, L toe, R heel, R toe, L knee, R knee,
        // L up leg, R up leg.
        std::vector<int> role_to_track(static_cast<size_t>(Role::Count), -1);
        for (size_t i = 0; i < used.size(); ++i) role_to_track[R(used[i])] = static_cast<int>(i);
        std::string missing = "x";
        auto blocks = p.blocks;
        CHECK(BindRoles(blocks, role_to_track, &missing));
        CHECK(missing.empty());
        CHECK(blocks[0].conditions[0].signal.bones == std::vector<int>({0, 1}));
        CHECK(blocks[1].strength_signal.bones == std::vector<int>({2, 3}));
        CHECK(blocks[0].conditions[1].signal.bones == std::vector<int>({6, 4, 0}));  // up leg - knee - ankle
        CHECK(blocks[1].conditions[1].signal.bones == std::vector<int>({7, 5, 2}));
        CHECK(blocks[0].conditions[2].signal.bones == std::vector<int>({0, 1}));     // heel -> toe
        // A skeleton without knees: skipped, roles named once each.
        std::vector<int> no_knee = role_to_track;
        no_knee[R(Role::LeftKnee)] = no_knee[R(Role::RightKnee)] = -1;
        blocks = p.blocks;
        CHECK(!BindRoles(blocks, no_knee, &missing));
        CHECK(missing == "left knee, right knee");
        // A skeleton without a right toe.
        role_to_track[R(Role::RightToe)] = -1;
        blocks = p.blocks;
        CHECK(!BindRoles(blocks, role_to_track, &missing));
        CHECK(missing == "right toe");
    }

    // Story 10-3e -- a custom role's key from its name.
    {
        CHECK(RoleKeyFromName("Sword Tip") == "sword_tip");
        CHECK(RoleKeyFromName("  sword   tip  ") == "sword_tip");
        CHECK(RoleKeyFromName("R-Hand (IK) 2") == "r_hand_ik_2");
        CHECK(RoleKeyFromName("__tail__end__") == "tail_end");
        CHECK(RoleKeyFromName("\xC3\xA9p\xC3\xA9\x65 droite") == "p_e_droite");  // accents: a run of other bytes
        CHECK(RoleKeyFromName("").empty());
        CHECK(RoleKeyFromName(" -- ").empty());
        CHECK(RoleKeyFromName("\xC3\xA9\xC3\xA8").empty());
        CHECK(RoleKeyFromName("Left Heel") == RoleKey(Role::LeftHeel));  // the store refuses it (built-in clash)
        CHECK(RoleNameFromKey("tail_end") == "tail end");
    }

    if (g_fails == 0) std::printf("bone_roles: all tests passed\n");
    return g_fails == 0 ? 0 : 1;
}
