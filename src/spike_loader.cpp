// SPDX-License-Identifier: MIT
//
// THROWAWAY SPIKE (Spike 0 / Story 0-1) — NOT production code, do not merge to main.

#include "spike_loader.h"

#include <algorithm>
#include <cstring>
#include <map>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/gtc/type_ptr.hpp>

namespace spike {
namespace {

// aiMatrix4x4 is row-major; glm::mat4 is column-major. This is the single
// convertAssimpMatrix boundary (D3/AR9) for the spike.
glm::mat4 Convert(const aiMatrix4x4& m)
{
    glm::mat4 r;
    r[0][0] = m.a1; r[1][0] = m.a2; r[2][0] = m.a3; r[3][0] = m.a4;
    r[0][1] = m.b1; r[1][1] = m.b2; r[2][1] = m.b3; r[3][1] = m.b4;
    r[0][2] = m.c1; r[1][2] = m.c2; r[2][2] = m.c3; r[3][2] = m.c4;
    r[0][3] = m.d1; r[1][3] = m.d2; r[2][3] = m.d3; r[3][3] = m.d4;
    return r;
}

glm::vec3 Convert(const aiVector3D& v) { return {v.x, v.y, v.z}; }
glm::quat Convert(const aiQuaternion& q) { return glm::quat(q.w, q.x, q.y, q.z); }

// Walk the node tree producing the flat bone list. `carried` accumulates the
// transform of any non-bone nodes between this node and its nearest bone
// ancestor, so intermediate (non-bone) nodes don't lose their transform.
void WalkNodes(const aiNode* node, int parentBoneIdx, const glm::mat4& carried,
               const std::map<std::string, int>& boneNameToIdx,
               std::vector<Bone>& bones)
{
    const glm::mat4 local = carried * Convert(node->mTransformation);

    int thisBoneIdx = parentBoneIdx;
    glm::mat4 childCarried = local;

    auto it = boneNameToIdx.find(node->mName.C_Str());
    if (it != boneNameToIdx.end()) {
        Bone& b = bones[it->second];
        b.parentIdx = parentBoneIdx;
        b.localBind = local;
        thisBoneIdx = it->second;
        childCarried = glm::mat4(1.0f);  // reset for children below a bone
    }

    for (unsigned i = 0; i < node->mNumChildren; ++i)
        WalkNodes(node->mChildren[i], thisBoneIdx, childCarried, boneNameToIdx, bones);
}

}  // namespace

bool LoadModel(const std::string& path, Model& out, std::string& outError)
{
    try {
        Assimp::Importer importer;

        // CRITICAL for FBX (esp. Mixamo): without this, assimp splits every bone
        // into hidden `$AssimpFbx$` pre/post-transform nodes and keys the animation
        // on THOSE — so a name-based channel→bone map (ours) finds nothing and the
        // rig stays in bind pose. PreservePivots=0 bakes them and keys animation on
        // the real bone nodes. (Convention finding for SPIKE0_FINDINGS.md.)
        importer.SetPropertyInteger(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, 0);

        const aiScene* scene = importer.ReadFile(
            path,
            aiProcess_Triangulate | aiProcess_GenSmoothNormals |
            aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices |
            aiProcess_ImproveCacheLocality);

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
            outError = importer.GetErrorString();
            return false;
        }

        Model m;

        // Pass 1: collect bones (name -> index) and inverse-bind matrices.
        std::map<std::string, int> boneNameToIdx;
        for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
            const aiMesh* mesh = scene->mMeshes[mi];
            for (unsigned bi = 0; bi < mesh->mNumBones; ++bi) {
                const aiBone* bone = mesh->mBones[bi];
                const std::string name = bone->mName.C_Str();
                if (boneNameToIdx.count(name)) continue;
                const int idx = static_cast<int>(m.bones.size());
                boneNameToIdx[name] = idx;
                Bone b;
                b.name = name;
                b.inverseBind = Convert(bone->mOffsetMatrix);
                m.bones.push_back(std::move(b));
            }
        }

        // Pass 2: hierarchy (parentIdx + localBind) for every bone.
        if (!m.bones.empty())
            WalkNodes(scene->mRootNode, -1, glm::mat4(1.0f), boneNameToIdx, m.bones);

        // Pass 3: vertices + indices, concatenating all meshes against one skeleton.
        std::vector<int> slotCount;  // per-vertex used bone slots
        bool first = true;
        for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
            const aiMesh* mesh = scene->mMeshes[mi];
            const uint32_t baseVertex = static_cast<uint32_t>(m.vertices.size());

            for (unsigned vi = 0; vi < mesh->mNumVertices; ++vi) {
                Vertex v;
                v.pos = Convert(mesh->mVertices[vi]);
                if (mesh->HasNormals()) v.normal = Convert(mesh->mNormals[vi]);
                if (mesh->HasTextureCoords(0))
                    v.uv = {mesh->mTextureCoords[0][vi].x, mesh->mTextureCoords[0][vi].y};
                m.vertices.push_back(v);
                slotCount.push_back(0);

                const glm::vec3& p = v.pos;
                if (first) { m.aabbMin = m.aabbMax = p; first = false; }
                else {
                    m.aabbMin = glm::min(m.aabbMin, p);
                    m.aabbMax = glm::max(m.aabbMax, p);
                }
            }

            for (unsigned fi = 0; fi < mesh->mNumFaces; ++fi) {
                const aiFace& f = mesh->mFaces[fi];
                for (unsigned k = 0; k < f.mNumIndices; ++k)
                    m.indices.push_back(baseVertex + f.mIndices[k]);
            }

            for (unsigned bi = 0; bi < mesh->mNumBones; ++bi) {
                const aiBone* bone = mesh->mBones[bi];
                const int boneIdx = boneNameToIdx.at(bone->mName.C_Str());
                for (unsigned wi = 0; wi < bone->mNumWeights; ++wi) {
                    const aiVertexWeight& w = bone->mWeights[wi];
                    const uint32_t vGlobal = baseVertex + w.mVertexId;
                    int& slot = slotCount[vGlobal];
                    if (slot < kWeightsPerVertex) {
                        m.vertices[vGlobal].boneIds[slot]     = boneIdx;
                        m.vertices[vGlobal].boneWeights[slot] = w.mWeight;
                        ++slot;
                    }
                }
            }
        }

        // Renormalize weights (LimitBoneWeights already normalizes, but be safe).
        for (Vertex& v : m.vertices) {
            float sum = v.boneWeights.x + v.boneWeights.y + v.boneWeights.z + v.boneWeights.w;
            if (sum > 0.0001f) v.boneWeights /= sum;
        }

        // Pass 4: first animation clip.
        if (scene->mNumAnimations > 0) {
            const aiAnimation* anim = scene->mAnimations[0];
            // glTF time-unit handling is a known assimp wrinkle — record any surprise
            // in SPIKE0_FINDINGS.md (mTicksPerSecond may be 0, 1, 25, or 1000).
            const double tps = (anim->mTicksPerSecond != 0.0) ? anim->mTicksPerSecond : 25.0;
            m.clip.duration = static_cast<float>(anim->mDuration / tps);

            for (unsigned ci = 0; ci < anim->mNumChannels; ++ci) {
                const aiNodeAnim* ch = anim->mChannels[ci];
                auto it = boneNameToIdx.find(ch->mNodeName.C_Str());
                if (it == boneNameToIdx.end()) continue;

                AnimChannel c;
                c.boneIdx = it->second;
                for (unsigned k = 0; k < ch->mNumPositionKeys; ++k)
                    c.translation.emplace_back(static_cast<float>(ch->mPositionKeys[k].mTime / tps),
                                               Convert(ch->mPositionKeys[k].mValue));
                for (unsigned k = 0; k < ch->mNumRotationKeys; ++k)
                    c.rotation.emplace_back(static_cast<float>(ch->mRotationKeys[k].mTime / tps),
                                            Convert(ch->mRotationKeys[k].mValue));
                for (unsigned k = 0; k < ch->mNumScalingKeys; ++k)
                    c.scale.emplace_back(static_cast<float>(ch->mScalingKeys[k].mTime / tps),
                                         Convert(ch->mScalingKeys[k].mValue));
                m.clip.channels.push_back(std::move(c));
            }
        }

        out = std::move(m);
        return true;
    }
    catch (const std::exception& e) {
        outError = e.what();
        return false;
    }
    catch (...) {
        outError = "unknown assimp failure";
        return false;
    }
}

}  // namespace spike
