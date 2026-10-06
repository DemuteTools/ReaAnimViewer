// SPDX-License-Identifier: MIT
//
// Host test of the role mapping (src/bone_roles.h, Epic 10, spike 10-0): Mixamo,
// mixamorig<N>_ and Unreal names. No REAPER, no Windows: any C++17 compiler.

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
