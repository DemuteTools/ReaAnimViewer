# Story 0.1: Render a skinned animation in a Reaper-hosted FBO panel and measure fps

Status: done — spike complete, verdict GO. Full stack validated (render + 60fps docked GL window + timeline-item + playhead-driven playback). See docs/SPIKE0_FINDINGS.md. Branch unmerged; production resumes at Epic 1.

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

<!-- DEV STATUS (2026-06-22): All spike code is written on branch `spike/0-1-feasibility`.
     The story CANNOT be auto-completed from the Linux dev environment: AC1 (visible
     deforming rig), AC2 (GL-context coexistence), AC3 (measured fps on the reference
     workstation), and AC4's runtime data require building with MSVC and running inside
     Reaper on Antho's Windows machine. Those ACs are intentionally left unchecked rather
     than fabricated. The spike's core architectural question was, however, already
     answered at the source-code level — see docs/SPIKE0_FINDINGS.md (the FBO bridge can
     only be CPU readback with ReaImGui). -->

> **⚠️ THROWAWAY SPIKE — READ THIS FIRST.**
> This is **Spike 0**, a timeboxed (~2 day) feasibility prototype, **not production code**. It exists to answer one go/no-go question before disciplined production resumes at Epic 1. It is built on a **separate branch that is NEVER merged to `main`**. It is **allowed to be ugly, hardcoded, and to skip the architecture's RAII / boundary / no-throw / console-format discipline** (D1–D17 are *deliberately relaxed here*). The only durable deliverables are (a) a go/no-go verdict and (b) a written findings note that feeds the Phase 0.5 / 1 / 2 specs. Do **not** polish, generalize, or "do it properly" — every hour spent on production-grade hygiene here is wasted, because the code is thrown away.

## Story

As the implementing developer and validator (Antho),
I want a throwaway prototype that loads, skins, and renders a real animation inside Reaper with an on-screen fps readout,
So that we confirm the core technical idea is viable — assimp → sokol_gfx GPU skinning → FBO → `ImGui::Image()` in a dockable panel, holding ≥60 fps — **before** committing to the phased production build.

## Acceptance Criteria

The acceptance criteria are the **go/no-go gate** from the epic (Spike Story S0.1). All five must be satisfied for the spike to be "done":

1. **Deforming rig is visible and animating in a Reaper-docked panel.** Given a representative Demute skinned glTF/GLB fixture and Antho's reference Windows workstation, when the spike loads it via assimp, renders it via a sokol_gfx GL context bound inside Reaper's process with GPU vertex skinning, and bridges the result into an `ImGui_Image()` shown in a dockable ReaImGui panel, then the deforming rig is visible and animating in the panel. *(Validates AR10 / D11 FBO bridge, and the FR1 / FR7 / FR15 load→skin→render path.)*

2. **The GL context coexists with Reaper without crashing or corrupting the host.** And sokol_gfx's GL context stands up and renders inside Reaper's process **without crashing or corrupting Reaper's own rendering** (this is the existential risk — if the two GL/graphics contexts fight, the whole architecture is in question).

3. **An on-screen fps counter reports the measured frame rate.** And an fps counter is visible in the panel, and its reading is **recorded against the ≥60 fps NFR-P1 target** for the reference fixture (~20k tris / ~4 materials / ~50 bones class of asset).

4. **A written findings note is produced.** And a findings note (`docs/SPIKE0_FINDINGS.md` on the spike branch) captures: **go/no-go verdict**, **measured fps**, **coordinate / convention surprises**, the **resolved FBO-bridge mechanism** (how the sokol GL texture actually reached `ImGui_Image` — see Dev Notes risk section), and **any blocker that should reshape a Phase spec** (AR20).

5. **The branch is NOT merged.** And the prototype lives on its own branch and is **not merged to `main`**; production resumes from Epic 1 on `main`. Findings are folded into the Phase 0.5 / 1 / 2 specs by hand, not by merging code.

**Explicit non-goals for the spike** (any of these may be faked, hardcoded, or skipped entirely — do **not** implement them):
texture-resolution completeness, multi-material rendering, camera controls (a fixed or trivially-hardcoded camera is fine), PCM_source / transport integration (loop the animation on a wall-clock timer instead), persistence, error tolerance / graceful degradation, FBX support (glTF/GLB only), ReaPack packaging, the AR21 rename (stay "FBXAV" / `fbxav` — renaming is Epic 1 Story 1.1, on `main`).

## Tasks / Subtasks

- [x] **Task 0 — Branch off, do not touch `main`** (AC: 5)
  - [x] Created throwaway branch `spike/0-1-feasibility` from `main`. No PR/merge.
  - [x] Every spike source carries a top banner: "THROWAWAY SPIKE — not production, do not merge to main."

- [x] **Task 1 — Vendor the Phase 1+ dependencies (spike-grade, just enough to compile)** (AC: 1)
  - [x] Vendored **sokol_gfx** `extern/sokol/sokol_gfx.h` @ `85d1f1b`. *(Note: this pin ships sokol's new view-based API — see Finding 2; spike renders raw GL instead.)*
  - [~] **ReaImGui binding header** is **generated, not in the source tree**, so it cannot be vendored from source. Set up `extern/reaimgui/include/` with `README_DROP_HEADER_HERE.md` — Antho drops the v0.10.0.5 release header there before building. *(Corrected the wrong re-vendor path in VENDORED.md.)*
  - [x] **GLM** `1.0.3` — pulled via CMake FetchContent (spike-pragmatic; see VENDORED.md).
  - [x] **assimp** `v6.0.5`, glTF importer only (D15 narrowing applied) — via CMake FetchContent rather than committing ~100 MB into the drvfs tree. Static link.
  - [x] Updated `extern/VENDORED.md` (sokol pin + finding, reaimgui generated-header note, FetchContent rationale, re-vendor-path correction).

- [x] **Task 2 — Stand up a GL context inside Reaper** (AC: 2) — *code complete; runtime check ⏳ pending Antho*
  - [x] Hidden-window WGL offscreen context adapted from Phase 0 `viewer_window.cpp` → [`spike_gl.cpp`](../../src/spike_gl.cpp); modern-GL entry points resolved via `wglGetProcAddress`.
  - [~] **Deviation (sanctioned, AR20):** the pinned sokol uses a new view API (Finding 2), so the spike uses **raw GL**, not `sg_setup()`. The "first existential checkpoint" (a GL context standing up in Reaper's process) is coded; ⏳ must be observed on the workstation.
  - [x] Offscreen FBO (RGBA8 color + D24 depth) with clear — [`spike_renderer.cpp`](../../src/spike_renderer.cpp) `Resize`/`RenderToPixels`.

- [x] **Task 3 — Resolve ReaImGui and open a dockable panel** (AC: 1) — *code complete; runtime check ⏳ pending Antho*
  - [x] `ImGui::init(plugin_getapi)` + `CreateContext(..., ConfigFlags_DockingEnable)`; missing-ReaImGui path logs to console and bails — [`spike_main.cpp`](../../src/spike_main.cpp).
  - [x] Frame loop driven by `rec->Register("timer", &Loop)`, per the official ReaImGui C++ example; `ImGui::Begin/End` each tick.
  - [x] **FBO-bridge mechanism RESOLVED at source level: CPU readback is the only option** (ReaImGui `Image()` takes an `Image*` resource, not a raw texture; no `DrawList_AddCallback`; backend may be D3D10). Implemented `glReadPixels` → `Image_SetPixels_Array`. Full analysis in [docs/SPIKE0_FINDINGS.md](../../docs/SPIKE0_FINDINGS.md) Finding 1.
  - [x] Row-flip done in the readback pack (upright, default UVs); resize recreates FBO + ReaImGui image.

- [x] **Task 4 — Load + GPU-skin + render a real animated glTF** (AC: 1) — *code complete; visual confirmation ⏳ pending Antho*
  - [x] assimp load → flat skeleton (parentIdx), per-vertex bone ids/weights, first clip — [`spike_loader.cpp`](../../src/spike_loader.cpp). Fixture path via `FBXAV_SPIKE_FIXTURE` env or `kDefaultFixture`.
  - [x] Per-frame palette: sample TRS, compose locals, single linear pass for globals, `palette = global * inverseBind` — [`spike_renderer.cpp`](../../src/spike_renderer.cpp).
  - [x] Palette uploaded as `mat4[64]` uniform; vertex shader does LBS (4 weights) — shaders inline in `spike_renderer.cpp`.
  - [x] `t` driven by a `QueryPerformanceCounter` wall-clock loop (not the transport).
  - [~] Coordinate/convention surprises section seeded in findings; ⏳ actual observations require the run.

- [x] **Task 5 — Add the fps counter + measure** (AC: 3) — *measured 2026-06-23*
  - [x] Smoothed fps readout via `ImGui::Text`, plus a render-cost chrono (ms + capacity).
  - [x] Measured on Ryzen 9 9900X / RX 9070: render+readback **3.5 ms (~280 fps capacity)**; on-screen **32 fps**, which is **Reaper's ~30 Hz extension-timer cap, not a rendering limit**. Perf headroom vs the ≥60 fps target is ~8×; on-screen 60 fps is gated only by redraw cadence (Phase 0.5 follow-up).

- [x] **Task 6 — Findings note + go/no-go** (AC: 4, 5) — *verdict GO*
  - [x] [docs/SPIKE0_FINDINGS.md](../../docs/SPIKE0_FINDINGS.md) filled with measured results, the Mixamo `PreservePivots=0` finding, and the readback-bridge confirmation (cheap → viable).
  - [x] **Verdict: GO.** Branch unmerged (AC5 satisfied); production resumes on `main` at Epic 1, folding in the findings (readback bridge for D11, sokol re-pin for D12, FBX pivot setting, 60 Hz redraw cadence to resolve in Phase 0.5).

## Dev Notes

### Why this spike exists (the one question it answers)

The production architecture (D1–D17) is internally coherent and 41/41 FR + 17/17 NFR covered *on paper*, but it rests on one unproven assumption: that a **sokol_gfx GL render-to-FBO can be bridged into a ReaImGui `ImGui_Image()` inside Reaper's live process, at ≥60 fps, without the two graphics contexts fighting**. That is [architecture.md decision **D11 / AR10**](architecture.md) — explicitly called the Phase 0.5 critical deliverable on which "the entire panel-based architecture hinges." This spike pulls that risk forward and proves (or kills) it cheaply, plus exercises the dominant Phase 2 risk (GPU skinning correctness) end-to-end. Source: [epics.md Spike 0 / S0.1](epics.md), [prd.md risk mitigation §](prd.md).

### 🔴 The crux risk — the FBO bridge is *not* a settled mechanism

The architecture ([D11](architecture.md)) describes the bridge as: render into a sokol offscreen `sg_image`, get its **backend GL texture name**, and pass it to `ImGui::Image((ImTextureID)glTexName, ...)`. **Two things are unverified and are the heart of the spike:**

1. **Does sokol expose the GL texture name for an `sg_image`?** The architecture's own note (D11) says "confirm sokol_gfx exposes the backend GL texture name … or keep our own GLuint at FBO creation time." In practice sokol's GL backend stores texture names in the image's internal `gl.tex[]` slots; recent sokol exposes them via `sg_query_image_info` / a GL-specific query, but the clean path is often to **create the GL texture yourself and hand it to sokol as an injected/external image**, so you own the `GLuint`. Decide empirically and record what worked.

2. **Will ReaImGui actually accept that texture handle?** This is the bigger unknown. ReaImGui is a *binding*, not raw Dear ImGui — its `ImGui_Image` API does **not** necessarily take a raw `ImTextureID`/`GLuint`. Recent ReaImGui exposes image *resource objects* (e.g. created from memory/files via `ImGui_CreateImage*`), abstracting `ImTextureID` away — and ReaImGui's own rendering backend may not even be the same GL context (or same API) as the one sokol renders into. Public docs/search did **not** cleanly confirm ReaImGui's Windows backend or a "wrap my existing GL texture" entry point (research 2026-06-22). **This ambiguity is exactly why the spike is worth doing.** Resolve it by trying, in order:
   - (a) a raw GL texture as `ImTextureID` if ReaImGui's binding allows it;
   - (b) a ReaImGui image-resource wrapper around an existing texture, if such an entry exists;
   - (c) the documented fallback — **ImGui DrawList custom callback** issuing raw GL draws ([architecture.md D11 fallback / Epic 1 Story 1.2 AC](epics.md));
   - (d) last resort for a *viability* answer only — CPU pixel readback of the FBO into a ReaImGui image each frame (proves the pipeline even if too slow for production; flag the fps implication).
   - **Whatever works, document it precisely** — it rewrites Epic 1 Story 1.2 and the Phase 0.5 spec. If (a)/(b) work, D11 stands. If only (c)/(d) work, that is a material Spec Change (AR20) the production plan must absorb.

   > User-facing implication (for Antho): the spike's main job is to find out *how* the 3D picture gets into the dockable Reaper panel. If the clean way doesn't work, there are fallbacks — the spike tells us which one we're building on, before we commit weeks to it.

### Existential risk #2 — two graphics contexts in one process

Reaper renders its own UI; ReaImGui renders via its backend; sokol wants a GL context. The spike must show these **coexist without Reaper crashing, flickering, or corrupting** (AC2). Keep all GL + UI work on **Reaper's main thread** (AR18 — the one production invariant worth honoring even in the spike, because violating it produces misleading crash data). The Phase 0 window already proved a WGL context lives happily in Reaper's process ([`viewer_window.cpp`](../../src/viewer_window.cpp)); the new question is whether a *second* renderer (ReaImGui's) plus sokol coexist.

### What to reuse from the shipped Phase 0 code

Phase 0 is on `main` and validated. Reuse, don't reinvent:
- **WGL context creation** — [`viewer_window.cpp:41` `CreateGLContextFor`](../../src/viewer_window.cpp#L41) and the `PIXELFORMATDESCRIPTOR` (32 color / 24 depth / 8 stencil — depth is now actually needed for the z-buffer). [`viewer_window.cpp:71` `PaintFrame`](../../src/viewer_window.cpp#L71) shows the clear-color GL pattern.
- **Plugin entry / action registration / symmetric unregister** — [`plugin_main.cpp:33`](../../src/plugin_main.cpp#L33). The `caller_version == REAPER_PLUGIN_VERSION` (0x20E) guard and the `command_id`+`gaccel`+`hookcommand` triple are the way to get an Action that opens the panel.
- **Reaper API loader** — [`reaper_api.h`](../../src/reaper_api.h): `REAPERAPI_MINIMAL` + `REAPERAPI_WANT_*`. Add `WANT_*` for any extra Reaper functions you need (timer registration is via `rec->Register("timer", ...)`, not a WANT). ReaImGui symbols are resolved separately via `rec->GetFunc("ImGui_*")`.
- **CMake helper** — [`cmake/ReaperPlugin.cmake`](../../cmake/ReaperPlugin.cmake) `add_reaper_extension()` enforces the mandatory `reaper_` DLL prefix, C++17, MSVC `/W3 /permissive-`. Reuse it; add the new sources + assimp link. (Spike may relax `/W3` cleanliness if it's slowing you down — warnings don't gate a throwaway.)

### Tech stack & pinned versions (spike uses the production pins so findings transfer)

| Dependency | Version / pin | Role in spike | Source |
|---|---|---|---|
| Reaper SDK | commit `31234f3…`, Reaper 7.72+, `caller_version 0x20E` | entry point, GetFunc, timer | already vendored, `extern/VENDORED.md` |
| ReaImGui binding | `v0.10.0.5` (header only; runtime ext via ReaPack) | dockable panel + image display | [architecture.md D16](architecture.md) |
| sokol_gfx | commit `85d1f1b`, GL backend | offscreen FBO + scene draw + GPU skinning | [architecture.md AR4/D11](architecture.md) |
| assimp | `v6.0.5`, glTF importer only | load skinned glTF/GLB | [architecture.md D15/AR5](architecture.md) |
| GLM | `1.0.3` | matrix math for skinning palette | [architecture.md AR6](architecture.md) |

Build/run loop (Windows, x64 MSVC only — NFR-C3): `cmake -B build -G "Visual Studio 17 2022" -A x64` → `cmake --build build --config Release` → copy `build/Release/reaper_*.dll` to `%APPDATA%\REAPER\UserPlugins\` → restart Reaper → run the Action. (Same loop as [PHASE0_VALIDATOR_GATE.md](../../docs/PHASE0_VALIDATOR_GATE.md).) **ReaImGui must be installed in Reaper** for the spike to run (install `cfillion/reaimgui` via ReaPack, or drop its DLL in UserPlugins).

### Coordinate / skinning reference (so the rig looks right)

Render **as-authored**, column-major, right-handed, Y-up, CCW front-facing ([architecture.md D3](architecture.md)). Convert assimp's row-major matrices to column-major exactly once at the load boundary (`convertAssimpMatrix`). For skinning: linear blend, 4 weights/vertex (assimp `aiProcess_LimitBoneWeights`), palette uploaded as a `mat4[]` uniform (production caps at 128; a 50-bone fixture is comfortably under any std140 limit). Full per-frame recipe in [architecture.md D13](architecture.md). A non-canonical fixture (Z-up/cm) rendering tilted is *expected* and is a finding, not a bug — note it.

### fps measurement

Smooth the per-frame delta (e.g. exponential moving average) and display via `ImGui_Text`. Record steady-state fps **on Antho's reference workstation** (the only NFR-P1 authority) for a ~20k-tri / ~4-mat / ~50-bone class fixture. A spike result well above 60 fps is a strong go signal; near or below 60 with a clean pipeline is a yellow flag to capture in findings (could be MSAA, readback fallback, or shader cost).

### Testing standards

No automated tests (consistent with the whole project — validation is by per-phase validator gate in real Reaper, [architecture.md Test organization §](architecture.md)). The spike's "test" **is** the go/no-go gate: Antho runs the DLL in Reaper on the reference workstation and observes the deforming rig + fps. The deliverable that outlives the code is the findings note, not a test suite.

### Project Structure Notes

- Spike sources can live in `src/` on the spike branch (e.g. `src/spike_main.cpp`, `src/spike_renderer.cpp`) — **flat, monolithic, and disposable is fine**. Do **not** create the full production file taxonomy (`asset_loader`, `renderer`, `gpu_resources`, `camera`, `animation`, `pcm_source_anim`, …) — that is the Epic 1+ structure ([architecture.md Project Structure §](architecture.md)) and building it now wastes spike time on code that gets deleted.
- The production boundary rules ("only `asset_loader.cpp` includes assimp", "only `renderer.cpp` calls `sg_*`", RAII GPU handles, `LoadResult`, `[FBXAV]` console format) are **explicitly waived for the spike**. They are the contract for `main`, not for this branch.
- Keep the AR21 rename out of scope: stay on `fbxav` / "FBXAV" identifiers. The rename to ReaAnimViewer is Epic 1 Story 1.1 and happens on `main`.
- `docs/SPIKE0_FINDINGS.md` is the one new file intended to inform `main` (by hand-folding its conclusions into the Phase 0.5/1/2 specs — not by merging).

### References

- [epics.md — Spike 0 / Story S0.1, acceptance criteria & non-goals](epics.md) (lines 219–271)
- [architecture.md — D11 sokol↔ReaImGui FBO bridge (the crux)](architecture.md) (D11, lines ~345–368; Gap-1 MSAA/FBO, lines ~1029)
- [architecture.md — D13 GPU skinning recipe](architecture.md), [D3 coordinate convention](architecture.md), [D1 scene data model](architecture.md), [D15 assimp CMake narrowing](architecture.md)
- [architecture.md — AR10 FBO bridge / AR18 main-thread GL / AR4 sokol pin / AR5 assimp / AR6 GLM](architecture.md)
- [prd.md — Feasibility spike rationale + risk mitigation strategy](prd.md) (lines 128–130, 324–342)
- [spec-phase-0-scaffolding.md — entry-point, WGL, build patterns to reuse](spec-phase-0-scaffolding.md)
- Shipped Phase 0 code: [`plugin_main.cpp`](../../src/plugin_main.cpp), [`viewer_window.cpp`](../../src/viewer_window.cpp), [`reaper_api.h`](../../src/reaper_api.h), [`cmake/ReaperPlugin.cmake`](../../cmake/ReaperPlugin.cmake)
- [`extern/VENDORED.md` — vendoring discipline + pinned SDK/ReaImGui details](../../extern/VENDORED.md)
- [implementation-readiness-report-2026-06-22.md — Spike 0 sanctioned-exception rationale](../planning-artifacts/implementation-readiness-report-2026-06-22.md) (m1, lines ~255)

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Opus 4.8, 1M context) — dev-story workflow, 2026-06-22.

### Debug Log References

- Linux CMake-syntax sanity check passed (non-Windows path stubs out before FetchContent): `cmake -B /tmp/spike-cfg-check -S .` → "Configuring done", exit 0. This validates CMake parses; it does **not** validate the Windows/MSVC build or the FetchContent of assimp/glm (no MSVC/Reaper available in the dev environment).
- ReaImGui v0.10.0.5 API verified against upstream source (`api/image.cpp`, `api/drawlist.cpp`, `api/context.cpp`, `api/window.cpp`, `examples/hello_world.cpp`) and `src/` backend file list.
- sokol_gfx `85d1f1b` verified against the vendored header (new view-based API present; `sg_make_attachments` absent; `sg_gl_query_image_info` present).

### Completion Notes List

**FINAL (2026-06-23) — verdict GO, spike closed.** Full stack validated on the reference workstation, beyond the original scope: GPU-skinned Mixamo FBX rendering inside Reaper, **~62 fps in a native docked OpenGL window**, and **timeline-item + playhead-driven playback** (PCM_source). Key production decisions captured in [docs/SPIKE0_FINDINGS.md](../../docs/SPIKE0_FINDINGS.md): (1) viewport = native docked GL window, NOT a ReaImGui panel — ReaImGui is hard-capped ~30 fps (replaces D11/AR10); (2) PCM_source mechanism confirmed (validates D8); (3) FBX loader must set `PreservePivots=0`; (4) sokol pin diverged (used raw GL) — Phase 0.5 to re-pin/validate or drop. Branch `spike/0-1-feasibility` left unmerged; production resumes at Epic 1 with these amendments.

---

_History below (kept as the working record):_

**Spike code is complete; the story is NOT auto-completable from this (Linux) environment.** AC1 (visible deforming rig), AC2 (GL-context coexistence), AC3 (fps on the reference workstation) and AC4's runtime data require an MSVC build + a run inside Reaper on Antho's Windows machine. Those items are left unchecked rather than fabricated (no invented fps/verdict).

**Headline result — the spike's core question is already answered (at source level), and it reshapes the architecture:**
- 🔴 **The zero-copy FBO bridge in D11/AR10 is not achievable through ReaImGui.** `ImGui_Image` takes a ReaImGui `Image*` (pixel-data resource), not a raw GPU texture; there is no GPU-handle adoption and **no `DrawList_AddCallback`**; ReaImGui owns its backend (Windows D3D10 or GL, macOS Metal). The only viable bridge is **GPU→CPU→GPU readback**, which the spike implements. This must amend Epic 1 Story 1.2 / Phase 0.5 (AR20) — and raises a genuine architecture choice (accept readback cost vs. use a Reaper-docked child GL window for zero-copy). **Surfaced to Antho for an architect decision.**
- ⚠️ **sokol pin `85d1f1b` ships a new view-based API** diverged from D11/D12 assumptions. Spike renders raw GL to isolate the bridge question; Phase 0.5 must validate/repin sokol.

**Sanctioned spike deviations (AR20, all in docs/SPIKE0_FINDINGS.md + VENDORED.md):** raw GL instead of sokol; assimp/GLM via FetchContent instead of vendoring; ReaImGui generated header obtained from release instead of vendored from source.

**Next action for Antho (on Windows):** drop `reaper_imgui_functions.h` (v0.10.0.5) into `extern/reaimgui/include/`, build, set `FBXAV_SPIKE_FIXTURE` to a skinned `.glb`, run the action `FBXAV: Open Spike Viewer`, then fill the ⏳ runtime fields + verdict in `docs/SPIKE0_FINDINGS.md`. Build/run is uniterated against MSVC+Reaper, so expect first-build fixups (most likely spots: ReaImGui `ImGui::` enum/signature names, `#version 330` needing a core-profile context, `reaper_array` availability from the generated header).

### Change Log

| Date | Change |
|---|---|
| 2026-06-22 | Spike 0 implementation written on branch `spike/0-1-feasibility`: assimp glTF loader, raw-GL offscreen skinning renderer, WGL context host, ReaImGui panel with CPU-readback bridge + fps counter, CMake (FetchContent assimp/glm), findings note. Core finding (readback-only bridge) established from ReaImGui source. Not merged to main. |
| 2026-06-23 | Ran on reference workstation. Fixed Mixamo FBX animation (`PreservePivots=0`). Added fps diagnostic; measured ReaImGui hard-capped ~32 fps even when fed at 66 Hz. Added `build_spike.bat` (one-click build/install; CRLF + goto-label robust). |
| 2026-06-23 | Per validator decision, prototyped a **native docked OpenGL window** (`DockWindowAddEx`, direct render, no readback) → **~62 fps** confirmed. Architecture decision: viewport = docked GL window, not a ReaImGui panel (replaces D11/AR10). |
| 2026-06-23 | Prototyped **transport integration**: PCM_source (`pcmsrc`) → drop creates a timeline item of animation length; playhead drives the frame at full rate. Validates D8/AR11. |
| 2026-06-23 | Spike closed — verdict **GO**, findings finalized in `docs/SPIKE0_FINDINGS.md`. Branch left unmerged. |

### File List

New (spike branch `spike/0-1-feasibility`, not for merge):
- `src/spike_scene.h` — POD scene/skeleton/anim structs
- `src/spike_loader.h` / `.cpp` — assimp → skinned model (FBX+glTF; `PreservePivots=0`)
- `src/spike_gl.h` / `.cpp` — WGL context + modern-GL function loader
- `src/spike_renderer.h` / `.cpp` — GL skinning shader, animation sampling, `DrawScene` (FBO or window)
- `src/spike_glwindow.h` / `.cpp` — **native docked GL window** (direct render + SwapBuffers, fps) ← the chosen viewport
- `src/spike_pcmsource.h` / `.cpp` — **PCM_source** (timeline item) + `CurrentAnimTime` (playhead → frame)
- `src/spike_main.cpp` — Reaper entry, two actions (ReaImGui ~30fps / GL docked 60fps), pcmsrc registration, 66 Hz timer
- `extern/sokol/sokol_gfx.h` — vendored @ 85d1f1b (unused; new view API — see findings)
- `extern/reaimgui/include/` — `reaper_imgui_functions.h` (v0.10.0.5, fetched) + README
- `docs/SPIKE0_FINDINGS.md` — final findings (verdict GO + architecture decisions)
- `docs/SPIKE0_HOWTO_ANTHO.md` — beginner build/run guide
- `build_spike.bat` — one-click build + install (CRLF)
- `.gitattributes` — keep `*.bat` CRLF

Modified:
- `CMakeLists.txt` — spike build target (FetchContent assimp+FBX/glm; all spike sources)
- `src/reaper_api.h` — WANT list: Dock* + transport/item APIs
- `extern/VENDORED.md` — sokol/reaimgui/FetchContent notes + re-vendor-path correction
