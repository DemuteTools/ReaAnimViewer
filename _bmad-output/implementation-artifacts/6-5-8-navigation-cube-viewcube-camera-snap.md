---
baseline_commit: 83b40da73f2fcd413d98d8529fa83eaf148944e0
---

# Story 6.5.8: Navigation cube (ViewCube) for camera snap

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer orienting a character in the viewport,
I want a small colored cube in the bottom-right that rotates with the camera and lets me click a face/edge/corner to jump to that view,
so that I always know which way I'm looking and can reach a clean front/side/top/three-quarter view in one click.

## Acceptance Criteria

1. **(FR54 — orientation cue)** With the viewport open and a model loaded, a small **colored cube** is drawn in the **bottom-right corner** (axis-colored faces) that **rotates in lock-step with the camera** — its drawn orientation always reflects the current `OrbitCamera` yaw/pitch, so it is a constant read of where the camera points.
2. **(highlight)** Hovering a **face**, an **edge**, or a **corner** of the cube **highlights** that element (visually distinguished from the rest), so it is clear what a click would select.
3. **(snap on click)** Clicking a cube element **smoothly animates the camera (~0.2 s)** to look at the scene centre **from that element's direction**, **keeping the current distance**: a face → straight-on that face, an edge → the 45° bisector of its two faces, a corner → the ~35° three-quarter view down that corner. **The angle is derived purely from the element's direction-to-centre — no hardcoded angle is written down.**
4. **(camera contract preserved)** The snap **only changes the camera's `yaw`/`pitch`** — `target`, `distance`, `frameRadius`, and zoom are untouched; the resulting view persists through project save/load via Story 6.2 like any other camera state (no new persisted state of its own).
5. **(gimbal + shortest path)** The **top/bottom** snap respects the existing gimbal clamp (`pitch ∈ [−1.55, +1.55]`, `camera.h:57`) — it lands just under ±90°, never exactly at the pole; and the **yaw tween takes the shortest path** (wraps to the nearest equivalent angle — never spins the long way around).
6. **(boundary + non-fatal + no regression)** The widget is **GL/ImGui-boundary-only** and **non-fatal**: if ImGui is not ready it is a **no-op** and the viewport still runs (AR17). **Session-only** — no new persisted/registered state, **AR15** register-symmetry untouched. **D2 zero-alloc per-frame** (draws via ImGui's draw list, like the existing tool UI). **Orbit/zoom/pan/reset (FR22–FR26) are unchanged**, and the cube never steals the mouse from the camera except on its own hit area.
7. **(perf + gate)** **≥60 fps** holds (NFR-P1). Validation is **Antho's in-Reaper Windows gate §11** (AR19) — feel constants (cube size, margin, tween duration, hit-cell sizing) are tuned there, not on the Linux dev box.

## Tasks / Subtasks

- [x] **Task 1 — `camera.h`: add snap target + shortest-path tween (AC: 3, 4, 5)**
  - [x] Add tween state to `OrbitCamera`: `float targetYaw`, `float targetPitch`, `bool animating = false`, and a tween clock (`float animElapsed`, plus a `kSnapDuration ≈ 0.2f` constant near the other gate-tunable knobs, `camera.h:23-30`).
  - [x] Add `void SnapToDirection(const glm::vec3& dirFromCentre)`: convert the unit direction-from-centre into `yaw`/`pitch` using the **inverse of `Eye()`'s spherical form** (`camera.h:39-44`: `eyeOffset = distance * (cos(pitch)·sin(yaw), sin(pitch), cos(pitch)·cos(yaw))`). So `pitch = asin(clamp(dir.y, -1, 1))`, `yaw = atan2(dir.x, dir.z)`. Store these into `targetYaw`/`targetPitch`, **clamp `targetPitch` to ±1.55** (AC5 — never the pole), and set `animating = true`, `animElapsed = 0`. **Do NOT touch `target`, `distance`, `frameRadius`** (AC4).
  - [x] **Shortest-path yaw** (AC5): before tweening, unwrap `targetYaw` to the nearest equivalent of the current `yaw` — `targetYaw = yaw + remainderf(targetYaw - yaw, 2π)` (or add/subtract 2π until `|targetYaw - yaw| ≤ π`). Pitch needs no unwrap (already clamped to a sub-π band).
  - [x] Add `void AdvanceAnim(float dt)`: if `!animating` return; advance `animElapsed += dt`, compute `t = clamp(animElapsed / kSnapDuration, 0, 1)`, ease (smoothstep `t*t*(3-2t)` for a soft start/stop), lerp `yaw`/`pitch` from their **stored start** toward the targets. **Stash the start yaw/pitch** when `SnapToDirection` arms the tween (e.g. `startYaw`/`startPitch`) so the lerp is start→target, not incremental drift. When `t ≥ 1`, snap exactly to target and set `animating = false`. Keep it **header-only inline** (no `.cpp`, no CMake change — `camera.h:8-9`).
  - [x] A live orbit/pan/zoom or `Reset()` while a tween is in flight should **cancel the tween** (`animating = false`) so the user's drag wins — verify `Orbit/Pan/Zoom/Reset` either clear `animating` or the dev adds a one-line `animating = false` to each (cheap, avoids a fighting tween). `Reset()` already overwrites yaw/pitch (`camera.h:92-93`) — just also clear `animating`.

- [x] **Task 2 — `renderer.{h,cpp}`: thin pass-through (AC: 1, 3)**
  - [x] `renderer.h`: add inline pass-throughs next to `Camera()`/`ResetCamera()` (`renderer.h:76-80`): `void SnapCameraTo(const glm::vec3& dir) { cam_.SnapToDirection(dir); }` and `void AdvanceCameraAnim(float dt) { cam_.AdvanceAnim(dt); }`. (The widget can also reach `cam_` directly via the existing `OrbitCamera& Camera()` getter — expose whichever reads cleaner; a getter for current `yaw`/`pitch` is unnecessary since `Camera()` already returns the mutable camera.)
  - [x] **No new GL resources, no shader change** — the cube draws via ImGui's draw list in `viewer_window.cpp` (Task 3), exactly like `LightDirectionPad`. `renderer.cpp` stays otherwise **net-of-this-change** (only the two inline forwards in the header if you prefer pass-throughs over direct `Camera()` access).

- [x] **Task 3 — `viewer_window.cpp`: `NavCubeWidget()` ImGui overlay, bottom-right (AC: 1, 2, 3, 6)**
  - [x] Model it on **`LightDirectionPad`** (`viewer_window.cpp:399-434`): `ImGui::GetWindowDrawList()` + `ImGui::InvisibleButton` for the hit area + `ImDrawList` primitives; **mutate state only**, push to the renderer on click (AR18, no synchronous re-render).
  - [x] **Placement (bottom-right):** a **second frameless ImGui window** (its own `ImGui::Begin("##navcube", …)` with the same `kFlags` as `##tools`), positioned with `ImGui::SetNextWindowPos(ImVec2(g_client_w - margin, g_client_h - margin), ImGuiCond_Always, ImVec2(1.0f, 1.0f))` (bottom-right pivot) — mirrors the FPS readout's right-edge pivot (`viewer_window.cpp:600-601`) but with a bottom-right pivot. Use `g_client_w`/`g_client_h` (live size, kept current in WM_SIZE). Draw it inside the **same `NewFrame`/`Render` pair** as `DrawToolUi` (never a second `NewFrame`) — call `NavCubeWidget()` from inside `DrawToolUi`, after the `##tools` window and the read-out overlays, before `ImGui::Render()` (`viewer_window.cpp:618`).
  - [x] **Cube orientation (AC1):** read the live camera (`g_renderer.Camera().yaw`/`.pitch`) and build the **camera's view rotation** the cube is seen through. Project the 8 cube corners (±1,±1,±1) through that rotation to 2D screen offsets around the widget centre (orthographic; ignore translation, fixed cube scale). The cube must **rotate in lock-step with the camera** — when the user orbits, the drawn cube turns the same way (it shows the scene's axes from the camera's vantage). Axis-color the faces (e.g. +X/−X, +Y/−Y, +Z/−Z distinct colors, per the reference screenshots). **Draw only visible (front-facing) faces** (back-face cull by winding/normal·viewDir) so the picked element is the one facing the user.
  - [x] **Picking (recommended — Autodesk 3×3 per visible face, AC2/AC3):** subdivide each visible face into a 3×3 grid → centre cell = that **face**, edge cells = the **edge** shared with the neighbour face, corner cells = the **corner**. 6 faces + 12 edges + 8 corners = **26 pickable directions**, each a unit vector of ±1 axis components **normalized** (face = one axis, edge = two, corner = three). Hit-test the cursor (`ImGui::GetIO().MousePos` vs the projected cells) only when `ImGui::IsItemHovered()`/the invisible button covers the cube. *(Alternative: project-and-pick the cube geometry directly — acceptable if cleaner at dev time. Either way the output is a single unit direction.)*
  - [x] **Highlight (AC2):** the hovered element's cell/face/edge/corner is drawn brighter / outlined (visually distinguished). No highlight when the cursor is off the cube.
  - [x] **Click → snap (AC3):** on click of an element, call `g_renderer.SnapCameraTo(dir)` (or `g_renderer.Camera().SnapToDirection(dir)`) with that element's **normalized direction-from-centre**. **Do not compute or hardcode any angle** — pass the direction; the camera derives yaw/pitch (Task 1). The 45°/35° outcomes fall out of the edge/corner geometry automatically.
  - [x] **No-op safety (AC6):** guard the whole widget on `g_imgui_ready` (already true inside `DrawToolUi`, which returns early at `viewer_window.cpp:444` — so a call from there is already covered). The widget is **NoSavedSettings**; do not let it steal the mouse outside its hit area (its `InvisibleButton` only captures over the cube — like the light pad; the camera arbitration via `ImGuiWantsMouse()`/`WantCaptureMouse` at `viewer_window.cpp:798` already routes a click over the widget to ImGui, not the orbit handler).

- [x] **Task 4 — advance the tween once per frame (AC: 3, 5)**
  - [x] In `RenderTick` (`viewer_window.cpp:281-359`), compute a per-frame `dt` (QPC delta — the tick already queries `QueryPerformanceCounter`; keep a `g_last_tick` `LARGE_INTEGER` and derive `dt` in seconds, or reuse the existing fps QPC reads) and call `g_renderer.AdvanceCameraAnim(dt)` (or `g_renderer.Camera().AdvanceAnim(dt)`) **immediately before** `g_renderer.RenderFrame(...)` (`viewer_window.cpp:338`). Because `RenderFrame` derives `view_ = cam_.ViewMatrix()` at `renderer.cpp:716`, advancing the tween first means the new yaw/pitch is reflected **this same frame** — smooth, no stutter.
  - [x] Clamp `dt` to a sane max (e.g. `min(dt, 0.1f)`) so a paused/resumed panel (the `g_render_paused` gap, `viewer_window.cpp:373-385`) doesn't teleport the tween. Reset/seed `g_last_tick` on resume (mirror the fps `g_fps_last` reseed at `viewer_window.cpp:382`).

- [x] **Task 5 — scope audit + docs (AC: 6)**
  - [x] **AR15 grep gate (must be empty):** confirm the diff touches **only** `camera.h` + `renderer.{h,cpp}` + `viewer_window.cpp`. `git diff --stat -- src/` shows no `rec->Register` / `REAPERAPI_WANT_` / `plugin_main` / `reaper_api` / `pcm_source` / `asset_loader` / `gl_loader.h` / `gpu_resources.h` / `scene.h` / `console_log` / `CMakeLists.txt` change. (camera.h is header-only → no CMake source change; renderer.cpp change is optional/inline-only.)
  - [x] **`architecture.md` AR20 Spec Change Log** — add a short "Navigation cube (ViewCube)" entry (mirrors the 6.5.6/6.5.7 entries) **only if** a new tunable constant is introduced (`kSnapDuration` tween duration, cube size/margin): note that the camera-snap **derives yaw/pitch from the element's direction-to-centre, preserving distance** (no hardcoded angle), shortest-path yaw, top/bottom clamped to the gimbal limit. AR15 register-symmetry untouched; session-only; D2 zero-alloc.
  - [x] **`docs/PHASE4.5_VALIDATOR_GATE.md` §11** — author the gate section (mirror §10's structure: intro + scope note + fixtures + a click-based check table + a **PENDING** Result line). The §11 checks are click-based (Antho hovers/clicks the cube in-Reaper) — see Dev Notes "Gate §11" for the suggested table.
  - [x] **`deferred-work.md`** — record anything intentionally out of scope (see Dev Notes "Out of scope").

### Review Findings (code review 2026-06-29)

- [x] [Review][Decision] **Top/bottom face dead-centre click resets yaw to 0 (azimuth spin)** — RESOLVED (Antho, 2026-06-29): *keep as-is* — tested in-Reaper, the cube behaviour suits him; no patch. Detail: clicking the exact centre of the +Y or −Y face yields `hitDir = (0, ±1, 0)`, so `SnapToDirection`'s inverse map gives `yaw = atan2(0, 0) = 0` and the tween drives the azimuth to `yaw = 0` (looking along +Z) regardless of the prior heading — a horizontal sweep going overhead. `pitch` is correctly clamped to ±1.55 (AC5). The azimuth-on-overhead is a feel choice; Antho's in-Reaper §11 read accepts the current canonical "+Z up-north" landing. (Off-centre top/bottom clicks have non-zero x/z and are unaffected.) Dismissed siblings (all noise): `remainder` ±π tie-break (still shortest-path), first-array-hit on shared silhouette edge (faces tile w/o overlap), QPF==0 freeze (impossible on Windows), tiny-window overlap (clamped on-screen), `kSnapDuration<=0` (guarded).

## Dev Notes

### What this story is
A **net-new navigation gizmo** (ViewCube, as in Maya / Fusion / Blender): a bottom-right colored cube that (a) **shows** the current orientation by rotating with the camera, and (b) **sets** the orientation when you click a face/edge/corner — snapping the camera to look at the scene centre from that element's direction at the current distance. It is **pure pre-ship UX polish, fully session-side**, orthogonal to all other 6.5 stories. The clicked view persists for free via Story 6.2 (the camera state already serializes).

### The one geometric principle (do not over-think it)
**No angle is ever hardcoded.** A clicked element → a unit **direction from the cube centre** → place the camera along that direction looking at centre, keep distance. The viewing angle *falls out of the geometry*: a **face** = straight-on (one ±axis), an **edge** = the **45°** bisector of two faces (two ±axes), a **corner** = the **~35°** three-quarter (three ±axes). Never write 45 or 35 anywhere — `SnapToDirection(normalize(±1 components))` produces them.

### Reuse — build on two shipped foundations, do NOT reinvent
- **`OrbitCamera` (Story 2.4, `src/camera.h`)** — the view is **derived from `{target, distance, yaw, pitch}` every frame** (AR13: the camera derives the view; the cube *sets* yaw/pitch, nothing remaps the scene). `Eye()` (`camera.h:39-44`) is the **forward** spherical map; `SnapToDirection` is its **inverse**. The gimbal clamp (`pitch ∈ [−1.55, +1.55]`, `camera.h:57`) is the AC5 top/bottom guard — clamp the target pitch, never the pole. `Reset()` (`camera.h:78-94`) already rewrites yaw/pitch; clearing `animating` there avoids a fighting tween.
- **`LightDirectionPad` (Story 6.5.3, `viewer_window.cpp:399-434`)** — the **exact pattern** to copy: `GetWindowDrawList()` + `InvisibleButton` (hit area) + `ImDrawList` primitives, frameless ImGui window, mutate-state-only, push to renderer on interaction, returns/acts on change. The NavCube is "LightDirectionPad but it draws a 3D cube instead of a flat disc and snaps the camera instead of the light." **Same spherical convention** (Y-up, AR9) — `LightDirFromAngles` (`viewer_window.cpp:186-190`) is the same `cos(elev)·sin/·cos(azim)` form as `Eye()`; reuse the math idiom.

### Existing frame flow (where everything plugs in)
`RenderTick` (`viewer_window.cpp:281`) → poll transport → **[Task 4: `AdvanceCameraAnim(dt)`]** → `g_renderer.RenderFrame(...)` (`:338`, derives `view_ = cam_.ViewMatrix()` at `renderer.cpp:716`) → `DrawToolUi()` (`:342`, the ImGui overlay — **[Task 3: call `NavCubeWidget()` inside here]**) → `SwapBuffers` (`:344`) → fps roll-up. Per-frame order means: click registers in `DrawToolUi` this frame → next frame `AdvanceCameraAnim` starts moving → `RenderFrame` shows it. One-frame latency, imperceptible. The cube reads `g_renderer.Camera().yaw/.pitch` **after** `RenderFrame` for the current frame's orientation (already up to date).

### Source tree — files to touch (and ONLY these — AR15)
- **`src/camera.h`** — `SnapToDirection` + `AdvanceAnim` + tween state + `kSnapDuration`. **Header-only, inline, no CMake change** (`camera.h:8-9`).
- **`src/renderer.{h,cpp}`** — thin inline pass-throughs (`SnapCameraTo`, `AdvanceCameraAnim`) next to `Camera()`/`ResetCamera()` (`renderer.h:76-80`). Or skip them and use the existing `Camera()` getter. **No new GL resources, no shader change.**
- **`src/viewer_window.cpp`** — `NavCubeWidget()` (the bulk), called from `DrawToolUi`; `dt` + `AdvanceCameraAnim` in `RenderTick`. New globals if needed (cube colors/size constants) go with the other 6.5.x `g_*` UI state near `viewer_window.cpp:130-182`.

### Architecture compliance (invariants — non-negotiable)
- **AR15 — canonical register-symmetry untouched.** No `rec->Register`, no new `REAPERAPI_WANT_*`, no `CMakeLists.txt` / `plugin_main` / `reaper_api` / `pcm_source` / `asset_loader` / `gl_loader.h` / `gpu_resources.h` / `scene.h` / `console_log` change. The grep gate (Task 5) is the proof. This story is the same boundary-clean shape as 6.5.3/6.5.7.
- **AR17 — non-fatal.** `DrawToolUi` already returns early if `!g_imgui_ready` (`viewer_window.cpp:444`); the NavCube inherits that. A widget that can't draw is a no-op; the viewport runs.
- **AR18 — main-thread / no-throw / no synchronous re-render.** The widget runs on the pump-driven render tick; click handlers **mutate state only** (set the tween target) and let the next `RenderFrame` show it — exactly like every other tool widget (`viewer_window.cpp:476-477`, `505`).
- **AR13 — camera derives the view.** The cube sets `yaw`/`pitch`; the view is still derived each frame in `RenderFrame`. Nothing remaps the scene.
- **D2 — zero-alloc per-frame.** The tool UI already rebuilds its draw list every frame; the NavCube adds only `ImDrawList` calls + a fixed-size set of projected corners (stack/array, no heap). The tween is a few floats. No per-frame allocation.
- **D14 — camera controller.** `SnapToDirection`/`AdvanceAnim` are new members of the existing `OrbitCamera`; they preserve `{target, distance, frameRadius}` and only move `{yaw, pitch}`.
- **Session-only / D9 untouched.** No new persisted state; `pcm_source_anim.cpp` byte-for-byte unchanged. The resulting camera persists via Story 6.2 (panel/viewport state already serializes `OrbitCamera`) — **do not add any persistence here.**
- **NFR-P1 ≥60 fps.** The cube is a handful of filled polys + lines per frame; negligible.

### Critical edges (each maps to an AC — handle them)
1. **Top/bottom face → gimbal pole (AC5).** A +Y (top) or −Y (bottom) face direction maps to `pitch = ±π/2`, which `lookAt` degenerates at (eye aligns with world-up). **Clamp `targetPitch` to ±1.55** in `SnapToDirection` (the existing `Orbit` limit, `camera.h:57`) — land just under the pole, never on it. The cube top/bottom click gives the cleanest near-top view the gimbal allows.
2. **Yaw shortest path (AC5).** Naive lerp from yaw 3.0 to −3.0 spins ~6 rad the long way. **Unwrap** the target to within ±π of the current yaw before tweening (`remainderf`, or add/subtract 2π). Without this the snap "spins the long way around" — a named risk in the proposal.
3. **Tween vs live drag.** If the user grabs orbit/pan/zoom mid-tween, **cancel the tween** (`animating = false`) so the drag wins — don't let a stale tween fight the cursor. `Reset()` (Recenter) likewise clears it (it overwrites yaw/pitch anyway).
4. **Start-pose capture.** `AdvanceAnim` lerps **start→target**, so capture `startYaw`/`startPitch` when arming the tween (in `SnapToDirection`), not the live yaw each frame (that would ease-out incorrectly / never converge cleanly).
5. **`dt` on panel resume.** Clamp `dt` and reseed the tick clock on `g_render_paused` → resume (`viewer_window.cpp:380-385`) so a hidden-then-shown panel doesn't jump the tween a full second.
6. **Back-face cull the picking.** Only the front-facing (visible) faces are pickable, so a click maps to the element the user actually sees — standard ViewCube behavior; avoids picking a hidden back corner.

### Why this can't be judged on Linux (self-review posture)
The Linux dev box compiles the non-`_WIN32` units but **cannot build the `_WIN32` viewer/renderer/ImGui units or open Reaper** — the entire UI lives behind `#ifdef _WIN32` (`camera.h:13`, the renderer/ImGui TUs). So the dev implements + self-reviews against the **proven `LightDirectionPad` pattern** and the **proven `OrbitCamera` spherical math**, and **feel** (cube size, margin, tween duration, hit-cell sizing, colors) is tuned at **Antho's in-Reaper Windows gate §11** — the same constraint as the orbit-sensitivity constants (`camera.h:23-30`) and every other 6.5.x feel knob. Don't try to finalize feel numbers offline; pick sane defaults and flag them gate-tunable.

### Gate §11 (author in `docs/PHASE4.5_VALIDATOR_GATE.md`, mirror §10) — suggested checks
All click-based (Antho hovers/clicks in-Reaper). Result line = **PENDING** until Antho validates.

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Cube present + rotates with camera | Bottom-right shows a small **colored cube**; **right-drag orbit** → the cube **turns in lock-step** with the view. | AC1 |
| 2 | Hover highlights | Hover a **face / edge / corner** → that element **highlights**; move off → highlight clears. | AC2 |
| 3 | Face snap | Click a **face** → camera animates (~0.2 s) to a **straight-on** view of that side, looking at centre. | AC3 |
| 4 | Edge / corner snap | Click an **edge** → ~**45°** view; click a **corner** → clean **3/4** view — both looking at centre, **same distance**. | AC3 |
| 5 | Distance + target preserved | After any snap, **zoom level / framing is unchanged** (only the angle moved); **Recenter** still works. | AC4 |
| 6 | Top/bottom + shortest path | **Top** click → near-overhead (no flip/black); snaps never **spin the long way around**. | AC5 |
| 7 | Tools coexist | **Orbit / zoom / pan / Recenter / Light / MSAA / Shadow** all still work; a drag **mid-tween** takes over cleanly. | AC6 |
| 8 | Perf + clean reopen | **≥60 fps** holds; **close → reopen** → cube back, no ghost/leak, console **silent** (6.5.2). | AC7/AC6 |

### Out of scope (record in `deferred-work.md`)
- Cube **face labels / text** ("Front/Top/Right") — the GL/ImGui text path is minimal; colors + geometry are the MVP cue (add later if Antho wants letters).
- A **persisted** cube size/position or a settings UI for tween duration (in-code constants, gate-tuned).
- **Roll** / arbitrary up-vector, ortho-projection toggle, "home" button, double-click-to-fit — not requested.
- **Animating `target`/`distance`** on snap (we only move the angle, AC4) — a "frame this element" zoom is post-MVP.

### Project Structure Notes
- Touches `src/camera.h`, `src/renderer.{h,cpp}`, `src/viewer_window.cpp` — all behind `#ifdef _WIN32`, all already part of the build. **No new file, no CMake change** (`camera.h` is header-only; the new GL-free widget rides the existing ImGui overlay). Aligns with the boundary the architecture's project-structure section (`architecture.md:743+`, `:878-879`) and AR15 define.
- No conflict with any open 6.5.x story: 6.5.5 (review) is Performance-section UI; 6.5.8 is an independent bottom-right overlay + camera-method addition. Orthogonal.

### References
- [Source: _bmad-output/planning-artifacts/sprint-change-proposal-2026-06-29-viewcube.md] — the full Correct Course proposal (issue, impact, picking model, edges, risks).
- [Source: _bmad-output/planning-artifacts/epics.md#Story-6.5.8] (`epics.md:734`) + FR54 (`epics.md:60`).
- [Source: src/camera.h] — `OrbitCamera` `{target,distance,yaw,pitch}`, `Eye()` spherical form (`:39-44`), `Orbit`/clamp (`:50-59`), `Reset` (`:78-94`), gate-tunable constants (`:23-30`).
- [Source: src/viewer_window.cpp] — `LightDirectionPad` (`:399-434`), `DrawToolUi` (`:442-620`), frame loop `RenderTick` (`:281-359`), `g_client_w/h` (`:105-106`), readout overlay pivot pattern (`:599-605`), camera-vs-ImGui arbitration `ImGuiWantsMouse` (`:798`), `LightDirFromAngles` (`:186-190`).
- [Source: src/renderer.h] — `Camera()` getter (`:76`), `ResetCamera` (`:80`); [src/renderer.cpp] — view derivation `view_ = cam_.ViewMatrix()` (`:716-718`), `ResetCamera` (`:633`).
- [Source: _bmad-output/planning-artifacts/architecture.md] — AR13 camera-tolerance (`:41`), AR15 register-symmetry (`:92`, `:878-879`), AR18 main-thread (`:95`), D2 (`:211`), D14 (`:412`), AR20 Spec Change Log pattern (6.5.6/6.5.7 entries `:1149-1162`, `:1140`).
- [Source: docs/PHASE4.5_VALIDATOR_GATE.md] — §10 (6.5.7) is the format to mirror for §11; AR19 = Antho's in-Reaper Windows judgement IS the gate.

## Dev Agent Record

### Agent Model Used

Claude Opus 4.8 (1M context) — `claude-opus-4-8[1m]`.

### Debug Log References

- **AR15 scope grep gate (Task 5) — empty (clean):** `git diff --stat -- src/` shows **only** `camera.h` (+67), `renderer.h` (+9), `viewer_window.cpp` (+176). A `git diff` grep for `rec->Register | REAPERAPI_WANT_ | plugin_main | reaper_api | pcm_source | asset_loader | gl_loader.h | gpu_resources.h | scene.h | console_log | CMakeLists` over added lines returned **nothing**. `renderer.cpp` is byte-for-byte unchanged (the pass-throughs are inline in `renderer.h`); `camera.h` is header-only (no CMake source change).
- **`camera.h` syntax check (Linux):** `g++ -std=c++17 -D_WIN32 -fsyntax-only -I <glm> -Wall -Wextra` → **exit 0** (no warnings) on a harness that constructs an `OrbitCamera`, calls `SnapToDirection` + `AdvanceAnim`.
- **Picking-math numeric harness** (isolated, glm + the widget's exact projection/inverse-map/cell math): front-face centre click → `(0,0,1)`; top-edge click → `(0, 0.71, 0.71)` (45°); top-right-corner click → `(0.577, 0.577, 0.577)` (three-quarter); after a yaw=90° orbit, centre click → `(1,0,0)` (lock-step rotation). **All asserts passed.**
- **Snap-math numeric harness** (`camera.h` directly, `-D_WIN32`): `SnapToDirection((0,0,1))` → yaw 0 / pitch 0 with `distance`+`target` **preserved** (AC4); `SnapToDirection((0,1,0))` → `targetPitch` **clamped to 1.55**, not π/2 (AC5 gimbal); from yaw 3.0 a target near yaw −3.0 unwraps to 3.283 (**step 0.283**, the short way — never the ~6.0 long way, AC5); smoothstep midpoint eases ~half. **All asserts passed.**
- **ImGui API signatures verified** against the vendored `imgui.h` (v1.91.5): `AddConvexPolyFilled(const ImVec2*, int, ImU32)`, `AddQuad`, `AddQuadFilled`, `InvisibleButton`, `ImGuiWindowFlags_NoBackground` — all present with the signatures used.

### Completion Notes List

Implemented the navigation cube (ViewCube) end-to-end across the 3 permitted files, building on the two shipped foundations exactly as the spec directed (no reinvention):

- **Task 1 — `camera.h` (header-only):** added the tween state (`animating`, `animElapsed`, `start/target {Yaw,Pitch}`) + `kSnapDuration` (≈0.2 s, gate-tunable). `SnapToDirection(dir)` solves the **inverse of `Eye()`** (`pitch = asin(dir.y)`, `yaw = atan2(dir.x, dir.z)`), **clamps `targetPitch` to ±1.55** (the existing Orbit gimbal guard — AC5), and **shortest-path unwraps** `targetYaw` via `std::remainder` (AC5). `AdvanceAnim(dt)` eases the **captured start→target** with smoothstep and disarms at `t≥1`. Moves **only `{yaw, pitch}`** — `target`/`distance`/`frameRadius` untouched (AC4). `Orbit`/`Pan`/`Zoom`/`Reset` each clear `animating` so a live drag/Recenter wins.
- **Task 2 — `renderer.h`:** two inline pass-throughs (`SnapCameraTo`, `AdvanceCameraAnim`) next to `Camera()`/`ResetCamera()`. **No `renderer.cpp` change, no new GL resources, no shader edit.**
- **Task 3 — `viewer_window.cpp` `NavCubeWidget()`:** a 2nd frameless ImGui window (bottom-right pivot, `g_client_w/h`, `NoBackground`), modelled on `LightDirectionPad` — `GetWindowDrawList` + `InvisibleButton` + `ImDrawList`. Reads live `Camera().yaw/.pitch`, builds the camera view basis, orthographically projects the cube (**lock-step rotation**, AC1). **Plain solid cube** with highlight-only edge/corner picking (see the 2026-06-29 review-feedback revisions below): 6 flat axis-coloured faces, only the front-facing ones drawn (back-face cull). Picking subdivides the hovered visible face into a 3×3 grid — centre = face, edge cells = the shared edge, corner cells = the corner — to **classify + derive the snap direction**, but the cells are **never drawn**; only the **hover highlight** shows the element: a translucent fill on the face, an amber **bar along the actual cube edge**, or an amber **dot on the actual cube corner** (AC2). Click → `SnapCameraTo(dir)` (AC3). **No hardcoded angle** — face/edge/corner = straight-on / 45° / ~35° fall out of the ±1 geometry. Called from inside `DrawToolUi`, after the read-out overlays, before `ImGui::Render()`.
- **Task 4 — `viewer_window.cpp` `RenderTick`:** a QPC `dt` (clamped ≤0.1 s, new `g_last_tick`, reseeded to 0 on panel resume) drives `AdvanceCameraAnim(dt)` **immediately before** `RenderFrame`, so a snap shows the **same frame**.
- **Task 5 — docs:** AR15 grep gate run (empty); AR20 Spec Change Log entry added to `architecture.md`; gate **§11** authored in `docs/PHASE4.5_VALIDATOR_GATE.md` (mirrors §10, Result **PENDING**); out-of-scope items recorded in `deferred-work.md`.

**Validation posture (same as every Epic-6.5 story):** the whole viewport UI is behind `#ifdef _WIN32`, so the Linux dev box cannot build the viewer/renderer/ImGui units or open Reaper. There is **no Windows-UI test harness** in this repo — validation is **self-review against proven patterns + numeric harnesses for the pure math + Antho's in-Reaper Windows gate §11 (AR19)**. The core geometry (picking directions, the `Eye()` inverse, gimbal clamp, shortest-path yaw, smoothstep) is **numerically verified** above; `camera.h` compiles `-Wall -Wextra` clean; the ImGui glue mirrors `LightDirectionPad` with verified API signatures. **Feel constants** (`kNavCubeSize`, `kNavCubeMargin`, `kSnapDuration`) are gate-tunable. Gate §11 Result is **PENDING** (not fabricated).

### File List

- `src/camera.h` — `OrbitCamera` tween state + `kSnapDuration`; `SnapToDirection` + `AdvanceAnim`; cancel-tween in `Orbit`/`Pan`/`Zoom`/`Reset`.
- `src/renderer.h` — inline `SnapCameraTo` / `AdvanceCameraAnim` pass-throughs.
- `src/viewer_window.cpp` — `kNavCubeSize`/`kNavCubeMargin`; `NavCubeWidget()`; `g_last_tick` + per-frame `dt`/`AdvanceCameraAnim` in `RenderTick`; resume-reseed; `NavCubeWidget()` call in `DrawToolUi`.
- `_bmad-output/planning-artifacts/architecture.md` — AR20 Spec Change Log entry (2026-06-29, Story 6.5.8).
- `docs/PHASE4.5_VALIDATOR_GATE.md` — §11 gate section (Result PENDING).
- `_bmad-output/implementation-artifacts/deferred-work.md` — Story 6.5.8 out-of-scope items.

## Change Log

| Date | Change |
|------|--------|
| 2026-06-29 | Story 6.5.8 implemented (dev-story): navigation cube (ViewCube) — `OrbitCamera::SnapToDirection`/`AdvanceAnim` shortest-path smoothstep tween (camera.h), `SnapCameraTo`/`AdvanceCameraAnim` pass-throughs (renderer.h), `NavCubeWidget()` bottom-right ImGui overlay + per-frame `dt` tween advance (viewer_window.cpp). AR15-clean (3 src files), session-only, D2 zero-alloc, AR17 non-fatal. Core math numerically verified; `camera.h` `-Wall -Wextra` clean. Gate §11 PENDING (AR19). Status → review. |
| 2026-06-29 | Review-feedback refinement #1 (Antho, "préférerais sélectionner arêtes/coins plutôt que diviser chaque face en 9"): briefly tried a chamfered (beveled) ViewCube with 26 drawn regions. |
| 2026-06-29 | In-Reaper Windows gate §11 **PASSED** (Antho, AR19): cube rotates with the camera, hover highlights face/edge/corner, click snaps the camera (front/45°/corner) at the same distance, top/bottom clamp + shortest-path OK, tools coexist. Gate §11 Result PENDING → PASS. Story stays **review** pending Antho's code-review (which will close it to done + commit). |
| 2026-06-29 | Review-feedback refinement #2 (Antho, "c'est mieux mais moche — je voulais pas des faces pour les arêtes/coins, juste pouvoir les highlight + cliquer", + reference screenshots): reverted the chamfer to a **plain solid cube** (smooth flat faces). The 3×3-per-visible-face picking is kept **only to classify** face/edge/corner + derive the snap direction — the cells are **never drawn**. The **hover highlight** now draws directly on the cube geometry: translucent fill on the face, an amber **bar along the actual cube edge**, an amber **dot on the actual cube corner** (matches the reference). Same `viewer_window.cpp` (no scope change, still AR15-clean); picking-direction math re-verified (face/edge/corner + lock-step). Docs (AR20, gate §11) updated to match. |
