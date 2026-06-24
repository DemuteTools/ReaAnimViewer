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

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <system_error>
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
