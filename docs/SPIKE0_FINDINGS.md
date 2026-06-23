# Spike 0 — Feasibility findings (go/no-go)

**Branch:** `spike/0-1-feasibility` (THROWAWAY — never merged to `main`)
**Story:** [0-1-render-a-skinned-animation-in-a-reaper-hosted-fbo-panel-and-measure-fps](../_bmad-output/implementation-artifacts/0-1-render-a-skinned-animation-in-a-reaper-hosted-fbo-panel-and-measure-fps.md)
**Workstation:** Antho's reference Windows PC — AMD Ryzen 9 9900X / Radeon RX 9070
**Dates:** 2026-06-22 → 2026-06-23

---

## Verdict: 🟢 GO (full stack, beyond the original spike scope)

Everything the production concept depends on was proven to work inside Reaper, with
large performance headroom:

- ✅ assimp loads a real **Mixamo FBX** rig; **GPU vertex skinning** poses it correctly.
- ✅ Renders inside Reaper's process via a GL context **without crashing/corrupting** the host.
- ✅ **Docked viewport at ~62 fps** at a real panel size (1055×604).
- ✅ **Transport integration**: dropping the file creates a correctly-sized **timeline item**, and the **Reaper playhead drives the displayed frame** at full frame rate.

Production resumes from Epic 1 on `main`, folding in the decisions below. The spike
branch is **not merged**.

---

## Measured results

| Item | Result |
|---|---|
| Build (`build_spike.bat`, MSVC x64, FetchContent assimp v6.0.5 + GLM 1.0.3) | ✅ |
| GL context coexists with Reaper, no crash (AC2) | ✅ |
| Mixamo rig loads + animates correctly (AC1) | ✅ (after the pivot fix, Finding 3) |
| Render cost (skinning + draw) | ~1.8–3.5 ms → **~280–600 fps capacity** |
| **ReaImGui panel** on-screen rate | **~32 fps — hard cap** (see Finding 1) |
| **Docked OpenGL window** on-screen rate | **~62 fps** (1055×604) ✅ |
| Drop `.fbx` → timeline item of animation length | ✅ |
| Playhead drives displayed frame | ✅ (console tag `[timeline-driven]`) |
| NFR-P1 ≥ 60 fps on-screen | ✅ **via the docked GL window** (not via ReaImGui) |

---

## Finding 1 — 🔑 Viewport = native docked OpenGL window, NOT a ReaImGui panel

The decisive architectural outcome, settled on evidence:

- A **ReaImGui panel cannot exceed ~30 fps** for displayed content. Proven: feeding
  ReaImGui at ~66 Hz left its own framerate stuck at ~32 fps — ReaImGui's presentation
  is tied to Reaper's ~30 Hz UI loop, independent of how fast we feed it. ReaImGui also
  cannot accept a GPU texture (its `Image` takes a pixel-data resource; no GPU-handle
  adoption; no `DrawList` callback) — so any GPU viewport inside ReaImGui needs a
  per-frame CPU readback **and** is capped at 30 fps anyway.
- A **raw OpenGL window docked via Reaper's native docker** (`DockWindowAddEx`) renders
  directly (no FBO/readback), driven by our own ~66 Hz timer + `SwapBuffers`, and hits
  **~62 fps**.

**Decision:** the 3D viewport is a **native docked GL window**, not a ReaImGui panel.
It still docks like any Reaper panel. ReaImGui remains available for *auxiliary* UI
(the Epic 5 animation browser, a small toolbar) as separate docked panels.

**Rewrites the Phase 0.5 / Epic 1 plan:** decision **D11/AR10** ("ReaImGui panel +
sokol FBO bridge") is replaced by "native docked GL window, direct render." This also
removes the readback path entirely from the viewport (simpler and faster).

> Plain-language note: visually it's the same for the user (a dockable 3D panel in
> Reaper). The change is purely "how it's built underneath," and it's what buys 60 fps.

## Finding 2 — Transport integration works (the core product mechanic)

A minimal **PCM_source** registered via `Register("pcmsrc", …)`:
- Dropping a `.fbx/.glb/.gltf` on a track **creates a timeline item** whose **length =
  animation duration** (`GetLength()`), as a silent/0-channel non-audio source.
- The viewer maps the **playhead → frame**: `animTime = playPos − itemStart` (found by
  iterating items and matching our source type), and renders that pose; it falls back
  to a wall-clock loop when no item spans the cursor.
- **During playback it's genuinely ~60 fps**, because Reaper's play position advances
  continuously (audio clock), not at the 30 Hz UI rate — sampling it at ~60 Hz yields
  60 distinct poses/sec.

**Validates D8/AR11** (PCM_source as the central Reaper integration surface) — the
architecture had flagged the exact registration mechanism as "to verify." It is:
`pcmsrc_register_t { CreateFromType, CreateFromFile, EnumFileExtensions }`. Production
(Epic 4) builds the real version (SaveState/LoadState, per-item asset, item selection
by track priority, etc.) on this confirmed mechanism.

## Finding 3 — 🔑 Mixamo / assimp FBX needs `PreservePivots=0`

By default assimp's FBX importer splits each bone into hidden `$AssimpFbx$`
pre/post-transform nodes and keys the animation on *those*, not the bone — so a
name-based channel→bone map finds nothing and the rig stays in bind pose (observed as
"it animated something else" — only the camera moved). Fix, **required in the
production FBX loader**:
```cpp
importer.SetPropertyInteger(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, 0);
```
Feeds Epic 3 (skinning) and Epic 6 (FBX validation). glTF/FBX anim time-unit
(`mTicksPerSecond`) handled as seconds = mTime/tps; Mixamo played at correct speed.

## Finding 4 — sokol_gfx pin `85d1f1b` ships a new view-based API

`sg_make_attachments` is gone (replaced by `sg_view`). The spike rendered with **raw
OpenGL** to isolate the real questions from a sokol-API relearn. **Phase 0.5 must
validate sokol's new offscreen/view API or re-pin sokol** — or, given Finding 1 (direct
render to a window, no FBO), reconsider whether sokol is even needed for MVP vs. the
small amount of raw GL the spike already demonstrates.

---

## Blockers / amendments for the production specs (AR20)

1. **D11/AR10 replaced** — viewport is a native docked GL window (direct render), not a
   ReaImGui panel + FBO bridge. ReaImGui only for auxiliary panels. *(Phase 0.5 / Epic 1)*
2. **D12 / sokol** — re-pin or validate the new sokol view API, or drop sokol in favor
   of the raw-GL approach proven here. *(Phase 0.5 / Epic 1)*
3. **D8 confirmed** — PCM_source mechanism verified; build the real source in Epic 4.
4. **FBX loader must set `PreservePivots=0`.** *(Epic 3 / Epic 6)*
5. Minor: docked-window lifecycle (close via docker X), vsync policy, and per-item asset
   loading are spike-stubbed — production details for Epic 1/4.

## Disposition

Branch `spike/0-1-feasibility` stays **unmerged**. Conclusions above are folded by hand
into the Phase 0.5 / Epic 1–4 specs. Production resumes on `main`.
