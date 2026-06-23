---
stepsCompleted: [1, 2, 3, 4, 5, 6, 7, 8]
lastStep: 8
status: 'complete'
completedAt: '2026-05-12'
inputDocuments:
  - '_bmad-output/planning-artifacts/prd.md'
  - '_bmad-output/planning-artifacts/prfaq-FBXAnimationViewer.md'
  - '_bmad-output/implementation-artifacts/spec-phase-0-scaffolding.md'
  - '_bmad-output/implementation-artifacts/deferred-work.md'
  - 'docs/PHASE0_VALIDATOR_GATE.md'
workflowType: 'architecture'
project_name: 'FBXAnimationViewer'
user_name: 'Antho'
date: '2026-05-12'
classification:
  projectType: 'desktop_app'
  projectSubType: 'DAW extension / native plugin (Reaper)'
  domain: 'general'
  domainSubContext: 'creative production tooling / game audio'
  complexity: 'medium'
  projectContext: 'brownfield'
releaseMode: 'phased'
---

# Architecture Decision Document — FBXAnimationViewer

**Author:** Antho
**Date:** 2026-05-12

_This document builds collaboratively through step-by-step discovery. Sections are appended as we work through each architectural decision together._

## Project Context Analysis

### Requirements Overview

**Functional Requirements (41 FRs across 8 capability groups):**

The PRD organizes capabilities into eight architecturally meaningful clusters that map onto distinct subsystems:

- **Animation Loading & Format Support (FR1–FR7):** glTF 2.0 binary (`.glb`) and multi-file (`.gltf` + siblings), FBX with 60% target compatibility, tolerance for non-standard up-axis / unit-scale / bone-name conventions, static-mesh fallback when no animation channels are present, TRS sampling including root motion. Implies a single unified loader funnel through assimp with explicit handling of three texture-packaging variants.
- **Timeline Integration (FR8–FR14):** Drag-to-track ingestion, item length = animation duration, item-relative time mapping (`animTime = playheadTime − itemStart`, clamped), coexistence with audio/video/MIDI, multi-item support across tracks, current-item selection by playhead+priority. Drives the PCM_source plugin design as the central Reaper integration surface.
- **3D Rendering & Visual Fidelity (FR15–FR20):** Skinned mesh with per-frame bone deformation, diffuse-textured Blinn-Phong + per-material specular response, multi-material meshes, textures resolved from GLB-embedded / glTF siblings / FBX-embedded variants.
- **Camera & Viewport Control (FR22–FR26):** Orbit (right-click drag), zoom (scroll), pan (middle-click drag), reset-to-bounding-box, continuous-update during interaction (no pause-on-camera).
- **Panel & Docking (FR27–FR30):** ReaImGui-driven panel, dockable in any Reaper docker, Action registration (`FBXAV: Open Viewer Window`), graceful console diagnostic + bail-out when ReaImGui is absent at load.
- **Project State Persistence (FR31–FR33):** Per-item state (animation path, camera angle, optional time offset/scale) round-trips via PCM_source `SaveState`/`LoadState`; panel dock position delegates to ReaImGui's own state persistence.
- **Error Tolerance & Graceful Degradation (FR34–FR37):** No host crash on malformed input, console diagnostic with file path + error category, manual reload button per item, single-item failure isolation.
- **Distribution & Operation (FR38–FR41):** ReaPack one-click with ReaImGui as auto-install dependency, manual DLL-copy install path, fully offline (zero network surface), updates entirely delegated to ReaPack.

**Non-Functional Requirements (17 NFRs):**

- **Performance (P1–P6):** ≥60 fps at 20k tris / 4 materials / 50 bones; ≤2 s initial load for typical Demute fixture; ≤1 s manual reload; ≤2 s DLL load at Reaper startup; transport scrub latency ≤16 ms (one rendered frame at 60 fps); ≥10 simultaneous animation items per project.
- **Reliability (R1–R5):** Zero Reaper-host crashes attributable to the extension from Phase 4 onward; no Reaper-project data loss; clean unload (no leaked GL contexts, no leaked window classes, no dangling Reaper API pointers); failures are non-lethal (other items keep working); MSVC `/W3 /permissive-` warning-free.
- **Compatibility (C1–C5):** Reaper 7.x with `caller_version == 0x20E`; ReaImGui (`cfillion/reaimgui`) auto-install dep; MSVC x64 ABI exclusively on Windows MVP; assimp pinned in `extern/VENDORED.md`; FBX 60% compatibility against representative Demute exports.

**Scale & Complexity:**

- Primary technical domain: native desktop DAW extension (Reaper plugin DLL).
- Complexity level: **medium** — native C++ in hostile host process + embedded 3D pipeline + GPU vertex skinning + DAW transport integration + project-state serialization. Single-user, single-machine, mono-threaded.
- Estimated architectural components: ~6–8 subsystems (plugin lifecycle/entry, Reaper API loader, ReaImGui panel host, sokol_gfx renderer + FBO bridge, assimp asset loader, PCM_source plugin + item state I/O, camera controller, project-state serialization).
- Platform commitment: Windows x64 MVP; Mac/Linux deferred post-MVP (architected portable via ReaImGui+SWELL and sokol_gfx Metal/GL backends).

### Technical Constraints & Dependencies

**Hard constraints (locked by PRFAQ, Phase 0 evidence, or PRD 2026-05-10 decision):**

- **Language / ABI:** C++17, MSVC x64 ABI on Windows. No mingw, no clang-cl, no 32-bit (NFR-C3 + Reaper SDK requirement at `reaper_plugin_functions.h:27`).
- **No exceptions in plugin entry path** — Reaper is a hostile host; any escape crashes the DAW.
- **Threading:** UI + GL bound to Reaper's main thread (ReaImGui callback model + sokol_gfx GL context). Worker threads are explicitly out of scope for MVP.
- **API binding:** All Reaper APIs resolved at load time via `rec->GetFunc("FunctionName")` — no static link.
- **Build:** CMake; SDK headers vendored under `extern/reaper-sdk/sdk/` (drvfs/WSL pragmatism — see Phase 0 Spec Change Log, 2026-05-09).
- **Rendering stack:** sokol_gfx (GL backend on Windows; Metal post-MVP on Mac).
- **Animation parsing:** assimp (pinned commit, FBX + glTF in one library).
- **Math:** GLM.
- **UI framework:** ReaImGui (`cfillion/reaimgui`) — runtime dependency, ReaPack auto-install. Phase 0's standalone Win32 window is superseded by a ReaImGui dockable panel (PRD 2026-05-10).
- **Distribution:** ReaPack only, MIT, no-SLA, 100% offline (no telemetry / no network surface at all).
- **State persistence:** Per-item state via PCM_source `SaveState`/`LoadState`; panel dock position via ReaImGui's own state.
- **Project files:** Read-only access to glTF / GLB / FBX via assimp's `Importer::ReadFile`; no writes outside Reaper's project ext-state.
- **Reaper version:** Reaper 7.x, SDK `caller_version == 0x20E` (SDK update 2026-05-07 for Reaper 7.72).

**Brownfield context (Phase 0 already shipped, validated 2026-05-09):**

- Build toolchain proven: CMake produces `reaper_fbxanimationviewer.dll` linking cleanly with zero `/W3` warnings.
- Plugin entry-point pattern validated: `caller_version` handshake, action registration, symmetric unregister.
- Win32 + WGL window proven loadable and unloadable inside Reaper without leaks — but this surface is being **replaced in Phase 0.5** by the ReaImGui panel.
- WDL/SWELL deferred from Phase 0 (drvfs friction); will be needed by ReaImGui transitively on Mac/Linux post-MVP.

### Cross-Cutting Concerns Identified

1. **Convention tolerance by camera, not by remapping.** Loader renders glTF/FBX content as-authored — different up-axes, units, bone-naming conventions are absorbed by camera-reset / user adjustment. Auto-remap is explicitly post-MVP. This decision propagates into the loader, the camera controller, and the diagnostics surface.
2. **Texture resolution across 3 packaging variants.** GLB-embedded / glTF-with-siblings / FBX-aiTexture-embedded must funnel through one unified texture-resolution path in the assimp boundary layer. Affects loader, GPU upload path, and diagnostics for missing-texture cases.
3. **Symmetric register/unregister of every Reaper API surface.** NFR-R3 mandates that every `rec->Register("name", ...)` is paired with a `rec->Register("-name", ...)` on unload. This is a transverse invariant covering action registration, hookcommand, PCM_source factory, project-state hooks — every load-time registration must be tracked and reversed.
4. **Console diagnostics as the canonical user-feedback channel.** No message boxes, no toasts, no popup dialogs from this extension. `ShowConsoleMsg` is the only feedback path for non-fatal errors (file load failure, missing ReaImGui, malformed rig). This concern spans the loader, PCM_source plugin, panel, and lifecycle.
5. **Failure isolation: one bad item never breaks others.** A malformed glTF/FBX must not crash the host (NFR-R1), must not invalidate other items in the session (FR37), and must leave the rest of the project intact (NFR-R2). Implies per-item error containment with explicit boundaries between loader, renderer, and host integration.
6. **Threading discipline.** Reaper main-thread-only for UI + GL. Any future worker (file load, hashing) must marshal results back to main thread before touching ReaImGui or sokol_gfx. This rule is invariant across all subsystems and shapes the design of the loader and reload paths.

**Phase commitment trajectory:**

- ✅ Phase 0 — Scaffolding (shipped, validated 2026-05-09)
- ⏳ Phase 0.5 — ReaImGui refactor (sokol_gfx FBO ↔ `ImGui::Image()`)
- ⏳ Phase 1 — Static mesh rendering + camera
- ⏳ Phase 2 — Skinned animation playback **(dominant technical risk per PRFAQ + Winston party-mode)**
- ⏳ Phase 3 — Reaper transport sync + PCM_source plugin
- ⏳ Phase 4 — File reload + camera persistence + FBX validation
- ⏳ Phase 5 — Polish + ReaPack release

## Starter Template Evaluation

### Primary Technology Domain

Native desktop DAW extension (Reaper plugin DLL). No conventional starter template applies — the Reaper Extension SDK ecosystem has no official scaffolding tool comparable to `create-next-app` or `cargo new`. Community reference implementations (SWS extension, ReaImGui source, sokol_gfx samples) serve as patterns to read, not templates to clone.

### Brownfield Foundation in Lieu of a Starter

This project is brownfield: Phase 0 (commit `7677c70`, validated 2026-05-09) has already established the scaffolding that a starter would otherwise provide. The decisions baked into the current `main` branch:

| Domain | Decision in place | Locked by |
|---|---|---|
| Build system | CMake ≥ 3.20 with `cmake/ReaperPlugin.cmake` helper, MSVC `/W3 /permissive-`, C++17 | Phase 0 |
| Toolchain (Windows) | Visual Studio 2022, MSVC x64 ABI exclusively | NFR-C3 |
| SDK vendoring | Reaper SDK headers in-tree under `extern/reaper-sdk/sdk/`, documented in `extern/VENDORED.md`. Submodule strategy abandoned for drvfs/WSL friction (Spec Change Log 2026-05-09). |
| Plugin entry pattern | `REAPER_PLUGIN_ENTRYPOINT`, `caller_version == 0x20E`, `rec->Register(...)` triple, symmetric unregister on unload | Phase 0 `plugin_main.cpp` |
| Reaper API loader | `REAPERAPI_MINIMAL` + per-symbol `WANT_*` defines in `src/reaper_api.h`, resolved at load via `rec->GetFunc("...")`. Incremental expansion per phase. |
| Repo layout | `src/` (plugin code), `cmake/` (helpers), `extern/` (vendored deps), `reapack/` (distribution manifest), `docs/` (validator gates), `_bmad-output/` (planning + implementation artifacts) |
| License | MIT, SPDX header per source file | Phase 0 |
| Version control | git, `main`, hooks enforced (no `--no-verify`, no force-push) | Phase 0 Boundaries |

### Dependency Audit (current versions, 2026-05-12)

| Dependency | Current version | Date | Type | Pickup phase |
|---|---|---|---|---|
| ReaImGui (`cfillion/reaimgui`) | `v0.10.0.5` | 2026-04-16 | Runtime ReaPack dep (auto-install); we vendor the binding header only | Phase 0.5 |
| sokol_gfx (`floooh/sokol`) | commit `85d1f1b` | 2026-05-11 | Header-only, pinned by SHA in `extern/VENDORED.md` | Phase 0.5 / Phase 1 |
| assimp | `v6.0.5` | 2025-04-30 | CMake subproject, importers narrowed to glTF + FBX | Phase 1 |
| GLM | `1.0.3` | 2025-12-31 | Header-only, vendored under `extern/glm/` | Phase 1 |

**ReaImGui clarification:** What we vendor is the **C/C++ binding header** declaring the `ImGui_*` symbols exported by the ReaImGui Reaper extension. Our code resolves each function at runtime via `rec->GetFunc("ImGui_Begin")` — identical to the pattern used for `ShowConsoleMsg`. The ReaImGui extension itself lives in Reaper's process and is installed via ReaPack as a declared dependency. The `reapack/index.xml` manifest must declare a runtime dep on `cfillion/reaimgui >= v0.10.0.5`.

**assimp build narrowing:** Default assimp builds ~40 importers including legacy formats (MD5, MDL, X, AC, B3D…). PRD scope is **glTF + FBX only**. Top-level `CMakeLists.txt` must set `ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF` and explicitly enable `ASSIMP_BUILD_GLTF_IMPORTER=ON` and `ASSIMP_BUILD_FBX_IMPORTER=ON`. Benefits: smaller binary, reduced attack surface (assimp v6.0.x security advisories concentrated in unused parsers), faster build.

**sokol pinning rule:** Header-only repos with no release tags pin by commit SHA. The SHA chosen at Phase 0.5 freezes until a documented reason to bump (Metal backend need on Mac, observed bug, required feature). Consistent with the Reaper SDK vendoring discipline.

### Selected "Starter" — Phase 0 Baseline

**Rationale:** The Phase 0 commit on `main` IS our foundation. It has been validated against the eight-row validator-gate checklist in `docs/PHASE0_VALIDATOR_GATE.md` (7 of 8 green, the eighth diagnostic-only) and satisfies all NFR-C* compatibility constraints. There is no benefit to re-evaluating starter templates we cannot use; cost of re-scaffolding would be measured in days for no gain.

**Initialization Command (for new developer onboarding, not for fresh project creation):**

```bash
git clone <repo>
cd FBXAnimationViewer
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**Note:** This section deliberately departs from the BMAD template's web-app starter evaluation pattern. For native C++ DAW extensions, the equivalent step is "audit and pin current versions of header-only deps before Phase 0.5/1 vendoring" — done above; specific architectural decisions on dependency integration are handled in step-04 (Architectural Decisions).

## Core Architectural Decisions

### Authoring Note

The decisions below are made by Claude as implementing developer (per the validator/dev role split confirmed 2026-05-12: Antho commissions and validates the *experience*, Claude owns the *implementation*). User-facing implications are surfaced separately in plain language during the workflow. Code-level rationale stays here.

### Decision Priority Analysis

**Critical (block implementation — must be settled before Phase 0.5 starts):**

- Scene data model (D1) — every downstream subsystem depends on it
- GPU resource ownership pattern (D2) — drives Asset lifecycle and reload semantics
- Coordinate convention (D3) — once a single mat4 transposes wrongly at the assimp boundary, debugging a skinned rig is hell
- ReaImGui ↔ sokol_gfx FBO bridge (D11) — Phase 0.5 deliverable; the entire panel-based architecture hinges on this working

**Important (shape architecture — settled here, refined later if needed):**

- Manual reload semantics (D4), exception strategy (D5), error propagation (D6), console line format (D7), PCM_source plugin pattern (D8), persistence layering (D9), loader API expansion (D10), render pipeline (D12), GPU skinning (D13), camera controller (D14), assimp CMake narrowing (D15), ReaPack dep declaration (D16), install validation (D17)

**Deferred (post-MVP, intentionally not decided now):**

- Asset cache / sharing between PCM_source instances — accept VRAM duplication for MVP (D2)
- File-watcher auto-reload (PRD: P2 polish; downgraded from Phase 4)
- Per-project axis remap (PRD: post-MVP — convention tolerance is by camera, not by code)
- Worker threads for loading — added if any Demute fixture exceeds 1s parse time
- Normal maps / richer PBR — post-MVP per PRD Growth Features
- Mac/Linux ports — architected portable, deferred per PRD platform commitment

### Data Architecture (D1–D4)

#### D1 — Scene data model

A single aggregate `Asset` per loaded file, composed of plain POD-style structs (no inheritance, no virtuals):

```cpp
struct SceneVertex   { vec3 pos; vec3 normal; vec2 uv; ivec4 boneIds; vec4 boneWeights; };
struct SceneMesh     { GpuBuffer vb; GpuBuffer ib; uint32_t indexCount; uint32_t materialIdx; bool skinned; };
struct SceneMaterial { GpuImage baseColor; vec3 specularColor; float shininess; };
struct SceneBone     { int parentIdx; mat4 inverseBindMatrix; std::string name; };
struct SceneSkeleton { std::vector<SceneBone> bones; };
struct AnimChannel   { std::vector<KeyframeT> translation; std::vector<KeyframeR> rotation; std::vector<KeyframeS> scale; };
struct SceneAnimation{ float duration; std::vector<AnimChannel> channels; };  // channels indexed by bone idx
struct Asset {
    std::vector<SceneMesh>      meshes;
    std::vector<SceneMaterial>  materials;
    SceneSkeleton               skeleton;     // empty for static-mesh fallback (FR6)
    std::vector<SceneAnimation> animations;   // MVP: first one used; multi-clip is post-MVP
    glm::mat4                   modelRoot;    // identity for glTF-canonical, leaves non-canonical as-authored
};
```

Bones stored flat with `parentIdx` (root = -1) so a single linear pass computes global matrices. Skinned flag at mesh level enables the static-mesh fallback path (FR6).

#### D2 — GPU resource ownership

**Per-item ownership with RAII handles.** Each `PCM_source` instance owns its `Asset`. No shared cache, no ref counting.

```cpp
struct GpuBuffer { sg_buffer h{}; GpuBuffer()=default; GpuBuffer(GpuBuffer&&) noexcept; ~GpuBuffer(){ if(h.id) sg_destroy_buffer(h); } /* non-copyable */ };
// Identical pattern for GpuImage, GpuShader, GpuPipeline
```

Trade-off: same `.glb` on two tracks → two VRAM copies. Budget: 10 items × (~8 MB textures + ~5 MB geometry) ≈ 130 MB max — well under any modern GPU's budget. Refactor to a shared cache is delimited (factory pattern) if ever needed post-MVP.

#### D3 — Coordinate / matrix conventions

| Aspect | Choice | Rationale |
|---|---|---|
| Matrix major | Column-major (GLM default, GLSL native) | Zero swap at uniform upload |
| Handedness | Right-handed | glTF canonical; matches assimp glTF default |
| Up-axis | Y-up | glTF canonical; no `aiProcess_MakeLeftHanded` flag |
| Winding | Counter-clockwise front-facing | glTF canonical; sokol_gfx default |
| assimp → GLM | Single boundary function `convertAssimpMatrix(const aiMatrix4x4&)` transposing row-major to column-major | One conversion point, easy to audit |
| Convention tolerance | Render as-authored (no auto-remap) | Per PRD: Z-up / cm / non-canonical FBX rendered tilted; user compensates via camera |

`Asset::modelRoot` stays at identity for canonical files; non-canonical files render visually wrong but Reset Camera (D14) reframes them — that is the MVP contract.

#### D4 — Manual reload semantics (FR36, NFR-P3)

Atomic swap with graceful fallback:

```
On user clicks Reload for item I:
  1. Parse file at I.path → tempAsset (assimp + GPU upload to temp handles)
  2. If success:
       std::swap(I.asset, tempAsset);  // RAII destroys old GPU resources on scope exit
       console: "[FBXAV] info: reloaded <path>"
  3. If failure:
       leave I.asset untouched
       console: "[FBXAV] error: reload failed <path>: <category>"
  4. Camera state UNCHANGED (lives in panel state, not in Asset)
  5. Animation playback time UNCHANGED (lives in PCM_source time mapping)
```

Guarantees: ≤1s for typical fixture (NFR-P3) thanks to synchronous main-thread parse + upload; zero host crash on broken file (NFR-R1, FR37); camera and timeline-mapping preserved (FR33 consistency).

### Error Discipline (BMAD: Authentication & Security) (D5–D7)

#### D5 — Exception strategy

- **Plugin entry path (REAPER_PLUGIN_ENTRYPOINT, hookcommand, PCM_source virtual methods, ReaImGui callbacks): NO escaping exception, ever.** Wrap any call site that may throw with try/catch and translate to a `LoadResult` or `bool + outError`.
- **Internal code: MSVC `/EHsc` default stays on.** No `-fno-exceptions`. assimp throws `std::exception`-derived; wrap `Importer::ReadFile` in a single try/catch at the loader boundary in `src/asset_loader.cpp`.
- Document the no-throw invariant on every Reaper-facing entry point with a header comment.

#### D6 — Error propagation

Result type instead of `std::expected` (which is C++23, not C++17):

```cpp
enum class LoadErrorCategory { Ok, FileNotFound, ParseFailed, UnsupportedFormat, GpuUploadFailed, OutOfMemory, Unknown };

struct LoadResult {
    std::optional<Asset> asset;
    LoadErrorCategory category = LoadErrorCategory::Ok;
    std::string detail;            // human-readable, included in console line
};
```

Functions returning `LoadResult`: every asset-loading entry point. Internal helpers can return `bool` with an out-param for `std::string& outError`. No exceptions cross subsystem boundaries.

#### D7 — Console diagnostic format

Single-line format, three levels:

```
[FBXAV] info: extension loaded (Phase 0)
[FBXAV] info: reloaded <path>
[FBXAV] warn: <message>
[FBXAV] error: <context>: <category>: <detail>
[FBXAV] error: load failed C:/anim/dragon_v11.glb: ParseFailed: Unexpected EOF at offset 124
```

Levels: `info`, `warn`, `error`. Every line starts with `[FBXAV]`. Path included verbatim when relevant. Greppable for users who want to mine logs. No multi-line messages (Reaper's console concatenates poorly). No popups, no message boxes — `ShowConsoleMsg` is the only output channel (per cross-cutting concern from step 2).

### Reaper Integration Surfaces (BMAD: API & Communication) (D8–D10)

#### D8 — PCM_source plugin pattern

Subclass `PCM_source` from `reaper_plugin.h`. Register a factory via the Reaper SDK's PCM_source registration entry point (exact mechanism: `rec->Register("PCM_source", &factory_function)` returning a new `PCM_source*` instance for a given file extension — **to verify against SDK headers at Phase 3 design step**; see also reference patterns in SWS and ReaImGui source).

Extensions registered: `.glb`, `.gltf`, `.fbx`.

Per-item state lives on the PCM_source instance:
- `Asset` (owned, RAII)
- File path (string)
- Camera state ({targetPoint, distance, yaw, pitch})
- Time mapping: offset, scale (defaults: 0, 1)
- Current playback time (derived from playhead, recomputed every frame, not stored)

Virtual methods implemented (mapped to PRD FRs):
- `GetType()` returns `"FBXAV"` (or similar tag)
- `GetSampleRate()`, `GetNumChannels()` return 0 (we're not audio)
- `GetLength()` returns animation duration in seconds (FR9)
- `SaveState(ProjectStateContext*)`, `LoadState(...)` for ext-state I/O (D9)
- `PropertiesWindow(...)` opens our config (deferred — optional, may delegate to panel)
- No audio rendering methods used (we don't pump samples)

Verification work for Phase 3 design step (NOT now): exact SDK names for the registration entry, and how Reaper routes file drops with our registered extensions to our factory.

#### D9 — Persistence layering

Three independent persistence surfaces, each owned by a different layer:

| Surface | Owner | Persistence mechanism | Format |
|---|---|---|---|
| Per-item state (path, camera, offset, scale) | PCM_source instance | `SaveState` / `LoadState` virtual methods, called by Reaper during project save/load | Line-based `key=value` text inside Reaper's project chunk for our PCM_source |
| Panel dock position, panel size, dock target | ReaImGui itself | ReaImGui's internal state persistence (already handles this) | Opaque to us |
| Global viewer prefs (if any, e.g. show grid toggle) | Project-level ext-state | Reaper `ProjectExtensionConfig` hook | Single line in our extension's named chunk |

Forward-compat: SaveState writes all known keys; LoadState ignores unknown keys (per PRD's "older extension reading newer project: opportunistic" clause).

#### D10 — Reaper API loader expansion strategy

Stay with the `REAPERAPI_MINIMAL` + `WANT_*` pattern from Phase 0. Add per phase:

| Phase | New WANT_* defines |
|---|---|
| Phase 0 (current) | `WANT_ShowConsoleMsg` |
| Phase 0.5 | ReaImGui exports — resolved with the same `rec->GetFunc("ImGui_Begin")` pattern, lazily on first use or eagerly at load |
| Phase 3 | `WANT_GetPlayPosition2Ex`, `WANT_GetPlayStateEx`, PCM_source registration helpers, ext-state I/O helpers, `WANT_Main_OnCommand` (if action dispatching needed beyond hookcommand) |
| Phase 4 | Project-state extension hooks if needed beyond PCM_source SaveState; `WANT_EnumProjects`, `WANT_GetProjectPath` if reload-relative-paths are introduced |
| Phase 5 | Polish-only (`WANT_GetMediaTrackInfo_Value`, etc. if track inspection is needed for diagnostics) |

ReaImGui binding: since each ReaImGui function is also resolved via `rec->GetFunc()`, we maintain a single `imgui_api.h` parallel to `reaper_api.h` with the ReaImGui symbol list. The binding header from `cfillion/reaimgui` is vendored to `extern/reaimgui/` at the pinned version.

### Render Pipeline & ReaImGui Bridge (BMAD: Frontend Architecture) (D11–D14)

#### D11 — sokol_gfx ↔ ReaImGui FBO bridge

> **⛔ SUPERSEDED 2026-06-23 (Spike 0 — see Spec Change Log at the end of this document).**
> The "render to an FBO and display it via `ImGui::Image()` in a ReaImGui panel" approach
> is replaced by a **native OpenGL window docked via `DockWindowAddEx`** (direct render, no
> FBO, no readback): a ReaImGui panel is hard-capped ~30 fps and cannot display a foreign
> GPU texture. The original design below is retained for historical context only.

Per-frame sequence inside the ReaImGui panel callback:

```
1. ImGui::Begin("FBX Animation Viewer")
2. ImVec2 panelSize = ImGui::GetContentRegionAvail()
3. If panelSize != lastFboSize: destroy old offscreen sg_image, create new one at panelSize
4. sg_begin_pass(offscreen_pass)         // bind FBO
     sg_apply_pipeline(scene_pipeline)
     sg_apply_bindings(sceneMesh.bindings)
     sg_apply_uniforms(vs_stage, &uniforms, ...)
     sg_draw(0, sceneMesh.indexCount, 1)
   sg_end_pass()
5. Acquire the underlying GL texture name from the sg_image (via sokol_gfx's backend introspection or a parallel-tracked GLuint we keep alongside the sg_image when we create it)
6. ImGui::Image((ImTextureID)(intptr_t)glTextureName, panelSize, ImVec2(0,1), ImVec2(1,0))
   // UV-flip because GL framebuffers are bottom-up while ImGui samples top-down
7. Handle camera input on the ImGui::Image() rect (IsItemHovered, IsMouseDragging)
8. ImGui::End()
```

The single GL context is shared between sokol_gfx (which renders into the FBO) and ReaImGui (which composites the FBO texture into the panel). Both run on Reaper's main thread; no context switching needed.

Verification work at Phase 0.5 day 1: confirm sokol_gfx exposes the backend GL texture name for an `sg_image` (or keep our own GLuint at FBO creation time). Fallback (last resort per PRD Risk Mitigation): use `ImGui::DrawList` custom callback to issue raw GL draws. Forecast: not needed; `sg_query_image_info()` or manual handle tracking should suffice.

#### D12 — Render pipeline

> **⚠️ AMENDED 2026-06-23 (Spike 0 — see Spec Change Log).** Rendering targets the docked
> GL window's default framebuffer **directly** — no offscreen FBO, no MSAA-resolve-for-
> sampling, no CPU readback. The Forward/opaque/depth-test/back-face-cull/shader rows below
> still apply; the "MSAA 4× in offscreen pass" row is obsolete (MSAA, if wanted, comes from
> the window pixel format).

| Property | Choice | Notes |
|---|---|---|
| Architecture | Forward, single-pass, opaque-only | MVP scope — no transparency, no shadows, no deferred |
| Color format | RGBA8 | Sufficient for diffuse + Blinn-Phong specular |
| Depth/stencil | D24S8 (depth 24, stencil 8 unused) | Standard pairing; supersedes the deferred-work `cDepthBits=24 cStencilBits=8` note (D/S now needed by z-buffer) |
| MSAA | 4× in offscreen pass | Smooth silhouettes on 20k-tri characters; 4× VRAM cost for color/depth accepted under NFR-P1 budget |
| Cull | Backface, CCW front | glTF canonical winding |
| Depth test | `GL_LESS` | Standard z-buffer |
| Shader (Phase 1) | Blinn-Phong + diffuse texture + per-material specular | Per PRD FR16 |
| Shader (Phase 2) | Add skinning multiply in vertex stage (D13) | Compose, don't fork |

#### D13 — GPU skinning

- **Algorithm:** Linear Blend Skinning (LBS) — sufficient for game-character rigs, standard in glTF/FBX, GPU-cheap.
- **Weights per vertex:** 4 (glTF canonical; assimp `aiProcess_LimitBoneWeights` trims excess at load).
- **Bone matrix upload:** uniform array of `mat4[128]` per draw call. 128 × 64 B = 8 KB, comfortably under the 16 KB std140 uniform block limit. Caps the practical skeleton size at 128 bones (vs. PRD's 50-bone target — 2.5× headroom).
- **Per-frame CPU work:**
  1. Sample animation channels at current time → per-bone local TRS
  2. Compose local matrix from TRS
  3. Traverse skeleton (single linear pass thanks to flat parent-index layout): `globalMat[i] = globalMat[parent[i]] * localMat[i]`
  4. Skinning matrix: `paletteMat[i] = globalMat[i] * inverseBindMatrix[i]`
  5. Upload palette as uniform array, issue draw
- **Vertex shader:** position transformed by `Σ weight[k] * paletteMat[boneId[k]]` for k=0..3, then by MVP.
- **Static-mesh fallback (FR6):** if `Asset::skeleton.bones` is empty, use a non-skinned pipeline variant (no palette upload, position direct → MVP).

#### D14 — Camera controller

State: `{ vec3 target; float distance; float yaw; float pitch; }` per panel (single global viewer panel per MVP).

| Input | Action | Math |
|---|---|---|
| Right-click drag (FR22) | Orbit | `yaw += dx * sensitivity`; `pitch += dy * sensitivity` (clamped to ±pi/2 − ε) |
| Scroll (FR23) | Zoom | `distance *= exp(-scrollDelta * 0.1)` (exponential for natural feel) |
| Middle-click drag (FR24) | Pan | `target += (-right * dx + up * dy) * distance * panSensitivity` (pan scales with distance) |
| Reset Camera button (FR25) | Reframe | Compute `Asset` mesh-union AABB → `target = bbox.center()`; `distance = bbox.diagonal() * 1.5`; `yaw = 0`; `pitch = 0.3 rad` |

Continuous update (FR26): camera state is read each frame; no pause-on-interaction. The camera state is part of panel state, NOT Asset state — survives reload (D4).

View matrix derived as `lookAt(target + spherical(yaw, pitch) * distance, target, vec3(0,1,0))`. Projection: perspective, 50° vertical FOV, near=0.01, far=1000 (units depend on the file's authored scale — fine, since we render as-authored per D3).

### Build & Distribution (BMAD: Infrastructure & Deployment) (D15–D17)

#### D15 — assimp CMake narrowing

In top-level `CMakeLists.txt`, before `add_subdirectory(extern/assimp)`:

```cmake
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)  # static linked into our DLL
add_subdirectory(extern/assimp EXCLUDE_FROM_ALL)
```

Result: only glTF + FBX importers compiled, no exporters, no samples, no tests, statically linked into `reaper_fbxanimationviewer.dll`. Single-DLL deliverable (no extra `assimp-vc143-mt.dll` to copy alongside).

#### D16 — ReaPack manifest dep declaration

`reapack/index.xml` extension entry declares the runtime dep on `cfillion/reaimgui >= v0.10.0.5` using ReaPack's manifest schema. Exact syntax: ReaPack supports auto-install dependencies via the metadata `<link rel="dependency">` or named-dep block — **verification deferred to Phase 5 (release packaging step)**. The architectural requirement is fixed now: from ReaPack's perspective, installing FBXAnimationViewer pulls ReaImGui automatically without manual user action.

Manifest also declares: MIT license, Demute author/maintainer, "as-is / no-SLA" description line, GitHub repo URL (TBD per PRFAQ Open Items), version bumping per phase (`0.0.1-phase0` → `0.5.0-phase05` → `1.0.0-mvp` for Phase 5 release).

#### D17 — Install path validation

- **Dev install (Phase 0 → Phase 5 internal):** copy `build/Release/reaper_fbxanimationviewer.dll` to `%APPDATA%\REAPER\UserPlugins\`. Documented in `docs/PHASE0_VALIDATOR_GATE.md` (and per-phase validator gates as we go).
- **Released install (Phase 5+):** ReaPack subscription URL pointing at the GitHub-hosted `reapack/index.xml`. Subscribe in Reaper → install in two clicks (FBXAnimationViewer + ReaImGui auto-pulled). Restart Reaper, done. Documented in README.md to be written in Phase 5.

### Animation Browser & Transient Preview (D18)

*(Added 2026-06-22, post-original-architecture, to cover the Animation Browser scope — FR42–FR45, Epic 5 — that postdates D1–D17. Epic 5 slots after Phase 3 / Epic 4, reusing the PCM_source surface (D8) and the viewer (D11–D14).)*

#### D18 — Browser panel + transient preview asset lifecycle

**Browser panel (FR42, FR43).** A second ReaImGui panel (own dock, same `RAV` action family, distinct window id) that renders a filesystem listing. Directory traversal and listing run on Reaper's main thread (AR18) using `std::filesystem`; entries are filtered to `.glb`/`.gltf`/`.fbx` for the *selectable* list (directories always shown for navigation). A directory that fails to read emits a `ShowConsoleMsg` diagnostic and leaves the prior listing intact — never throws across the panel callback (AR16, AR17, D5). No background indexing, no watcher (consistent with manual-reload posture, D4).

**The new problem — display source ambiguity.** Until Epic 5, the viewer's displayed `Asset` is unambiguous: it comes from the PCM_source item spanning the playhead (D8/D14, panel state). Preview (FR44) requires displaying an `Asset` that is **bound to no item and no track**. The decision resolves who owns that asset and how the viewer chooses what to show.

**Decision — a single owned "preview slot" with explicit precedence:**

- The browser owns an optional `std::unique_ptr<Asset> g_preview_asset` (RAII, D2) — at most one live preview at a time. Selecting "preview" loads the chosen file through the *same* loader path as items (D1/D6 `LoadResult`); a malformed file yields a console diagnostic and leaves the previous preview (and the playhead-driven display) intact (AR17, FR34-aligned).
- **Viewer display precedence is explicit and total:** if `g_preview_asset` is non-null, the viewer renders the preview; otherwise it renders the playhead-spanning item's asset (the pre-Epic-5 behaviour). There is never an attempt to show both. This keeps Epic 5 from perturbing the Epic 4 transport-driven path — that path is untouched when no preview is active.
- **Preview teardown** (frees the GPU resources via RAII) happens on exactly one of: the user dismisses preview, selects a different file to preview (old slot replaced), or places the previewed file on a track (FR45).

**Place-on-track (FR45) reuses Epic 4, no parallel path.** "Place" calls the same PCM_source creation used by drag-drop (D8, Epic 4 Story 4.1/4.2): it creates an item bound to the chosen path on the target track. The transient preview asset is then dropped; from that point the asset is owned by the item exactly like a drag-dropped one, and the playhead drives it via the normal Epic 4 path. The browser never creates a second class of item.

**Camera/transport interaction.** Preview honours the existing camera controller (D14) and is poseable/playable without an item; if the file has animation channels, preview can loop or hold a pose locally (browser-local time, not the Reaper transport — the transport only drives item-bound assets). This is the one place where displayed animation time is *not* `playheadTime − itemStart`; it is documented here so Epic 4's invariant (D8) is understood to apply only to item-bound display.

**Open sub-decisions to settle in the Epic 5 story spec (AR20):** whether the browser is a distinct panel or a mode toggle inside the viewer panel; whether preview animation auto-plays or holds frame 0; remembered last-browsed directory persistence (likely via ReaImGui state, like dock position). These are UX-grain choices that do not change the ownership/precedence model above.

### Decision Impact Analysis

**Implementation sequence (decisions × phases):**

| Phase | Decisions activated |
|---|---|
| Phase 0.5 (ReaImGui refactor) | D3 (coord), D5 (no-throw entry), D7 (console format), D10 (loader expansion for ImGui_*), D11 (FBO bridge), partial D14 (camera scaffolding) |
| Phase 1 (static mesh + camera) | D1 (Asset partial: static-mesh path), D2 (RAII handles), D6 (LoadResult), D12 (render pipeline), D14 (camera fully) |
| Phase 2 (skinned animation) | D1 (Asset fully: skinned path, animations), D13 (GPU skinning) — dominant risk per PRFAQ |
| Phase 3 (transport sync) | D8 (PCM_source plugin), D9 (per-item SaveState), D10 (loader expansion for transport APIs) |
| Epic 5 (animation browser, post-Phase 3) | D18 (browser panel + transient preview), reuses D8 (place-on-track), D11–D14 (viewer), D1/D6 (loader) |
| Phase 4 (reload + persistence) | D4 (reload swap), D9 (full persistence layering + cross-machine path portability FR46), partial D15 (FBX validation against Demute fixtures) |
| Phase 5 (polish + release) | D15 (assimp CMake final), D16 (ReaPack manifest final), D17 (install validation) |

**Cross-component dependencies:**

- D2 (RAII handles) gates D4 (reload swap is trivial only because of RAII)
- D1 (flat bone layout) gates D13 (single linear pass for global matrices)
- D3 (coord convention) gates D11 (FBO render result matches ImGui sampling expectations) and D13 (skinning math correctness)
- D11 (FBO bridge) gates everything in Phase 1+ — it's the foundation for visible output
- D8 (PCM_source) gates D9 (per-item persistence rides on PCM_source virtuals)
- D15 (assimp narrowing) gates D17 (single-DLL install only works because assimp is statically linked)

## Implementation Patterns & Consistency Rules

### Authoring Note

The BMAD template categories here (DB naming, REST endpoint format, JSON wrapper) are web/SaaS-oriented and do not apply to a native C++ Reaper extension. This section remaps the spirit of the step — *codify conventions so downstream phases don't drift* — to the C++ / native plugin domain. Conventions are aligned with what Phase 0 already established in `src/plugin_main.cpp` and friends.

### Pattern Categories Defined

Single-developer (Claude) implementation across 5 phases. The "AI agent conflict" risk reduces to *future-Claude consistency drift across phases*. Patterns below freeze that drift.

### Naming Patterns

| Symbol kind | Convention | Example | Rationale |
|---|---|---|---|
| Types (struct, class, enum class) | PascalCase | `Asset`, `SceneMesh`, `LoadErrorCategory` | Standard C++ idiom, matches GLM/sokol_gfx user-facing types |
| Functions (free, member) | PascalCase | `OpenViewerWindow`, `LoadAsset`, `OnHookCommand` | Established by Phase 0 |
| Variables (locals, members) | snake_case | `command_id`, `tex_handle`, `model_root` | C-idiom, contrasts with PascalCase functions |
| Globals (file-scope) | `g_` prefix + snake_case | `g_command_id`, `g_reaper_main`, `g_hinstance` | Established by Phase 0; visual flag for "this lives forever" |
| Constants (compile-time) | `k` prefix + PascalCase | `kCommandName`, `kActionDesc`, `kMaxBones` | Established by Phase 0; Google-style |
| Macros (avoid; only for `REAPER_PLUGIN_ENTRYPOINT` etc.) | UPPER_SNAKE | (only SDK macros) | We don't introduce new macros — SDK already provides too many |
| File-internal symbols | anonymous namespace | `namespace { ... }` inside `.cpp` | Established by Phase 0 |
| Project namespace | `fbxav` (top-level) | `namespace fbxav { ... }` wrapping all our code | Established by Phase 0 |
| SDK types (untouched) | follow Reaper SDK style | `gaccel_register_t`, `REAPER_PLUGIN_HINSTANCE`, `PCM_source` | Don't rewrap SDK types |

### Structural Patterns

**File layout:**

- `src/<feature>.h` + `src/<feature>.cpp` paired. One feature per pair (e.g. `asset_loader.h`, `viewer_panel.h`, `pcm_source_anim.h`).
- Headers self-contained (each compiles standalone if `#include`d first). Include what you use, forward-declare what you can.
- No precompiled headers. No header-only project files (third-party header-only deps live in `extern/`).
- Tests, if added post-MVP, go in `tests/<feature>_test.cpp`. Not in MVP scope.

**SPDX header on every source file** (.h, .cpp, CMake fragments):

```
// SPDX-License-Identifier: MIT
//
// <One-line file purpose>. <Optional second line for phase context.>
```

**Comment discipline:**

- Default: write no comments. Identifiers must carry their meaning.
- Comment ONLY when the WHY is non-obvious: a hidden constraint, a subtle invariant, a workaround for a specific Reaper SDK quirk, behavior that would surprise a reader.
- Never narrate WHAT the code does. Never reference task / commit / story IDs.
- One-line comments preferred. Multi-line only for explaining a tricky algorithm boundary (e.g. coordinate conversion math).

### Format Patterns

**Console log format (codifies D7):**

```
[FBXAV] <level>: <message>
```

- Levels: `info` | `warn` | `error` — no others.
- Path/context inline, no multi-line indentation:
  - `[FBXAV] error: load failed C:/anim/dragon_v11.glb: ParseFailed: Unexpected EOF at offset 124`
- Single-line per event. Reaper console concatenates multi-line entries poorly.
- Helper macro/function: `fbxav::LogInfo(fmt, ...)`, `LogWarn`, `LogError` — all funnel through `ShowConsoleMsg` after `snprintf` into a stack buffer.

**Result type (codifies D6):**

```cpp
struct LoadResult {
    std::optional<Asset> asset;
    LoadErrorCategory category = LoadErrorCategory::Ok;
    std::string detail;
};
```

- Functions that can fail return `LoadResult` (heavy) or `bool` + `std::string& out_error` (light internal).
- No throwing across subsystem boundaries.

**Project ext-state line format (codifies D9 per-item):**

```
key=value
```

One key per line, no nesting. Unknown keys ignored on load (forward-compat). Known keys per PCM_source:

```
file=<absolute or project-relative path>
cam_target=x,y,z
cam_distance=d
cam_yaw=y
cam_pitch=p
time_offset=t
time_scale=s
```

### Communication Patterns

**Reaper API resolution (codifies D10):**

```cpp
// in reaper_api.h
#define REAPERAPI_IMPLEMENT
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_ShowConsoleMsg
// + per-phase WANT_* additions
#include "../extern/reaper-sdk/sdk/reaper_plugin_functions.h"
```

- Add a single `WANT_*` define per symbol needed. Never link statically.
- The loader runs at `ReaperPluginEntry(rec)` with `caller_version == 0x20E` guard.
- Symmetric unregister: every `rec->Register("name", ptr)` is paired with `rec->Register("-name", ptr)` on unload (`rec == nullptr` second call). Maintain a list of registrations at the top of `plugin_main.cpp` so the inverse path is obvious.

**ReaImGui API resolution (Phase 0.5+):**

Same pattern as Reaper API. Vendored binding header at `extern/reaimgui/reaper_imgui_functions.h` (pinned to v0.10.0.5). Each `ImGui_*` function resolved via `rec->GetFunc("ImGui_Begin")` at extension load. Document the resolution failure as a fatal-but-graceful: log to console, do not register our action, return 0 (per PRD FR30).

### Process Patterns

**No exceptions across Reaper boundaries (codifies D5):**

```cpp
// Every function callable from Reaper has this contract at its top:
extern "C" int ReaperPluginEntry(...) noexcept { ... }
bool OnHookCommand(int command, int flag) noexcept { ... }
// PCM_source virtual methods: implementations marked noexcept where the SDK signature allows
```

Internal call sites that touch assimp wrap with try/catch and translate to LoadResult.

**Resource ownership (codifies D2):**

- Every GPU handle wrapped in a single-responsibility RAII struct.
- Move-only (`= default` for move ctor / move-assign; `= delete` for copy ctor / copy-assign).
- Destructor calls `sg_destroy_*` ONLY if the handle is valid (`h.id != 0`).
- Asset is move-constructible / move-assignable so `std::swap(item.asset, tempAsset)` in D4 reload is trivially safe.

**No allocation in hot paths after Phase 1:**

- Render loop, animation sampling, bone matrix update: no `new`/`malloc`/`std::vector::push_back`-on-empty.
- Preallocate at load time: bone matrix array sized to `skeleton.bones.size()`, uniform staging buffer, etc.
- Acceptable: `std::string` for log lines (allocation happens once per error event, not per frame).

**Build hygiene (codifies NFR-R5):**

- MSVC `/W3 /permissive-`. Every new warning gates the phase.
- No PCH. No `using namespace std;` in headers (free in `.cpp` if scoped to a function).
- CMake: `target_link_libraries(reaper_fbxanimationviewer PRIVATE ...)` for every dep — no `PUBLIC` propagation.
- No `file(GLOB ...)` for sources — list explicitly so adds/removes appear in git diffs.

### Enforcement Guidelines

**All implementation work (any phase) MUST:**

- Pass `cmake --build build --config Release` with zero `/W3` warnings.
- Maintain symmetric register/unregister for every Reaper API surface added.
- Funnel all user-visible errors through `[FBXAV] <level>:` console lines.
- Wrap every GPU handle in a RAII struct (no raw `sg_*_t` lifetimes outside Asset members).
- Avoid letting exceptions escape any function reachable from a Reaper callback.

**Pattern violation discovery:** logged inline in the deferred-work tracker (`_bmad-output/implementation-artifacts/deferred-work.md`), not silently fixed mid-phase. Architectural drift gets a Spec Change Log entry on the active phase spec, not a casual rename.

**Pattern updates:** patterns evolve through architecture-doc amendments (this file), not through ad-hoc refactors. New conventions go through the same architectural decision channel as D1–D17.

### Pattern Examples

**Good — RAII GPU handle:**

```cpp
struct GpuImage {
    sg_image h{};
    GpuImage() = default;
    GpuImage(GpuImage&& o) noexcept : h(o.h) { o.h = {}; }
    GpuImage& operator=(GpuImage&& o) noexcept {
        if (this != &o) { if (h.id) sg_destroy_image(h); h = o.h; o.h = {}; }
        return *this;
    }
    GpuImage(const GpuImage&) = delete;
    GpuImage& operator=(const GpuImage&) = delete;
    ~GpuImage() { if (h.id) sg_destroy_image(h); }
};
```

**Good — symmetric registration:**

```cpp
// load
g_command_id = g_register("command_id", const_cast<char*>(kCommandName));
g_register("gaccel", &g_accel);
g_register("hookcommand", reinterpret_cast<void*>(&OnHookCommand));
// ... record these in a list ...

// unload (rec == nullptr)
g_register("-hookcommand", reinterpret_cast<void*>(&OnHookCommand));
g_register("-gaccel", &g_accel);
// command_id is freed by Reaper on shutdown — no inverse needed
```

**Anti-pattern — silent exception swallowing:**

```cpp
// BAD: hides root cause, leaves Reaper guessing
try { LoadAsset(path); } catch (...) { /* nothing */ }

// GOOD: translate to result + console
LoadResult LoadAsset(const std::string& path) {
    try { /* ... assimp work ... */ }
    catch (const std::exception& e) {
        return { std::nullopt, LoadErrorCategory::ParseFailed, e.what() };
    }
}
```

**Anti-pattern — raw GPU lifetime:**

```cpp
// BAD: who owns h? When does sg_destroy_image fire?
sg_image LoadTexture(const aiTexture* t) { ... return img; }

// GOOD: ownership encoded in the type
GpuImage LoadTexture(const aiTexture* t) { ... return img; }
```

**Anti-pattern — narration comments:**

```cpp
// BAD
// This function loops over all the bones and computes their global matrices
// by multiplying with the parent matrix.
void ComputeGlobalBones(...) { ... }

// GOOD: identifier carries meaning; comment only if there's a non-obvious WHY.
void ComputeGlobalBones(...) { ... }

// If there IS a non-obvious WHY, e.g.:
// glTF inverseBindMatrix is already in mesh-relative space, so we don't
// premultiply by the scene-root transform here — see KHR_skinning.
void ComputeSkinningPalette(...) { ... }
```

## Project Structure & Boundaries

### Complete Project Directory Structure (MVP target — Phase 5 end-state)

```
fbxanimationviewer/
├── .gitignore
├── .gitmodules                  # Phase 1 — assimp submodule (vendoring kept for headers-only deps)
├── CMakeLists.txt               # top-level: target def, /W3 /permissive-, assimp narrowing (D15)
├── LICENSE.md                   # MIT, Demute Studio
├── README.md                    # install + build, written at Phase 5
├── cmake/
│   └── ReaperPlugin.cmake       # add_reaper_extension() helper — enforces reaper_ prefix, C++17, MSVC flags
├── docs/
│   ├── PHASE0_VALIDATOR_GATE.md
│   ├── PHASE05_VALIDATOR_GATE.md   # Phase 0.5
│   ├── PHASE1_VALIDATOR_GATE.md    # Phase 1
│   ├── PHASE2_VALIDATOR_GATE.md    # Phase 2
│   ├── PHASE3_VALIDATOR_GATE.md    # Phase 3
│   ├── PHASE4_VALIDATOR_GATE.md    # Phase 4
│   └── PHASE5_VALIDATOR_GATE.md    # Phase 5 / release
├── extern/
│   ├── VENDORED.md              # upstream URLs, pinned commits/tags, re-vendoring procedure
│   ├── reaper-sdk/sdk/          # vendored Phase 0 — reaper_plugin.h + reaper_plugin_functions.h
│   ├── reaimgui/                # Phase 0.5 — pinned binding header at v0.10.0.5
│   ├── sokol/                   # Phase 0.5 — sokol_gfx.h (+ sokol_gl.h optional) pinned at commit 85d1f1b
│   ├── glm/                     # Phase 1 — header-only at 1.0.3
│   └── assimp/                  # Phase 1 — submodule at v6.0.5, importers narrowed (D15)
├── reapack/
│   └── index.xml                # ReaPack manifest, declares cfillion/reaimgui >= 0.10.0.5 dep (D16)
├── src/
│   ├── plugin_main.cpp          # Phase 0 — REAPER_PLUGIN_ENTRYPOINT, action registration, hookcommand, symmetric unregister
│   ├── reaper_api.h             # Phase 0 — REAPERAPI_MINIMAL + WANT_* defines (expanded per phase, D10)
│   ├── reaper_api.cpp           # Phase 0 — REAPERAPI_LoadAPI() call
│   ├── imgui_api.h              # Phase 0.5 — ReaImGui binding API loader
│   ├── imgui_api.cpp            # Phase 0.5
│   ├── log.h                    # Phase 0.5 — LogInfo/Warn/Error funneling to ShowConsoleMsg (D7)
│   ├── viewer_panel.h           # Phase 0.5 — ReaImGui dockable panel host (supersedes viewer_window)
│   ├── viewer_panel.cpp         # Phase 0.5 — panel begin/end, FBO bridge (D11), input routing
│   ├── renderer.h               # Phase 0.5 → Phase 2 — sokol_gfx context, FBO, pipelines, draw
│   ├── renderer.cpp             # Phase 0.5 (FBO + clear), Phase 1 (mesh draw + Blinn-Phong), Phase 2 (+ skinning)
│   ├── gpu_resources.h          # Phase 1 — RAII wrappers: GpuBuffer, GpuImage, GpuPipeline, GpuShader (D2)
│   ├── scene.h                  # Phase 1 — Asset/SceneMesh/SceneMaterial/SceneSkeleton/SceneAnimation types (D1)
│   ├── asset_loader.h           # Phase 1 — LoadAsset(path) → LoadResult (D6); assimp boundary
│   ├── asset_loader.cpp         # Phase 1 (static + textures), Phase 2 (skin + anim parsing)
│   ├── camera.h                 # Phase 1 — orbit camera state, view matrix, reset-to-bbox (D14)
│   ├── camera.cpp               # Phase 1
│   ├── animation.h              # Phase 2 — animation sampling + bone palette compute (D13)
│   ├── animation.cpp            # Phase 2
│   ├── pcm_source_anim.h        # Phase 3 — PCM_source subclass declaration
│   ├── pcm_source_anim.cpp      # Phase 3 — factory registration, SaveState/LoadState (D9), GetLength etc. (D8)
│   ├── project_state.h          # Phase 4 — project-level state hooks (if needed beyond PCM_source ext-state)
│   └── project_state.cpp        # Phase 4
├── _bmad/                       # BMAD config + custom overrides (not shipped in DLL)
├── _bmad-output/
│   ├── planning-artifacts/
│   │   ├── prfaq-FBXAnimationViewer.md
│   │   ├── prd.md
│   │   ├── architecture.md      # THIS document
│   │   ├── epics.md             # Phase 1 step output
│   │   └── stories/             # Phase 1 step output (per-story file)
│   └── implementation-artifacts/
│       ├── spec-phase-0-scaffolding.md   # done
│       ├── spec-phase-05-reaimgui-refactor.md
│       ├── spec-phase-1-static-mesh.md
│       ├── spec-phase-2-skinned-animation.md
│       ├── spec-phase-3-transport-sync.md
│       ├── spec-phase-4-reload-persistence.md
│       ├── spec-phase-5-polish-release.md
│       └── deferred-work.md
└── build/                       # gitignored — CMake output (Visual Studio sln + Release/ artifacts)
```

### Requirements to Structure Mapping

Mapping PRD FR groups → owning subsystem files:

| FR group | Files | Primary phase |
|---|---|---|
| **FR1–FR7** Animation Loading & Format Support | `asset_loader.{h,cpp}`, `scene.h`, `extern/assimp/`, `extern/VENDORED.md` (assimp pinning) | Phase 1 (static), Phase 2 (skinned) |
| **FR8–FR14** Timeline Integration | `pcm_source_anim.{h,cpp}` | Phase 3 |
| **FR15–FR20** 3D Rendering & Visual Fidelity | `renderer.{h,cpp}`, `gpu_resources.h`, `scene.h`, `animation.{h,cpp}` (for FR15 skinning) | Phase 1 (static), Phase 2 (skinned) |
| **FR22–FR26** Camera & Viewport Control | `camera.{h,cpp}` | Phase 1 |
| **FR27–FR30** Panel & Docking | `viewer_panel.{h,cpp}`, `imgui_api.{h,cpp}`, `plugin_main.cpp` (action reg + ReaImGui detect) | Phase 0.5 |
| **FR31–FR33** Project State Persistence | `pcm_source_anim.cpp` (SaveState/LoadState), `project_state.{h,cpp}` if needed, ReaImGui handles panel dock | Phase 3 (per-item) + Phase 4 (project-level) |
| **FR34–FR37** Error Tolerance & Graceful Degradation | `asset_loader.cpp` (try/catch boundary), `log.h` (diagnostics), per-PCM_source instance error state | All phases (cross-cutting) |
| **FR38–FR41** Distribution & Operation | `reapack/index.xml`, `CMakeLists.txt` (single-DLL static link), `README.md` | Phase 5 |

| NFR group | Where enforced |
|---|---|
| **NFR-P1–P6** Performance | `renderer.cpp` (60 fps render loop, MSAA, FBO resize), `animation.cpp` (CPU bone matrix cost), per-phase validator gates |
| **NFR-R1–R5** Reliability | Project-wide: noexcept boundaries (D5), RAII (D2), symmetric register (D10), `/W3 /permissive-` enforced in `cmake/ReaperPlugin.cmake` |
| **NFR-C1–C5** Compatibility | `plugin_main.cpp` (caller_version check), `reapack/index.xml` (ReaImGui dep), `CMakeLists.txt` (MSVC x64 only), `extern/VENDORED.md` (assimp pinned) |

### Architectural Boundaries

**Layers (bottom → top):**

```
┌──────────────────────────────────────────────────────────────┐
│  Reaper host process                                         │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  reaper_fbxanimationviewer.dll                         │  │
│  │  ┌─────────────────────────────────────────────────┐   │  │
│  │  │  plugin_main.cpp — entry, lifecycle              │   │  │
│  │  │  ├── reaper_api (REAPERAPI_MINIMAL + WANT_*)     │   │  │
│  │  │  ├── imgui_api  (ReaImGui binding resolution)    │   │  │
│  │  │  └── log        (ShowConsoleMsg funnel)          │   │  │
│  │  ├─────────────────────────────────────────────────┤   │  │
│  │  │  pcm_source_anim — Reaper PCM_source contract    │   │  │
│  │  │   (owns: Asset, camera state, time mapping)      │   │  │
│  │  ├─────────────────────────────────────────────────┤   │  │
│  │  │  viewer_panel — ReaImGui panel host              │   │  │
│  │  │   (queries current PCM_source from playhead)     │   │  │
│  │  │   ├── renderer (sokol_gfx + FBO)                 │   │  │
│  │  │   ├── camera                                     │   │  │
│  │  │   └── animation (sampling, bone palette)         │   │  │
│  │  ├─────────────────────────────────────────────────┤   │  │
│  │  │  asset_loader — assimp boundary                  │   │  │
│  │  │   → scene types + gpu_resources (RAII)           │   │  │
│  │  └─────────────────────────────────────────────────┘   │  │
│  │                       │                                │  │
│  │   statically linked: assimp + sokol_gfx + GLM         │  │
│  └────────────────────────────────────────────────────────┘  │
│                          │                                   │
│   runtime dependency: cfillion/reaimgui (loaded by Reaper)  │
└──────────────────────────────────────────────────────────────┘
```

**Boundary rules:**

- **`asset_loader.cpp` is the only file that includes `<assimp/...>` headers.** Everything else consumes the `scene.h` types. This contains all exception risk at one site (D5).
- **`renderer.cpp` is the only file that calls `sg_*` functions.** Everything else consumes the renderer's interface (init/shutdown, draw scene, resize FBO). GPU handle creation always returns RAII wrappers from `gpu_resources.h`.
- **`viewer_panel.cpp` is the only file that calls `ImGui_*` functions.** Renderer never touches ImGui directly; it returns an offscreen `sg_image` handle that the panel consumes via `ImGui::Image()`.
- **`pcm_source_anim.cpp` is the only file that subclasses Reaper's `PCM_source`.** It is the integration boundary with Reaper's timeline/transport.
- **`plugin_main.cpp` is the only file that calls `rec->Register(...)`.** All other modules expose their registration needs as functions that `plugin_main` calls at load and inverts at unload — making the symmetric-register invariant (D10, NFR-R3) statically auditable.
- **`log.h` is the only header that wraps `ShowConsoleMsg`.** No bare `ShowConsoleMsg` calls outside `log.cpp` — keeps the `[FBXAV] level:` format invariant (D7).

### Integration Points

**Reaper-facing surfaces (entries Reaper drives into our code):**

1. `ReaperPluginEntry(rec)` — DLL load and unload (twice; second with `rec==nullptr`).
2. `OnHookCommand(command, flag)` — action dispatch when user runs `FBXAV: Open Viewer Window`.
3. PCM_source factory function — invoked by Reaper when user drops a `.glb`/`.gltf`/`.fbx` on a track.
4. PCM_source virtual methods — `GetLength`, `SaveState`, `LoadState`, etc., called during project save/load and timeline operations.
5. ReaImGui frame callbacks (Phase 0.5+) — invoked each frame Reaper renders, we draw our panel inside.

**Internal data flow per render frame (Phase 0.5+):**

```
Reaper main loop tick
  └─> ReaImGui frame
        └─> viewer_panel::Draw()
              ├─> determine current PCM_source from playhead position (FR14)
              ├─> read camera state from panel
              ├─> sample animation at currentTime → bone palette  [animation.cpp]
              ├─> renderer::BeginFrame() / BeginOffscreenPass()
              ├─> renderer::DrawScene(asset, camera, bonePalette)
              ├─> renderer::EndOffscreenPass() → produces sg_image
              ├─> ImGui::Image(textureHandle, panelSize, uv_flip)
              ├─> handle camera input on the image rect (orbit/zoom/pan)
              └─> ImGui::End()
```

**External integrations (dependencies):**

- **Reaper SDK** — vendored headers; bound at runtime via `rec->GetFunc()`.
- **ReaImGui** — runtime ReaPack dep; bound at runtime via `rec->GetFunc("ImGui_*")`.
- **assimp** — static link, called only from `asset_loader.cpp`.
- **sokol_gfx** — header-only static, called only from `renderer.cpp`.
- **GLM** — header-only static, math used pervasively but with no global state.

**No external integrations (per NFR offline 100%):**

- No HTTP, no sockets, no DNS, no clipboard, no shell exec, no telemetry, no analytics, no license check.

### File Organization Patterns

**Configuration files:**

- `CMakeLists.txt` — top-level only. No nested CMakeLists in `src/` (sources listed explicitly per pattern in D17 / build hygiene).
- `cmake/ReaperPlugin.cmake` — helper module included by top-level.
- `.gitignore` — VS artifacts, `build/`, `out/`, `.vs/`, `*.user`.
- `.gitmodules` — assimp submodule only (Phase 1+).
- `extern/VENDORED.md` — manual record of upstream URLs and pinned commits/tags for header-only vendored deps.

**Source organization:**

- Flat `src/` — no subdirectories. ~20–25 files total at MVP end. Subdirectory introduction triggered only post-MVP if/when file count exceeds ~40.
- Header + cpp pair per feature module. No header-only modules in `src/` (the codebase is small enough that compile units stay separate for clarity).
- Public headers (consumed by sibling modules) live alongside their `.cpp`. No `include/` separation — there's no SDK to ship beyond the DLL itself.

**Test organization:**

- Not in MVP scope. Post-MVP `tests/` directory with one fixture file per feature.
- Pre-MVP validation strategy: per-phase validator gate in `docs/PHASE<N>_VALIDATOR_GATE.md`, run by Antho against real Reaper.

**Asset organization:**

- No bundled assets in the DLL — the extension reads user-supplied `.glb`/`.gltf`/`.fbx` files at runtime.
- ReaPack manifest at `reapack/index.xml` is the only "asset" we ship beyond the DLL.

### Development Workflow Integration

**Development server structure:** N/A — native DLL, hosted by Reaper. "Dev loop" = build DLL → copy to `%APPDATA%\REAPER\UserPlugins\` → restart Reaper → trigger action → observe.

**Build process structure:**

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
copy build\Release\reaper_fbxanimationviewer.dll "%APPDATA%\REAPER\UserPlugins\"
```

CMake target hierarchy:

- `reaper_fbxanimationviewer` (SHARED LIBRARY) — our DLL
  - links PRIVATE: `assimp` (static), `opengl32`, `gdi32`, `user32`
  - includes: `extern/reaper-sdk/sdk`, `extern/reaimgui`, `extern/sokol`, `extern/glm`, `extern/assimp/include`
  - sources: explicit list from `src/*.cpp` (no `file(GLOB ...)`)
- `assimp` (STATIC LIBRARY) — narrowed (D15), built as part of our build via `add_subdirectory(extern/assimp EXCLUDE_FROM_ALL)`

**Deployment structure:**

- Single-DLL output: `build/Release/reaper_fbxanimationviewer.dll` (~5–15 MB once assimp is statically linked).
- ReaPack publishes the DLL plus `index.xml`; users install via ReaPack subscription (D17).
- No installer, no MSI, no signed binary requirement (Reaper does not enforce signing).

## Architecture Validation Results

### Coherence Validation ✅

**Decision compatibility:** All 17 architectural decisions (D1–D17) compose without contradiction. Verified pairwise dependencies:

- D1 (flat bone layout) ↔ D13 (skinning) — single linear pass for global matrices is enabled by parent-index storage
- D2 (per-item RAII) ↔ D4 (swap-on-reload) — RAII makes `std::swap(item.asset, tempAsset)` trivially correct
- D3 (column-major, Y-up RH) ↔ D11 (FBO bridge) — coherent; explicit UV-flip handles GL bottom-up FBO vs ImGui top-down sampling
- D5 (no-throw boundary) ↔ D6 (LoadResult) — exceptions translated at one boundary, propagated as values internally
- D7 (console format) ↔ all error paths — `log.h` funnel enforces `[FBXAV] level:` invariant
- D8 (PCM_source) ↔ D9 (SaveState/LoadState) — per-item state rides on PCM_source virtuals
- D10 (loader expansion) ↔ Phase commitments — each phase activates exactly the symbols its features need
- D12 (forward + MSAA) ↔ D11 (FBO) — MSAA offscreen pass uses a resolve attachment; the non-MSAA resolved texture is what ImGui samples (see Gap-1 below)
- D13 (matrix palette uniform) ↔ D1 (flat bones) — naturally aligned data layout
- D14 (orbit camera) ↔ D11 (input on ImGui::Image rect) — input routed via `ImGui::IsItemHovered()` / `IsMouseDragging` on the panel image
- D15 (assimp narrowing) ↔ D17 (single-DLL install) — coherent: static-linked narrowed assimp is what makes the single-DLL deliverable possible

**Pattern consistency:** Implementation patterns (naming, RAII, no-throw boundary, console funnel, symmetric register) all align with the architectural decisions they support. No naming convention conflicts identified.

**Structure alignment:** The flat `src/` layout maps to architectural boundaries one file per subsystem; the boundary rules in step 6 ("only file X includes Y") are statically auditable.

### Requirements Coverage Validation ✅

**Functional Requirements (41/41 covered):**

| Group | Status | Implementing files |
|---|---|---|
| FR1–FR7 Animation Loading & Format Support | ✅ | `asset_loader`, `scene`, `extern/assimp` (narrowed via D15) |
| FR8–FR14 Timeline Integration | ✅ | `pcm_source_anim` (D8); multi-item via per-item PCM_source instances (D2) |
| FR15–FR20 3D Rendering & Visual Fidelity | ✅ | `renderer` (D12) + `animation` (D13) + RAII via `gpu_resources` (D2) |
| FR21 (recategorized to NFR-P1) | ✅ | per-phase validator gate |
| FR22–FR26 Camera & Viewport Control | ✅ | `camera` (D14) + `viewer_panel` input routing |
| FR27–FR30 Panel & Docking | ✅ | `viewer_panel` (D11) + ReaImGui detect in `plugin_main` (FR30) |
| FR31–FR33 Project State Persistence | ✅ | `pcm_source_anim::SaveState/LoadState` (D9) + ReaImGui dock state native |
| FR34–FR37 Error Tolerance & Graceful Degradation | ✅ | no-throw (D5) + `LoadResult` (D6) + `log.h` (D7) + per-item containment (D4) |
| FR38–FR41 Distribution & Operation | ✅ | `reapack/index.xml` (D16) + single-DLL static link (D15) + offline-by-construction |

**Non-Functional Requirements (17/17 covered):**

| NFR | Architectural address |
|---|---|
| NFR-P1 60 fps @ 20k tris / 4 mats / 50 bones | D12 (forward, MSAA 4x, opaque-only) + per-phase validator gate |
| NFR-P2 ≤2 s initial load | Synchronous parse + GPU upload on main thread |
| NFR-P3 ≤1 s manual reload | D4 (atomic swap with RAII destruction of old GPU resources) |
| NFR-P4 ≤2 s DLL load | Phase 0 baseline maintained — no heavy init work in entry path |
| NFR-P5 ≤16 ms scrub latency | Render every frame, no async between playhead read and draw |
| NFR-P6 ≥10 simultaneous items | D2 (per-item RAII, VRAM budget ~13 MB × 10 = 130 MB ≪ modern GPU budget) |
| NFR-R1 zero host crash | D5 (noexcept boundary) + D2 (RAII) + try/catch at assimp boundary |
| NFR-R2 no project data loss | D9 (SaveState only writes our own chunk; never touches other Reaper state) |
| NFR-R3 clean unload | D10 (symmetric register) + D2 (RAII destruction order) + sokol_gfx shutdown sequence |
| NFR-R4 non-lethal failures | D4 (per-item containment) + FR37 (single-item failure isolation) |
| NFR-R5 `/W3 /permissive-` warning-free | Enforced in `cmake/ReaperPlugin.cmake` from Phase 0 |
| NFR-C1 Reaper 7.x SDK 0x20E | `plugin_main.cpp` caller_version check (Phase 0) |
| NFR-C2 ReaImGui auto-install dep | D16 (reapack manifest schema, exact syntax verified in Phase 5) |
| NFR-C3 MSVC x64 only | CMake target setup, no mingw/clang-cl/32-bit |
| NFR-C4 assimp pinned | D15 (v6.0.5) + `extern/VENDORED.md` |
| NFR-C5 FBX 60% target | Per-phase validation against Demute fixture corpus (Phase 4 dedicated FBX validation) |

### Implementation Readiness Validation ✅

**Decision completeness:** All 17 decisions documented with rationale and dependency map. Versions pinned where applicable (D15 assimp v6.0.5, D16 ReaImGui ≥v0.10.0.5, sokol commit `85d1f1b`, GLM 1.0.3).

**Structure completeness:** Project tree enumerated at Phase 5 end-state granularity (24 source files + 7 validator gates + per-phase implementation specs). Each file's owning phase and FR-group mapping documented.

**Pattern completeness:** Naming, file layout, error handling, RAII, register-symmetry, no-throw boundary, console format, ext-state format all codified with concrete examples and anti-patterns.

### Gap Analysis Results

**Critical Gaps:** None.

**Important Gaps (3 — all scheduled into owning phases, no blocker for moving forward):**

1. **MSAA + FBO interaction in D11 / D12.** The MSAA color attachment is not directly sampleable by `ImGui::Image()` — sokol_gfx requires a resolve attachment (multisample → resolve to non-MSAA texture) before the texture is consumable. Owner phase: Phase 0.5 day 1. Mitigation: documented in `spec-phase-05-reaimgui-refactor.md` (to be authored at Phase 0.5 spec stage); fallback if blocking is MSAA off (1× sample).
2. **PCM_source registration mechanism in D8.** The exact SDK signature for registering a PCM_source factory tied to file extensions (`.glb`, `.gltf`, `.fbx`) needs verification against `reaper_plugin_functions.h` + reference implementations (SWS, ReaImGui where applicable). Owner phase: Phase 3 design spike (1–2h before implementation).
3. **ReaPack manifest auto-install dep syntax in D16.** Exact XML schema for declaring a runtime auto-install dependency on `cfillion/reaimgui` — between `<link rel="dependency">` and metadata `dep` block. Owner phase: Phase 5 packaging step. Mitigation: inspect existing cfillion packages and popular ReaImGui-dependent extensions (Reaticulate, ReaPack Browser).

**Nice-to-have Gaps:**

- Dear ImGui transitive version not explicitly pinned by our manifest dep (we constrain `cfillion/reaimgui >= v0.10.0.5`; ImGui API surface follows from ReaImGui's binding). Acceptable per current contract; vendored binding header is the compile-time pin.
- Phase 0 `viewer_window.{h,cpp}` files are scheduled for removal in Phase 0.5 (superseded by `viewer_panel.{h,cpp}`). Already flagged in `deferred-work.md` 2026-05-10 entry; no architectural risk.

### Validation Issues Addressed

Antho explicitly reviewed gap analysis and post-MVP scope at the validation menu and accepted all three Important Gaps as appropriately scoped into their owning phases (no escalation requested).

### Architecture Completeness Checklist

**Requirements Analysis**

- [x] Project context thoroughly analyzed
- [x] Scale and complexity assessed
- [x] Technical constraints identified
- [x] Cross-cutting concerns mapped

**Architectural Decisions**

- [x] Critical decisions documented with versions
- [x] Technology stack fully specified
- [x] Integration patterns defined
- [x] Performance considerations addressed

**Implementation Patterns**

- [x] Naming conventions established
- [x] Structure patterns defined
- [x] Communication patterns specified
- [x] Process patterns documented

**Project Structure**

- [x] Complete directory structure defined
- [x] Component boundaries established
- [x] Integration points mapped
- [x] Requirements to structure mapping complete

### Architecture Readiness Assessment

**Overall Status:** **READY FOR IMPLEMENTATION**
(All 16 checklist items `[x]`; no Critical Gaps; the 3 Important Gaps are scheduled into their owning phases with documented mitigation.)

**Confidence Level:** **High** — based on (a) Phase 0 already shipped and validated against an 8-row gate, (b) 41/41 FR + 17/17 NFR coverage with explicit file mapping, (c) every architectural decision derived from explicit PRD/PRFAQ constraints rather than invented, (d) 3 gaps identified are known-unknowns with concrete spike plans, not unknown-unknowns.

**Key Strengths:**

- Brownfield foundation: Phase 0 has already de-risked the most uncertain layer (Reaper plugin entry, build toolchain, action lifecycle).
- Single-developer (Claude) implementation means coordination overhead is zero — patterns are enforced by one consistent author.
- Boundary discipline: each subsystem has exactly one file that touches its external dependency (`asset_loader` ↔ assimp, `renderer` ↔ sokol_gfx, `viewer_panel` ↔ ReaImGui, `pcm_source_anim` ↔ Reaper PCM_source). Failure containment is structural, not aspirational.
- Validator-bound critical path is honest: every phase ends at a real Reaper test by Antho, not at a passing unit test suite.

**Areas for Future Enhancement (post-MVP):**

- Asset cache to deduplicate VRAM when the same file is on multiple tracks
- File-watcher auto-reload (PRD Growth Features)
- Per-project axis/scale remap (PRD Growth Features)
- Worker thread for asset loading on fixtures exceeding 1s parse time
- Normal maps and richer material shading toward simplified PBR
- Mac/Linux ports via ReaImGui+SWELL and sokol_gfx Metal/GL backends
- Multi-clip animation support within one source file
- Alembic format support (PRFAQ Phase 2)
- Engine-side export helpers (Unity/Unreal one-click batch export)

### Implementation Handoff

**AI agent guidelines (Claude as implementing developer):**

- Treat D1–D17 as decisions, not suggestions. Deviation requires a Spec Change Log entry on the active phase spec, with the trigger / amendment / KEEP discipline used in Phase 0's `spec-phase-0-scaffolding.md`.
- Implementation patterns from step 5 are the consistency contract. Naming, RAII, no-throw boundary, console funnel, symmetric register — every PR must hold them.
- Boundary rules from step 6 are statically auditable. If a new `.cpp` file needs to `#include <assimp/...>`, that is an architectural change, not a coincidence — propose it as a spec amendment first.
- Per-phase validator gates in `docs/PHASE<N>_VALIDATOR_GATE.md` are the only authority on phase completion. Code that passes type-check and warning-check but fails the validator gate is not done.

**First implementation priority:**

Phase 0.5 — ReaImGui refactor. Specifically: stand up the FBO ↔ `ImGui::Image()` bridge end-to-end (D11) with the MSAA-resolve detail verified on day 1, and replace the Phase 0 Win32 viewer window with a dockable ReaImGui panel that renders a fixed clear color through the FBO bridge. Validator gate: equivalent to Phase 0's 8-row gate, adapted for panel + docking instead of top-level window.

Phase 0.5 spec to be authored next (`_bmad-output/implementation-artifacts/spec-phase-05-reaimgui-refactor.md`) following the same template as `spec-phase-0-scaffolding.md`.

## Spec Change Log

### 2026-06-23 — Spike 0 findings: viewport is a docked OpenGL window, not a ReaImGui panel (supersedes D11/AR10; amends D12)

**Trigger:** Spike 0 (throwaway feasibility prototype, branch `spike/0-1-feasibility`, verdict **GO** — see `docs/SPIKE0_FINDINGS.md`) measured on the reference workstation (Ryzen 9 9900X / RX 9070):
- A **ReaImGui panel is hard-capped ~30 fps** for displayed content (its presentation is tied to Reaper's ~30 Hz UI loop; feeding it at ~66 Hz still showed ~32 fps), and ReaImGui **cannot display a foreign GPU texture** (its image API copies pixel data; there is no `DrawList` custom-callback in the binding).
- A **native OpenGL window docked via `DockWindowAddEx`**, rendering directly and presenting via `SwapBuffers` on an independent ~66 Hz timer, reached **~62 fps** at 1055×604.

**Amendment:**
- **D11 / AR10 superseded.** The 3D viewport is a **native OpenGL window docked into Reaper via `DockWindowAddEx`**, rendering the scene directly into the window framebuffer (no offscreen FBO, no `ImGui::Image()` bridge, no CPU readback), driven by a frame timer independent of Reaper's UI loop.
- **D12 amended.** Render targets the window's default framebuffer; the offscreen-FBO + MSAA-resolve + readback rows are obsolete. Forward / opaque / depth-test / back-face-cull / shader choices stand. MSAA, if wanted, comes from the window pixel format.
- **ReaImGui dependency deferred to Epic 5.** ReaImGui is no longer needed for the viewport. It is introduced at the **animation browser (Epic 5)** — the first genuine ReaImGui panel — which is where AR3 / NFR-C2 (the `cfillion/reaimgui` ReaPack auto-dependency) and FR30 (graceful ReaImGui-missing diagnostic) now belong. **Epic 1 has no ReaImGui dependency.**
- **sokol_gfx (AR4 / D12).** The pinned commit `85d1f1b` ships a new view-based API diverged from the assumptions above. Phase 0.5 must **re-pin/validate sokol, or drop it** in favor of the raw-GL approach the spike demonstrated for a windowed viewport. (Leaning: raw GL for the Windows MVP; revisit sokol only if a Mac/Metal backend is needed post-MVP.)

**Confirmed (no change):** **D8 / AR11 (PCM_source)** validated — registering `pcmsrc_register_t` via `Register("pcmsrc", …)` makes a dropped `.fbx/.glb/.gltf` a timeline item of the animation's length, and the playhead drives the displayed frame (`animTime = playPos − itemStart`); the play position is continuous (audio clock), so playback renders at the full window frame rate.

**New loader requirement (feeds Epic 3 / Epic 6):** the FBX path **must** set `importer.SetPropertyInteger(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, 0)` — otherwise assimp keys animation on hidden `$AssimpFbx$` nodes and Mixamo rigs stay in bind pose.

**KEEP:** cross-cutting invariants (AR15 symmetric register, AR16 console-only diagnostics, AR17 failure isolation, AR18 main-thread GL) and decisions D1–D7, D9–D10, D13–D18 are unaffected.
