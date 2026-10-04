// SPDX-License-Identifier: MIT
//
// See bone_sampling.h.

#include "bone_sampling.h"

#ifdef _WIN32

#include <cmath>

#include <glm/glm.hpp>

#include "animation.h"

namespace rav {

std::vector<BoneTrack> SampleBoneTracks(const CpuAsset& asset, const std::vector<int>& bone_indices, double rate_hz)
{
    if (asset.animations.empty() || bone_indices.empty() || !(rate_hz > 0.0) || !std::isfinite(rate_hz)) return {};
    const SceneSkeleton& skel = asset.skeleton;
    const SceneAnimation& anim = asset.animations[0];
    const size_t nb = skel.bones.size();
    if (nb == 0 || anim.channels.size() != nb || !(anim.duration >= 0.0f) || !std::isfinite(anim.duration)) return {};
    for (int b : bone_indices)
        if (b < 0 || static_cast<size_t>(b) >= nb) return {};

    const double mpu = (asset.metersPerUnit > 0.0f) ? asset.metersPerUnit : 1.0;
    const size_t n = static_cast<size_t>(std::floor(static_cast<double>(anim.duration) * rate_hz + 1e-6)) + 1;

    std::vector<BoneTrack> tracks(bone_indices.size());
    for (BoneTrack& t : tracks) {
        t.rate_hz = rate_hz;
        t.pos.resize(n);
    }
    std::vector<glm::mat4> palette(nb), global(nb);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(static_cast<double>(i) / rate_hz);
        ComputePose(skel, anim, t, palette, global);
        for (size_t k = 0; k < bone_indices.size(); ++k) {
            const glm::vec4 p = asset.modelRoot * global[static_cast<size_t>(bone_indices[k])][3];
            tracks[k].pos[i] = Vec3d{p.x * mpu, p.y * mpu, p.z * mpu};
        }
    }
    return tracks;
}

}  // namespace rav

#endif  // _WIN32
