---
stepsCompleted: ['step-01-validate-prerequisites', 'step-02-design-epics', 'step-03-create-stories', 'step-04-final-validation']
status: 'complete'
completedAt: '2026-06-22'
inputDocuments:
  - '_bmad-output/planning-artifacts/prd.md'
  - '_bmad-output/planning-artifacts/architecture.md'
  - '_bmad-output/planning-artifacts/prfaq-FBXAnimationViewer.md'
---

# FBXAnimationViewer - Epic Breakdown

## Overview

This document provides the complete epic and story breakdown for FBXAnimationViewer, decomposing the requirements from the PRD and Architecture decisions (D1–D17) into implementable stories. No standalone UX Design document exists; UI/interaction requirements are captured inline in the PRD functional requirements (Panel & Docking, Camera & Viewport) and the architecture's panel/FBO-bridge decisions.

## Requirements Inventory

### Functional Requirements

**Animation Loading & Format Support**

- **FR1**: Load animation files in glTF 2.0 binary container (`.glb`) format.
- **FR2**: Load animation files in glTF 2.0 multi-file (`.gltf` + `.bin` + textures) format.
- **FR3**: Load animation files in FBX format, ≥60% of common Demute exports rendering correctly.
- **FR4**: Load files whose coordinate convention differs from glTF-canonical (Y-up vs Z-up, m vs cm) without per-file pre-configuration.
- **FR5**: Load files using any bone-naming convention, including non-English names.
- **FR6**: Render a static mesh when the loaded file contains no animation channels.
- **FR7**: Sample animation channels for translation, rotation, scale per bone, including root motion when present.

**Timeline Integration**

- **FR8**: Drag an animation file (`.glb`, `.gltf`, `.fbx`) onto a Reaper track to create a media item bound to that animation.
- **FR9**: Create a Reaper media item whose timeline length matches the animation's duration.
- **FR10**: Map the Reaper playhead to animation time via item-relative offset (`animTime = playheadTime − itemStart`, clamped to `[0, itemLength]`).
- **FR11**: Position, move, resize, color, rename animation items using Reaper's native item controls, identically to other media items.
- **FR12**: Coexist with other media types (audio, video, MIDI) on the same session without interfering with their playback.
- **FR13**: Support multiple animation items across one or more tracks in the same project.
- **FR14**: Determine which animation to display based on the item spanning the playhead; on overlap, display the one on the highest-priority track.

**3D Rendering & Visual Fidelity**

- **FR15**: Render skinned mesh geometry with per-frame bone deformation.
- **FR16**: Render diffuse-texture-mapped shading with per-material specular response sufficient to distinguish material types (matte leather vs polished steel).
- **FR17**: Resolve textures embedded in a GLB binary container.
- **FR18**: Resolve textures referenced as external sibling files in multi-file glTF.
- **FR19**: Resolve textures embedded in FBX containers.
- **FR20**: Render meshes composed of multiple materials, each with its own material parameters.
- *(FR21 recategorized as NFR-P1 — 60 fps is a quality attribute, not a capability.)*

**Camera & Viewport Control**

- **FR22**: Orbit the camera around the rig with right-click drag inside the viewer panel.
- **FR23**: Zoom the camera with mouse scroll inside the viewer panel.
- **FR24**: Pan the camera with middle-click drag inside the viewer panel.
- **FR25**: Reset the camera to a default framing containing the rig's bounding box, via a dedicated toolbar button.
- **FR26**: Continue updating the displayed animation frame while the camera is manipulated (no pause-on-interaction).

**Panel & Docking**

- **FR27**: Present the preview inside a docked viewport panel. *(Reinterpreted 2026-06-23 per Spike 0: a native OpenGL window docked via `DockWindowAddEx`, not a ReaImGui panel — ReaImGui caps at ~30 fps. ReaImGui is used for auxiliary panels from Epic 5.)*
- **FR28**: Dock the viewer panel into any Reaper docker (top, bottom, left, right, floating) using ReaImGui's native docking gestures.
- **FR29**: Register a Reaper Action (`FBXAV: Open Viewer Window`) that opens the panel when triggered.
- **FR30**: Detect, on extension load, whether ReaImGui is installed; emit a user-readable console diagnostic if missing and fail gracefully without registering the Action.

**Project State Persistence**

- **FR31**: Serialize per-item state — animation file path, camera angle, optional time offset/scale — into the Reaper project file so it survives save/load.
- **FR32**: Serialize the viewer panel's dock position into the project file (delegated to ReaImGui's own state persistence).
- **FR33**: Reopen a saved project and find each item's binding and viewport state restored without manual reconfiguration.

**Error Tolerance & Graceful Degradation**

- **FR34**: Avoid crashing the Reaper host on malformed/unsupported/partially-parseable input; display the file as best the parser allows.
- **FR35**: Emit a user-readable console diagnostic when a file fails to load, including file path and an error category.
- **FR36**: Manually reload the animation file bound to an item via a "Reload" button, picking up on-disk changes since the last load.
- **FR37**: Continue operating after a single animation item fails to load, without affecting other items in the session.

**Distribution & Operation**

- **FR38**: Install via ReaPack from the Demute repository as a one-click action that also installs `cfillion/reaimgui` as an auto-dependency.
- **FR39**: Install manually by copying the DLL to `%APPDATA%\REAPER\UserPlugins\` (ReaImGui already present) and be fully functional after the next Reaper restart.
- **FR40**: Operate fully offline: no network access, telemetry, remote license check, or remote asset loading.
- **FR41**: Receive updates via ReaPack's standard update flow; no in-extension update mechanism.

**Animation Browser / Explorer** *(new — added 2026-06-22 per Antho)*

- **FR42**: Open an in-Reaper animation browser panel (ReaImGui) that navigates the local filesystem and mounted disks, without leaving Reaper.
- **FR43**: Filter the browser to supported animation formats (`.glb`, `.gltf`, `.fbx`).
- **FR44**: Preview a selected animation in the viewer directly from the browser, before committing it to a track.
- **FR45**: Place a browsed animation onto a track as a media item, binding the chosen file path to that item, entirely from within the browser.
- **FR46**: Reopen a project saved on a different machine and resolve each item's animation path without forcing a full media re-import (path-portability strategy — relative path + missing-media remap; final mechanism decided in the persistence epic).

### NonFunctional Requirements

**Performance**

- **NFR-P1**: ≥60 fps on the validator's reference Windows workstation for fixtures up to ~20k tris, 4 materials, 50 bones.
- **NFR-P2**: Initial load of a typical Demute glTF/GLB fixture (≤20k tris, ≤4 materials, ≤50 bones, ≤8 MB textures) within 2 s from file drop to first rendered frame.
- **NFR-P3**: Manual reload of a replaced-on-disk file within 1 s from Reload click to first updated frame.
- **NFR-P4**: DLL loads at Reaper startup in under 2 s (Phase 0 baseline maintained).
- **NFR-P5**: Transport scrub latency — displayed frame matches playhead within one rendered frame (≈16 ms at 60 fps) under normal load.
- **NFR-P6 (capacity)**: ≥10 simultaneous animation items per project without measurable degradation on the reference workstation; up to ~50k tris render correctly but may exhibit lower frame rates.

**Reliability**

- **NFR-R1**: Zero Reaper-host crashes attributable to the extension during Demute internal usage from Phase 4 onward.
- **NFR-R2**: No Reaper-project data loss — must not corrupt the project file, drop tracks, or invalidate item references during save/load.
- **NFR-R3**: Clean unload (`rec == nullptr`): no leaked GL contexts, window classes, dangling Reaper API pointers, or orphaned timers. Symmetric register/deregister for every `rec->Register` made at load.
- **NFR-R4**: File-load failure is non-lethal: host stays up, other items keep working, user gets a console diagnostic.
- **NFR-R5**: Build remains warning-free at MSVC `/W3 /permissive-`. Any new warning blocks the phase gate.

**Compatibility & Integration**

- **NFR-C1**: Targets Reaper 7.x with `caller_version == 0x20E`; loading on older builds fails cleanly with `return 0`.
- **NFR-C2**: Declares ReaImGui (`cfillion/reaimgui`) as an auto-install ReaPack dependency.
- **NFR-C3**: Windows build targets MSVC x64 ABI exclusively (no mingw, clang-cl, or 32-bit).
- **NFR-C4**: glTF 2.0 conformance follows assimp's parser at the pinned commit in `extern/VENDORED.md`; re-vendoring requires re-validation.
- **NFR-C5**: FBX support follows assimp's parser at the pinned version; 60% target against representative recent Demute exports.

*Out of scope by design: Security, Scalability (beyond NFR-P6), Accessibility (inherits ReaImGui defaults, revisit Phase 5).*

### Additional Requirements

*(Derived from Architecture D1–D17, technical constraints, and cross-cutting concerns. These shape stories but are not user-facing features.)*

**Foundation / brownfield baseline (no starter template):**

- **AR1**: Phase 0 commit on `main` IS the foundation (CMake ≥3.20, `cmake/ReaperPlugin.cmake`, MSVC `/W3 /permissive-` C++17, vendored Reaper SDK under `extern/reaper-sdk/sdk/`, plugin entry pattern, MIT SPDX headers). No re-scaffolding.
- **AR2**: Reaper API loader pattern: `REAPERAPI_MINIMAL` + per-symbol `WANT_*` in `src/reaper_api.h`, resolved at load via `rec->GetFunc(...)`, expanded incrementally per phase.

**Dependency integration (vendoring & pinning):**

- **AR3**: ReaImGui binding header only is vendored; `ImGui_*` symbols resolved at runtime via `rec->GetFunc(...)`. `reapack/index.xml` declares runtime dep `cfillion/reaimgui >= v0.10.0.5`.
- **AR4**: sokol_gfx (GL backend) header-only, pinned by commit SHA in `extern/VENDORED.md`; freeze until a documented reason to bump.
- **AR5**: assimp `v6.0.5` as CMake subproject with `ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF`, only `GLTF_IMPORTER` and `FBX_IMPORTER` enabled (smaller binary, reduced attack surface).
- **AR6**: GLM `1.0.3` header-only, vendored under `extern/glm/`.

**Core architecture decisions to honor:**

- **AR7 (D1)**: Single aggregate `Asset` per file, POD-style structs, bones stored flat with `parentIdx`, skinned flag at mesh level for static-mesh fallback.
- **AR8 (D2)**: Per-item GPU resource ownership via RAII handles; no shared cache for MVP (accept VRAM duplication).
- **AR9 (D3)**: Coordinate convention pinned — column-major, right-handed, Y-up, CCW front-facing; single `convertAssimpMatrix()` boundary; render as-authored (no auto-remap).
- **AR10 (D11)**: ~~ReaImGui ↔ sokol_gfx FBO bridge~~ — **SUPERSEDED 2026-06-23 (Spike 0):** the 3D viewport is a **native OpenGL window docked via `DockWindowAddEx`** (direct render, no FBO/readback). A ReaImGui panel caps at ~30 fps; see architecture Spec Change Log 2026-06-23. ReaImGui itself is deferred to Epic 5.
- **AR11 (D8)**: PCM_source plugin is the central Reaper integration surface; item state I/O via `SaveState`/`LoadState`.
- **AR12 (D13)**: GPU vertex skinning with matrix-palette uniform; Phase 2 dominant technical risk.

**Cross-cutting invariants (every subsystem):**

- **AR13**: Convention tolerance by camera, not by remapping.
- **AR14**: Texture resolution funnels through one unified path across 3 packaging variants (GLB-embedded / glTF-siblings / FBX-aiTexture).
- **AR15**: Symmetric register/unregister of every Reaper API surface (NFR-R3 invariant).
- **AR16**: Console diagnostics (`ShowConsoleMsg`) are the only user-feedback channel — no message boxes/toasts/dialogs.
- **AR17**: Failure isolation — one bad item never crashes the host or breaks other items.
- **AR18**: Threading discipline — UI + GL on Reaper's main thread only; no exceptions in the plugin entry path.

**Process / governance:**

- **AR19**: Each phase ships against a `docs/PHASE<N>_VALIDATOR_GATE.md` checklist — the sole authority on phase completion (code passing type/warning checks but failing the gate is not done).
- **AR20**: Decisions D1–D17 are binding; deviation requires a Spec Change Log entry on the active phase spec.
- **AR21**: Rename the product from "FBXAnimationViewer" to **"ReaAnimViewer"** (no longer FBX-only; glTF/GLB is the primary format). Touches: repo/folder identity, DLL name (`reaper_animviewer.dll`), Reaper Action (`RAV: Open Viewer`), namespaces/SPDX headers, `reapack/index.xml` manifest, and docs. Executed as the first story of Epic 1 (Phase 0.5), which already rewrites the entry point and manifest.

### UX Design Requirements

*No standalone UX Design document was produced (the optional UX step was skipped). UI and interaction requirements live in the PRD functional requirements — Panel & Docking (FR27–FR30), Camera & Viewport Control (FR22–FR26) — and in the architecture's panel/FBO-bridge decisions (AR10). The canonical feedback channel is the Reaper console, not graphical UI (AR16).*

### FR Coverage Map

FR1: Epic 2 — load .glb (glTF binary)
FR2: Epic 2 — load multi-file .gltf
FR3: Epic 6 — FBX support, validated to 60% target
FR4: Epic 2 — load non-canonical coordinate conventions as-authored
FR5: Epic 2 — load any bone-naming convention
FR6: Epic 2 — static-mesh fallback (no animation channels)
FR7: Epic 3 — sample TRS channels per bone incl. root motion
FR8: Epic 4 — drag file onto track → item
FR9: Epic 4 — item length = animation duration
FR10: Epic 4 — playhead → animTime mapping (item-relative, clamped)
FR11: Epic 4 — native Reaper item controls (move/resize/color/rename)
FR12: Epic 4 — coexist with audio/video/MIDI
FR13: Epic 4 — multiple items across tracks
FR14: Epic 4 — current-item selection by playhead + track priority
FR15: Epic 3 — skinned mesh, per-frame bone deformation
FR16: Epic 2 — diffuse-textured Blinn-Phong + per-material specular
FR17: Epic 2 — textures embedded in GLB
FR18: Epic 2 — textures as glTF sibling files
FR19: Epic 6 — textures embedded in FBX
FR20: Epic 2 — multi-material meshes
FR22: Epic 2 — orbit camera (right-click drag)
FR23: Epic 2 — zoom (scroll)
FR24: Epic 2 — pan (middle-click drag)
FR25: Epic 2 — reset camera to bounding box (toolbar button)
FR26: Epic 2 — continuous update during camera manipulation
FR27: Epic 1 — docked GL viewport (reinterpreted from "ReaImGui-driven panel" per Spike 0, 2026-06-23)
FR28: Epic 1 — dock into any Reaper docker
FR29: Epic 1 — register Reaper Action to open panel
FR30: Epic 5 — detect missing ReaImGui, console diagnostic, graceful bail (moved from Epic 1 per Spike 0: ReaImGui first appears with the browser)
FR31: Epic 6 — serialize per-item state (path, camera, time offset/scale)
FR32: Epic 6 — serialize panel dock position (via ReaImGui)
FR33: Epic 6 — restore bindings + viewport state on project reopen
FR34: Epic 6 — no host crash on malformed input
FR35: Epic 6 — console diagnostic with path + error category on load failure
FR36: Epic 6 — manual Reload button per item
FR37: Epic 6 — single-item failure isolation
FR38: Epic 7 — ReaPack one-click install + ReaImGui auto-dep
FR39: Epic 7 — manual DLL-copy install
FR40: Epic 7 — fully offline operation
FR41: Epic 7 — updates delegated to ReaPack
FR42: Epic 5 — in-Reaper animation browser panel (disk navigation)
FR43: Epic 5 — filter browser to .glb/.gltf/.fbx
FR44: Epic 5 — preview from browser before placing
FR45: Epic 5 — place browsed animation on track, bind path to item
FR46: Epic 6 — cross-machine path portability (relative + missing-media remap)

_(FR21 reclassified as NFR-P1.) Cross-cutting NFRs (R1–R5) and invariants (AR13–AR18) are honored from Epic 1 onward and re-verified at every validator gate; AR21 rename is Epic 1, Story 1._

## Epic List

### Spike 0: Feasibility walking skeleton (THROWAWAY — not production)
A timeboxed (~2 days) throwaway prototype that proves the full stack stands up inside Reaper's process before disciplined production begins: assimp loads a skinned glTF → sokol_gfx renders it with GPU skinning → the FBO bridges into an `ImGui::Image()` in a dockable panel → an fps counter measures it on the reference workstation. Exists to answer one go/no-go question, not to deliver user value. Built on a separate branch, never merged to `main`; its learnings (measured fps, surprises, coordinate/convention findings) are folded back into the Phase 0.5/1/2 specs.
**No FRs owned** — pre-validates AR10 (FBO bridge), the assimp→render→skinning path (FR1/FR7/FR15), and NFR-P1 (≥60 fps). *(New, before Phase 0.5, per Antho 2026-06-22.)*

### Epic 1: Viewer lives inside Reaper (dockable panel)
Rename the project to ReaAnimViewer, then replace the Phase 0 standalone Win32 window with a ReaImGui dockable panel rendering through the sokol_gfx FBO bridge. After this epic, the rendered viewport docks anywhere in Reaper and opens via a Reaper Action; the extension fails gracefully (console diagnostic) when ReaImGui is absent.
**FRs covered:** FR27 (reinterpreted: docked GL viewport), FR28, FR29 — plus AR21 (rename), AR10 (revised: docked GL window). **FR30 + AR3 (ReaImGui dependency) moved to Epic 5** (per Spike 0). *(Phase 0.5)*

### Epic 2: See a textured 3D model with camera control
Load a static glTF/GLB file, render it with diffuse-texture-mapped Blinn-Phong + per-material specular across multiple materials, and orbit / zoom / pan / reset the camera inside the panel. Renders non-canonical files (Z-up, cm, foreign bone names) as-authored. After this epic, a user can open any static glTF/GLB and inspect it visually.
**FRs covered:** FR1, FR2, FR4, FR5, FR6, FR16, FR17, FR18, FR20, FR22, FR23, FR24, FR25, FR26. *(Phase 1)*

### Epic 3: Watch the rig animate
Add skinned-mesh playback: bone hierarchy, matrix palette, GPU vertex skinning, and TRS channel sampling per bone including root motion. After this epic, a loaded animated rig deforms correctly frame-by-frame. *(Dominant technical risk — 7–10 days.)*
**FRs covered:** FR7, FR15. *(Phase 2)*

### Epic 4: Drive animation from the Reaper transport
The core workflow: dropping an animation file on a track creates a PCM_source-backed item of matching length; the Reaper playhead drives animation time (item-relative, clamped); items behave like native media (move/resize/color/rename), coexist with audio/video/MIDI, support multiple items across tracks, and the panel shows whichever item spans the playhead. After this epic, the engine-capture replacement loop works.
**FRs covered:** FR8, FR9, FR10, FR11, FR12, FR13, FR14. *(Phase 3)*

### Epic 5: Browse and place animations without leaving Reaper
A built-in ReaImGui animation browser that navigates disks, filters to `.glb/.gltf/.fbx`, previews a selection in the viewer, and places it on a track with the path bound to the item. Reinforces the "stay in Reaper, zero context-switch" value proposition. *(New MVP epic per Antho, 2026-06-22.)*
**FRs covered:** FR42, FR43, FR44, FR45 — plus **FR30 + AR3/NFR-C2 (ReaImGui dependency, moved from Epic 1 per Spike 0:** the browser is the first ReaImGui panel). *(New — slots after Phase 3, before reliability/release)*

### Epic 6: Reliable across sessions, machines, and file changes
Manual Reload button; FBX support validated to the 60% target including embedded FBX textures; per-item and panel state persisted through project save/load and resolved across machines (relative path + missing-media remap, no forced re-import); graceful degradation (no host crash on malformed input, console diagnostics, one bad item never breaks others). After this epic, real Demute projects survive save/reopen and animator iterations.
**FRs covered:** FR3, FR19, FR31, FR32, FR33, FR34, FR35, FR36, FR37, FR46. *(Phase 4)*

### Epic 7: Install and ship via ReaPack
ReaPack one-click install pulling ReaImGui as auto-dependency, manual DLL-copy path, fully offline operation, updates delegated to ReaPack, final polish, and passing the Phase 5 validator gate to release.
**FRs covered:** FR38, FR39, FR40, FR41. *(Phase 5)*

## Spike 0: Feasibility walking skeleton (THROWAWAY — not production)

A timeboxed (~2 days) throwaway vertical slice proving the whole stack stands up inside Reaper before production starts. **This is a prototype, not production code**: it is built on a separate branch, is allowed to be ugly, hardcoded, and to skip the architecture's RAII/boundary/diagnostic discipline, and is **never merged to `main`**. Its only deliverables are a go/no-go verdict and a short findings note feeding the Phase 0.5/1/2 specs.

### Spike Story S0.1: Render a skinned animation in a Reaper-hosted FBO panel and measure fps

As the implementing developer and validator,
I want a throwaway prototype that loads, skins, and renders a real animation inside Reaper with an fps readout,
So that we confirm the core idea is technically viable before committing to the phased production build.

**Acceptance Criteria (go/no-go gate):**

**Given** a representative Demute skinned glTF/GLB fixture and Antho's reference Windows workstation
**When** the spike loads it via assimp, renders it via a sokol_gfx GL context bound inside Reaper's process with GPU vertex skinning, and bridges the result into an `ImGui::Image()` shown in a dockable ReaImGui panel
**Then** the deforming rig is visible and animating in the panel (validates AR10, FR1/FR7/FR15 path)
**And** sokol_gfx's GL context coexists with Reaper's own rendering without crashing or corrupting the host (existential risk)
**And** an on-screen fps counter reports the measured frame rate, recorded against the ≥60 fps NFR-P1 target
**And** a written findings note captures: go/no-go verdict, measured fps, coordinate/convention surprises, and any blocker that should reshape a Phase spec (AR20)
**And** the prototype branch is NOT merged to `main`; production resumes from Epic 1.

**Explicit non-goals for the spike:** texture-resolution completeness, multi-material, camera controls, PCM_source/transport, persistence, error tolerance, FBX, packaging. Any of these may be faked or hardcoded.

## Epic 1: Viewer lives inside Reaper (dockable panel)

Rename the project to ReaAnimViewer, then replace the Phase 0 standalone Win32 window with a **native OpenGL viewport docked into Reaper via `DockWindowAddEx`**, rendering the scene directly (no FBO, no readback). After this epic, the rendered viewport docks anywhere in Reaper and opens via a Reaper Action, at 60 fps. *(Phase 0.5. **Approach revised per Spike 0 — see architecture Spec Change Log 2026-06-23: a ReaImGui panel caps at ~30 fps, so the viewport is a docked GL window; ReaImGui is deferred to Epic 5, so Epic 1 has no ReaImGui dependency.** Honors AR15 symmetric register, AR16 console-only diagnostics, AR18 main-thread GL.)*

### Story 1.1: Rename FBXAnimationViewer to ReaAnimViewer

As the maintainer,
I want the project, binary, Action, and manifest renamed to ReaAnimViewer,
So that the tool's name reflects that it handles glTF/GLB (primary) and FBX, not FBX alone.

**Acceptance Criteria:**

**Given** the Phase 0 codebase named FBXAnimationViewer
**When** the rename is applied
**Then** the built artifact is `reaper_animviewer.dll`, the registered Reaper Action id/label is `RAV: Open Viewer`, and source namespaces/SPDX headers, the `reapack/index.xml` manifest, `docs/`, and repo/folder identity all use "ReaAnimViewer"
**And** the build still produces zero MSVC `/W3 /permissive-` warnings (NFR-R5) and loads in Reaper in under 2 s (NFR-P4)
**And** no stale "FBXAnimationViewer"/"FBX-only" string remains in shipped code, the manifest, or user-facing strings (planning artifacts under `_bmad-output/` may retain historical names).

### Story 1.2: Docked OpenGL viewport (direct render at 60 fps)

As the implementing developer,
I want a native OpenGL window that renders the scene directly and presents at 60 fps,
So that the viewport architecture (revised D11 — see architecture Spec Change Log 2026-06-23) is proven before anything else is built on it.

**Acceptance Criteria:**

**Given** a WGL/OpenGL context created on Reaper's main thread (AR18)
**When** the renderer draws into the window's framebuffer directly and presents via `SwapBuffers`
**Then** the rendered content is visible and correctly oriented, and holds **≥60 fps** on the reference workstation (NFR-P1) — the spike measured ~62 fps
**And** the render is driven by a frame timer independent of Reaper's ~30 Hz UI loop (the spike proved a ReaImGui panel cannot exceed ~30 fps)
**And** the viewport resizes with the window without leaking GL resources (RAII handles, D2/AR8). *(No offscreen FBO, no `ImGui::Image()` bridge, no CPU readback — those are superseded.)*

### Story 1.3: Dockable GL window opened by a Reaper Action

As a sound designer,
I want the viewer to appear as a panel I can dock anywhere in Reaper, opened from an Action,
So that the preview lives where I already work, with no floating window.

**Acceptance Criteria:**

**Given** ReaAnimViewer is loaded
**When** I trigger the `RAV: Open Viewer` Action (FR29)
**Then** the OpenGL viewport docks into any Reaper docker — top, bottom, left, right, or floating (FR28) — via `DockWindowAddEx` (FR27, reinterpreted: a docked GL viewport, not a ReaImGui panel)
**And** the Phase 0 standalone Win32 window is removed entirely
**And** the per-frame render runs on Reaper's main thread and the window docks/undocks/closes/reopens (including via the docker's close button) without leaking GL contexts or window classes (NFR-R3).

### Story 1.4: Graceful GL context / init failure handling

As a sound designer on an unusual GPU/driver,
I want a clear console message instead of a crash if the viewport can't initialize,
So that I know what went wrong instead of losing Reaper.

**Acceptance Criteria:**

**Given** the OpenGL context or required GL functions fail to initialize
**When** I trigger the `RAV: Open Viewer` Action
**Then** the extension emits a user-readable `ShowConsoleMsg` diagnostic and bails cleanly — no broken window, no host crash (AR16, AR17)
**And** every `rec->Register` performed before the bail is symmetrically reversed, leaving no dangling Reaper API pointers (NFR-R3, AR15)
**And** *(re-scoped from the former "ReaImGui missing" story)* the ReaImGui-absent diagnostic + graceful bail (FR30) is **relocated to Epic 5**, where the animation browser introduces the first ReaImGui panel and the `cfillion/reaimgui` dependency (AR3, NFR-C2).

## Epic 2: See a textured 3D model with camera control

Load a static glTF/GLB file and render it with diffuse-texture-mapped Blinn-Phong + per-material specular across multiple materials, with full camera control. Non-canonical files render as-authored. *(Phase 1. Loader funnels through assimp with narrowed importers AR5 and the single `convertAssimpMatrix` boundary AR9.)*

### Story 2.1: Load and display a static glTF/GLB mesh

As a sound designer,
I want to open a static `.glb`/`.gltf` and see its geometry shaded in the panel,
So that I can confirm the file loaded and inspect its shape.

**Acceptance Criteria:**

**Given** a Khronos-sample or Demute static glTF/GLB file
**When** the loader reads it via assimp into the `Asset` model (D1) and the renderer draws it with basic Blinn-Phong lighting
**Then** the mesh appears correctly lit and oriented for a canonical (Y-up, meters, CCW) file (FR1, FR2, AR9)
**And** a file containing no animation channels still renders as a static mesh via the skinned=false path (FR6)
**And** a non-canonical file (Z-up, centimeters, or non-English bone names) loads without error and renders as-authored, tilted/scaled rather than remapped (FR4, FR5, AR13)
**And** assimp is built with only the glTF and FBX importers enabled (AR5).

### Story 2.2: Diffuse texture resolution across GLB-embedded and glTF-sibling sources

As a sound designer,
I want textures applied to the model from both packaging styles,
So that I can see surface detail regardless of how the asset was exported.

**Acceptance Criteria:**

**Given** a GLB with embedded textures and a multi-file glTF with sibling image files
**When** the loader resolves textures through the unified texture-resolution path (AR14) and uploads them to GPU images (RAII, D2)
**Then** the GLB-embedded textures display correctly (FR17) and the glTF-sibling textures display correctly (FR18)
**And** a missing/unresolvable texture produces a console diagnostic (AR16) and falls back to a flat base color rather than failing the whole load.

### Story 2.3: Multi-material rendering with per-material specular

As a sound designer,
I want each material on a mesh rendered with its own parameters,
So that I can visually distinguish material types like matte leather vs polished steel.

**Acceptance Criteria:**

**Given** a mesh composed of multiple materials with differing specular properties
**When** the renderer draws each `SceneMesh` with its referenced `SceneMaterial` (baseColor, specularColor, shininess)
**Then** each sub-mesh shows its own diffuse texture and specular response (FR16, FR20)
**And** a visibly matte surface and a visibly glossy surface are distinguishable in the same frame.

### Story 2.4: Camera orbit, zoom, pan, and reset

As a sound designer,
I want to move the camera around the rig and reset its framing,
So that I can inspect the model from any angle and recover from odd orientations/scales.

**Acceptance Criteria:**

**Given** a model displayed in the panel
**When** I right-click-drag, scroll, and middle-click-drag inside the panel
**Then** the camera orbits (FR22), zooms (FR23), and pans (FR24) respectively
**And** clicking the toolbar **Reset Camera** button reframes the camera around the model's bounding box (FR25), correctly recovering a sideways/tiny non-canonical rig (Journey 2)
**And** camera manipulation never pauses or stalls the displayed frame (FR26).

## Epic 3: Watch the rig animate

Add skinned-mesh playback: skeleton parsing, TRS channel sampling including root motion, and GPU vertex skinning. *(Phase 2 — the dominant technical risk, 7–10 days. Validated per fixture against Blender / FBX Review. Coordinate pipeline pinned per D3/AR9.)*

### Story 3.1: Parse skeleton and skin binding into the Asset model

As the implementing developer,
I want the bone hierarchy and per-vertex skin weights loaded into the Asset,
So that the data needed for deformation exists in a single, auditable structure.

**Acceptance Criteria:**

**Given** a skinned glTF/GLB rig
**When** the loader populates `SceneSkeleton` (bones flat with `parentIdx`, `inverseBindMatrix`) and per-vertex `boneIds`/`boneWeights` (D1)
**Then** the bone count, parent links, and bind matrices match the source as cross-checked against Blender/FBX Review for at least one Demute fixture
**And** matrices pass through the single `convertAssimpMatrix` boundary (row-major → column-major) exactly once (AR9).

### Story 3.2: Sample animation channels and compute per-frame bone matrices

As the implementing developer,
I want TRS channels sampled at an arbitrary time into global bone matrices,
So that the pose at any frame can be computed deterministically.

**Acceptance Criteria:**

**Given** a `SceneAnimation` with translation/rotation/scale channels (and root motion when present)
**When** the animation is sampled at time t and the flat bone array is walked once to compose global matrices
**Then** all TRS channels are applied including baked root motion (FR7)
**And** a clip with rotation-only channels and a clip with full TRS both pose correctly
**And** sampling at t=0 and t=duration both produce valid poses (boundary clamp).

### Story 3.3: GPU vertex skinning renders the deformed rig

As a sound designer,
I want the animated rig to deform smoothly frame-by-frame,
So that I can watch the actual motion I need to score.

**Acceptance Criteria:**

**Given** the per-frame bone matrices from Story 3.2
**When** they are uploaded as a matrix-palette uniform and the vertex shader skins each vertex by its bone ids/weights (D13)
**Then** the rendered mesh deforms correctly across the full clip, matching Blender/FBX Review for the validated fixture (FR15)
**And** rendering holds ≥60 fps on the reference workstation at ~20k tris / 4 materials / 50 bones (NFR-P1)
**And** a static (unskinned) mesh continues to render unchanged via the non-skinned path (no regression to Epic 2).

## Epic 4: Drive animation from the Reaper transport

The core engine-capture-replacement loop: dropping an animation file on a track creates a PCM_source-backed item of matching length, the playhead drives animation time, and items behave like native media. *(Phase 3. PCM_source is the central Reaper integration surface, D8/AR11, with symmetric register AR15.)*

### Story 4.1: PCM_source plugin registers and creates a source from a file

As the implementing developer,
I want a PCM_source factory registered with Reaper that builds a source from an animation file path,
So that animations can become first-class Reaper media.

**Acceptance Criteria:**

**Given** ReaAnimViewer loaded
**When** the PCM_source factory is registered at load (D8)
**Then** Reaper can instantiate a ReaAnimViewer source bound to a given `.glb/.gltf/.fbx` path
**And** the factory and all related registrations are symmetrically deregistered on unload with no dangling pointers (NFR-R3, AR15).

### Story 4.2: Drop an animation file on a track to create a correctly-sized item

As a sound designer,
I want to drag an animation file onto a Reaper track and get an item the length of the animation,
So that it sits on my timeline exactly like a video reference does today.

**Acceptance Criteria:**

**Given** a track in a Reaper project
**When** I drag a `.glb`, `.gltf`, or `.fbx` from any source onto it (FR8)
**Then** a media item is created, named after the file, whose timeline length equals the animation's duration (FR9)
**And** loading the typical Demute fixture from drop to first rendered frame completes within 2 s (NFR-P2).

### Story 4.3: Playhead position drives the displayed animation frame

As a sound designer,
I want the rig to follow the Reaper transport as I play and scrub,
So that I can align sound to motion frame-accurately.

**Acceptance Criteria:**

**Given** an animation item on a track
**When** the playhead moves over the item
**Then** the panel displays the frame at `animTime = playheadTime − itemStart`, clamped to `[0, itemLength]` (FR10)
**And** the displayed frame matches the playhead within one rendered frame (≈16 ms) under normal load (NFR-P5)
**And** moving the playhead outside the item's span clamps to the item's first/last frame without error.

### Story 4.4: Animation items behave like native media and coexist with other tracks

As a sound designer,
I want to move, resize, color, and rename animation items and run them alongside audio/video/MIDI,
So that they integrate into my normal session without special handling.

**Acceptance Criteria:**

**Given** an animation item and other media items in the same session
**When** I move, resize, recolor, or rename the animation item using Reaper's native controls
**Then** it responds identically to other media items (FR11)
**And** audio, video, and MIDI on other tracks continue to play back unaffected (FR12).

### Story 4.5: Multiple animation items and current-item selection

As a sound designer,
I want several animations across tracks with the viewer showing the one under the playhead,
So that I can score a sequence of animations in one session.

**Acceptance Criteria:**

**Given** multiple animation items across one or more tracks (FR13)
**When** the playhead spans one or more of them
**Then** the panel displays the item under the playhead; on overlap it shows the one on the highest-priority track (FR14)
**And** at least 10 simultaneous animation items run without measurable performance degradation on the reference workstation (NFR-P6).

## Epic 5: Browse and place animations without leaving Reaper

A built-in ReaImGui animation browser that navigates disks, filters to supported formats, previews a selection in the viewer, and places it on a track with the path bound to the item. *(New MVP epic per Antho, 2026-06-22. Reuses the PCM_source surface from Epic 4 and the viewer from Epics 2–3.)*

### Story 5.1: Filesystem browser panel filtered to animation formats

As a sound designer,
I want a panel that lets me browse my disks for animation files,
So that I can find an animation without leaving Reaper or opening a file dialog.

**Acceptance Criteria:**

**Given** the browser panel is open
**When** I navigate folders and mounted disks within it
**Then** I can traverse the filesystem (FR42) and only `.glb`, `.gltf`, and `.fbx` files are listed as selectable (FR43)
**And** navigation and listing run on Reaper's main thread without blocking the UI for a typical folder; a folder that fails to read shows a console diagnostic rather than crashing (AR16, AR17).

### Story 5.2: Preview a selected animation before placing it

As a sound designer,
I want to preview a highlighted animation in the viewer from the browser,
So that I can confirm it's the right file before committing it to my timeline.

**Acceptance Criteria:**

**Given** an animation file selected in the browser
**When** I trigger preview
**Then** the file loads into the viewer panel and plays/poses without creating a track item (FR44)
**And** a malformed selection shows a console diagnostic and leaves any prior preview intact (FR34-aligned, AR17).

### Story 5.3: Place a browsed animation on a track with the path bound to the item

As a sound designer,
I want to send a browsed animation to a track directly,
So that I get the same item-on-timeline result as drag-and-drop, from inside the browser.

**Acceptance Criteria:**

**Given** an animation selected in the browser
**When** I place it on the target track
**Then** a PCM_source-backed item is created with the chosen file path bound to it (FR45), identical in behavior to a drag-dropped item (Epic 4)
**And** the item length equals the animation duration and the playhead drives it exactly as in Stories 4.2–4.3.

## Epic 6: Reliable across sessions, machines, and file changes

Persistence through project save/load, cross-machine path resolution, manual reload, FBX support to the 60% target, and graceful degradation. *(Phase 4. Per-item state via PCM_source `SaveState`/`LoadState`, D9. From this epic onward, zero host crashes is enforced — NFR-R1.)*

### Story 6.1: Persist and restore per-item and panel state through project save/load

As a sound designer,
I want my animation bindings, camera, and panel layout to survive saving and reopening a project,
So that I don't reconfigure anything when I come back to a session.

**Acceptance Criteria:**

**Given** a project with animation items and an adjusted camera and docked panel
**When** I save and reopen the project on the same machine
**Then** each item's animation path, camera angle, and optional time offset/scale are restored via PCM_source `SaveState`/`LoadState` (FR31, FR33)
**And** the panel's dock position is restored via ReaImGui's own state persistence (FR32)
**And** saving/loading never corrupts the project file, drops tracks, or invalidates item references (NFR-R2).

### Story 6.2: Resolve animation paths when a project opens on a different machine

As a sound designer collaborating across machines,
I want my session to find its animations on another PC without re-importing gigabytes,
So that projects are portable without ballooning disk usage.

**Acceptance Criteria:**

**Given** a project saved on machine A referencing animation files
**When** it is opened on machine B where the absolute paths differ
**Then** paths resolve via the chosen portability strategy (relative-to-project path with missing-media remap) without forcing a full media re-import (FR46)
**And** a path that cannot be resolved surfaces a missing-media diagnostic and leaves the rest of the session working (AR17)
**And** the final mechanism (relative path vs remap vs optional re-import) is recorded as a decision in this epic's story spec before implementation (AR20).

### Story 6.3: Manual reload picks up on-disk changes

As a sound designer whose animator just pushed a new version,
I want a Reload button that reloads the bound file,
So that I see the latest animation without recreating the item.

**Acceptance Criteria:**

**Given** an item whose source file has been replaced on disk
**When** I click the panel's **Reload** button (FR36)
**Then** the item's Asset is atomically swapped to the new content with graceful fallback if the reload fails (D4), keeping the old render until the new one is ready
**And** reload completes within 1 s from click to first updated frame for a typical fixture (NFR-P3).

### Story 6.4: Graceful degradation and per-item failure isolation

As a sound designer feeding the tool real, messy production files,
I want bad files to fail safely,
So that one broken animation never takes down Reaper or my other items.

**Acceptance Criteria:**

**Given** a malformed, unsupported, or partially-parseable file
**When** it is loaded
**Then** Reaper does not crash and the file renders as best the parser allows (FR34)
**And** a console diagnostic reports the file path and an error category (FR35, AR16)
**And** other animation items in the same session keep working unaffected (FR37, AR17)
**And** no GL contexts or Reaper API pointers leak from the failed load (NFR-R3).

### Story 6.5: FBX support validated to the 60% Demute target

As a sound designer whose clients export FBX,
I want common Demute FBX exports to render correctly,
So that I can use the tool on FBX-based projects, not just glTF.

**Acceptance Criteria:**

**Given** a representative sample of recent Demute client FBX exports
**When** they are loaded through the same assimp funnel (FBX importer, AR5)
**Then** at least 60% render correctly, including FBX-embedded textures (FR3, FR19, NFR-C5)
**And** FBX files that exceed assimp's parser capability degrade gracefully per Story 6.4 rather than crashing
**And** the validated/failed fixtures are recorded for the regression corpus.

## Epic 7: Install and ship via ReaPack

ReaPack one-click install with ReaImGui auto-dependency, manual install path, fully-offline operation, ReaPack-delegated updates, and the Phase 5 release gate. *(Phase 5. Closes the MVP.)*

### Story 7.1: ReaPack one-click install with ReaImGui auto-dependency

As a community sound designer,
I want to install ReaAnimViewer from ReaPack in one click and have ReaImGui installed alongside,
So that the tool just works after install + restart.

**Acceptance Criteria:**

**Given** a Reaper with the Demute ReaPack repository subscribed
**When** I click install on ReaAnimViewer
**Then** `reaper_animviewer.dll` installs and `cfillion/reaimgui (>= v0.10.0.5)` is pulled automatically as a declared dependency (FR38, NFR-C2, AR3)
**And** after a Reaper restart the `RAV: Open Viewer` Action is available and functional.

### Story 7.2: Manual DLL-copy install path

As a sound designer who installs manually,
I want to drop the DLL into UserPlugins and have it work,
So that I'm not forced to use ReaPack.

**Acceptance Criteria:**

**Given** ReaImGui already installed
**When** I copy `reaper_animviewer.dll` into `%APPDATA%\REAPER\UserPlugins\` and restart Reaper (FR39)
**Then** the extension loads and is fully functional
**And** loading on a Reaper older than the targeted 7.x (`caller_version != 0x20E`) fails cleanly with `return 0` (NFR-C1).

### Story 7.3: Fully offline operation and ReaPack-delegated updates

As a sound designer on an air-gapped or privacy-sensitive workstation,
I want the extension to make no network access and to update only via ReaPack,
So that I can trust it makes no hidden connections.

**Acceptance Criteria:**

**Given** ReaAnimViewer running with no network available
**When** it loads and operates (load, browse, render, save)
**Then** it opens no sockets and performs no telemetry, license check, or remote asset loading (FR40)
**And** the extension contains no in-extension update mechanism; updates arrive solely through ReaPack's standard flow (FR41).

### Story 7.4: Final polish and Phase 5 validator gate

As the validator (Antho),
I want a Phase 5 validator-gate checklist that proves the MVP is shippable,
So that release is gated on evidence, not optimism.

**Acceptance Criteria:**

**Given** Epics 1–6 complete
**When** the Phase 5 work finishes
**Then** a `docs/PHASE5_VALIDATOR_GATE.md` checklist exists and all rows pass (AR19)
**And** the build is warning-free at `/W3 /permissive-` (NFR-R5), zero host crashes were observed in Demute internal usage from Phase 4 onward (NFR-R1), and the ReaPack listing is published
**And** the engine-capture-replacement loop is demonstrated end-to-end on at least one real Demute project (Success Criteria).
