// SPDX-License-Identifier: MIT
//
// Direct glTF/GLB skin-weight reader — a targeted work-around for an assimp 6.0.5 bug.
//
// assimp's Windows build mis-reads the skin weights of glTF meshes that use the second
// influence set (JOINTS_1/WEIGHTS_1, i.e. >4 bones per vertex — Unreal's default glTF
// export): it returns only ~2% of the weights, non-deterministically, so ~3/4 of the
// mesh loses its skin and freezes into flat shards. The mesh, skeleton and animation
// come back correct; ONLY the per-vertex weights are corrupt. (Linux/GCC assimp reads
// the same file perfectly, so it is a compiled-library defect, not our code.)
//
// So for .glb/.gltf we read JOINTS_*/WEIGHTS_* straight from the file ourselves and
// keep the four heaviest influences per vertex — the exact data assimp should have
// produced — then overwrite assimp's weights in the loader. FBX/Collada still go
// through assimp untouched. Everything here is header/JSON only (no GL), and every
// parse step is guarded so a malformed file degrades to "no data" (caller falls back
// to assimp) rather than crossing the host boundary (AR18).

#pragma once

#ifdef _WIN32

#include <array>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace rav {

// One mesh primitive's skin, top-4 influences per vertex. `jointLocal[v][k]` indexes
// into GltfSkinData::jointNames; `weight[v][k]` is the matching (renormalizable) weight.
// Empty slots carry joint 0 / weight 0. `firstPos`/`vertexCount` let the caller match
// this primitive to the corresponding aiMesh (assimp keeps skinned verts mesh-local and
// in-order, so the match is by vertex count + first position).
struct GltfMeshSkin {
    size_t                            vertexCount = 0;
    glm::vec3                         firstPos{0.0f};
    std::vector<glm::vec3>            positions;    // per vertex, mesh-local (ground truth)
    std::vector<uint32_t>            indices;      // triangle list (empty if non-indexed)
    std::vector<std::array<int, 4>>   jointLocal;   // per vertex, 4 joint-array indices
    std::vector<std::array<float, 4>> weight;       // per vertex, 4 weights (top-4)
};

// The whole file's skin: the skin joint node names (indexed by glТF joint-array index)
// and one GltfMeshSkin per mesh primitive, in file order. `ok` is false if the file is
// not glTF, has no skinned primitive, or could not be parsed — the caller then keeps
// assimp's weights.
struct GltfSkinData {
    bool                      ok = false;
    std::vector<std::string>  jointNames;    // skin.joints -> node name
    std::vector<glm::mat4>    inverseBind;    // skin.inverseBindMatrices (per joint; empty=identity)
    std::vector<GltfMeshSkin> meshes;        // one per skinned primitive
};

// Reads the skin of a .glb (embedded buffer) or .gltf (external / base64 buffers).
// Never throws; returns {ok=false} on any anomaly.
GltfSkinData LoadGltfSkin(const std::string& path);

}  // namespace rav

#endif  // _WIN32
