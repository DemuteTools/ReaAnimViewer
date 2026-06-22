// SPDX-License-Identifier: MIT
//
// THROWAWAY SPIKE (Spike 0 / Story 0-1) — NOT production code, do not merge to main.
//
// Plain data the spike passes from the assimp loader to the GL renderer.
// Deliberately monolithic and POD-ish: the production scene model is D1 in
// architecture.md and is built fresh in Epic 1+, not derived from this.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace spike {

constexpr int kMaxBones        = 64;   // GLSL palette size; D13 production cap is 128
constexpr int kWeightsPerVertex = 4;

struct Vertex {
    glm::vec3 pos{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec2 uv{0.0f};
    glm::ivec4 boneIds{0};
    glm::vec4 boneWeights{0.0f};
};

// Flat bone list (parentIdx layout per D1) so global matrices are one linear pass.
struct Bone {
    std::string name;
    int         parentIdx = -1;      // -1 == root
    glm::mat4   localBind{1.0f};     // node default transform (used when no anim channel)
    glm::mat4   inverseBind{1.0f};   // aiBone::mOffsetMatrix, converted to column-major
};

// One animated node's TRS keyframes (sampled by spike::SampleClip).
struct AnimChannel {
    int boneIdx = -1;
    std::vector<std::pair<float, glm::vec3>> translation;
    std::vector<std::pair<float, glm::quat>> rotation;
    std::vector<std::pair<float, glm::vec3>> scale;
};

struct AnimClip {
    float                    duration = 0.0f;   // seconds
    std::vector<AnimChannel> channels;
};

struct Model {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    std::vector<Bone>     bones;
    AnimClip              clip;             // first clip only (MVP/spike)
    glm::mat4             modelRoot{1.0f};  // identity for glTF-canonical (render as-authored, D3)
    glm::vec3            aabbMin{0.0f};
    glm::vec3            aabbMax{0.0f};

    bool skinned() const { return !bones.empty() && !clip.channels.empty(); }
};

}  // namespace spike
