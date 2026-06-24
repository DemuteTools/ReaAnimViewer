# Story 2.1: Load and display a static glTF/GLB mesh

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want to open a static `.glb`/`.gltf` and see its geometry shaded in the panel,
So that I can confirm the file loaded and inspect its shape.

This is the first story of **Epic 2 (Phase 1)** and the first time the viewport renders a **real file** instead of the Epic 1 built-in test cube. It stands up three brand-new subsystems — the `Asset` scene model (D1), the assimp loader boundary (D5/D6), and the upgrade from the Lambert test shader to a Blinn-Phong material pipeline — and wires assimp into the build (AR5/D15). Get this right and Stories 2.2 (textures), 2.3 (multi-material specular), 2.4 (camera), and all of Epic 3 (skinning) plug into stable seams.

## Acceptance Criteria

From [epics.md#Story 2.1](../planning-artifacts/epics.md) (lines 340–354), verbatim BDD:

**Given** a Khronos-sample or Demute static glTF/GLB file
**When** the loader reads it via assimp into the `Asset` model (D1) and the renderer draws it with basic Blinn-Phong lighting
1. **Then** the mesh appears correctly lit and oriented for a canonical (Y-up, meters, CCW) file (FR1, FR2, AR9)
2. **And** a file containing no animation channels still renders as a static mesh via the `skinned=false` path (FR6)
3. **And** a non-canonical file (Z-up, centimeters, or non-English bone names) loads without error and renders **as-authored, tilted/scaled rather than remapped** (FR4, FR5, AR13)
4. **And** a static Collada (`.dae`) file loads through the same assimp path and renders as-authored, its `<up_axis>` handled by the camera-tolerance mechanism with no special-casing (FR47, AR13)
5. **And** assimp is built with **only the glTF, FBX, and Collada importers** enabled (AR5)

Implied, non-negotiable (system must stay working end-to-end — these are requirements even though not in the AC text):

6. A malformed/unreadable file produces a single `[RAV] error:` console line and the viewport stays alive — **Reaper never crashes** (AR16, AR17, D5).
7. The build stays **warning-free under `/W3 /permissive-`** (NFR-R5) and the deliverable remains a **single `reaper_animviewer.dll`** with no side-DLL (D15/D17).
8. No GL resources leak across model load / viewport close-reopen (D2/AR8) — Epic 1's clean teardown must survive.

## Tasks / Subtasks

- [x] **Task 1 — Vendor assimp into the build, narrowed to 3 importers (AC: 5, 7)**
  - [x] In root `CMakeLists.txt`, add assimp **v6.0.5** via `FetchContent` (mirror the existing GLM block at `CMakeLists.txt:22-27` — `GIT_SHALLOW TRUE`, pinned tag). Vendoring the source tree is impractical on the drvfs/WSL mount; FetchContent is the sanctioned pattern here (same rationale as GLM, see `extern/VENDORED.md`).
  - [x] Set the D15 narrowing cache vars **before** `FetchContent_MakeAvailable`: `ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF`, then `ASSIMP_BUILD_GLTF_IMPORTER ON`, `ASSIMP_BUILD_FBX_IMPORTER ON`, `ASSIMP_BUILD_COLLADA_IMPORTER ON`; plus `ASSIMP_BUILD_TESTS OFF`, `ASSIMP_BUILD_SAMPLES OFF`, `ASSIMP_BUILD_ASSIMP_TOOLS OFF`, `ASSIMP_INSTALL OFF`, `ASSIMP_NO_EXPORT ON`, `ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF`, `BUILD_SHARED_LIBS OFF` (static link → single DLL). Use `CACHE BOOL "" FORCE` on each (exact block in Dev Notes §A).
  - [x] Add `src/asset_loader.cpp` to the explicit `add_reaper_extension(animviewer SOURCES …)` list (no GLOB). `src/scene.h` is header-only — no source entry.
  - [x] Link `assimp` PRIVATE alongside `opengl32 gdi32 user32`. Add assimp's include dir as a **SYSTEM** include (like GLM at `CMakeLists.txt:39`) so its headers can't trip `/W3 /permissive-`.
  - [x] Confirm the configure step succeeds: `cmake -S . -B /tmp/rav_build` (in-tree `build/` hits a drvfs `configure_file` restriction — always configure out of tree on this box). First build will be **long** (full assimp compile) — note it in `build.bat` expectations.

- [x] **Task 2 — Create `src/scene.h`: the D1 `Asset` model (AC: 1, 2, 3)**
  - [x] New header-only file, namespace `rav`, SPDX header + one-line purpose. Define the full D1 aggregate (see Dev Notes §B for the exact struct set). POD-style, no inheritance/virtuals (AR7).
  - [x] `Asset` is **move-only** (it owns RAII GPU handles) and must be move-constructible/assignable so it can be swapped in atomically (D4, Epic 6 reload).
  - [x] Static-path note: only `meshes`, `materials`, `modelRoot`, `aabbMin/aabbMax` are populated in this story; `skeleton`/`animations` stay empty (FR6). Each `SceneMesh.skinned = false`.

- [x] **Task 3 — Add a `GpuImage` (texture) RAII handle (AC: 1)**
  - [x] In `src/gpu_resources.h`, add `struct TextureDeleter { void operator()(GLuint h) const noexcept { glDeleteTextures(1, &h); } };` and `using GpuImage = GpuHandle<TextureDeleter>;`. `SceneMaterial::baseColor` needs the type to exist now; **no texture is uploaded in this story** (that's 2.2) — the handle stays 0/unbound and the shader uses the flat base-color factor.

- [x] **Task 4 — Build the assimp loader boundary `src/asset_loader.{h,cpp}` (AC: 1, 2, 3, 4, 6)**
  - [x] `asset_loader.h`: declare `LoadResult LoadAsset(const std::string& path);` and the D6 `LoadResult` / `LoadErrorCategory` types (Dev Notes §C). Header includes **no** assimp/GL headers — consumers see only `scene.h` types.
  - [x] `asset_loader.cpp` is the **only** TU that includes `<assimp/...>`. Wrap the entire `Importer::ReadFile` → parse → upload body in one `try { … } catch (const std::exception& e) { return {std::nullopt, LoadErrorCategory::ParseFailed, e.what()}; } catch (...) { return {std::nullopt, LoadErrorCategory::Unknown, "unknown loader failure"}; }` (D5, AR18). assimp throws `std::exception`-derived; **nothing may escape this function.**
  - [x] Importer config: `Assimp::Importer importer;` then `importer.SetPropertyInteger(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, 0);` (set now though glTF doesn't need it — the loader is shared with FBX from Epic 7 and Mixamo rigs break without it; cheap insurance, see Spike Finding 3).
  - [x] Post-process flags: `aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights`. **Do NOT** add `aiProcess_MakeLeftHanded`, `aiProcess_FlipWindingOrder`, or `aiProcess_PreTransformVertices` — they would defeat D3/AR13 "render as-authored." (`aiProcess_PreTransformVertices` also destroys the node hierarchy Epic 3 needs.)
  - [x] Walk `aiScene->mMeshes`: for each, build a CPU vertex array in `SceneVertex` layout (pos, normal, uv from channel 0; `boneIds`/`boneWeights` zero-filled for the static path) and a `uint32_t` index array from the (triangulated) faces. Track the union AABB across all meshes → `Asset::aabbMin/aabbMax`.
  - [x] Bake node world transforms into vertex positions for the static path: traverse `mRootNode`, accumulate each node's transform through the **single** `convertAssimpMatrix(const aiMatrix4x4&)` boundary (Dev Notes §D — transpose row-major→column-major, exactly one call site, AR9), and transform each referenced mesh's vertices into model space. Leave `Asset::modelRoot = identity` (canonical files render upright; non-canonical render tilted — that's the AR13 contract, recovered later by Reset Camera).
  - [x] Materials: for each `aiMaterial`, read `AI_MATKEY_COLOR_DIFFUSE` → `baseColorFactor` (default mid-grey `vec3(0.8)` if absent), `AI_MATKEY_COLOR_SPECULAR` → `specularColor` (default `vec3(0.04)`), `AI_MATKEY_SHININESS` → `shininess` (default `32.0`, clamp ≥ 1). Leave `baseColor` (texture) unbound — Story 2.2.
  - [x] **GPU upload happens here** (this TU is allowed to call modern GL — see boundary note in Dev Notes §E): for each mesh create `GpuBuffer vb/ib` via `glGenBuffers`/`glBufferData(GL_STATIC_DRAW)`, store `indexCount`. On any GL failure return `LoadErrorCategory::GpuUploadFailed`. **Precondition: a current GL context** — `LoadAsset` is called on the main thread after the viewport context is current (AR18); document this in the header.
  - [x] Empty result guard: if `mNumMeshes == 0`, return `UnsupportedFormat` with a useful detail string rather than an empty Asset.

- [x] **Task 5 — Upgrade the renderer from test cube to Blinn-Phong asset draw (AC: 1, 2, 3)**
  - [x] `src/gl_loader.h`: append to `RAV_GL_FUNCS` the uniform setters the material shader needs — `glUniform3fv`, `glUniform1f`, `glUniform1i` (and `glActiveTexture` is **not** needed yet; textures are 2.2). `glGenTextures`/`glBindTexture`/`glTexImage2D`/`glTexParameteri`/`glDeleteTextures` are GL 1.1 core (already in `<gl/GL.h>`) — no table rows needed for them.
  - [x] Replace the inline Lambert shader (`renderer.cpp:29-57`) with Blinn-Phong (Dev Notes §F): VS outputs world-space position + normal + uv; FS computes `ambient + diffuse·Lambert + specular·(half-vector)^shininess` using uniforms `u_baseColor` (vec3), `u_specularColor` (vec3), `u_shininess` (float), `u_viewPos` (vec3, camera world pos), single hardcoded directional light. Keep `#version 330 core`. Extend the vertex layout from `{pos,normal,color}` to the `SceneVertex` layout; enable attribs **0/1/2 (pos/normal/uv) only** — leave the bone attribs (3/4) for Epic 3 so the buffer layout never churns.
  - [x] Add `void Renderer::SetAsset(Asset&& asset);` — stores the moved Asset and computes the framing camera from `aabbMin/aabbMax` (auto-fit: `center = 0.5*(min+max)`, `radius = 0.5*length(max-min)`, eye = `center + dir*radius*3`, near/far derived from radius). This guarantees AC1/AC3 "visible and framed" for any scale/orientation. Remove the built-in cube/ground generation (`AppendFace`, `ground_draw_`, `cube_draw_`).
  - [x] `RenderFrame(time, w, h)`: if an Asset is loaded, iterate `asset.meshes` — per mesh `glBindBuffer` its `vb`/`ib`, re-specify the attrib pointers on the shared `vao_`, set the material uniforms from `materials[mesh.materialIdx]`, set `u_mvp`/`u_normal`/`u_viewPos`, `glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0)`. If no Asset, just clear (no crash, blank viewport). **No heap allocation on the per-frame path** (D2, established in 1.2 review).
  - [x] `Shutdown()` must release the stored Asset's GPU handles while the context is current (the Asset's RAII members do this on destruction/clear — verify ordering matches the existing `Shutdown` at `renderer.cpp:247-256`).

- [x] **Task 6 — Wire model loading into the viewport lifecycle (AC: 1, 6)**
  - [x] In `viewer_window.cpp` `StartRendering` (`:197-263`), **after** `g_renderer.Init(err)` succeeds (context is current here), resolve a model path and call `LoadAsset`. There is no file-picker UI until Epic 5 — for this story resolve the path from environment variable **`RAV_MODEL`** (so Antho can point the gate at canonical / non-canonical / no-anim / `.dae` fixtures without rebuilding); if unset, render the blank viewport and log `[RAV] info: no RAV_MODEL set — viewport idle`.
  - [x] On `LoadAsset` success: `g_renderer.SetAsset(std::move(result.asset.value()))` and `LogInfo("loaded %s (%zu meshes)", …)`. On failure: `LogError("load failed [%s]: %s", category, result.detail.c_str())` and continue with a blank viewport (AR17 — never bail the window, never crash Reaper).
  - [x] Keep all Epic 1 lifecycle invariants intact: visibility-gated render park (don't destroy on hide), symmetric `StopRendering`, the `WM_CREATE` no-throw boundary (AR18). The `LoadAsset` call is inside `StartRendering`, which is already inside the `WM_CREATE` try/catch — but `LoadAsset` is itself no-throw, so a bad file degrades gracefully without relying on that outer catch.

- [x] **Task 7 — Author the Phase 1 validator gate (AC: 1–4)**
  - [x] Create `docs/PHASE1_VALIDATOR_GATE.md` (only `PHASE0_…` and `PHASE0.5_…` exist). Seed it with Story 2.1 checks: canonical glTF renders lit+upright; GLB renders; a no-animation file renders (FR6 static path); a Z-up/cm/foreign-bone-name file loads without error and renders tilted/scaled (not remapped); a static `.dae` loads; a deliberately corrupt file logs one error line and Reaper survives; build is `/W3`-clean; single DLL (no `assimp-*.dll` beside it). This file is the **sole authority** on Phase 1 completion (AR19).

### Review Findings

Code review 2026-06-24 (BMAD adversarial: Blind Hunter + Edge Case Hunter + Acceptance Auditor). 1 decision-needed, 8 patch, 0 defer, 8 dismissed. Story was already validator-PASS; the loader-path patches only change behaviour on malformed/adversarial input, so they do **not** invalidate the passing Windows gate (well-formed fixtures unaffected).

- [x] [Review][Decision→Patch, fixed] Non-ASCII / Unicode model paths corrupted by `CP_ACP` narrowing — `PromptForModelFile` round-trips the wide path through `WideCharToMultiByte(CP_ACP,…)`, then assimp/`ifstream` opens it narrow; any path the active codepage can't map (non-Latin usernames, e.g. `C:\Users\<accented>\…`) is lossily substituted and misreported as `file-not-found`. Fix now (carry the path wide/UTF-8 end-to-end + custom assimp `IOSystem` — non-trivial) vs accept ASCII-only for MVP and track as a hardening item. [src/viewer_window.cpp:PromptForModelFile → src/asset_loader.cpp:FileExists] (blind+edge)
- [x] [Review][Patch] **CRITICAL** — `materialIdx` out-of-bounds read on the render hot path: `RenderFrame` does `asset_.materials[mesh.materialIdx]` (unchecked `operator[]`) with an index copied verbatim from `aiMesh::mMaterialIndex`; the `if(materials.empty())` fallback only covers zero-material files, not an index `>= materials.size()` from a malformed file → per-frame UB. Clamp at load time once the materials list is finalized. [src/asset_loader.cpp:WalkBake / src/renderer.cpp:RenderFrame] (blind+edge)
- [x] [Review][Patch] NaN/Inf vertex positions poison the AABB and bypass the degenerate-radius guard (`radius < 1e-4f` is false for NaN) → NaN camera/projection → silent black viewport, no diagnostic. Guard with a finite-radius test (`!(radius > eps)`) or reject non-finite verts. [src/asset_loader.cpp:AppendMesh / src/renderer.cpp:SetAsset] (blind+edge)
- [x] [Review][Patch] `UploadMesh` GL error handling is narrow: it tests only `glGetError()==GL_OUT_OF_MEMORY` once, doesn't drain the queue, and can consume a stale pre-existing error (masking a real upload failure). Clear errors before `glBufferData`, then loop `glGetError` until `GL_NO_ERROR` and fail on any error. [src/asset_loader.cpp:UploadMesh] (blind+edge)
- [x] [Review][Patch] `scene.h` struct primitives have no default member initializers (`SceneMesh::indexCount/materialIdx/skinned`, `SceneMaterial::shininess`, `SceneBone::parentIdx`, `SceneAnimation::duration`) — safe today (all writers exhaustive) but a future partial-fill reads uninitialized. Add `= 0`/`{}` defaults to match `Asset`'s own member-init style. [src/scene.h] (edge)
- [x] [Review][Patch] `Renderer::Init` treats **any** `-1` uniform location as fatal; a driver/optimizer that eliminates a dead uniform returns `-1` for a correctly-compiled program, bricking the viewer. `glUniform*(-1,…)` is a documented no-op — keep `u_mvp` strict, relax the rest. [src/renderer.cpp:Init] (blind)
- [x] [Review][Patch] `cached_aspect_` projection-rebuild sentinel is `0.0f`, a value a real aspect could (after a refactor of the `width<1→1` clamp) collide with. Use an impossible `-1.0f` sentinel. [src/renderer.cpp:SetAsset/Init] (blind)
- [x] [Review][Patch] `node->mMeshes[i]` indexes `scene->mMeshes` without a bounds check vs `scene->mNumMeshes` — a wild `aiMesh*` deref (UB, not a catchable throw) on a corrupt/incomplete file. Cheap one-line guard. [src/asset_loader.cpp:WalkBake] (edge)
- [x] [Review][Patch] Doc/comment drift after the PO-approved `RAV_MODEL`→file-dialog swap: `PHASE1_VALIDATOR_GATE.md` §5 rows 1/6/7 still instruct "Point `RAV_MODEL` at …" (un-actionable — the env var is no longer read) while §4 describes the dialog; `renderer.cpp` keeps a stale "no RAV_MODEL" comment. Rewrite the rows to the dialog flow + scrub the comment. [docs/PHASE1_VALIDATOR_GATE.md, src/renderer.cpp] (auditor)

## Dev Notes

### Critical orientation — the architecture doc is stale on names/backend; trust the live code

`architecture.md` predates the AR21 rename and the Spike-0 backend pivot. Its **struct definitions, D-decision semantics, AR list, and flag names are authoritative**, but these mechanism names in it are **dead** — do not reintroduce them:

- ❌ `fbxav` namespace → ✅ **`rav`**   ❌ `reaper_fbxanimationviewer.dll` → ✅ **`reaper_animviewer.dll`**
- ❌ `[FBXAV]` log prefix → ✅ **`[RAV]`** (confirmed in `console_log.h`)
- ❌ sokol / `sg_*` types → ✅ **raw OpenGL** (`gl_loader.h` X-macro + `gpu_resources.h` wrap `GLuint`)
- ❌ offscreen FBO / `ImGui::Image()` bridge / CPU readback / ReaImGui → ✅ **direct render into the docked GL window** (Epic 1, shipped). **Never add `glGenFramebuffers` or any ReaImGui dependency in Epic 2.**
- ❌ `viewer_panel.{cpp,h}`, `log.{cpp,h}`, `imgui_api.*` → ✅ **`viewer_window.{cpp,h}`, `console_log.{cpp,h}`** (no imgui file exists)

`src/` is **flat** (no `include/`, no subdirs until file count > ~40). assimp is **not yet present** anywhere on this branch.

### §A — assimp CMake block (Task 1)

Insert near the GLM FetchContent block in root `CMakeLists.txt`, inside the `if(WIN32)` section, **before** `FetchContent_MakeAvailable`:

```cmake
set(ASSIMP_BUILD_TESTS                          OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES                        OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS                   OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL                              OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT                            ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT       OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT       OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER                  ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER                   ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_COLLADA_IMPORTER               ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS                           OFF CACHE BOOL "" FORCE)
FetchContent_Declare(assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v6.0.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(assimp)
# link: target_link_libraries(animviewer PRIVATE assimp opengl32 gdi32 user32)
# include assimp headers as SYSTEM so /W3 /permissive- stays clean:
#   target_include_directories(animviewer SYSTEM PRIVATE "${assimp_SOURCE_DIR}/include" "${assimp_BINARY_DIR}/include")
```

The proven pin is **v6.0.5** (Spike 0). Static link is mandatory — no `assimp-vc143-mt.dll` may ship alongside (D15/D17).

### §B — `src/scene.h` D1 model (Task 2)

Verbatim from architecture D1 (`architecture.md:193-206`), with **two deliberate refinements** flagged in Project Structure Notes:

```cpp
struct SceneVertex   { glm::vec3 pos; glm::vec3 normal; glm::vec2 uv;
                       glm::ivec4 boneIds; glm::vec4 boneWeights; };   // bone* zero-filled (static path)
struct SceneMesh     { GpuBuffer vb; GpuBuffer ib; uint32_t indexCount;
                       uint32_t materialIdx; bool skinned; };
struct SceneMaterial { GpuImage baseColor; glm::vec3 baseColorFactor;  // baseColorFactor = refinement (see notes)
                       glm::vec3 specularColor; float shininess; };
struct SceneBone     { int parentIdx; glm::mat4 inverseBindMatrix; std::string name; };
struct SceneSkeleton { std::vector<SceneBone> bones; };
// Keyframe/animation types are declared now so Asset's shape is final (Epic 3 fills them):
struct KeyframeT     { float time; glm::vec3 value; };
struct KeyframeR     { float time; glm::quat value; };
struct KeyframeS     { float time; glm::vec3 value; };
struct AnimChannel   { std::vector<KeyframeT> translation;
                       std::vector<KeyframeR> rotation; std::vector<KeyframeS> scale; };
struct SceneAnimation{ float duration; std::vector<AnimChannel> channels; };  // channels indexed by bone idx
struct Asset {
    std::vector<SceneMesh>      meshes;
    std::vector<SceneMaterial>  materials;
    SceneSkeleton               skeleton;      // empty for static-mesh fallback (FR6)
    std::vector<SceneAnimation> animations;    // empty in Story 2.1
    glm::mat4                   modelRoot{1.0f};// identity for canonical; non-canonical left as-authored (AR13)
    glm::vec3                   aabbMin, aabbMax;// refinement: union AABB for camera auto-fit / Reset (see notes)
};
```

Bones stored flat with `parentIdx` (root = -1) so a single linear pass computes globals later (Epic 3). The `skinned` flag at mesh level is what selects the static-mesh fallback path (D13 line 407, FR6) — this story always sets it `false`.

### §C — `LoadResult` / `LoadErrorCategory` (D6, Task 4)

```cpp
enum class LoadErrorCategory { Ok, FileNotFound, ParseFailed, UnsupportedFormat,
                               GpuUploadFailed, OutOfMemory, Unknown };
struct LoadResult {
    std::optional<Asset> asset;
    LoadErrorCategory    category = LoadErrorCategory::Ok;
    std::string          detail;   // human-readable, goes into the console line
};
LoadResult LoadAsset(const std::string& path);   // PRECONDITION: current GL context (uploads geometry)
```

Error propagation rule (architecture `:575`): public load entry points return `LoadResult`; light internal helpers may use `bool` + `std::string& out_error`. **No throwing across subsystem boundaries.**

### §D — `convertAssimpMatrix` (AR9/D3, Task 4)

The **single** assimp→GLM conversion boundary (architecture `:230`). assimp's `aiMatrix4x4` is row-major; GLM is column-major — transpose exactly once:

```cpp
inline glm::mat4 convertAssimpMatrix(const aiMatrix4x4& m) {
    return glm::mat4(m.a1, m.b1, m.c1, m.d1,
                     m.a2, m.b2, m.c2, m.d2,
                     m.a3, m.b3, m.c3, m.d3,
                     m.a4, m.b4, m.c4, m.d4);   // transpose: column k built from assimp row k
}
```

This is the **only** place a matrix crosses the assimp boundary (AR9 "exactly once"). Convention is pinned: **column-major, right-handed, Y-up, CCW front-facing** (D3). Do not flip handedness or winding. The existing renderer already enforces the matching GL state (`glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW); glDepthFunc(GL_LESS)`).

### §E — Boundary discipline (which TU may include what)

- **`asset_loader.cpp` is the only file that includes `<assimp/...>`** (architecture `:871`, D5) — contains all parse-exception risk at one site.
- Modern GL (≥1.5, via `gl_loader.h`) is called only in `renderer.cpp`, `gl_loader.cpp`, **and now `asset_loader.cpp`** (the loader performs GPU upload — `LoadErrorCategory::GpuUploadFailed` is the loader's own error category per D6, so the upload legitimately lives here). This is the one boundary extension this story introduces; record it in your File List / completion notes. Everything else consumes only `scene.h` types.
- All user feedback goes through `console_log.h` `LogInfo/LogWarn/LogError` → `[RAV] <level>:` single line (AR16, D7). **No message boxes, toasts, or dialogs, ever.**

### §F — Blinn-Phong shader (Task 5)

Evolve the current Lambert shader (`renderer.cpp:29-57`). The existing `u_normal` (mat3 = `transpose(inverse(mat3(model)))`) is correct under non-uniform scale — keep it. Add a world-position output and a half-vector specular term:

- **VS** in: `a_pos`(0) vec3, `a_normal`(1) vec3, `a_uv`(2) vec2. uniforms: `u_mvp` (mat4), `u_model` (mat4, for world pos), `u_normal` (mat3). out: `v_worldpos` (vec3), `v_normal` (vec3, world), `v_uv` (vec2).
- **FS** uniforms: `u_baseColor` (vec3), `u_specularColor` (vec3), `u_shininess` (float), `u_viewPos` (vec3). One hardcoded directional light (reuse the 1.2 direction `normalize(vec3(0.4,0.9,0.5))`). Compute `N=normalize(v_normal)`, `L`=light dir, `V=normalize(u_viewPos - v_worldpos)`, `H=normalize(L+V)`; `diff=max(dot(N,L),0)`; `spec=pow(max(dot(N,H),0), u_shininess)`; out `= u_baseColor*(0.15 + 0.85*diff) + u_specularColor*spec`. (Texture sampling `u_baseColor *= texture(...)` is added in Story 2.2 — leave a clean seam, don't add the sampler uniform yet.)
- `u_viewPos` = camera world position = `glm::vec3(glm::inverse(view_)[3])` — compute once when the framing camera is set in `SetAsset`.

### §G — Renderer integration seams (already designed for this)

The Epic 1 renderer was built anticipating this swap. `renderer.h:8-9`: *"Epic 2 swaps the test scene for a loaded Asset behind this same interface."* `renderer.h:46-49` `DrawRange`: *"Epic 2 grows this from two fixed meshes to a per-asset list of ranges."* The fixed `glm::lookAt` camera (FOV 50°, near 0.01, far 1000 — `renderer.cpp:60-62`, matches D14) is replaced by the AABB auto-fit in `SetAsset`. Real interactive camera (orbit/zoom/pan/reset) is **Story 2.4** — do not build it here; a static auto-fit framing satisfies AC1/AC3.

### Scope fences — what this story does NOT do

- **No texture upload / resolution** — Story 2.2 (FR17/FR18, AR14). `SceneMaterial::baseColor` stays unbound; shader uses `baseColorFactor`.
- **No multi-material specular *validation*** — Story 2.3 (FR16/FR20). But you **do** wire per-mesh/per-material `specularColor`/`shininess` uniforms now (the draw loop is per-mesh-per-material); 2.3 only adds the "matte vs glossy distinguishable" fixtures/proof.
- **No camera controls** — Story 2.4 (FR22–26). Auto-fit framing only.
- **No skeleton/skin/animation parsing** — Epic 3. `skinned=false`, `skeleton`/`animations` empty. Set `AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS=0` now anyway (Spike Finding 3).
- **No file-picker / drag-drop UI** — Epics 4/5. Model path comes from the `RAV_MODEL` env var for this story's validator gate.

### Testing standards

No automated test harness exists (architecture `:933-935`); validation is the **per-phase validator gate run by Antho in real Reaper** (AR19). Dev-side verification on this Linux/WSL box is limited to: `cmake -S . -B /tmp/rav_build` configures clean, and source greps. **All runtime ACs (visual correctness, orientation, no-crash, single-DLL, warning-free build) are Antho's Windows gate** — author `docs/PHASE1_VALIDATOR_GATE.md` (Task 7) as the checklist. Per project memory `feedback_trust_ingame_validation`: when Antho validates in-Reaper and it works, that IS the gate — don't demand re-validation. Collada is **best-effort against ≥1 public sample** (Khronos/assimp test `.dae`), not gated on a Demute corpus (sprint-change-proposal-2026-06-24).

### Project Structure Notes

- **New files:** `src/scene.h`, `src/asset_loader.h`, `src/asset_loader.cpp`, `docs/PHASE1_VALIDATOR_GATE.md`. **Modified:** `CMakeLists.txt` (assimp + new source), `src/gpu_resources.h` (`GpuImage`), `src/gl_loader.h` (uniform setters), `src/renderer.{h,cpp}` (Blinn-Phong + `SetAsset`), `src/viewer_window.cpp` (load wire-in). No Reaper API symbols added (`reaper_api.h` unchanged — loading is pure C++/GL).
- **Two deliberate D1 refinements** (architecture D1 is slightly incomplete for the static path) — implement and flag in completion notes:
  1. `SceneMaterial.baseColorFactor` (vec3): D1's `SceneMaterial` has only a `baseColor` *texture* and no diffuse RGB factor, but a static render with no texture (this story) and the 2.2 "missing-texture → flat base color" fallback both need a color. Source it from `AI_MATKEY_COLOR_DIFFUSE`.
  2. `Asset.aabbMin/aabbMax` (vec3): needed for camera auto-fit here and Reset-Camera-to-bounding-box in Story 2.4 (D14). The Spike computed the AABB at load — same approach.
- Naming conventions (architecture `:517-524`): Types/functions **PascalCase** (`Asset`, `LoadAsset`), locals/members **snake_case** (`index_count`, `model_root`), file-scope globals **`g_`+snake_case**, compile-time constants **`k`+PascalCase** (`kFovYDegrees`), file-internal symbols in **anonymous namespaces**. SPDX header + one-line purpose on every file. **Comment WHY only** (hidden constraint / SDK quirk), never WHAT; no story/commit IDs in comments.

### Previous Story Intelligence (Epic 1 + Spike 0)

- **AR18 no-throw boundary (Story 1.4):** an exception crossing into Reaper's message pump = host crash. `LoadAsset` must be no-throw (catch-all at the assimp boundary). It runs inside `StartRendering` → already within the `WM_CREATE` try/catch, but must not rely on that — degrade gracefully on its own.
- **RAII / context ordering (Story 1.2):** GPU handle destructors call `glDelete*` and **require a current context**. The Asset's `GpuBuffer`/`GpuImage` members must be released in `Renderer::Shutdown` / before context destruction — mirror the existing `StopRendering` order (`KillTimer → wglMakeCurrent → Shutdown → DestroyGLContext`, `viewer_window.cpp:268-279`).
- **GL function loading (Spike/1.2):** `opengl32.dll` exports only GL 1.1; everything ≥1.5 resolves via `wglGetProcAddress` against a current context — append to `RAV_GL_FUNCS`, never call modern GL directly. Legacy `wglCreateContext` yields a profile high enough for `#version 330` on the reference box (proven). If a shader ever fails to compile, the fix is a `wglCreateContextAttribsARB` 3.3-core context — flag it, don't pre-build it.
- **Pipeline state once, no per-frame alloc (1.2 review):** depth/cull/front-face set once in `Init`; matrices cached, rebuilt only on aspect change; zero heap allocation in `RenderFrame`. 10 findings in the 1.2 review centered on exactly this discipline — keep the draw loop allocation-free.
- **Windows `.bat` (project memory):** CRLF, flat `errorlevel` gotos, `cd /d "%~dp0"`, `pause`, close Reaper before copying the DLL (it's locked while Reaper runs). The untracked `build_spike.bat`/`build_forcefail.bat` must stay out of this story's commits. First build with assimp will be much slower — set expectations in `build.bat`.
- **Spike 0 reuse (unmerged branch `spike/0-1-feasibility`):** the spike proved the entire load→upload→draw path (~62 fps docked, GPU-skinned Mixamo FBX). Retrieve its battle-tested loader/scene/renderer as **reference templates** (build production fresh — the spike code is disposable): `git show spike/0-1-feasibility:src/{spike_loader.cpp,spike_scene.h,spike_renderer.cpp,spike_gl.h}` and its `CMakeLists.txt`. Reuse: the `convertAssimpMatrix` transpose, the importer config + post-process flags, the AABB-during-vertex-pass, the VBO/IBO upload + VAO setup, the assimp FetchContent recipe. **Ignore** its FBO/`RenderToPixels`/readback path entirely (Spike Finding 1 killed ReaImGui at ~30 fps — production is the direct-render docked window already shipped).

### Latest tech notes

- **assimp v6.0.5** (pinned, Spike-proven). Security advisories for the v6.0.x line concentrate in parsers we're **not** compiling (the AR5 narrowing to glTF/FBX/Collada is partly an attack-surface reduction). Keep `ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF`.
- **glTF time-unit wrinkle (Epic 3, note for awareness):** `aiAnimation::mTicksPerSecond` is often 0 for glTF → guard `tps = (x != 0.0) ? x : 25.0` when you reach animation. Not exercised in this static story.
- **GLM 1.0.3** already in the build via FetchContent (`CMakeLists.txt:22-27`); `glm::quat` (used in `KeyframeR`) needs `<glm/gtc/quaternion.hpp>`.

### References

- Story + ACs: [epics.md#Story 2.1](../planning-artifacts/epics.md) (lines 340–354); Epic 2 intro (line 336–338); FR map (lines 171–196).
- D1 Asset model: [architecture.md] lines 188–209. D2/AR8 RAII: 211–220. D3/AR9 `convertAssimpMatrix`: 222–233. D5 exceptions: 256–260. D6 `LoadResult`: 262–276. D12 render pipeline: 376–394. D13 static-mesh fallback: 407. D14 camera/FOV: 410–422. D15 assimp CMake: 426–445. Boundary rules: 871–873; raw-GL amendment: 1146–1148.
- ARs: [epics.md] AR5 (138), AR7 (143), AR8 (144), AR9 (145), AR13 (152), AR14 (153), AR16 (155), AR17 (156), AR18 (157), AR19 (161), AR21 (163).
- Spec Change Logs: [architecture.md] 1126–1148 (Spike 0 / raw GL); [sprint-change-proposal-2026-06-24.md] (Collada `.dae`).
- Live code seams: `renderer.{h,cpp}` (test scene to replace), `gpu_resources.h:45-51` (RAII aliases), `gl_loader.h:51-76` (X-macro table), `viewer_window.cpp:197-263` (`StartRendering` wire-in), `console_log.h` (`[RAV]` funnel), `CMakeLists.txt:22-40` (FetchContent + link), `cmake/ReaperPlugin.cmake` (`/W3 /permissive-`, `reaper_animviewer.dll`).

## Dev Agent Record

### Agent Model Used

claude-opus-4-8 (1M context) — BMAD dev-story workflow.

### Debug Log References

- `cmake -S . -B /tmp/rav_build` — configures clean. NOTE: this Linux/WSL dev box
  hits the `else()` (non-Windows) branch, which stubs the target and does **not**
  exercise the `if(WIN32)` assimp FetchContent block. The assimp clone/compile,
  the actual C++ compile, `/W3` warning-freeness, single-DLL output, and all
  runtime/visual ACs are confirmed by Antho's Windows gate (`docs/PHASE1_VALIDATOR_GATE.md`).
- Source-consistency greps: dead Epic-1 symbols (`AppendFace`, `ground_draw_`,
  `cube_draw_`, `DrawRange`, `struct Vertex`) fully removed from the renderer; every
  modern-GL call in `asset_loader.cpp`/`renderer.cpp` is either in the `RAV_GL_FUNCS`
  table or GL 1.1 core (`glGetError`, `glDrawElements`); `asset_loader.cpp` is the
  sole TU including `<assimp/...>`.

### Completion Notes List

- **All 7 tasks / 8 ACs implemented.** Runtime ACs (1–6: visual correctness,
  orientation as-authored, no-anim static path, `.dae`, corrupt-file survival) plus
  the implied invariants (warning-free `/W3`, single DLL, no GL leak) are validated
  by the Windows gate — they cannot be exercised on the Linux dev box.
- **assimp 6.0.5** vendored via FetchContent, narrowed to glTF/FBX/Collada importers,
  `BUILD_SHARED_LIBS OFF` → single static-linked DLL (D15/D17). Added
  `ASSIMP_WARNINGS_AS_ERRORS OFF` beyond the story's §A block: assimp's own tree
  warns under MSVC and would fail its build otherwise; our extension's `/W3` is
  unaffected because assimp headers are a SYSTEM include.
- **Two deliberate D1 refinements** implemented as flagged in the spec:
  (1) `SceneMaterial.baseColorFactor` (vec3) sourced from `AI_MATKEY_COLOR_DIFFUSE`
  — needed for a textureless static render and the 2.2 missing-texture fallback;
  (2) `Asset.aabbMin/aabbMax` union AABB for camera auto-fit / Reset (D14).
- **Boundary extension recorded (Dev Notes §E):** `asset_loader.cpp` is now a third
  TU permitted to call modern GL (it owns the geometry upload → `GpuUploadFailed`).
- **Static-path baking:** node world transforms are baked into vertex positions via
  the single `ConvertAssimpMatrix` transpose boundary (AR9); one `SceneMesh` per
  (node, referenced-mesh) pair so instancing keeps distinct transforms and
  per-material grouping. `modelRoot` left identity — non-canonical files render
  tilted/scaled as-authored (AR13), no `MakeLeftHanded`/`FlipWindingOrder`/
  `PreTransformVertices`.
- **No-throw boundary (AR18):** the whole `LoadAsset` body is wrapped — `bad_alloc`→
  `OutOfMemory`, other `std::exception`→`ParseFailed`, `...`→`Unknown`. Plus a
  `FileExists` pre-check → `FileNotFound`, and empty-mesh/empty-geometry guards →
  `UnsupportedFormat`. A defensive synthetic default material guarantees the render
  hot path can never index `materials` out of bounds.
- **Hot-path discipline preserved (D2):** the per-frame draw loop does no heap
  allocation; pipeline state is set once in `Init`; only attribs 0/1/2 are enabled
  (bone attribs 3/4 reserved for Epic 3 so the layout never churns).
- **Model selection — PO-approved deviation from Task 6 / the Epic-5 scope fence:**
  the story specified a `RAV_MODEL` env var (deliberately avoiding any file-picker
  until Epic 5). Antho (PO) found the env-var/command-line workflow unusable for a
  non-dev validator and approved replacing it, mid-story, with a **minimal native
  Windows "Open file" dialog** (`GetOpenFileNameW`, links `comdlg32`). It is shown in
  `OpenViewerWindow` *before* the window is created (a modal dialog inside `WM_CREATE`
  would be unsafe) and the picked path is handed to `StartRendering` via a global.
  This is NOT the Epic 5 browser (no filter panel, no preview, no item path-binding)
  — just the smallest click-based way to choose a file. Cancel → idle viewport
  (`no file selected`); success → `loaded <path> (N meshes)`; failure → one
  `[RAV] error: load failed [category]: detail` line, viewport stays alive (AR17).
  The implied invariant "loader is no-throw / Reaper never crashes" is unchanged.
- **Scope fences respected:** no texture upload (2.2), no camera controls (2.4), no
  skeleton/animation parsing (Epic 3) — though `AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS=0`
  is set now (Spike Finding 3).

### File List

**New:**
- `src/scene.h` — D1 `Asset` scene model (header-only)
- `src/asset_loader.h` — `LoadAsset` / `LoadResult` / `LoadErrorCategory` (D6)
- `src/asset_loader.cpp` — assimp boundary + GPU upload (sole `<assimp/...>` TU)
- `docs/PHASE1_VALIDATOR_GATE.md` — Phase 1 gate (AR19), seeded with Story 2.1 checks

**Modified:**
- `CMakeLists.txt` — assimp 6.0.5 FetchContent (3 importers, static), `asset_loader.cpp` source, SYSTEM includes + link
- `src/gpu_resources.h` — `GpuImage` / `TextureDeleter` RAII handle
- `src/gl_loader.h` — `glUniform3fv` / `glUniform1f` / `glUniform1i` table rows + routing
- `src/renderer.h` — `SetAsset(Asset&&)`, held `Asset`, Blinn-Phong uniform locations, per-asset near/far
- `src/renderer.cpp` — Blinn-Phong shader, AABB auto-fit camera, per-mesh asset draw loop; removed the test cube/ground
- `src/viewer_window.cpp` — `RAV_MODEL` load wire-in inside `StartRendering` (post-`Init`, context current)

## Change Log

| Date | Change |
|---|---|
| 2026-06-24 | Story 2.1 implemented — static glTF/GLB/Collada load + Blinn-Phong asset render. assimp 6.0.5 vendored (glTF/FBX/Collada, static, single DLL); new `Asset` model (D1), `asset_loader` boundary (D5/D6), `GpuImage` handle; renderer upgraded from test cube to AABB-framed asset draw; `RAV_MODEL` load wire-in; Phase 1 validator gate authored. Status → review. |
| 2026-06-24 | PO-approved deviation: replaced the `RAV_MODEL` env-var model selection with a native Windows "Open file" dialog (`GetOpenFileNameW`, +`comdlg32`) — Antho found the command-line workflow unusable for a non-dev validator. Validator gate updated to the dialog method. Not the Epic 5 browser; minimal picker only. |
| 2026-06-24 | Code review (BMAD adversarial, 3 layers): 9 findings applied — **CRITICAL** `materialIdx` out-of-bounds clamp at load; NaN/Inf vertex → AABB poison guard (loader + `SetAsset`); `glGetError` drain/widen in `UploadMesh`; `node->mMeshes` bounds guard; relaxed `Init` uniform check (only `u_mvp` fatal); `cached_aspect_` sentinel `0.0f`→`-1.0f`; `scene.h` default member initializers; Unicode model paths now carried as UTF-8 (`CP_UTF8` + `std::filesystem::u8path`, validator-gate row 9 added); `RAV_MODEL` doc/comment drift scrubbed. 8 findings dismissed as noise. **Patches are source-only — not yet compiled/run on Windows.** They only alter malformed-input/cold paths + the path encoding (ASCII unaffected), so they don't invalidate the prior gate PASS, but warrant a quick happy-path + Unicode (row 9) re-check at the next Windows build. |
