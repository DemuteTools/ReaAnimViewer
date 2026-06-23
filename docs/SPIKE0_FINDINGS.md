# Spike 0 — Feasibility findings (go/no-go)

**Branch:** `spike/0-1-feasibility` (THROWAWAY — not merged to `main`)
**Story:** [0-1-render-a-skinned-animation-in-a-reaper-hosted-fbo-panel-and-measure-fps](../_bmad-output/implementation-artifacts/0-1-render-a-skinned-animation-in-a-reaper-hosted-fbo-panel-and-measure-fps.md)
**Question:** Can we load → GPU-skin → render a glTF rig inside Reaper, show it in a
dockable ReaImGui panel, and hold ≥60 fps?

> Status legend: ✅ established · ⏳ **needs Antho's run on the Windows reference workstation** · ⚠️ risk/blocker

---

## Verdict

**GO** — measured on Antho's reference workstation (Ryzen 9 9900X / RX 9070), 2026-06-23.

One-line rationale: the full stack runs in Reaper without crashing, a real Mixamo
rig loads and animates correctly, and render+readback costs only **3.5 ms
(≈280 fps capacity)** — the CPU-readback bridge has ~8× headroom over the 60 fps
target. The only thing capping the on-screen rate to **32 fps is Reaper's ~30 Hz
extension timer**, not rendering or the bridge. That redraw-cadence question is a
bounded Phase 0.5 follow-up, not a feasibility blocker.

---

## Finding 1 — The FBO bridge ⚠️ (the headline result, established from ReaImGui source)

**The clean zero-copy bridge in architecture D11 / AR10 is NOT achievable through
ReaImGui's public API.** Confirmed by reading ReaImGui v0.10.0.5 `api/image.cpp`,
`api/drawlist.cpp`, and `src/`:

- ✅ `ImGui_Image(ctx, ImGui_Image*, w, h, …)` takes a **ReaImGui `Image` object**, not
  a raw `ImTextureID`/`GLuint`. Every way to create one — `CreateImage` (file),
  `CreateImageFromMem` (PNG/JPEG bytes), `CreateImageFromLICE` (LICE bitmap),
  `CreateImageFromSize` + `Image_SetPixels_Array` (raw pixels) — **copies pixel
  data into ReaImGui's own backend**. There is no "adopt my existing GPU texture"
  entry point.
- ✅ **ReaImGui owns its render backend per-platform and we don't control it:**
  Windows uses `d3d10_renderer.cpp` **or** `win32_opengl.cpp`; macOS uses Metal
  (`metal_renderer.mm`); Linux uses GDK OpenGL. So we cannot assume our sokol GL
  texture even lives in the same API/context as ReaImGui's renderer.
- ✅ **There is no `DrawList_AddCallback`** — the DrawList API exposes only
  high-level primitives. So the architecture's documented fallback (c) "ImGui
  DrawList custom GL callback" **does not exist in ReaImGui** (it assumed raw Dear
  ImGui).

⟹ **The only viable bridge is GPU→CPU→GPU readback:** sokol/GL renders to an
offscreen FBO → `glReadPixels` → upload pixels into a ReaImGui `Image` via
`CreateImageFromSize` + `Image_SetPixels_Array` (format `0xRRGGBBAA`). That is what
this spike implements ([spike_renderer.cpp](../src/spike_renderer.cpp) readback +
[spike_main.cpp](../src/spike_main.cpp) upload).

**Implication for production (rewrites Epic 1 Story 1.2 / Phase 0.5, AR20):**
the D11 decision must be amended. Either (a) accept the readback cost if it holds
60 fps at panel resolution (measure below), or (b) reconsider whether the 3D
viewport should be a ReaImGui panel at all vs. a Reaper-docked child GL window
(which *can* do zero-copy GL) — trading ReaImGui's docking ergonomics for GPU
efficiency. **This is an architecture decision for Antho + the architect, not a
code detail.**

Measured readback cost: ⏳ _(see fps below; note panel size at measurement)_

---

## Finding 2 — sokol_gfx pin has moved to a new API ⚠️ (established from the vendored header)

✅ The pinned sokol_gfx `85d1f1b` ships the **new view-based** pass/attachment API
(`sg_view`, `sg_gl_query_view_info`; `sg_make_attachments` is gone) — diverged
from what D11/D12 assumed. To avoid conflating "learn the new sokol API" with the
real question (the ReaImGui bridge), **the spike renders with raw OpenGL**, which
exercises the same risks (GL context in Reaper, GPU skinning, readback). Sanctioned
spike deviation (AC1 names sokol; see Change Log in the story).

**Implication for production:** Phase 0.5 must validate sokol's new view-based
offscreen-pass API specifically, or re-pin sokol to a commit matching the
architecture's assumed API. Either way, D12 needs a small refresh.

---

## Finding 3 — Driving ReaImGui from a C++ extension ✅ (from the official example)

✅ Pattern confirmed via ReaImGui's `examples/hello_world.cpp`: `ImGui::init(plugin_getapi)`
once, `ImGui::CreateContext(name, flags)`, then `plugin_register("timer", &loop)` and
build the UI each tick with `ImGui::Begin/End`; ImGui calls throw `ImGui_Error` (must
be caught at the loop boundary). The spike follows this exactly.

⏳ **Verify on run:** that a Reaper-`timer`-driven loop renders smoothly and that the
window docks into a Reaper docker with `ConfigFlags_DockingEnable`.

---

## Runtime results — measured 2026-06-23 (Ryzen 9 9900X / RX 9070)

| Item | Result |
|---|---|
| Build (`build_spike.bat` / CMake VS2022 x64) | ✅ pass (after enabling FBX importer) |
| ReaImGui header at `extern/reaimgui/include/reaper_imgui_functions.h` | ✅ (v0.10.0.5, fetched from release) |
| DLL loads in Reaper, action `FBXAV: Open Spike Viewer` appears | ✅ |
| GL context coexists with Reaper — no crash/corruption (AC2) | ✅ |
| Deforming rig visible & animating in the docked panel (AC1) | ✅ (after the Mixamo pivot fix below) |
| **Render + readback cost** | ✅ **3.5 ms → ~280 fps capacity** |
| **On-screen rate (tick)** | **32 fps — capped by Reaper's ~30 Hz extension timer, NOT by rendering** |
| Render-capacity ≥ 60 fps (NFR-P1 perf headroom) | ✅ yes, ~8× margin |
| On-screen ≥ 60 fps | ❌ not yet — blocked only by redraw cadence (Phase 0.5 follow-up) |
| Shaders (`#version 330`) compiled | ✅ (legacy WGL context was sufficient) |

Fixture used: a rigged + animated character exported from **Mixamo (FBX, With Skin)**.

---

## Coordinate / convention surprises (found 2026-06-23)

- 🔑 **Mixamo / assimp FBX pivot nodes (de-risks Epic 3 + Epic 6 FBX).** By default
  assimp's FBX importer splits each bone into hidden `$AssimpFbx$` pre/post-transform
  nodes and keys the animation on *those*, not on the bone node. A name-based
  channel→bone map (the natural approach) then finds nothing and the rig stays in
  bind pose (observed: "it played something else" = only the camera moved). Fix:
  `importer.SetPropertyInteger(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, 0)` — bakes the
  pivots and keys animation on real bones. **The production FBX loader must set this.**
- glTF/FBX anim time-unit (`mTicksPerSecond`) handled as seconds = mTime/tps (default
  25 if 0); Mixamo's value worked correctly with the animation playing at the right speed.
- Orientation/scale: rendered fine via the auto-fit camera (Mixamo's cm scale absorbed).

---

## Blockers / amendments that should reshape a Phase spec (AR20)

1. **D11 / Epic 1 Story 1.2 — the bridge is CPU readback (not zero-copy), and that
   is FINE.** ReaImGui can't take a GPU texture, so we render to an FBO and push pixels
   via `Image_SetPixels_Array`. Measured cost **3.5 ms (~280 fps)** — ample headroom.
   Amend D11 to specify the readback bridge; drop the zero-copy assumption. The earlier
   worry that readback would be too slow is **disproven**.
2. **Redraw cadence (the one real follow-up) — on-screen 32 fps = Reaper's ~30 Hz
   extension timer, not perf.** Phase 0.5 must determine how to drive the panel redraw
   at 60 Hz (faster tick / ReaImGui refresh mechanism), or consciously accept ~30 fps
   for the preview. Not a feasibility blocker.
3. **D12 — sokol pin `85d1f1b` uses a new view-based API.** Spike used raw GL; Phase 0.5
   must validate or re-pin sokol (and confirm GPU skinning carries over — it worked in
   raw GL here).
4. **FBX loader must set `PreservePivots=0`** (see convention surprises) — feeds Epic 3/6.

---

## Disposition

Per the spike charter: the branch is **not merged**. Production resumes from Epic 1
on `main`; the conclusions above are folded by hand into the Phase 0.5 / 1 / 2 specs.
