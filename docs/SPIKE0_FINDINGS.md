# Spike 0 — Feasibility findings (go/no-go)

**Branch:** `spike/0-1-feasibility` (THROWAWAY — not merged to `main`)
**Story:** [0-1-render-a-skinned-animation-in-a-reaper-hosted-fbo-panel-and-measure-fps](../_bmad-output/implementation-artifacts/0-1-render-a-skinned-animation-in-a-reaper-hosted-fbo-panel-and-measure-fps.md)
**Question:** Can we load → GPU-skin → render a glTF rig inside Reaper, show it in a
dockable ReaImGui panel, and hold ≥60 fps?

> Status legend: ✅ established · ⏳ **needs Antho's run on the Windows reference workstation** · ⚠️ risk/blocker

---

## Verdict

**GO / NO-GO / CONDITIONAL:** ⏳ _(fill after running on the reference workstation)_

One-line rationale: _______________________________________________

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

## Runtime results ⏳ (Antho — capture these on the reference Windows workstation)

| Item | Result |
|---|---|
| Build: `cmake -B build -G "Visual Studio 17 2022" -A x64` then `cmake --build build --config Release` | ⏳ pass / errors: ______ |
| ReaImGui header dropped at `extern/reaimgui/include/reaper_imgui_functions.h` | ⏳ |
| DLL loads in Reaper, action `FBXAV: Open Spike Viewer` appears | ⏳ |
| GL context coexists with Reaper — no crash/corruption (AC2) | ⏳ |
| Deforming rig visible & animating in the docked panel (AC1) | ⏳ |
| **Measured fps** (fixture: ____ tris / ____ bones, panel ____×____) | ⏳ ____ fps |
| fps ≥ 60 target (NFR-P1) | ⏳ yes / no |
| Shaders compiled (if `#version 330` failed, note it — needs a core-profile context) | ⏳ |

Fixture used (path / source): __________________________________

---

## Coordinate / convention surprises ⏳

_(handedness, up-axis, units, bind-pose correctness; glTF anim time-unit / `mTicksPerSecond`
behavior — the loader assumes seconds = mTime / ticksPerSecond. Record anything that
rendered tilted, mis-scaled, or mis-posed — directly de-risks Phase 2/Epic 3.)_

- ______________________________________________

---

## Blockers / amendments that should reshape a Phase spec (AR20)

1. **D11 / Epic 1 Story 1.2 — FBO bridge is readback-only with ReaImGui.** _(decide a/b above)_
2. **D12 — sokol pin uses new view API.** _(validate or re-pin in Phase 0.5)_
3. ______________________________________________

---

## Disposition

Per the spike charter: the branch is **not merged**. Production resumes from Epic 1
on `main`; the conclusions above are folded by hand into the Phase 0.5 / 1 / 2 specs.
