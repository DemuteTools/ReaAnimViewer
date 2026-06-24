# Story 2.2: Diffuse texture resolution across GLB-embedded and glTF-sibling sources

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want textures applied to the model from both packaging styles,
so that I can see surface detail regardless of how the asset was exported.

This is the **second story of Epic 2 (Phase 1)**. Story 2.1 stood up the loader → `Asset` → Blinn-Phong render path with materials carrying only a flat `baseColorFactor`; the `SceneMaterial::baseColor` texture handle was deliberately left **unbound (0)** and the fragment shader was built with a clean seam for `u_baseColor *= texture(...)`. This story fills that seam: it resolves the diffuse texture for each material through **one unified path** (AR14) that handles the two glTF packaging variants — **GLB-embedded** (`FR17`) and **multi-file glTF with sibling image files** (`FR18`) — decodes the image bytes, uploads a GPU texture (RAII `GpuImage`, D2/AR8), and samples it in the shader. A texture that can't be resolved degrades to the flat base color with a console diagnostic, never failing the whole load (AR16/AR17). FBX-embedded textures (FR19) are the **same** `GetEmbeddedTexture` branch but are explicitly **Epic 6** scope — the unified path is built to absorb them with no new code, but no FBX texture fixture is gated here.

## Acceptance Criteria

From [epics.md#Story 2.2](../planning-artifacts/epics.md) (lines 356–367), verbatim BDD:

**Given** a GLB with embedded textures and a multi-file glTF with sibling image files
**When** the loader resolves textures through the unified texture-resolution path (AR14) and uploads them to GPU images (RAII, D2)
1. **Then** the GLB-embedded textures display correctly (FR17)
2. **And** the glTF-sibling textures display correctly (FR18)
3. **And** a missing/unresolvable texture produces a console diagnostic (AR16) and falls back to a flat base color rather than failing the whole load.

Implied, non-negotiable (system must stay working end-to-end — requirements even though not in the AC text):

4. The deliverable remains a **single `reaper_animviewer.dll`** with no side-DLL (D15/D17), and the build stays **warning-free under `/W3 /permissive-`** for our own sources (NFR-R5). The new image decoder is header-only and included as a **SYSTEM** include like GLM/assimp.
5. A **textureless** material (no diffuse texture in the file) and a **non-canonical / Collada / no-animation** file all still render exactly as in Story 2.1 — the flat-`baseColorFactor` path is preserved as the fallback, so Story 2.1's passing gate rows do not regress.
6. **No GL or CPU resource leaks**: each decoded image's CPU pixel buffer is freed after upload, and every uploaded texture is owned by a `GpuImage` (freed on `Asset` teardown / viewport close-reopen, D2/AR8). The loader stays **no-throw** across its boundary (AR18) — a decode/IO failure returns through the existing diagnostic path, never an exception into Reaper.
7. Texture work happens **only in `asset_loader.cpp`** (the GL-upload boundary already sanctioned in 2.1) and **`renderer.cpp`** (the sampler bind). No new TU gains assimp or modern-GL access.

## Tasks / Subtasks

- [x] **Task 1 — Vendor an image decoder (`stb_image`) into the build (AC: 1, 2, 4)**
  - [x] assimp does **not** decode embedded compressed textures — its glTF importer leaves an embedded PNG/JPG as raw file bytes in `aiTexture` (`mHeight == 0`, bytes in `pcData`, size `mWidth`) and exposes **no public decode API**. A decoder is therefore a genuine new dependency. Use **`stb_image`** (single public-domain header, `nothings/stb`) — the standard, dependency-free choice and what assimp itself uses internally.
  - [x] Add it via `FetchContent` mirroring the GLM block (`CMakeLists.txt:22-27`), **inside** the existing `if(WIN32)` section, pinned by **commit SHA** (stb is unversioned — no tags). Add `stb` to the `FetchContent_MakeAvailable(glm assimp)` call and add `"${stb_SOURCE_DIR}"` to the **SYSTEM** `target_include_directories` list (so stb's header can't trip `/W3 /permissive-`, NFR-R5). Exact block in Dev Notes §A.
  - [x] **No new link library and no new source file** — `stb_image.h` is compiled into `asset_loader.cpp` via `#define STB_IMAGE_IMPLEMENTATION` in that one TU (Dev Notes §C). Single-DLL invariant (D15/D17) is unaffected: header-only, statically compiled in.
  - [x] Record the pin in `extern/VENDORED.md`: new "stb_image" section with upstream URL, public-domain/MIT license note, pinned commit SHA, and "FetchContent, not in-tree" delivery (mirror the GLM section). Introduced by Story 2.2.
  - [x] Confirm `cmake -S . -B /tmp/rav_build` still configures clean (the Linux branch stubs the target — the actual fetch/compile is the Windows gate, same as 2.1; note the extra clone in `build.bat` expectations is negligible — stb is one small header).

- [x] **Task 2 — Extend the GL loader with the texture entry points (AC: 1, 2)**
  - [x] In `src/gl_loader.h` `RAV_GL_FUNCS`, append two rows: `X(void, glActiveTexture, (GLenum))` (GL 1.3) and `X(void, glGenerateMipmap, (GLenum))` (GL 3.0). Add the matching `#define glActiveTexture rav_glActiveTexture` / `#define glGenerateMipmap rav_glGenerateMipmap` lines in the routing block. The X-macro auto-generates the decl/def/load — **no edit to `gl_loader.cpp`** is needed.
  - [x] `glGenTextures` / `glBindTexture` / `glTexImage2D` / `glTexParameteri` / `glDeleteTextures` / `glPixelStorei` are **GL 1.1 core** (already in `<gl/GL.h>`) — do **not** add table rows for them (the `TextureDeleter` in `gpu_resources.h` already calls `glDeleteTextures` directly, confirming this).
  - [x] Add the one missing enum to the `#ifndef GL_FRAGMENT_SHADER` block in `gl_loader.h`: `#define GL_TEXTURE0 0x84C0` (GL 1.3, needed by `glActiveTexture`). Everything else the upload needs — `GL_TEXTURE_2D`, `GL_RGB`, `GL_RGBA`, `GL_UNSIGNED_BYTE`, `GL_TEXTURE_MIN_FILTER`/`MAG_FILTER`, `GL_TEXTURE_WRAP_S`/`T`, `GL_LINEAR`, `GL_LINEAR_MIPMAP_LINEAR`, `GL_REPEAT`, `GL_UNPACK_ALIGNMENT` — is GL 1.1 core, already defined.

- [x] **Task 3 — Build the unified texture-resolution + decode + upload path in `asset_loader.cpp` (AC: 1, 2, 3, 6)**
  - [x] Add a single helper `GpuImage ResolveAndUploadDiffuse(const aiScene* scene, const aiMaterial* mat, const std::filesystem::path& model_dir, const std::string& model_path)` in the anonymous namespace. This is the **one unified funnel** (AR14): every packaging variant converges here, returns a ready `GpuImage` (or an empty/unbound handle on failure). Decode logic in Dev Notes §D.
  - [x] **Get the diffuse texture reference:** `aiString tex_path; if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &tex_path) != AI_SUCCESS) return {};` — no diffuse texture → unbound handle → shader uses flat `baseColorFactor` (this is the textureless path, AC5, **not** a failure, **no** diagnostic).
  - [x] **Branch via `scene->GetEmbeddedTexture(tex_path.C_Str())`** — this single call resolves both the GLB `*0`-style index reference **and** (later) FBX embedded textures, which is exactly why it is the unified seam (AR14):
    - **Embedded (`aiTexture*` non-null):** if `tex->mHeight == 0` the data is a **compressed** file image (`pcData` = `mWidth` bytes of PNG/JPG) → `stbi_load_from_memory`. If `mHeight != 0` it is **uncompressed** `aiTexel` BGRA (`mWidth × mHeight`) → swizzle BGRA→RGBA into a `std::vector<unsigned char>` (no stb needed). (FR17.)
    - **External sibling (`GetEmbeddedTexture` returns null):** resolve `tex_path` relative to `model_dir` (`model_dir / std::filesystem::u8path(tex_path.C_Str())`), then `stbi_load` the file. (FR18.)
  - [x] **Upload** the RGBA pixels through one `UploadTexture(const unsigned char* rgba, int w, int h)` helper → `glGenTextures` → `glBindTexture(GL_TEXTURE_2D)` → `glPixelStorei(GL_UNPACK_ALIGNMENT, 1)` → `glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba)` → wrap `GL_REPEAT` (glTF default), min `GL_LINEAR_MIPMAP_LINEAR` / mag `GL_LINEAR`, `glGenerateMipmap(GL_TEXTURE_2D)`. Always decode/upload as **4-channel RGBA** (`stbi_load(..., 4)`) so the GL format is uniform regardless of source channel count. Free the CPU buffer (`stbi_image_free` or let the `std::vector` drop) immediately after upload (AC6).
  - [x] **Call site:** in `LoadAsset`, the material loop currently does `asset.materials.push_back(ConvertMaterial(...))`. After building each `SceneMaterial`, set `material.baseColor = ResolveAndUploadDiffuse(scene, scene->mMaterials[mi], model_dir, path);`. Compute `model_dir` once from `std::filesystem::u8path(path).parent_path()`. Keep `ConvertMaterial` for the factors; texture resolution is the new step beside it.
  - [x] Texture upload sits **inside** the existing `LoadAsset` try/catch, so a `std::bad_alloc` from a huge image is already caught as `OutOfMemory`. But a per-texture **decode/IO failure must NOT abort the load** — `ResolveAndUploadDiffuse` returns an empty handle and the load continues (Task 5). Only catastrophic allocation failure propagates to the catch.
  - [x] **Dedup is out of scope** — if two materials reference the same image, decode+upload it twice. Per-material `GpuImage` ownership matches D1's `SceneMaterial{ GpuImage baseColor; }` exactly and avoids restructuring `scene.h`; material count is ≤4 (NFR), so the duplication is negligible. (Noted in Scope Fences.)

- [x] **Task 4 — Sample the base-color texture through the Blinn-Phong shader + draw loop (AC: 1, 2)**
  - [x] **Fragment shader** (`renderer.cpp:40-60`): add `uniform sampler2D u_baseColorTex;` and `uniform int u_hasTexture;`. Replace the `u_baseColor` term so the texture **modulates** the flat factor: compute `vec3 base = u_baseColor; if (u_hasTexture != 0) base *= texture(u_baseColorTex, v_uv).rgb;` then use `base` where `u_baseColor` was in the final color. This preserves the textureless path (AC5): `u_hasTexture == 0` → unchanged Story-2.1 output. `v_uv` is already an FS input — no VS change.
  - [x] **Init** (`renderer.cpp:Init`, `renderer.h` members): cache `u_base_color_tex_ = glGetUniformLocation(..., "u_baseColorTex")` and `u_has_texture_ = glGetUniformLocation(..., "u_hasTexture")`. Keep the 2.1 relaxed rule — only `u_mvp` is fatal on `-1`; a driver may legitimately optimize these out, and `glUniform*(-1,...)` is a no-op. Bind the sampler to texture unit 0 **once** after `glUseProgram`: `glUniform1i(u_base_color_tex_, 0)` (set it in `Init` after a `glUseProgram(program_.get())`, or once per frame before the mesh loop — Dev Notes §E).
  - [x] **Draw loop** (`renderer.cpp:RenderFrame`, per-mesh): after binding the material factors, bind its texture: `const GLuint tex = mat.baseColor.get(); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); glUniform1i(u_has_texture_, tex != 0 ? 1 : 0);`. A `0` handle (textureless or failed-resolve material) → `u_hasTexture = 0` → flat color. Keep the loop **allocation-free** (D2 hot-path discipline — no per-frame heap, established by the 1.2 review).
  - [x] `Shutdown()` needs **no change** — the per-material `GpuImage` lives inside `asset_.materials`, and `asset_ = Asset()` already releases the whole Asset's GL handles while the context is current (the texture handles ride along with the existing VBO/IBO teardown).

- [x] **Task 5 — Missing / unresolvable texture → console diagnostic + flat fallback (AC: 3)**
  - [x] When a material **declares** a diffuse texture (`GetTexture == AI_SUCCESS`) but it **cannot be resolved** — embedded decode fails, sibling file missing, or `stbi_load` returns null — emit **exactly one** `LogWarn` line naming the texture and the reason, then return an empty `GpuImage` so the material renders with its flat `baseColorFactor`. The whole-asset load **succeeds** (AC3). A material with *no* declared texture is silent (not a failure).
  - [x] This adds `#include "console_log.h"` to `asset_loader.cpp` — a **deliberate, recorded boundary extension**: the loader is the only site that knows the specific texture path/reason, so per-texture user feedback (AR16) belongs here, exactly as `GpuUploadFailed` diagnostics already live at this boundary (Dev Notes §E of Story 2.1). Use the `[RAV] warn:` level (D7) — a missing texture is a degradation, not an error (the load still produces a usable render). Suggested text: `LogWarn("texture unresolved for material %u (%s) — using flat color", mi, reason);`.
  - [x] Do **not** spam: one line per unresolved texture, not per pixel/frame. The diagnostic is emitted at **load time** only.

- [x] **Task 6 — Extend the Phase 1 validator gate with Story 2.2 rows (AC: 1, 2, 3)**
  - [x] Append a **"## 7. Story 2.2 — acceptance checks"** section to `docs/PHASE1_VALIDATOR_GATE.md` (the file already announces "Stories 2.2 (textures) … append their own", `PHASE1_VALIDATOR_GATE.md:6`). Rows:
    - GLB-embedded texture displays (FR17) — fixture `BoxTextured.glb` or `Duck.glb` (Khronos) shows the texture, not a flat grey.
    - glTF-sibling texture displays (FR18) — multi-file `BoxTextured/glTF/` (with sibling `.png`) shows the same texture.
    - Missing-texture fallback (AC3) — rename/remove a sibling `.png` (or point at a glTF whose image is absent): console logs **one** `[RAV] warn: texture unresolved …` line, the model still renders in **flat base color**, the load does **not** fail, Reaper survives.
    - No-regression — a **textureless** static file (Story 2.1's `Box`/`Duck` without texture) and a `.dae` still render exactly as before.
    - Single-DLL still holds — no `assimp*.dll`/decoder DLL beside `reaper_animviewer.dll` (stb is header-only).
  - [x] Add a **UV-orientation note** to that section: if a textured model appears **vertically mirrored**, the fix is a single flip toggle (`stbi_set_flip_vertically_on_load(true)` **or** `aiProcess_FlipUVs`) — flag it for a one-line follow-up rather than guessing the convention blind (Dev Notes §D covers the expected default: glTF top-left UVs + top-row-first upload are self-consistent, so **no flip** is the starting position).

## Dev Notes

### Critical orientation — same as Story 2.1: trust the live code, not the stale architecture names

`architecture.md` predates the AR21 rename and the Spike-0 backend pivot. Dead names that must **not** reappear (confirmed against the live tree): `fbxav`/`[FBXAV]` → **`rav`/`[RAV]`**; sokol/`sg_*` → **raw OpenGL**; FBO/`ImGui::Image()`/ReaImGui → **direct render into the docked GL window**; `viewer_panel.*`/`log.*` → **`viewer_window.*`/`console_log.*`**. `src/` is **flat**. The architecture's `SceneMaterial { GpuImage baseColor; ... }` (line 195) and the "texture resolution funnels through one unified path across 3 packaging variants" rule (AR14, line 153; architecture line 91) **are** authoritative — this story implements exactly that.

The Spike-0 prototype loaded **UVs but no textures** (`git show spike/0-1-feasibility:src/spike_loader.cpp` reads `mTextureCoords[0]` but never decodes/uploads an image) — so there is **no reference texture implementation to lift**. This story designs the unified path fresh from the assimp API. The seam, however, was pre-built in 2.1: `SceneMaterial::baseColor` (the `GpuImage` handle) and `gpu_resources.h`'s `TextureDeleter`/`GpuImage` alias already exist and are unused until now.

### §A — `stb_image` CMake block (Task 1)

Add inside the `if(WIN32)` section. Pin by commit SHA (stb has no release tags). Append `stb` to the existing `FetchContent_MakeAvailable` and to the SYSTEM include list:

```cmake
# stb_image (image decoder — header-only, public domain). assimp leaves embedded
# glTF textures COMPRESSED (aiTexture::mHeight==0, raw PNG/JPG bytes) and exposes no
# decode API, so Story 2.2 needs its own decoder. FetchContent (not in-tree) for the
# same drvfs/WSL reason as GLM/assimp; pinned by commit (stb is untagged). The
# implementation is compiled into asset_loader.cpp via STB_IMAGE_IMPLEMENTATION.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        <PIN_A_RECENT_COMMIT_SHA>   # record the exact SHA in extern/VENDORED.md
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(glm assimp stb)     # was: (glm assimp)

target_include_directories(animviewer SYSTEM PRIVATE
    "${glm_SOURCE_DIR}"
    "${assimp_SOURCE_DIR}/include"
    "${assimp_BINARY_DIR}/include"
    "${stb_SOURCE_DIR}")                        # new
```

`GIT_SHALLOW TRUE` with a commit SHA can fail on some Git versions (shallow fetch of an arbitrary SHA); if the configure errors on the stb fetch, drop `GIT_SHALLOW` for stb only. No change to `target_link_libraries` (header-only).

### §B — GL loader additions (Task 2)

Two new `RAV_GL_FUNCS` rows + two routing `#define`s + one enum. The X-macro in `gl_loader.cpp` regenerates the decl/def/runtime-load automatically — **only `gl_loader.h` changes**:

```c
// in RAV_GL_FUNCS(X), append:
    X(void,   glActiveTexture, (GLenum)) \
    X(void,   glGenerateMipmap, (GLenum))
// in the routing block, append:
#define glActiveTexture  rav_glActiveTexture
#define glGenerateMipmap rav_glGenerateMipmap
// in the #ifndef GL_FRAGMENT_SHADER enum block, add:
#define GL_TEXTURE0  0x84C0
```

All other texture GL — `glGenTextures`, `glBindTexture`, `glTexImage2D`, `glTexParameteri`, `glDeleteTextures`, `glPixelStorei`, and the `GL_TEXTURE_2D`/`GL_RGBA`/`GL_LINEAR_MIPMAP_LINEAR`/`GL_REPEAT`/`GL_UNPACK_ALIGNMENT` enums — is GL 1.1, already in `<gl/GL.h>` (the existing `TextureDeleter::operator()` calling `glDeleteTextures(1,&h)` proves it compiles without a table row).

### §C — stb_image inclusion (Task 1/3, asset_loader.cpp only)

`asset_loader.cpp` is already the one TU allowed to pull heavy third-party headers (it owns `<assimp/...>`). Add stb at the top, implementation macro in this TU only, narrowed to the formats glTF/Collada actually ship to reduce code/attack surface (mirrors the AR5 importer narrowing):

```cpp
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_NO_FAILURE_STRINGS   // optional: smaller, we log our own reason
#include <stb_image.h>
```

If `/W3` flags anything inside `stb_image.h` despite the SYSTEM include (MSVC occasionally warns on macro-expanded implementation code even in SYSTEM headers), the fallback is to wrap the include in `#pragma warning(push)` / `#pragma warning(disable: ...)` / `#pragma warning(pop)` — flag it; not expected.

### §D — The unified resolution path (Task 3) — assimp texture API specifics

The decode logic, in the one `ResolveAndUploadDiffuse` funnel (AR14):

```cpp
aiString tex_path;
if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &tex_path) != AI_SUCCESS)
    return {};                                   // no texture declared — flat path, silent

if (const aiTexture* t = scene->GetEmbeddedTexture(tex_path.C_Str())) {
    if (t->mHeight == 0) {
        // compressed: pcData is mWidth bytes of a PNG/JPG file
        int w, h, n;
        unsigned char* px = stbi_load_from_memory(
            reinterpret_cast<const unsigned char*>(t->pcData),
            static_cast<int>(t->mWidth), &w, &h, &n, 4);   // force RGBA
        ... upload(px, w, h); stbi_image_free(px); ...
    } else {
        // uncompressed: mWidth*mHeight aiTexel, stored B,G,R,A — swizzle to RGBA
        std::vector<unsigned char> rgba(size_t(t->mWidth) * t->mHeight * 4);
        for (...) { rgba[i*4+0]=texel.r; rgba[i*4+1]=texel.g; rgba[i*4+2]=texel.b; rgba[i*4+3]=texel.a; }
        ... upload(rgba.data(), t->mWidth, t->mHeight); ...
    }
} else {
    // external sibling file, path relative to the model
    std::filesystem::path file = model_dir / std::filesystem::u8path(tex_path.C_Str());
    int w, h, n;
    unsigned char* px = stbi_load(file.u8string().c_str(), &w, &h, &n, 4);
    ... if (!px) { LogWarn(...); return {}; }  upload(px,w,h); stbi_image_free(px); ...
}
```

- **`aiTexel` channel order is `b,g,r,a`** (assimp packs it BGRA) — swizzle to RGBA on the uncompressed branch. Compressed and sibling go through `stbi_load*` which already yields RGBA.
- **Force 4 channels** (`stbi_load(..., 4)`) so `glTexImage2D` is always `GL_RGBA` / `GL_UNSIGNED_BYTE` — uniform upload regardless of whether the source was RGB or grayscale.
- **`GL_UNPACK_ALIGNMENT = 1`** before `glTexImage2D`: RGBA rows are 4-byte aligned anyway, but set it defensively (cheap, and correct if a future path uploads RGB).
- **UV flip — start with NO flip.** glTF defines UV origin top-left; an image uploaded top-row-first into GL makes glTF UV `(0,0)`=top-left sample the first-uploaded row, which is self-consistent → no flip needed. `stbi_set_flip_vertically_on_load(false)` is the default — leave it. **If** the validator sees a vertically mirrored texture (Task 6 note), the one-line fix is `stbi_set_flip_vertically_on_load(true)` (and an equivalent flip for the uncompressed swizzle + a decision on whether to also flip the embedded-uncompressed branch). Documented as a gate fallback because it cannot be confirmed on the Linux dev box.
- **Sibling path encoding:** `tex_path` from assimp's glTF importer is already URI-decoded for the common cases; percent-encoded or absolute URIs are an edge — if `stbi_load` returns null, the Task-5 diagnostic fires and the model still renders. Carry the path as UTF-8 via `u8path`/`u8string` (consistent with 2.1's `FileExists` Unicode fix).

### §E — Renderer sampler wiring (Task 4)

- The sampler `u_baseColorTex` is bound to **texture unit 0** once — set `glUniform1i(u_base_color_tex_, 0)` after `glUseProgram` (it persists in program state; doing it once per frame before the mesh loop is also fine and avoids an Init-time `glUseProgram`). Per-mesh, only the **bound texture object** and `u_hasTexture` change.
- Final FS color becomes `vec3 base = u_baseColor; if (u_hasTexture != 0) base *= texture(u_baseColorTex, v_uv).rgb;` then `frag = vec4(base*(0.15+0.85*diff) + u_specularColor*spec, 1.0);`. The `0.15` ambient + `0.85` diffuse and the specular term are unchanged from 2.1 — only the diffuse albedo source gains the texture modulation. (Texture color is treated as **linear** for MVP — no sRGB decode; matches the spike and 2.1's direct-color shading. sRGB-correct sampling is a possible Epic-6 polish, not gated here.)
- Keep the draw loop allocation-free (D2). The texture bind (`glActiveTexture`+`glBindTexture`+`glUniform1i`) is three cheap calls per mesh, no heap.

### §F — Boundary discipline (which TU may include what) — unchanged shape, two recorded additions

Per Story 2.1 Dev Notes §E, the boundary rules are: `asset_loader.cpp` is the only `<assimp/...>` TU and (since 2.1) a sanctioned modern-GL caller for uploads. This story makes **two recorded, in-character extensions**, both inside that same boundary:
1. `asset_loader.cpp` now also `#include`s `<stb_image.h>` (its second heavy third-party header, alongside assimp) and `"console_log.h"` (to emit the per-texture `LogWarn` — the loader is the only site that knows which texture failed, AR16). Record both in the completion notes / File List.
2. `renderer.cpp` gains texture-binding GL calls — it is already a sanctioned modern-GL TU, so this is no new boundary, just new calls.

Everything else still consumes only `scene.h` types. No Reaper API symbols added (`reaper_api.h` unchanged).

### Scope fences — what this story does NOT do

- **No FBX-embedded textures (FR19)** — Epic 6, Story 6.5. The `GetEmbeddedTexture` branch will absorb them with **no new code** (that is the whole point of the unified AR14 path), but **no FBX texture fixture is gated here** and no FBX-specific handling is added.
- **No multi-material specular *proof*** — Story 2.3 (FR16/FR20). The per-mesh/per-material draw loop already iterates materials (built in 2.1); this story adds the texture per material but does not author the "matte vs glossy distinguishable" fixtures.
- **No normal/metallic/roughness/emissive/occlusion maps** — diffuse (base color) **only** (FR17/FR18 are diffuse). Other PBR channels are out of MVP scope entirely.
- **No texture cache / dedup across materials or assets** — D2/AR8 accept per-item VRAM duplication; within-asset, re-upload a shared image (≤4 materials, negligible). A shared cache is a delimited post-MVP refactor (architecture line 220).
- **No camera controls** — Story 2.4. Auto-fit framing from 2.1 stands.
- **No sampler-state fidelity** (per-texture wrap/filter from the glTF sampler block) — use sensible defaults (`GL_REPEAT`, trilinear). glTF sampler honoring is not required by FR17/FR18.

### Testing standards

No automated test harness exists (architecture line 933-935); validation is the **per-phase validator gate run by Antho in real Reaper** (AR19). Dev-side verification on this Linux/WSL box is limited to `cmake -S . -B /tmp/rav_build` configuring clean (the `else()` branch stubs the Windows target — the stb fetch, the C++ compile, `/W3` cleanliness, single-DLL output, and **all** visual/texture ACs are confirmed only on Antho's Windows gate) plus source greps. Per project memory `feedback_trust_ingame_validation`: when Antho validates in-Reaper and it works, that IS the gate. Textures specifically need a **visual** check (correct image, right orientation, right surface) that only the Windows gate provides — Task 6 authors those rows. Khronos **glTF-Sample-Assets** (`BoxTextured`, `Duck`) cover both the embedded (`.glb`) and sibling (`.gltf`+`.png`) variants in one well-known fixture pair.

### Project Structure Notes

- **New files:** none (stb is FetchContent, compiled into an existing TU). **Modified:**
  - `CMakeLists.txt` — stb `FetchContent` + SYSTEM include (Task 1)
  - `extern/VENDORED.md` — stb_image pin section (Task 1)
  - `src/gl_loader.h` — `glActiveTexture` / `glGenerateMipmap` rows + routing + `GL_TEXTURE0` enum (Task 2)
  - `src/asset_loader.cpp` — `stb_image.h` impl include, `ResolveAndUploadDiffuse` + `UploadTexture` helpers, material-loop call site, `console_log.h` for the missing-texture warn (Tasks 3, 5)
  - `src/renderer.cpp` — FS sampler + `u_hasTexture`, per-mesh texture bind, sampler→unit-0 (Task 4)
  - `src/renderer.h` — `u_base_color_tex_` / `u_has_texture_` location members (Task 4)
  - `docs/PHASE1_VALIDATOR_GATE.md` — Story 2.2 acceptance rows + UV-flip note (Task 6)
  - `src/scene.h` — **no change** (the `GpuImage baseColor` field already exists from 2.1; this story finally binds it)
- **Naming conventions** (architecture line 517-524): Types/functions **PascalCase** (`ResolveAndUploadDiffuse`, `UploadTexture`), locals/members **snake_case** (`tex_path`, `model_dir`, `u_base_color_tex_`), file-internal helpers in the **anonymous namespace**. SPDX header already on every file. **Comment WHY only** (the BGRA swizzle, the no-flip rationale, the compressed-vs-uncompressed branch are exactly the kind of non-obvious WHY worth a line); never narrate WHAT. No story/commit IDs in comments.

### Previous Story Intelligence (Story 2.1 + its code review)

- **Seam was pre-built for this story (2.1 Dev Notes §F):** "Texture sampling `u_baseColor *= texture(...)` is added in Story 2.2 — leave a clean seam, don't add the sampler uniform yet." That seam (FS `v_uv` input present, `GpuImage baseColor` field, `TextureDeleter`) is ready — this story plugs into it without churning the vertex layout or the material struct.
- **`materialIdx` clamp (2.1 CRITICAL review patch):** the render loop's `materials[mesh.materialIdx]` is already guaranteed in-bounds (clamp at load + synthetic default material). The new per-mesh texture bind reads `asset_.materials[mesh.materialIdx].baseColor` through that **same** index — it inherits the guarantee, no new bounds risk. Do not remove or weaken the clamp.
- **`UploadMesh` GL-error discipline (2.1 review):** the buffer upload drains `glGetError` before and after. `UploadTexture` should follow the **same** pattern — drain stale errors, then check after `glTexImage2D`/`glGenerateMipmap`; on a GL error, free the (partially-created) `GpuImage` and return empty (→ flat fallback + warn), consistent with the no-fatal-on-texture rule. (A GL failure on a *buffer* is still fatal `GpuUploadFailed` per 2.1; a GL failure on a *texture* degrades to flat, because the geometry is still drawable.)
- **No-throw boundary (AR18, Story 1.4 + 2.1):** the whole `LoadAsset` body is wrapped; stb_image is C and returns null (never throws), so the decode path is naturally no-throw. The `std::vector` for the uncompressed swizzle is the only allocation that can throw `bad_alloc` → already caught as `OutOfMemory`. Nothing new escapes.
- **RAII / context ordering (Story 1.2):** `GpuImage` destructors call `glDeleteTextures` and need a current context — they live inside `asset_.materials`, released by `asset_ = Asset()` in `Shutdown()` (context current, per `StopRendering`'s `wglMakeCurrent → Shutdown → DestroyGLContext` order). No teardown change needed; just verify the texture handles ride the existing path (they do — same `Asset` aggregate).
- **Hot-path discipline (1.2 review, 10 findings):** zero heap alloc in `RenderFrame`. The texture bind is alloc-free; keep it that way.
- **Windows `.bat` (project memory):** CRLF, flat `errorlevel` gotos, `cd /d "%~dp0"`, close Reaper before copying the DLL. The untracked `build_spike.bat`/`build_forcefail.bat` must stay **out** of this story's commits. The extra stb clone is tiny (one small header) — unlike assimp, no meaningful build-time change to flag.

### Latest tech notes

- **assimp v6.0.5** (pinned). `aiGetMaterialTexture`/`aiMaterial::GetTexture(aiTextureType_DIFFUSE, 0, ...)` and `aiScene::GetEmbeddedTexture(const char*)` are the stable API surface used here; `aiTexture::achFormatHint` (e.g. `"png"`, `"jpg"`) is available if a format-specific decoder branch is ever needed, but `stbi_load_from_memory` sniffs the format itself, so the hint is informational only.
- **glTF diffuse mapping:** assimp maps the glTF 2.0 `pbrMetallicRoughness.baseColorTexture` to `aiTextureType_DIFFUSE` (and `baseColorFactor` to `AI_MATKEY_COLOR_DIFFUSE`, already read in 2.1's `ConvertMaterial`). So `aiTextureType_DIFFUSE` is the correct channel for both packaging variants — do not look for `aiTextureType_BASE_COLOR` (assimp's glTF importer populates the legacy DIFFUSE slot).
- **stb_image** is the de-facto standard, public-domain, single-header decoder; assimp vendors a copy internally but does not export it, so an explicit pin is the clean choice. Pin a recent `master` commit (the API — `stbi_load`, `stbi_load_from_memory`, `stbi_image_free` — has been stable for years).

### References

- Story + ACs: [epics.md#Story 2.2](../planning-artifacts/epics.md) (lines 356–367); Epic 2 intro (336–338); FR map FR17/FR18 (188–189), FR19→Epic 6 (190).
- AR14 unified texture path: [epics.md] line 153; [architecture.md] line 91 ("Texture resolution across 3 packaging variants … one unified texture-resolution path in the assimp boundary layer"). AR5 importer narrowing: [epics.md] 138. AR8/D2 RAII no-cache: [architecture.md] 211–220. AR16 console-only: [epics.md] 155. AR17 failure isolation: 156. AR18 no-throw: 157. AR19 gate: 161.
- D1 `SceneMaterial { GpuImage baseColor; ... }`: [architecture.md] line 195. RAII texture anti-pattern (`GpuImage LoadTexture(const aiTexture*)`): [architecture.md] 713–721.
- Live code seams: `src/scene.h:43-48` (`SceneMaterial.baseColor` awaiting upload), `src/gpu_resources.h:48,53` (`TextureDeleter`/`GpuImage`), `src/gl_loader.h:51-79` (X-macro table to extend), `src/asset_loader.cpp:42-59` (`ConvertMaterial` — texture step goes beside it), `src/asset_loader.cpp:258-282` (material/mesh loops — call site), `src/renderer.cpp:40-60` (FS to extend), `src/renderer.cpp:230-237` (per-mesh material uniforms — texture bind goes here), `src/renderer.h:62-68` (uniform-location members), `CMakeLists.txt:22-72` (FetchContent + SYSTEM includes + link).
- Story 2.1 (full context, seam, review patches): [2-1-load-and-display-a-static-gltf-glb-mesh.md](2-1-load-and-display-a-static-gltf-glb-mesh.md). Gate to extend: [docs/PHASE1_VALIDATOR_GATE.md](../../docs/PHASE1_VALIDATOR_GATE.md).
- Spec Change Logs: [architecture.md] 1126–1148 (Spike 0 / raw GL — why direct render, no FBO); [sprint-change-proposal-2026-06-24.md] (Collada).

## Dev Agent Record

### Agent Model Used

claude-opus-4-8 (1M context) — bmad-dev-story workflow.

### Debug Log References

- `cmake -S . -B /tmp/rav_build` → configures clean (Linux `else()` branch stubs the
  Windows target; the stb FetchContent + C++ compile + `/W3` cleanliness + single-DLL
  + all visual ACs are confirmed only on Antho's Windows gate, same as Story 2.1).
- `git ls-remote https://github.com/nothings/stb.git HEAD` → `31c1ad37456438565541f4919958214b6e762fb4` (the SHA pinned in CMake + VENDORED.md).
- Dead-name grep over `src/` → clean (only pre-existing historical Spec-Change-Log
  comments in `gpu_resources.h`/`gl_loader.h` match `sg_`, which is expected).

### Completion Notes List

- **Unified AR14 funnel implemented** in `asset_loader.cpp::ResolveAndUploadDiffuse`:
  `GetTexture(aiTextureType_DIFFUSE)` → `GetEmbeddedTexture` branch (compressed
  `stbi_load_from_memory` / uncompressed BGRA→RGBA swizzle) / external sibling
  (`stbi_load`) → `UploadTexture`. One funnel, three sources, ready to absorb FBR19
  with no new code.
- **Always decode 4-channel RGBA** (`stbi_load(..., 4)`) so the GL upload is uniformly
  `GL_RGBA`/`GL_UNSIGNED_BYTE` regardless of source channel count. Mipmapped, `GL_REPEAT`
  wrap (glTF default), trilinear min / linear mag.
- **Texture failure is non-fatal** (AC3/AC6): `UploadTexture` mirrors `UploadMesh`'s
  `glGetError` drain-and-check but degrades to an empty `GpuImage` (→ flat color) on GL
  error instead of aborting; a decode/IO miss emits **one** `[RAV] warn:` line and
  returns empty. Only a catastrophic `std::bad_alloc` from the uncompressed-swizzle
  vector propagates to the existing `OutOfMemory` catch. No-throw boundary (AR18) intact
  — stb is C and returns null, never throws.
- **CPU buffers freed immediately after upload** (`stbi_image_free` / `std::vector` drop)
  — no CPU leak; every texture owned by a per-material `GpuImage`, released on
  `Asset` teardown via the existing `asset_ = Asset()` path (no `Shutdown()` change).
- **Shader modulation seam filled** (`renderer.cpp`): `base = u_baseColor; if (u_hasTexture != 0) base *= texture(u_baseColorTex, v_uv).rgb;` — `u_hasTexture == 0` reproduces
  Story-2.1 output exactly (textureless / failed-resolve / pre-2.1 regression rows hold).
  Sampler bound to unit 0 once per frame; per-mesh bind is 3 alloc-free GL calls (D2).
- **Deviation from the task signature (justified):** `ResolveAndUploadDiffuse`'s last
  param is `unsigned mi` (material index) rather than the task's `const std::string& model_path`. The diagnostic the story itself specifies uses `mi` (`LogWarn("… material %u …", mi)`), and `model_path` was otherwise unused — passing it would add a dead param
  (an `/W4` smell). `model_dir` (computed once from `u8path(path).parent_path()`) still
  anchors the sibling branch, so no behavior is lost.
- **Added beyond the letter of the story (hardening):** `#define STBI_WINDOWS_UTF8`
  before `stb_image.h`. Without it `stbi_load` opens the sibling file via system-codepage
  `fopen` and a texture under a non-ASCII folder (validator gate row 9 territory) would
  spuriously fail-to-flat. With it, stb converts our `u8string()` to wide + `_wfopen`,
  matching `FileExists`'s 2.1 Unicode fix. Embedded textures are byte buffers (no path),
  so they were never affected.
- **UV orientation:** shipped with **no flip** (glTF top-left UV + top-row-first upload
  are self-consistent). If the Windows gate sees a vertically mirrored texture, the
  one-line fix is `stbi_set_flip_vertically_on_load(true)` — flagged in the gate doc,
  not guessable on the Linux box.
- **Validation:** no automated test harness exists (architecture lines 933–935); per
  `feedback_trust_ingame_validation`, the gate is Antho's in-Reaper Windows run. Task 6
  authored the new gate rows 10–14 (embedded FR17, sibling FR18, missing-texture
  fallback AC3, textureless/`.dae` no-regression, single-DLL) + the UV-flip note.

### File List

- `CMakeLists.txt` — stb `FetchContent` (pinned commit) + `${stb_SOURCE_DIR}` SYSTEM include (Task 1)
- `extern/VENDORED.md` — stb_image pin section, license, FetchContent-not-in-tree note (Task 1)
- `src/gl_loader.h` — `glActiveTexture` / `glGenerateMipmap` table rows + routing `#define`s + `GL_TEXTURE0` enum (Task 2)
- `src/asset_loader.cpp` — stb_image impl include (+ `STBI_WINDOWS_UTF8`), `console_log.h`, `UploadTexture` + `ResolveAndUploadDiffuse` helpers, material-loop call site (Tasks 3, 5)
- `src/renderer.cpp` — FS `u_baseColorTex`/`u_hasTexture` modulation, sampler→unit-0 bind, per-mesh texture bind (Task 4)
- `src/renderer.h` — `u_base_color_tex_` / `u_has_texture_` location members (Task 4)
- `docs/PHASE1_VALIDATOR_GATE.md` — §7 Story 2.2 acceptance rows 10–14 + UV-flip note (Task 6)

## Change Log

| Date | Version | Description | Author |
|---|---|---|---|
| 2026-06-24 | 0.2.0 | Story 2.2 implemented — diffuse texture resolution across GLB-embedded (FR17) and glTF-sibling (FR18) sources via the unified AR14 funnel; stb_image vendored (FetchContent, header-only); missing-texture → flat fallback + one `[RAV] warn:` line (AC3); validator gate §7 added. Status → review. | Amelia (dev agent) |
