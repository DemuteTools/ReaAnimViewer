// SPDX-License-Identifier: MIT
//
// The ONE translation unit that includes <assimp/...> (D5). Every parse-exception
// risk is contained in the single try/catch in LoadAsset, so nothing throws across
// the subsystem boundary (AR18). This TU is also allowed to call modern GL: it owns
// the geometry upload, which is why LoadErrorCategory::GpuUploadFailed lives here
// (D6 — the loader reports its own GPU failures). See Dev Notes §E.

#include "asset_loader.h"

#ifdef _WIN32

#include <cmath>
#include <filesystem>
#include <system_error>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/gtc/matrix_inverse.hpp>

#include "gl_loader.h"  // modern-GL upload entry points (glGenBuffers/glBufferData)

namespace rav {
namespace {

// The single assimp -> GLM conversion site (AR9/D3). aiMatrix4x4 is row-major;
// glm::mat4 is column-major — transpose exactly once, here and nowhere else.
glm::mat4 ConvertAssimpMatrix(const aiMatrix4x4& m)
{
    return glm::mat4(m.a1, m.b1, m.c1, m.d1,
                     m.a2, m.b2, m.c2, m.d2,
                     m.a3, m.b3, m.c3, m.d3,
                     m.a4, m.b4, m.c4, m.d4);  // column k built from assimp row k
}

// Reads diffuse/specular/shininess factors into a SceneMaterial. Texture binding
// is Story 2.2 — baseColor stays an unbound (0) handle and the shader uses the
// flat baseColorFactor. Defaults match a neutral mid-grey Blinn-Phong surface.
SceneMaterial ConvertMaterial(const aiMaterial* mat)
{
    SceneMaterial out;
    out.baseColorFactor = glm::vec3(0.8f);   // mid-grey if the file carries no diffuse
    out.specularColor   = glm::vec3(0.04f);
    out.shininess       = 32.0f;
    if (!mat) return out;   // synthetic fallback material (file had none) — all defaults

    aiColor3D c;
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, c) == AI_SUCCESS)
        out.baseColorFactor = glm::vec3(c.r, c.g, c.b);
    if (mat->Get(AI_MATKEY_COLOR_SPECULAR, c) == AI_SUCCESS)
        out.specularColor = glm::vec3(c.r, c.g, c.b);
    float s = 0.0f;
    if (mat->Get(AI_MATKEY_SHININESS, s) == AI_SUCCESS && s >= 1.0f)
        out.shininess = s;
    return out;
}

// Appends one aiMesh, baked into model space by `world`, as a SceneMesh's CPU
// arrays. Vertices/indices go into the out params; the AABB is grown over the
// baked positions. Normals use the inverse-transpose so non-uniform node scale
// stays correct (matches the renderer's u_normal convention).
void AppendMesh(const aiMesh* mesh, const glm::mat4& world,
                std::vector<SceneVertex>& verts, std::vector<uint32_t>& indices,
                glm::vec3& aabb_min, glm::vec3& aabb_max, bool& aabb_seeded)
{
    const glm::mat3 normal_mat = glm::inverseTranspose(glm::mat3(world));
    const uint32_t base = static_cast<uint32_t>(verts.size());

    for (unsigned vi = 0; vi < mesh->mNumVertices; ++vi) {
        SceneVertex v{};
        const aiVector3D& p = mesh->mVertices[vi];
        const glm::vec3 wp = glm::vec3(world * glm::vec4(p.x, p.y, p.z, 1.0f));
        v.pos = wp;
        if (mesh->HasNormals()) {
            const aiVector3D& n = mesh->mNormals[vi];
            v.normal = glm::normalize(normal_mat * glm::vec3(n.x, n.y, n.z));
        }
        if (mesh->HasTextureCoords(0)) {
            v.uv = glm::vec2(mesh->mTextureCoords[0][vi].x,
                             mesh->mTextureCoords[0][vi].y);
        }
        // boneIds / boneWeights stay zero-filled — static path (FR6).
        verts.push_back(v);

        // Only finite positions grow the AABB: a corrupt file with a NaN/Inf vertex
        // would otherwise poison aabbMin/Max, and NaN bypasses SetAsset's radius
        // guard (NaN < eps is false) → a NaN camera and a silently black viewport.
        if (!(std::isfinite(wp.x) && std::isfinite(wp.y) && std::isfinite(wp.z)))
            continue;
        if (!aabb_seeded) { aabb_min = aabb_max = wp; aabb_seeded = true; }
        else { aabb_min = glm::min(aabb_min, wp); aabb_max = glm::max(aabb_max, wp); }
    }

    for (unsigned fi = 0; fi < mesh->mNumFaces; ++fi) {
        const aiFace& f = mesh->mFaces[fi];
        // Triangulate guarantees 3 indices/face; guard anyway against degenerate
        // points/lines assimp may keep.
        if (f.mNumIndices != 3) continue;
        indices.push_back(base + f.mIndices[0]);
        indices.push_back(base + f.mIndices[1]);
        indices.push_back(base + f.mIndices[2]);
    }
}

// Recursively bakes node world transforms into model space (static path). Each
// (node, referenced-mesh) pair becomes one SceneMesh so instanced meshes keep
// their distinct transforms and per-material grouping is preserved. The node
// hierarchy itself is discarded here (Epic 3 keeps it for skinning instead).
struct PendingMesh {
    std::vector<SceneVertex> verts;
    std::vector<uint32_t>    indices;
    uint32_t                 materialIdx;
};

void WalkBake(const aiScene* scene, const aiNode* node, const glm::mat4& parent_world,
              std::vector<PendingMesh>& out,
              glm::vec3& aabb_min, glm::vec3& aabb_max, bool& aabb_seeded)
{
    const glm::mat4 world = parent_world * ConvertAssimpMatrix(node->mTransformation);

    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        // assimp normally keeps node mesh indices in range, but a corrupt/incomplete
        // file can carry a stale one; an out-of-range deref here is UB, not a throw
        // the LoadAsset catch could convert. Guard before indexing scene->mMeshes.
        const unsigned mesh_index = node->mMeshes[i];
        if (mesh_index >= scene->mNumMeshes) continue;
        const aiMesh* mesh = scene->mMeshes[mesh_index];
        if (!mesh) continue;
        PendingMesh pm;
        pm.materialIdx = mesh->mMaterialIndex;
        AppendMesh(mesh, world, pm.verts, pm.indices,
                   aabb_min, aabb_max, aabb_seeded);
        if (!pm.verts.empty() && !pm.indices.empty())
            out.push_back(std::move(pm));
    }

    for (unsigned i = 0; i < node->mNumChildren; ++i)
        WalkBake(scene, node->mChildren[i], world, out, aabb_min, aabb_max, aabb_seeded);
}

// Uploads one CPU mesh to a fresh VBO/IBO. Returns false (handles cleaned up by
// RAII) if GL could not allocate the buffers. PRECONDITION: current GL context.
bool UploadMesh(const PendingMesh& pm, SceneMesh& out)
{
    glGenBuffers(1, out.vb.addr());
    glGenBuffers(1, out.ib.addr());
    if (out.vb.get() == 0 || out.ib.get() == 0)
        return false;

    // Drain any pre-existing error so the post-upload check observes only THIS
    // upload (glGetError reports the oldest error and clears one flag — a stale one
    // would otherwise mask a real GL_OUT_OF_MEMORY from glBufferData below).
    while (glGetError() != GL_NO_ERROR) {}

    glBindBuffer(GL_ARRAY_BUFFER, out.vb.get());
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(pm.verts.size() * sizeof(SceneVertex)),
                 pm.verts.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.ib.get());
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(pm.indices.size() * sizeof(uint32_t)),
                 pm.indices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // Any error from the upload (GL_OUT_OF_MEMORY for an oversized buffer, or
    // anything else) is a GPU failure; drain the whole queue so nothing leaks into
    // the next frame's error state.
    bool gl_failed = false;
    while (glGetError() != GL_NO_ERROR) gl_failed = true;
    if (gl_failed)
        return false;

    out.indexCount  = static_cast<uint32_t>(pm.indices.size());
    out.materialIdx = pm.materialIdx;
    out.skinned     = false;  // Story 2.1 is always the static path (FR6)
    return true;
}

bool FileExists(const std::string& path)
{
    // `path` is UTF-8 (the picker hands us CP_UTF8). u8path decodes it to the native
    // wide form so non-ASCII paths (e.g. an accented Windows user folder) resolve
    // correctly — a plain narrow ifstream would mangle them under the system codepage.
    // The error_code overload never throws (we are inside the no-throw boundary).
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::u8path(path), ec);
}

}  // namespace

const char* LoadErrorCategoryName(LoadErrorCategory category)
{
    switch (category) {
        case LoadErrorCategory::Ok:                return "ok";
        case LoadErrorCategory::FileNotFound:      return "file-not-found";
        case LoadErrorCategory::ParseFailed:       return "parse-failed";
        case LoadErrorCategory::UnsupportedFormat: return "unsupported-format";
        case LoadErrorCategory::GpuUploadFailed:   return "gpu-upload-failed";
        case LoadErrorCategory::OutOfMemory:       return "out-of-memory";
        case LoadErrorCategory::Unknown:           return "unknown";
    }
    return "unknown";
}

LoadResult LoadAsset(const std::string& path)
{
    // assimp throws std::exception-derived on some malformed inputs; the whole body
    // is wrapped so nothing escapes this function (AR18). Catch std::bad_alloc as
    // OutOfMemory specifically, then any other std::exception as ParseFailed.
    try {
        if (!FileExists(path))
            return {std::nullopt, LoadErrorCategory::FileNotFound, "cannot open " + path};

        Assimp::Importer importer;

        // Set now though glTF doesn't need it: the loader is shared with FBX from
        // Epic 7 and Mixamo rigs break without PreservePivots=0 (Spike Finding 3).
        importer.SetPropertyInteger(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, 0);

        // No MakeLeftHanded / FlipWindingOrder / PreTransformVertices — they would
        // defeat the "render as-authored" contract (D3/AR13) and PreTransform also
        // destroys the node hierarchy Epic 3 needs.
        const aiScene* scene = importer.ReadFile(
            path,
            aiProcess_Triangulate | aiProcess_GenSmoothNormals |
            aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights);

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
            return {std::nullopt, LoadErrorCategory::ParseFailed, importer.GetErrorString()};

        if (scene->mNumMeshes == 0)
            return {std::nullopt, LoadErrorCategory::UnsupportedFormat,
                    "file contains no meshes"};

        // Bake all node world transforms into model-space CPU meshes + union AABB.
        std::vector<PendingMesh> pending;
        glm::vec3 aabb_min(0.0f), aabb_max(0.0f);
        bool aabb_seeded = false;
        WalkBake(scene, scene->mRootNode, glm::mat4(1.0f), pending,
                 aabb_min, aabb_max, aabb_seeded);

        if (pending.empty())
            return {std::nullopt, LoadErrorCategory::UnsupportedFormat,
                    "file has meshes but no drawable geometry"};

        Asset asset;
        asset.aabbMin  = aabb_min;
        asset.aabbMax  = aabb_max;
        // modelRoot stays identity: canonical files render upright, non-canonical
        // render tilted/scaled as-authored (AR13), recovered later by Reset Camera.

        asset.materials.reserve(scene->mNumMaterials);
        for (unsigned mi = 0; mi < scene->mNumMaterials; ++mi)
            asset.materials.push_back(ConvertMaterial(scene->mMaterials[mi]));
        // assimp always emits a default material, but a mesh's materialIdx indexes
        // this list on the render hot path — guarantee at least one entry so a
        // malformed file can never drive an out-of-bounds read.
        if (asset.materials.empty())
            asset.materials.push_back(ConvertMaterial(nullptr));

        // ...and clamp any out-of-range index (a hand-edited/corrupt file can set
        // aiMesh::mMaterialIndex past mNumMaterials) to the guaranteed [0] entry, so
        // the renderer's materials[mesh.materialIdx] is always in bounds. The
        // empty-guard above only covers the zero-material case, not this one.
        const uint32_t material_count = static_cast<uint32_t>(asset.materials.size());
        for (PendingMesh& pm : pending)
            if (pm.materialIdx >= material_count) pm.materialIdx = 0;

        asset.meshes.reserve(pending.size());
        for (const PendingMesh& pm : pending) {
            SceneMesh sm;
            if (!UploadMesh(pm, sm))
                return {std::nullopt, LoadErrorCategory::GpuUploadFailed,
                        "GL buffer upload failed (out of GPU memory?)"};
            asset.meshes.push_back(std::move(sm));
        }

        return {std::move(asset), LoadErrorCategory::Ok, {}};
    }
    catch (const std::bad_alloc&) {
        return {std::nullopt, LoadErrorCategory::OutOfMemory, "out of memory while loading"};
    }
    catch (const std::exception& e) {
        return {std::nullopt, LoadErrorCategory::ParseFailed, e.what()};
    }
    catch (...) {
        return {std::nullopt, LoadErrorCategory::Unknown, "unknown loader failure"};
    }
}

}  // namespace rav

#endif  // _WIN32
