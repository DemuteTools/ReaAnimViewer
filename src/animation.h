// SPDX-License-Identifier: MIT
//
// The D13 pose sampler: given a parsed SceneAnimation and a time `t`, it produces
// the per-frame global bone matrices and the skinning palette. Pure glm, no assimp,
// no GL — so it is assimp-boundary-clean (D5) and can be included by asset_loader.cpp
// (Story 3.2's console audit) and renderer.cpp (Story 3.3's per-frame upload) alike.
// Header-only with inline free functions, exactly like camera.h: small pure-math
// modules stay out of CMake's source list so they add no CMakeLists.txt change (AC8).
//
// The hot path (ComputePose) writes into caller-pre-sized buffers and never resizes,
// so Story 3.3 can call it every frame with no allocation (D2). The single linear
// forward pass is valid because Story 3.1's DFS guarantees parentIdx < i for every
// bone — the parent's global matrix is always computed before its child's.

#pragma once

#ifdef _WIN32

#include <algorithm>  // std::upper_bound, std::clamp
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::translate, glm::scale
#include <glm/gtc/quaternion.hpp>        // glm::slerp, glm::mat4_cast, glm::normalize(quat)

#include "scene.h"

namespace rav {

// Samples a vec3 keyframe track (translation OR scale — both are KeyframeT/S =
// {time, vec3}) at time `t` by linear interpolation. `empty_default` distinguishes
// the two: a missing translation rests at 0, a missing scale rests at 1. The keys are
// time-sorted ascending (assimp emits them so; ParseAnimations preserves that order),
// so a binary search finds the bracketing pair; `t` is clamped to the track's ends
// (AC3 boundary clamp) — past either end it holds the first/last value, no extrapolation.
template <typename KeyVec3>
inline glm::vec3 SampleVec3(const std::vector<KeyVec3>& keys, float t,
                            const glm::vec3& empty_default)
{
    if (keys.empty())     return empty_default;
    if (keys.size() == 1) return keys.front().value;

    // First key strictly after t. begin() => t precedes the track (clamp low);
    // end() => t at/after the last key (clamp high).
    auto hi = std::upper_bound(keys.begin(), keys.end(), t,
                               [](float tt, const KeyVec3& k) { return tt < k.time; });
    if (hi == keys.begin()) return keys.front().value;
    if (hi == keys.end())   return keys.back().value;
    auto lo = hi - 1;

    const float span = hi->time - lo->time;
    const float f = (span > 0.0f) ? (t - lo->time) / span : 0.0f;  // guard coincident keys
    return glm::mix(lo->value, hi->value, f);
}

// Samples a rotation track at time `t` with spherical interpolation (slerp), not a
// component lerp — only slerp keeps a quaternion unit-length and the rotation rate
// constant, so an interpolated pose "poses correctly" (AC2). Clamped at both ends
// like SampleVec3; the result is normalized to shrug off accumulated float drift.
inline glm::quat SampleQuat(const std::vector<KeyframeR>& keys, float t)
{
    if (keys.empty())     return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // identity
    if (keys.size() == 1) return glm::normalize(keys.front().value);

    auto hi = std::upper_bound(keys.begin(), keys.end(), t,
                               [](float tt, const KeyframeR& k) { return tt < k.time; });
    if (hi == keys.begin()) return glm::normalize(keys.front().value);
    if (hi == keys.end())   return glm::normalize(keys.back().value);
    auto lo = hi - 1;

    const float span = hi->time - lo->time;
    const float f = (span > 0.0f) ? (t - lo->time) / span : 0.0f;
    return glm::normalize(glm::slerp(lo->value, hi->value, f));
}

// Composes one bone's local transform at time `t` in TRS order, column-major (D3):
// translate * rotate * scale. The rotation is re-normalized (slerp already does, but a
// single-key track returns the raw authored quat). No assimp matrix is ever touched
// here — the TRS values were copied component-wise on the glm side (§D), so this matrix
// is born column-major and must NOT pass through ConvertAssimpMatrix.
inline glm::mat4 ComposeLocal(const AnimChannel& ch, float t)
{
    const glm::vec3 trans = SampleVec3(ch.translation, t, glm::vec3(0.0f));
    const glm::quat rot   = SampleQuat(ch.rotation, t);
    const glm::vec3 scl   = SampleVec3(ch.scale, t, glm::vec3(1.0f));
    return glm::translate(glm::mat4(1.0f), trans) *
           glm::mat4_cast(rot) *
           glm::scale(glm::mat4(1.0f), scl);
}

// The D13 per-frame pass: sample every bone's local at `t`, accumulate globals in one
// linear sweep, and fold in each inverse-bind to get the skinning palette. The caller
// pre-sizes BOTH out_palette and scratch_global to bones.size() (once, at SetAsset for
// 3.3) so this never allocates in the hot path (D2). Degenerate sizes (mismatched or
// empty) return without writing rather than indexing out of bounds (AR18 discipline).
inline void ComputePose(const SceneSkeleton& skel, const SceneAnimation& anim, float t,
                        std::vector<glm::mat4>& out_palette,
                        std::vector<glm::mat4>& scratch_global)
{
    const size_t n = skel.bones.size();
    if (n == 0 || anim.channels.size() != n ||
        out_palette.size() != n || scratch_global.size() != n)
        return;

    t = glm::clamp(t, 0.0f, anim.duration);  // AC3: sample inside [0, duration]

    for (size_t i = 0; i < n; ++i) {
        const glm::mat4 local = ComposeLocal(anim.channels[i], t);
        const int parent = skel.bones[i].parentIdx;
        // parentIdx < i (3.1 DFS invariant) => parent global already written this pass.
        scratch_global[i] = (parent < 0) ? local : scratch_global[parent] * local;
        out_palette[i] = scratch_global[i] * skel.bones[i].inverseBindMatrix;
    }
}

}  // namespace rav

#endif  // _WIN32
