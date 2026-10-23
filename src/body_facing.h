// SPDX-License-Identifier: MIT
//
// Story 10-9o: the body's facing and an arm's state, read from the positions
// PhysicsAnalysis::role_pos keeps (smoothed as the parts), shared by the punches' straight test
// (motion_whoosh.cpp) and their arc test (auto_types_punch.cpp). Moved out of 10-9n's Strike.
//
// The facing: horizontal, perpendicular to the hip line (left up leg -> right up leg), else to the
// shoulder line; facing = up x (left -> right). The forward share of a hand's velocity relative to
// the torso (the chest, else the spine, else the hips) is its share along that facing (-1 backward
// .. +1 forward).
//
// Pure C++17, header only.

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "motion_physics.h"

namespace rav {

// The positions of a role over the analysed samples; null without the bone.
inline const std::vector<Vec3d>* FacingRolePos(const PhysicsAnalysis& a, Role r)
{
    const size_t i = static_cast<size_t>(r);
    if (i >= a.role_pos.size() || a.role_pos[i].size() != a.samples || a.samples == 0) return nullptr;
    return &a.role_pos[i];
}

// The positions a hand's facing reads: the hand, the torso and the line the facing is
// perpendicular to (null when missing: the share cannot be read).
struct FacingFrame {
    const std::vector<Vec3d>* hand = nullptr;
    const std::vector<Vec3d>* torso = nullptr;
    const std::vector<Vec3d>* left = nullptr;
    const std::vector<Vec3d>* right = nullptr;
};

// s: 0 = the left hand, 1 = the right.
inline FacingFrame FacingFrameOf(const PhysicsAnalysis& a, int s)
{
    FacingFrame f;
    f.hand = FacingRolePos(a, s ? Role::RightHand : Role::LeftHand);
    for (Role r : {Role::Chest, Role::Spine, Role::Hips})
        if (!f.torso) f.torso = FacingRolePos(a, r);
    f.left = FacingRolePos(a, Role::LeftUpLeg);
    f.right = FacingRolePos(a, Role::RightUpLeg);
    if (!f.left || !f.right) {
        f.left = FacingRolePos(a, Role::LeftShoulder);
        f.right = FacingRolePos(a, Role::RightShoulder);
    }
    if (!f.left || !f.right) f.left = f.right = nullptr;
    return f;
}

// The share of the hand's velocity relative to the torso along the body's facing at sample k
// (-1 backward .. +1 forward); NaN when it cannot be read.
inline double ForwardShare(const PhysicsAnalysis& a, const FacingFrame& f, long k)
{
    const long n = static_cast<long>(a.samples);
    if (!f.hand || !f.torso || !f.left || n < 3 || k < 0 || k >= n) return std::nan("");
    const long   k0 = std::max(0L, k - 1), k1 = std::min(n - 1, k + 1);
    const size_t i0 = static_cast<size_t>(k0), i1 = static_cast<size_t>(k1), i = static_cast<size_t>(k);
    const Vec3d& h0 = (*f.hand)[i0];
    const Vec3d& h1 = (*f.hand)[i1];
    const Vec3d& t0 = (*f.torso)[i0];
    const Vec3d& t1 = (*f.torso)[i1];
    const double vx = (h1.x - t1.x) - (h0.x - t0.x), vy = (h1.y - t1.y) - (h0.y - t0.y), vz = (h1.z - t1.z) - (h0.z - t0.z);
    const double vl = std::sqrt(vx * vx + vy * vy + vz * vz);
    double       lx = (*f.right)[i].x - (*f.left)[i].x, lz = (*f.right)[i].z - (*f.left)[i].z;
    const double ll = std::sqrt(lx * lx + lz * lz);
    if (!(vl > 1e-12) || !(ll > 1e-9)) return std::nan("");
    lx /= ll;
    lz /= ll;
    // Facing = up x (left -> right), horizontal.
    return (vx * lz - vz * lx) / vl;
}

}  // namespace rav
