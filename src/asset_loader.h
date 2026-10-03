// SPDX-License-Identifier: MIT
//
// The assimp boundary (D5/D6). LoadAsset reads a glTF/GLB/FBX/Collada file, parses
// it into the D1 `Asset` model, and uploads its geometry to the GPU. This header
// pulls in NO assimp or GL headers — consumers see only `scene.h` types; all
// parse-exception risk is contained in asset_loader.cpp's single try/catch (AR18).
//
// PRECONDITION: a current GL context. LoadAsset creates VBOs/IBOs, so it must be
// called on the main thread after the viewer's WGL context is made current.
//
// Story 11-2 splits the load in two so a model can be drawn by several GL contexts
// (the viewer on the UI thread, the video FX on REAPER's video thread):
//   LoadCpuAsset  — the GL-free parse into an immutable CpuAsset (textures kept as their
//                   encoded file bytes, decoded one at a time at upload), safe on any thread;
//   UploadAsset   — that CpuAsset's GPU copy for the CURRENT context.
// LoadAsset is both, as before, and also hands back the CpuAsset so the viewer can
// share it with the video thread (src/asset_cache.h).

#pragma once

#ifdef _WIN32

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "scene.h"

namespace rav {

// Why a category enum and not just a bool: the console funnel (D7) names the
// failure class, and the viewport degrades the same way for every class — log one
// line, keep the window alive (AR16/AR17). Ok is unused in the failure return.
enum class LoadErrorCategory {
    Ok,
    FileNotFound,
    ParseFailed,
    UnsupportedFormat,
    GpuUploadFailed,
    OutOfMemory,
    Unknown,
};

// Human-readable category label for the console line.
const char* LoadErrorCategoryName(LoadErrorCategory category);

// ---- GL-free model (Story 11-2) ---------------------------------------------
// The parsed model, before any GL upload. Immutable once built: it is shared between
// threads through std::shared_ptr<const CpuAsset>.

// One texture, kept in its smallest usable form: the ENCODED file bytes (PNG/JPG/TGA/BMP,
// header already validated) when it came from a file or an embedded compressed image, else
// decoded RGBA8 pixels (assimp's rare uncompressed embedded texels). UploadAsset decodes an
// encoded image just before its upload and frees the pixels right after (Story 2.2 AC6), so
// a load never holds every decoded texture at once. Empty = no texture (flat colour).
struct CpuImage {
    int width  = 0;
    int height = 0;
    std::vector<unsigned char> encoded;  // file bytes, decoded at upload
    std::vector<unsigned char> rgba;     // or decoded RGBA8, top row first
    bool empty() const { return (encoded.empty() && rgba.empty()) || width <= 0 || height <= 0; }
};

struct CpuMesh {
    std::vector<SceneVertex> verts;
    std::vector<uint32_t>    indices;
    uint32_t                 materialIdx = 0;
    bool                     skinned = false;  // mesh->mNumBones > 0 (D13/FR6 selector)
};

struct CpuMaterial {
    CpuImage  baseColor;     // colour → uploaded sRGB (GL_SRGB8_ALPHA8)
    CpuImage  normalMap;     // data → uploaded linear (GL_RGBA8)
    CpuImage  specularMap;   // data → linear
    CpuImage  glossMap;      // data → linear
    glm::vec3 baseColorFactor{0.8f};
    glm::vec3 specularColor{0.04f};
    float     shininess = 32.0f;
};

struct CpuAsset {
    std::vector<CpuMesh>        meshes;
    std::vector<CpuMaterial>    materials;      // never empty; every materialIdx in range
    SceneSkeleton               skeleton;
    std::vector<SceneAnimation> animations;
    glm::mat4                   modelRoot{1.0f};
    glm::vec3                   aabbMin{0.0f};  // posed bounds when animated (Epic 9)
    glm::vec3                   aabbMax{0.0f};
    float                       metersPerUnit = 1.0f;

    // Facts behind the user notices (LoadResult::notices), set by LoadCpuAsset.
    bool no_animation       = false;  // the file has no animation
    bool mesh_not_skinned   = false;  // animated, but no vertex is skinned
    bool animation_mismatch = false;  // animated + skinned, but no bone is animated
    int  missing_textures   = 0;      // declared but not found / undecodable
};

struct CpuLoadResult {
    std::shared_ptr<const CpuAsset> asset;      // null on failure
    LoadErrorCategory               category = LoadErrorCategory::Ok;
    std::string                     detail;
    std::string                     hint;
};

// The GL-free parse. No-throw; safe on any thread (assimp and stb are used per call).
// Logs texture warnings like LoadAsset, but no user notice (LoadAsset builds those).
CpuLoadResult LoadCpuAsset(const std::string& path);

// Uploads `cpu` into the CURRENT GL context. False when a buffer cannot be allocated
// (the model cannot be drawn; `out` is left untouched). A texture that fails to upload is
// non-fatal (flat colour) and counted in *out_failed_textures (optional).
bool UploadAsset(const CpuAsset& cpu, Asset& out, int* out_failed_textures);

struct LoadResult {
    std::optional<Asset> asset;
    LoadErrorCategory    category = LoadErrorCategory::Ok;
    std::string          detail;   // human-readable, goes into the console line
    // Plain-language reason shown to the user on a failure (no jargon; says what to fix
    // in the export when we can tell). Always set on failure.
    std::string          hint;
    // On SUCCESS: non-blocking problems shown to the user in the same plain language
    // (missing textures, no animation, mesh not skinned...). Empty when all is well.
    std::vector<std::string> notices;
    // On SUCCESS: the parse the GPU copy was made from, to share with the video thread
    // (Story 11-2). Null on failure.
    std::shared_ptr<const CpuAsset> cpu;
};

// No-throw across this boundary (assimp throws std::exception-derived internally;
// nothing escapes). On success: result.asset has a value, category == Ok. On
// failure: result.asset is empty and category/detail describe the cause.
LoadResult LoadAsset(const std::string& path);

// CPU-only duration probe for the PCM_source (Story 4.2). Returns the first
// animation clip's duration in SECONDS, or 0.0 for no-file / no-clip / parse
// failure. Unlike LoadAsset this is GL-FREE — it needs NO current GL context, so
// it is safe to call on a file drop when the viewer window (and its WGL context)
// may not exist. It does only the minimal assimp parse needed to read mDuration /
// mTicksPerSecond (no mesh post-processing, no GPU upload). No-throw (AR18).
double ProbeAnimationDuration(const std::string& path);

}  // namespace rav

#endif  // _WIN32
