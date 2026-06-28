// SPDX-License-Identifier: MIT
//
// The D1 scene model: plain data the asset loader fills and the renderer draws.
// POD-style aggregates, no inheritance/virtuals (AR7). `Asset` owns RAII GPU
// handles, so it is move-only and move-assignable — that lets a reload swap a new
// Asset in atomically (D4, Epic 6). The animation/skeleton types are declared now
// so `Asset`'s shape is final; Story 2.1 populates only the static-mesh fields and
// leaves skeleton/animations empty (the FR6 `skinned=false` path).

#pragma once

#ifdef _WIN32

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "gpu_resources.h"

namespace rav {

// Interleaved GPU vertex layout. Bone fields are zero-filled on the static path
// (Epic 3 fills them); the attribute layout is fixed now so the VBO never churns.
struct SceneVertex {
    glm::vec3  pos;
    glm::vec3  normal;
    glm::vec2  uv;
    glm::ivec4 boneIds;
    glm::vec4  boneWeights;
    // No per-vertex tangent: Story 6.5.1's normal mapping builds the tangent frame
    // per-fragment from screen-space derivatives (renderer.cpp), which is robust to
    // mirrored UVs where a per-vertex tangent cancels to zero. So no tangent attribute
    // is uploaded and aiProcess_CalcTangentSpace is not requested.
};

struct SceneMesh {
    GpuBuffer vb;
    GpuBuffer ib;
    uint32_t  indexCount  = 0;
    uint32_t  materialIdx = 0;
    bool      skinned     = false;  // D13/FR6 selector — always false in Story 2.1
};

struct SceneMaterial {
    GpuImage  baseColor;             // texture — unbound (0) in 2.1; Story 2.2 uploads it
    GpuImage  normalMap;             // tangent-space normal map — unbound (0) when none;
                                     // Story 6.5.1 uploads it LINEAR (GL_RGBA8), gated on
                                     // presence (empty handle → geometric normal, AC4)
    glm::vec3 baseColorFactor{0.8f}; // D1 refinement: flat diffuse RGB (AI_MATKEY_COLOR_DIFFUSE)
    glm::vec3 specularColor{0.04f};
    float     shininess = 32.0f;
};

struct SceneBone {
    int       parentIdx = -1;     // -1 == root; flat layout → one linear global pass
    glm::mat4 inverseBindMatrix{1.0f};
    std::string name;
};

struct SceneSkeleton {
    std::vector<SceneBone> bones;
};

// Keyframe/animation types declared now so Asset's shape is final (Epic 3 fills them).
struct KeyframeT   { float time; glm::vec3 value; };
struct KeyframeR   { float time; glm::quat value; };
struct KeyframeS   { float time; glm::vec3 value; };
struct AnimChannel {
    std::vector<KeyframeT> translation;
    std::vector<KeyframeR> rotation;
    std::vector<KeyframeS> scale;
};
struct SceneAnimation {
    float duration = 0.0f;
    std::vector<AnimChannel> channels;  // indexed by bone idx
};

// Move-only: it owns GPU handles (each member is move-only), so the compiler-
// generated move ops are correct and copy is implicitly deleted. Default-
// constructible = the "no asset loaded" empty state.
struct Asset {
    std::vector<SceneMesh>      meshes;
    std::vector<SceneMaterial>  materials;
    SceneSkeleton               skeleton;       // empty for the static-mesh path (FR6)
    std::vector<SceneAnimation> animations;     // empty in Story 2.1
    glm::mat4                   modelRoot{1.0f}; // identity; non-canonical stays as-authored (AR13)
    glm::vec3                   aabbMin{0.0f};   // union AABB → camera auto-fit / Reset (D14)
    glm::vec3                   aabbMax{0.0f};
};

}  // namespace rav

#endif  // _WIN32
