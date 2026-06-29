# Sprint Change Proposal — Navigation cube (ViewCube) for camera snap

- **Date:** 2026-06-29
- **Author:** Amelia (dev) with Antho
- **Trigger source:** Antho feature request (in-Reaper UX — spatial orientation + quick canonical views)
- **Change scope:** **Moderate** (new story in Epic 6.5; new FR54; backlog reorg; routes to create-story → dev-story)
- **Mode:** Batch

## 1. Issue Summary

While orbiting a character in the viewport, **there is no quick way to reach a canonical view** (front / side / top / a clean 3/4) and **no persistent cue of which way the camera is pointing** — easy to get lost in space, especially on a symmetric rig.

Antho requested a **navigation cube** (a "ViewCube", as in Maya / Fusion / Blender's nav gizmo) in the **bottom-right** corner of the viewport:

- A small **colored cube** (per the reference screenshots — axis-colored faces) that **rotates with the camera**, giving a constant read of the current orientation.
- **Hovering** a sub-element of the cube — a **face**, an **edge**, or a **corner** — **highlights** it.
- **Clicking** an element **snaps the camera** so it looks at the scene centre **from that element's direction**: click the top-front-right corner → the camera moves to where that corner sits and points at the centre.

**Key geometric principle (confirmed with Antho):** we hardcode **no angle**. The clicked element maps to a **direction from the cube centre**; the camera is placed along that direction, **made to look at the centre (0,0,0 / the model centre), keeping its current distance**. The viewing angle then *falls out of the geometry naturally* — an **edge** lands at exactly **45°** elevation (it bisects two faces, 90°/2), a **corner** at ~35° (it meets three faces), but neither number is ever written down: direction-to-centre is the whole spec.

This is a **net-new capability** (a navigation gizmo), distinct from the existing orbit/zoom/pan/reset (FR22–FR26) and the on-canvas tool sidebar (FR49). It is pure pre-ship UX polish, fully session-side, and a natural fit for Epic 6.5.

## 2. Impact Analysis

- **Epic impact:** Epic 6.5 (pre-ship polish). Adds **Story 6.5.8**. No other epic affected. Epic 7 (ReaPack) unchanged.
- **Story impact:** Builds on Story 2.4 (orbit camera — `camera.h` `OrbitCamera{target,distance,yaw,pitch}`) and Story 6.5.3 (Dear ImGui on-canvas UI). **Orthogonal** to all other 6.5 stories (rendering, MSAA, spec/gloss) — no conflict. The clicked view persists for free via Story 6.2 (panel/viewport state already serializes camera).
- **FR impact:** **New FR54** (navigation cube). Complements FR22–FR26 (camera control) and FR49 (on-canvas tools). No existing FR changed.
- **Architecture impact:** None to D-decisions or AR invariants. Adds the snap to the **camera-derives-the-view** model (AR13: tolerance by camera, not remap) — the cube *sets* yaw/pitch, the view is still derived each frame. AR20 Spec Change Log entry authored at dev time if any constant is introduced (e.g. tween duration).
- **Technical impact (expected scope — ImGui/GL-boundary-only, AR15-clean like 6.5.3/6.5.5, session-only):**
  - `viewer_window.cpp` — **the bulk.** A new `NavCubeWidget()` ImGui overlay, modeled on the existing `LightDirectionPad` (`InvisibleButton` + `ImDrawList`), anchored **bottom-right** (`SetNextWindowPos` from the live viewport size, `ImGuiCond_Always`, frameless). It reads the camera's current `yaw/pitch` to orient the drawn cube, hit-tests the cursor against the cube's elements, **highlights** the hovered one, and on click calls the renderer's snap.
  - `camera.h` — add `SnapToDirection(glm::vec3 dirFromCentre)` (sets `yaw/pitch` from a unit direction, **preserves `target` + `distance`**) plus a **smooth tween**: store `targetYaw/targetPitch` and lerp toward them in a per-frame `AdvanceAnim(dt)` (**shortest-path** yaw wrap so it never spins the long way). Header-only → no `CMakeLists.txt` change.
  - `renderer.{cpp,h}` — thin pass-through: expose the camera's current `yaw/pitch` (for the widget to orient the cube) and a `SnapCameraTo(dir)`; advance the tween in the existing per-frame tick. No new GL resources required (widget draws via ImGui's draw list, like the rest of the tool UI).
  - **Picking model (recommended, dev-confirmable):** Autodesk-style **3×3 subdivision per visible face** — centre cell = that **face**, edge cells = the **edge** shared with the neighbour, corner cells = the **corner**. 6 faces + 12 edges + 8 corners = 26 pickable directions, each a unit vector of ±1 axis components, normalized. Alternative (project-and-pick the cube geometry) is acceptable if cleaner at dev time.
  - **Known edge to handle:** a **top/bottom face** wants pitch ±90°, which the existing gimbal clamp forbids (`pitch ∈ [−1.55, +1.55]`, `camera.h:57`). Snap top/bottom to the clamp limit (just under ±90°), not exactly the pole — documented at dev time.
  - **No** `CMakeLists.txt` / `plugin_main` / `reaper_api` / `pcm_source` / `asset_loader` / registration change; **no** `rec->Register`, **no** new `REAPERAPI_WANT_*`. **Session-only**: persists nothing of its own (the resulting camera persists via 6.2). D2 zero-alloc per-frame preserved (the tool UI already rebuilds its draw list every frame — same pattern). NFR-P1 ≥60 fps.

**Risks & mitigations:**
- *Cube picking feels finicky / wrong element snaps* → the 3×3-per-face scheme is the industry-standard, well-understood model; tuned at the in-Reaper gate (a Linux dev box can't judge feel — same constraint as the orbit-sensitivity constants, `camera.h:23`).
- *Tween spins the long way around* → shortest-path yaw interpolation (wrap to the nearest equivalent angle) before lerping.
- *Top/bottom snap hits the gimbal pole* → snap to the existing clamp limit, never exactly ±90° (above).
- *Touches the live camera path* → purely additive; orbit/zoom/pan/reset untouched; if ImGui isn't ready the widget is a no-op (AR17). In-Reaper gate **§11** required (AR19); self-reviewed on Linux (can't build `_WIN32`).

## 3. Recommended Approach

**Direct Adjustment** — add **Story 6.5.8** to Epic 6.5 and a new **FR54**, then run **create-story** (context-rich spec) → **dev-story** → Antho in-Reaper gate §11. Same rhythm as Stories 6.5.6 / 6.5.7.

Rationale: contained, additive, no rollback, no MVP-scope reduction. It closes a real navigation/orientation gap before the ReaPack release and reuses two shipped foundations (the orbit camera and the Dear ImGui overlay) with zero touch to the Reaper integration surface.

## 4. Detailed Change Proposals

### 4.1 `epics.md` — add FR54 (after FR53, line 59)

```
- **FR54**: Present a navigation cube ("ViewCube") in the bottom-right corner of the viewport that rotates with the camera (constant orientation cue) and, when a face / edge / corner is hovered, highlights it and — on click — snaps the camera to look at the scene centre from that element's direction (canonical views: front/side/top + clean 3/4), preserving the current distance.
```

### 4.2 `epics.md` — add the FR-coverage-map line (after the FR53 map line, ~line 231)

```
FR54: Epic 6.5 — navigation cube (ViewCube) for camera snap to canonical views
```

### 4.3 `epics.md` — update the Epic 6.5 "FRs covered" note (line 267)

Append `, FR54` to the covered list and add to the parenthetical: `FR54 added 2026-06-29 by Story 6.5.8 — navigation cube (ViewCube) for camera snap.`

### 4.4 `epics.md` — add Story 6.5.8 (after Story 6.5.7, before `## Epic 7`, ~line 730)

```
### Story 6.5.8: Navigation cube (ViewCube) for camera snap *(added 2026-06-29 — Correct Course)*

As a sound designer orienting a character in the viewport,
I want a small colored cube in the bottom-right that rotates with the camera and lets me click a face/edge/corner to jump to that view,
So that I always know which way I'm looking and can reach a clean front/side/top/three-quarter view in one click.

**Acceptance Criteria:**

**Given** the viewport is open with a model loaded
**When** I look at the bottom-right corner
**Then** a small **colored cube** is drawn there (axis-colored faces, per the reference) that **rotates in lock-step with the camera** — so its orientation always reflects where the camera is pointing (FR54)

**Given** I hover a **face**, an **edge**, or a **corner** of the cube
**When** the cursor is over that element
**Then** that element **highlights** (the hovered face/edge/corner is visually distinguished), so it's clear what a click would select

**Given** I click a cube element
**When** the click registers
**Then** the camera **smoothly animates (~0.2 s)** to look at the scene centre **from that element's direction**, **keeping its current distance** — a face → straight-on that face, an edge → the 45° bisector of its two faces, a corner → the three-quarter view down that corner — the angle derived purely from the element's direction-to-centre (no hardcoded angle)

**And** the snap **only changes the camera's yaw/pitch** — `target`, `distance`, and zoom are preserved; the resulting view persists via Story 6.2 like any other camera state
**And** the top/bottom snap respects the existing gimbal clamp (never exactly ±90° pitch), and the yaw tween takes the **shortest path** (never spins the long way around)
**And** the cube is **GL/ImGui-boundary-only and non-fatal**: if ImGui isn't ready it's a no-op and the viewport still runs (AR17); **session-only** (no new persisted/registered state, AR15); D2 zero-alloc per-frame; orbit/zoom/pan/reset (FR22–FR26) unchanged

*(Reuses the orbit camera from Story 2.4 — `OrbitCamera{target,distance,yaw,pitch}`, view derived each frame — and the Dear ImGui overlay pattern from Story 6.5.3 / the `LightDirectionPad` widget. Recommended picking = Autodesk-style 3×3 subdivision per visible face → face/edge/corner. Feel constants tuned at the in-Reaper gate. Validation = Antho's in-Reaper Windows gate §11, AR19.)*
```

### 4.5 `sprint-status.yaml` — add the story to backlog (after the `6-5-7-*` line, ~line 125)

```
  6-5-8-navigation-cube-viewcube-camera-snap: backlog  # 2026-06-29 Correct Course (sprint-change-proposal-2026-06-29-viewcube.md) — bottom-right colored ViewCube that rotates with the camera; hover highlights face/edge/corner; click smoothly snaps (~0.2s tween) the camera to look at the scene centre from that element's direction, preserving distance (no hardcoded angle — geometry decides: edge=45°, corner≈35°). New FR54. Reuses OrbitCamera (2.4) + Dear ImGui overlay (6.5.3 / LightDirectionPad). Scope: viewer_window.cpp (NavCubeWidget) + camera.h (SnapToDirection + shortest-path tween) + renderer.{h,cpp} pass-through; ImGui/GL-boundary-only, AR15-clean, session-only, D2 zero-alloc. Top/bottom respects gimbal clamp. Gate §11 PENDING. Depends 2.4 + 6.5.3.
```

### 4.6 `architecture.md` — AR20 Spec Change Log

Authored at **dev time** (mirrors 6.5.6 / 6.5.7) *only if* a new tunable constant is introduced (tween duration, cube size/margin) — a short "Navigation cube (ViewCube)" entry noting the camera-snap derives yaw/pitch from element-direction-to-centre, preserving distance.

## 5. Implementation Handoff

- **Scope:** Moderate.
- **Route:** create-story → dev-story (Amelia), then Antho in-Reaper Windows gate §11 (AR19).
- **Success criteria:** A bottom-right colored cube rotates with the camera; hovering a face/edge/corner highlights it; clicking smoothly snaps the camera to that view (front/side/top/3-quarter) looking at the centre at the same distance; orbit/zoom/pan/reset unchanged; ≥60 fps; AR15-clean; no host instability.
