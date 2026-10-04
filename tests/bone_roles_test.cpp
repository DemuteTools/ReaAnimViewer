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

    // The Footsteps preset: four foot roles, plus the two knees with the knee bend on.
    {
        CHECK(RolesUsed(FootstepsPreset(false).blocks).size() == 4);
        Preset p = FootstepsPreset();  // knee bend on by default
        CHECK(p.blocks.size() == 2);
        CHECK(p.blocks[0].conditions.size() == 3);
        CHECK(FootstepsPreset(false).blocks[0].conditions.size() == 2);
        auto used = RolesUsed(p.blocks);
        CHECK(used.size() == 6);
        // Tracks in RolesUsed order: L heel, L toe, R heel, R toe, L knee, R knee.
        std::vector<int> role_to_track(static_cast<size_t>(Role::Count), -1);
        for (size_t i = 0; i < used.size(); ++i) role_to_track[R(used[i])] = static_cast<int>(i);
        std::string missing = "x";
        auto blocks = p.blocks;
        CHECK(BindRoles(blocks, role_to_track, &missing));
        CHECK(missing.empty());
        CHECK(blocks[0].conditions[0].signal.bones == std::vector<int>({0, 1}));
        CHECK(blocks[1].strength_signal.bones == std::vector<int>({2, 3}));
        CHECK(blocks[0].conditions[2].signal.bones == std::vector<int>({4}));
        CHECK(blocks[0].conditions[2].signal.ref_bones == std::vector<int>({0}));  // relative to the heel
        CHECK(blocks[1].conditions[2].signal.ref_bones == std::vector<int>({2}));
        // A skeleton without knees: skipped while the knee bend is on, fine with it off.
        std::vector<int> no_knee = role_to_track;
        no_knee[R(Role::LeftKnee)] = no_knee[R(Role::RightKnee)] = -1;
        blocks = p.blocks;
        CHECK(!BindRoles(blocks, no_knee, &missing));
        CHECK(missing == "left knee, right knee");
        blocks = FootstepsPreset(false).blocks;
        CHECK(BindRoles(blocks, no_knee, &missing));
        // A skeleton without a right toe.
        role_to_track[R(Role::RightToe)] = -1;
        blocks = p.blocks;
        CHECK(!BindRoles(blocks, role_to_track, &missing));
        CHECK(missing == "right toe");
    }

    if (g_fails == 0) std::printf("bone_roles: all tests passed\n");
    return g_fails == 0 ? 0 : 1;
}
