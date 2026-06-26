// SPDX-License-Identifier: MIT
//
// The ONE translation unit that includes <assimp/...> (D5). Every parse-exception
// risk is contained in the single try/catch in LoadAsset, so nothing throws across
// the subsystem boundary (AR18). This TU is also allowed to call modern GL: it owns
// the geometry AND texture upload, which is why LoadErrorCategory::GpuUploadFailed
// lives here (D6 — the loader reports its own GPU failures). See Dev Notes §E/§F.
//
// Story 2.2 adds two recorded boundary extensions, both in-character for this TU:
// it now also includes <stb_image.h> (assimp leaves embedded glTF textures as raw
// compressed bytes and exposes no decoder — see §C) and "console_log.h" (the loader
// is the only site that knows which texture failed, so the AR16 per-texture warning
// belongs here, exactly as the GPU-upload diagnostics already do).

#include "asset_loader.h"

#ifdef _WIN32

#include <algorithm>  // std::clamp for the shininess floor/ceiling
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>  // bone name -> global skeleton index (D1 skin remap)
#include <unordered_set>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/gtc/matrix_inverse.hpp>

// stb_image: implementation compiled into THIS TU only. Narrowed to the formats
// glTF/Collada actually ship (mirrors the AR5 importer narrowing) to trim code and
// attack surface. We log our own reason strings, so the failure-string table is off.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_NO_FAILURE_STRINGS
// stbi_load() opens the sibling file by name; on Windows the default fopen uses the
// system codepage and would mangle a non-ASCII path. UTF8 makes stb convert our
// u8string() to wide + _wfopen, matching FileExists's u8path Unicode handling so a
// texture under an accented/CJK folder (gate row 9) resolves instead of warning.
#define STBI_WINDOWS_UTF8
#include <stb_image.h>

#include "console_log.h"  // LogWarn for the per-texture unresolved diagnostic (AR16)
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

// Element-wise near-equality for the shared-bone offset-divergence check (§B/§F).
bool MatNearlyEqual(const glm::mat4& a, const glm::mat4& b)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::fabs(a[c][r] - b[c][r]) > 1e-4f) return false;
    return true;
}

// DFS the node tree pre-order, indexing a node the first time it is seen as a skin
// joint. Pre-order makes a node land in `bones` before any of its descendants, so
// parentIdx < index holds for every non-root bone — the invariant D13's single
// forward global-matrix pass relies on. `nearest_joint` is the global index of the
// closest joint ancestor (-1 at the root); intermediate non-joint nodes pass it
// through unchanged, so parentIdx points at the nearest *joint*, not the raw parent
// node (the skipped nodes' transforms are 3.2's concern).
void DfsIndexJoints(const aiNode* node, int nearest_joint,
                    const std::unordered_set<std::string>& joint_names,
                    std::unordered_map<std::string, int>& name_to_index,
                    SceneSkeleton& skel)
{
    int parent_for_children = nearest_joint;
    const std::string nm = node->mName.C_Str();
    if (joint_names.count(nm) && !name_to_index.count(nm)) {
        const int gidx = static_cast<int>(skel.bones.size());
        name_to_index[nm] = gidx;
        SceneBone bone;
        bone.parentIdx = nearest_joint;
        bone.name = nm;  // as-authored UTF-8, no transliteration (FR5/AR13)
        skel.bones.push_back(std::move(bone));  // inverseBindMatrix filled below
        parent_for_children = gidx;
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        DfsIndexJoints(node->mChildren[i], parent_for_children, joint_names,
                       name_to_index, skel);
}

// Builds the flat SceneSkeleton + a bone-name -> global-index map (Task 1 / §B).
// The bones are the skin joints — the dedup'd union of every mesh's aiBone names —
// ordered parent-before-child by the node-tree DFS above. The inverse-bind matrix is
// aiBone::mOffsetMatrix passed through the ONE ConvertAssimpMatrix boundary exactly
// once (AC2/D3/AR9). Returns an empty skeleton for a boneless (static) file (FR6).
SceneSkeleton BuildSkeleton(const aiScene* scene,
                            std::unordered_map<std::string, int>& name_to_index)
{
    SceneSkeleton skel;

    // 1. Collect the skin-joint names across every mesh's bones (dedup by name).
    std::unordered_set<std::string> joint_names;
    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        if (!mesh) continue;
        for (unsigned bi = 0; bi < mesh->mNumBones; ++bi)
            if (const aiBone* b = mesh->mBones[bi])  // null slot on a malformed file
                joint_names.insert(b->mName.C_Str());
    }
    if (joint_names.empty()) return skel;  // static path — empty skeleton (FR6)

    // 2. Assign global indices by DFS pre-order (parent-before-child, §B).
    DfsIndexJoints(scene->mRootNode, -1, joint_names, name_to_index, skel);

    // 3. Fill inverse-bind matrices from each bone's FIRST occurrence. A joint that
    //    is in mBones but absent from the node tree (malformed file) was missed by
    //    the DFS — append it as a root so its weights still resolve, and warn.
    std::unordered_set<std::string> bind_set;   // names whose matrix is already set
    std::unordered_set<std::string> diverged;   // warn-once guard (§F)
    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        if (!mesh) continue;
        for (unsigned bi = 0; bi < mesh->mNumBones; ++bi) {
            const aiBone* b = mesh->mBones[bi];
            if (!b) continue;  // null bone slot: a raw deref here is an SEH access
                               // violation (UB), NOT catchable — never cross the host
                               // boundary on a malformed file (AR18/NFR-R1).
            const std::string nm = b->mName.C_Str();
            const glm::mat4 ibm = ConvertAssimpMatrix(b->mOffsetMatrix);
            auto it = name_to_index.find(nm);
            if (it == name_to_index.end()) {
                const int gidx = static_cast<int>(skel.bones.size());
                name_to_index[nm] = gidx;
                SceneBone bone;
                bone.parentIdx = -1;
                bone.name = nm;
                bone.inverseBindMatrix = ibm;
                skel.bones.push_back(std::move(bone));
                bind_set.insert(nm);
                LogWarn("skeleton: bone '%s' absent from node tree - appended as root",
                        nm.c_str());
                continue;
            }
            if (!bind_set.count(nm)) {
                skel.bones[it->second].inverseBindMatrix = ibm;
                bind_set.insert(nm);
            } else if (!diverged.count(nm) &&
                       !MatNearlyEqual(skel.bones[it->second].inverseBindMatrix, ibm)) {
                // Same bone, different offset across meshes — keep the first; a shared
                // palette can't honor both (per-mesh palettes are a 3.3 call, §F).
                diverged.insert(nm);
                LogWarn("skeleton: bone '%s' has divergent bind matrices across meshes"
                        " - keeping first", nm.c_str());
            }
        }
    }
    return skel;
}

// The validator hook (Task 3 / §E, AC4): on a skinned load, dump the parsed skeleton
// through the [RAV] info channel so the bone count / names / parent links can be
// diffed by eye against Blender's Outliner or FBX Review — the only way to audit
// AC1 before 3.3 draws the rig deformed. Non-ASCII names print as their raw UTF-8
// bytes (the FR5 check). Silent for a boneless file, keeping the Epic 2 console clean.
void DumpSkeleton(const SceneSkeleton& skel, size_t skinned_verts)
{
    LogInfo("skeleton: %zu bones, %zu skinned verts", skel.bones.size(), skinned_verts);
    for (size_t i = 0; i < skel.bones.size(); ++i) {
        const SceneBone& b = skel.bones[i];
        const bool has_parent =
            b.parentIdx >= 0 && b.parentIdx < static_cast<int>(skel.bones.size());
        // parentIdx < i must hold for every non-root bone (DFS pre-order invariant);
        // surface a violation rather than letting 3.2's forward pass read it as valid.
        if (has_parent && b.parentIdx >= static_cast<int>(i))
            LogWarn("skeleton: bone [%zu] '%s' has parent %d >= index (ordering broken)",
                    i, b.name.c_str(), b.parentIdx);
        LogInfo("  [%2zu] %-20s parent %3d (%s)", i, b.name.c_str(), b.parentIdx,
                has_parent ? skel.bones[b.parentIdx].name.c_str() : "root");
    }
}

// Reads diffuse factor + derives a per-material Blinn-Phong specular into a
// SceneMaterial (texture binding is ResolveAndUploadDiffuse, Story 2.2). The
// specular derivation (Story 2.3) is what makes a matte dielectric and a polished
// metal show a different highlight. Defaults match a neutral mid-grey surface.
SceneMaterial ConvertMaterial(const aiMaterial* mat)
{
    SceneMaterial out;
    out.baseColorFactor = glm::vec3(0.8f);   // mid-grey if the file carries no diffuse
    out.specularColor   = glm::vec3(0.04f);  // dielectric F0 fallback
    out.shininess       = 32.0f;
    if (!mat) return out;   // synthetic fallback material (file had none) — all defaults

    aiColor3D c;
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, c) == AI_SUCCESS)
        out.baseColorFactor = glm::vec3(c.r, c.g, c.b);

    // assimp synthesizes SHININESS for every importer we ship: glTF metallic-roughness
    // from (1-roughness)^2*1000, glTF specular-glossiness from glossiness*1000, FBX/
    // Collada Phong from the authored exponent. A fully-rough glTF material yields 0,
    // so clamp to a small floor (a broad, present highlight) rather than snapping to 32.
    float s = 0.0f;
    if (mat->Get(AI_MATKEY_SHININESS, s) == AI_SUCCESS)
        // std::clamp(NaN,…) returns NaN, which would poison pow() in the shader;
        // gate on isfinite so a garbage exponent falls back to the safe default.
        out.shininess = std::isfinite(s) ? std::clamp(s, 2.0f, 1000.0f) : 32.0f;

    // Specular color. Honor an explicitly-authored specular first — that single key
    // covers FBX/Collada Phong, glTF KHR_materials_specular, and pbrSpecularGlossiness.
    // Otherwise (plain glTF metallic-roughness, where assimp leaves COLOR_SPECULAR
    // UNSET) derive it: a dielectric reflects a dim ~4% white highlight (F0=0.04), a
    // metal reflects its own base color — tint toward baseColor by metalness. Without
    // this, every metallic-roughness material shares one flat specular and matte vs
    // glossy is indistinguishable (FR16).
    aiColor3D spec;
    if (mat->Get(AI_MATKEY_COLOR_SPECULAR, spec) == AI_SUCCESS) {
        out.specularColor = glm::vec3(spec.r, spec.g, spec.b);
    } else {
        float metallic = 0.0f;
        mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic);  // leaves 0 (dielectric) if absent
        // metalness is defined on [0,1]; clamp (and reject NaN) so the mix stays a
        // convex blend — an out-of-range factor would push specular past baseColor or
        // negative, a NaN would poison the fragment.
        metallic = std::isfinite(metallic) ? std::clamp(metallic, 0.0f, 1.0f) : 0.0f;
        out.specularColor =
            glm::vec3(0.04f) + (out.baseColorFactor - glm::vec3(0.04f)) * metallic;
    }
    return out;
}

// Uploads a 4-channel RGBA buffer to a fresh GL texture and returns its owning
// GpuImage (empty on GL failure → flat fallback). PRECONDITION: current GL context.
// Mirrors UploadMesh's glGetError discipline, but a texture failure is NON-fatal:
// the geometry is still drawable, so we degrade to the flat baseColorFactor rather
// than aborting the whole load (a buffer failure stays fatal — see UploadMesh).
GpuImage UploadTexture(const unsigned char* rgba, int w, int h)
{
    GpuImage tex;
    glGenTextures(1, tex.addr());
    if (tex.get() == 0) return {};

    // Drain stale errors so the post-upload check observes only THIS upload.
    while (glGetError() != GL_NO_ERROR) {}

    glBindTexture(GL_TEXTURE_2D, tex.get());
    // RGBA rows are 4-byte aligned anyway, but set 1 defensively so a future RGB
    // path (odd row stride) would still upload correctly.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);  // glTF sampler default
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    bool gl_failed = false;
    while (glGetError() != GL_NO_ERROR) gl_failed = true;
    if (gl_failed) return {};  // free the partial texture (RAII) → flat fallback
    return tex;
}

// The ONE unified diffuse-texture funnel (AR14): every packaging variant — GLB
// embedded (FR17), glTF sibling file (FR18), and later FBX embedded (FR19, Epic 6) —
// converges on GetEmbeddedTexture, decodes to RGBA, and uploads. Returns an owning
// GpuImage, or an empty handle when the material declares no diffuse texture (silent
// — the textureless flat path, AC5) or the declared texture cannot be resolved (one
// LogWarn, then flat fallback — AC3). Always decodes 4-channel RGBA so the GL upload
// format is uniform regardless of the source's channel count. mi is the material
// index, used only to name the material in the diagnostic.
GpuImage ResolveAndUploadDiffuse(const aiScene* scene, const aiMaterial* mat,
                                 const std::filesystem::path& model_dir, unsigned mi)
{
    aiString tex_path;
    if (!mat || mat->GetTexture(aiTextureType_DIFFUSE, 0, &tex_path) != AI_SUCCESS)
        return {};   // no diffuse texture declared — flat path, silent (not a failure)

    if (const aiTexture* t = scene->GetEmbeddedTexture(tex_path.C_Str())) {
        // Embedded: GLB '*N' index reference today, FBX GetEmbeddedTexture tomorrow —
        // the same branch absorbs both, which is the whole point of the unified seam.
        if (t->mHeight == 0) {
            // Compressed: pcData holds mWidth bytes of a PNG/JPG file — stb decodes it.
            int w = 0, h = 0, n = 0;
            unsigned char* px = stbi_load_from_memory(
                reinterpret_cast<const unsigned char*>(t->pcData),
                static_cast<int>(t->mWidth), &w, &h, &n, 4);   // 4 = force RGBA
            if (!px) {
                LogWarn("texture unresolved for material %u (embedded image undecodable)"
                        " - using flat color", mi);
                return {};
            }
            GpuImage tex = UploadTexture(px, w, h);
            stbi_image_free(px);   // CPU pixels freed immediately after upload (AC6)
            if (tex.get() == 0)
                LogWarn("texture unresolved for material %u (GPU upload failed)"
                        " - using flat color", mi);
            return tex;
        }
        // Uncompressed: mWidth*mHeight aiTexel, stored B,G,R,A by assimp — read by the
        // named members so the result is RGBA regardless of in-memory channel order.
        const size_t count = static_cast<size_t>(t->mWidth) * t->mHeight;
        std::vector<unsigned char> rgba(count * 4);
        for (size_t i = 0; i < count; ++i) {
            const aiTexel& texel = t->pcData[i];
            rgba[i * 4 + 0] = texel.r;
            rgba[i * 4 + 1] = texel.g;
            rgba[i * 4 + 2] = texel.b;
            rgba[i * 4 + 3] = texel.a;
        }
        GpuImage tex = UploadTexture(rgba.data(), static_cast<int>(t->mWidth),
                                     static_cast<int>(t->mHeight));
        if (tex.get() == 0)
            LogWarn("texture unresolved for material %u (GPU upload failed)"
                    " - using flat color", mi);
        return tex;
    }

    // External sibling file — resolve relative to the model dir, decode from disk.
    // Path carried as UTF-8 via u8path/u8string (consistent with FileExists's Unicode
    // fix); a percent-encoded/absolute-URI edge case simply fails to stbi_load and
    // takes the diagnostic path below, the model still rendering in flat color.
    const std::filesystem::path file = model_dir / std::filesystem::u8path(tex_path.C_Str());
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(file.u8string().c_str(), &w, &h, &n, 4);  // 4 = force RGBA
    if (!px) {
        LogWarn("texture unresolved for material %u (%s missing or undecodable)"
                " - using flat color", mi, tex_path.C_Str());
        return {};
    }
    GpuImage tex = UploadTexture(px, w, h);
    stbi_image_free(px);
    if (tex.get() == 0)
        LogWarn("texture unresolved for material %u (GPU upload failed)"
                " - using flat color", mi);
    return tex;
}

// Appends one aiMesh, baked into model space by `world`, as a SceneMesh's CPU
// arrays. Vertices/indices go into the out params; the AABB is grown over the
// baked positions. Normals use the inverse-transpose so non-uniform node scale
// stays correct (matches the renderer's u_normal convention).
void AppendMesh(const aiMesh* mesh, const glm::mat4& world,
                const std::unordered_map<std::string, int>& bone_index,
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
        // boneIds / boneWeights left zero (SceneVertex v{}); the skin scatter below
        // fills them for skinned meshes, and a boneless mesh keeps the static path.
        verts.push_back(v);

        // Only finite positions grow the AABB: a corrupt file with a NaN/Inf vertex
        // would otherwise poison aabbMin/Max, and NaN bypasses SetAsset's radius
        // guard (NaN < eps is false) → a NaN camera and a silently black viewport.
        if (!(std::isfinite(wp.x) && std::isfinite(wp.y) && std::isfinite(wp.z)))
            continue;
        if (!aabb_seeded) { aabb_min = aabb_max = wp; aabb_seeded = true; }
        else { aabb_min = glm::min(aabb_min, wp); aabb_max = glm::max(aabb_max, wp); }
    }

    // Scatter skin influences into the global bone slots (Task 2 / §C). assimp's
    // per-mesh bone index is local; remap to the global skeleton index via the §B
    // name map so 3.3 can address its matrix palette as palette[boneId]. With
    // JoinIdenticalVertices active, mWeights[].mVertexId is in the same index space
    // as mVertices, so `base + id` lands on the vertex AppendMesh just pushed.
    for (unsigned lb = 0; lb < mesh->mNumBones; ++lb) {
        const aiBone* b = mesh->mBones[lb];
        if (!b) continue;                        // null bone slot on a malformed file (AR18)
        const auto it = bone_index.find(b->mName.C_Str());
        if (it == bone_index.end()) continue;   // skeleton built first → always found
        const int gb = it->second;
        for (unsigned w = 0; w < b->mNumWeights; ++w) {
            const aiVertexWeight& vw = b->mWeights[w];
            // Reject zero/negative AND non-finite weights: a NaN passes `<= 0`
            // (compares false), then defeats the `== 0.0f` free-slot sentinel below
            // and would upload as a NaN-deformed vertex in 3.3.
            if (!std::isfinite(vw.mWeight) || vw.mWeight <= 0.0f) continue;
            const size_t v = base + vw.mVertexId;
            if (v >= verts.size()) continue;     // defensive against a stale vertex id
            SceneVertex& vert = verts[v];
            for (int s = 0; s < 4; ++s) {        // next free slot; LimitBoneWeights caps
                if (vert.boneWeights[s] == 0.0f) {   // at 4, but never write past index 3
                    vert.boneIds[s]     = gb;
                    vert.boneWeights[s] = vw.mWeight;
                    break;
                }
            }
        }
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
    bool                     skinned = false;  // mesh->mNumBones > 0 (D13/FR6 selector)
};

void WalkBake(const aiScene* scene, const aiNode* node, const glm::mat4& parent_world,
              const std::unordered_map<std::string, int>& bone_index,
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
        pm.skinned     = (mesh->mNumBones > 0);
        AppendMesh(mesh, world, bone_index, pm.verts, pm.indices,
                   aabb_min, aabb_max, aabb_seeded);
        if (!pm.verts.empty() && !pm.indices.empty())
            out.push_back(std::move(pm));
    }

    for (unsigned i = 0; i < node->mNumChildren; ++i)
        WalkBake(scene, node->mChildren[i], world, bone_index, out,
                 aabb_min, aabb_max, aabb_seeded);
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
    out.skinned     = pm.skinned;  // data for 3.3's path selection; drives nothing yet
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

        // Parse the skin skeleton first: WalkBake's per-vertex weight scatter needs
        // the bone-name -> global-index map. Empty for a boneless file (FR6 static
        // path) — the scatter then no-ops and skinned stays false everywhere.
        std::unordered_map<std::string, int> bone_index;
        SceneSkeleton skeleton = BuildSkeleton(scene, bone_index);

        // Bake all node world transforms into model-space CPU meshes + union AABB.
        std::vector<PendingMesh> pending;
        glm::vec3 aabb_min(0.0f), aabb_max(0.0f);
        bool aabb_seeded = false;
        WalkBake(scene, scene->mRootNode, glm::mat4(1.0f), bone_index, pending,
                 aabb_min, aabb_max, aabb_seeded);

        if (pending.empty())
            return {std::nullopt, LoadErrorCategory::UnsupportedFormat,
                    "file has meshes but no drawable geometry"};

        Asset asset;
        asset.aabbMin  = aabb_min;
        asset.aabbMax  = aabb_max;
        // modelRoot stays identity: canonical files render upright, non-canonical
        // render tilted/scaled as-authored (AR13), recovered later by Reset Camera.

        // Resolve each material's diffuse texture beside its flat factors. model_dir
        // (computed once) anchors the sibling-file branch; a per-texture decode/IO
        // failure returns an empty handle and the load continues (only a catastrophic
        // std::bad_alloc from the swizzle buffer reaches the OutOfMemory catch).
        const std::filesystem::path model_dir =
            std::filesystem::u8path(path).parent_path();
        asset.materials.reserve(scene->mNumMaterials);
        for (unsigned mi = 0; mi < scene->mNumMaterials; ++mi) {
            SceneMaterial material = ConvertMaterial(scene->mMaterials[mi]);
            material.baseColor =
                ResolveAndUploadDiffuse(scene, scene->mMaterials[mi], model_dir, mi);
            asset.materials.push_back(std::move(material));
        }
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

        // Audit the parsed skeleton (skinned files only — a static load stays silent).
        if (!skeleton.bones.empty()) {
            size_t skinned_verts = 0;
            for (const PendingMesh& pm : pending)
                for (const SceneVertex& v : pm.verts)
                    if (v.boneWeights[0] != 0.0f || v.boneWeights[1] != 0.0f ||
                        v.boneWeights[2] != 0.0f || v.boneWeights[3] != 0.0f)
                        ++skinned_verts;
            DumpSkeleton(skeleton, skinned_verts);
        }
        asset.skeleton = std::move(skeleton);

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
