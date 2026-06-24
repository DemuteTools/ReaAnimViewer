# Story 2.4: Camera orbit, zoom, pan, and reset

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want to move the camera around the rig and reset its framing,
so that I can inspect the model from any angle and recover from odd orientations/scales.

This is the **final story of Epic 2 (Phase 1)** — the one that turns a fixed three-quarter snapshot into an interactive viewport. Stories 2.1–2.3 render a textured, multi-material mesh, but the camera is a **static `glm::lookAt`** computed once in `Renderer::SetAsset` ([renderer.cpp:154-181](../../src/renderer.cpp#L154-L181)) and never moves: there is **no orbit/zoom/pan state and no mouse-input handling at all** in the window proc. This story adds the D14 camera controller (`{ target, distance, yaw, pitch }`), wires right-drag / scroll / middle-drag inside the panel to it, and adds a **Reset Camera** button that re-frames the model's bounding box — which is also the documented recovery path (AR13) for a non-canonical (Z-up / cm / tilted) file that loads "wrong" (Journey 2).

Two facts decide the shape of the work:
- **The camera math has a single home today** — the renderer already owns `view_` / `view_pos_` / near-far and the AABB-based auto-fit. The clean move is to factor that into a small **`OrbitCamera`** the renderer owns, rebuild the view matrix **every frame** from it (so dragging is continuous, FR26), and make `SetAsset`'s auto-fit *be* the Reset operation. The existing auto-fit distance (`radius * 3`) already equals D14's `diagonal * 1.5` — Reset reproduces today's framing magnitude.
- **The viewport is a bare native GL child window** (the Spike-0 architecture — no ReaImGui until Epic 5), so the "toolbar Reset Camera button" is a **real Win32 child `BUTTON`** overlaid in a corner of the GL window. `WS_CLIPCHILDREN` is already set on the window ([viewer_window.cpp:470](../../src/viewer_window.cpp#L470)), so GL drawing/`SwapBuffers` is clipped around the child and the button stays visible over the 3D surface — no text renderer needed (see Dev Notes §C).

## Acceptance Criteria

From [epics.md#Story 2.4](../planning-artifacts/epics.md) (lines 382–394), verbatim BDD:

**Given** a model displayed in the panel
**When** I right-click-drag, scroll, and middle-click-drag inside the panel
1. **Then** the camera orbits (FR22), zooms (FR23), and pans (FR24) respectively
2. **And** clicking the toolbar **Reset Camera** button reframes the camera around the model's bounding box (FR25), correctly recovering a sideways/tiny non-canonical rig (Journey 2)
3. **And** camera manipulation never pauses or stalls the displayed frame (FR26).

Implied, non-negotiable (the system must stay working end-to-end — requirements even though not in the AC text):

4. **No regression to Stories 2.1 / 2.2 / 2.3.** The initial framing a user sees on open is unchanged (auto-fit on load = Reset), every textured/multi-material/non-canonical/`.dae`/corrupt row from the 2.1–2.3 gate still passes, and the empty-viewport (no asset) path still renders a live clear with no crash.
5. The deliverable remains a **single `reaper_animviewer.dll`**, builds **warning-free under `/W3 /permissive-`** for our own sources (NFR-R5), holds **≥60 fps** while dragging (NFR-P1), and every new input/window path stays inside the **no-throw host boundary** (AR18) — Reaper is never destabilized by a mouse event, a wheel notch, or the Reset button.
6. **Right-drag inside the panel must NOT raise a Win32 context menu** — we consume `WM_RBUTTONDOWN`/`WM_RBUTTONUP` for orbit, so no `WM_CONTEXTMENU` is generated (don't forward those to `DefWindowProc`).

## Tasks / Subtasks

- [x] **Task 1 — Add the `OrbitCamera` controller (D14 state + math) (AC: 1, 2)**
  - [x] New header **`src/camera.h`** — a small POD-style `OrbitCamera { glm::vec3 target; float distance, yaw, pitch; }` plus inline methods `Eye()`, `ViewMatrix()`, `Orbit(dx,dy)`, `Zoom(delta)`, `Pan(dx,dy)`, `Reset(aabbMin, aabbMax)`. Header-only (inline math, no `.cpp`) → **no `CMakeLists.txt` change** (CMake lists only `.cpp` files; headers aren't enumerated). Wrap the body in `#ifdef _WIN32` to match `scene.h` / `renderer.h`. SPDX header on top. Exact recipe in **Dev Notes §A**.
  - [x] `Eye()` = `target + distance * vec3(cos(pitch)·sin(yaw), sin(pitch), cos(pitch)·cos(yaw))`; `ViewMatrix()` = `glm::lookAt(Eye(), target, vec3(0,1,0))` (world-up Y, AR9).
  - [x] `Orbit`: `yaw += dx·kOrbitSens`; `pitch += dy·kOrbitSens`; **clamp `pitch` to ±(π/2 − ε)** so the eye never aligns with world-up (gimbal flip / degenerate `lookAt`).
  - [x] `Zoom`: `distance *= expf(-delta·kZoomSens)` (D14 exponential feel); then **clamp `distance` to `[frameRadius·0.05, frameRadius·20]`** (scale-independent, stored at Reset) so you can't zoom *through* the model to 0/negative or out to infinity. Guard non-finite.
  - [x] `Pan`: derive the view basis from the current eye — `fwd = normalize(target − Eye())`, `right = normalize(cross(fwd, up))`, `up' = cross(right, fwd)` — then `target += (-right·dx + up'·dy)·distance·kPanSens` (pan scales with distance so it feels constant on screen, D14).
  - [x] `Reset(aabbMin, aabbMax)`: `target = center`; `distance = diagonal·1.5` (== today's `radius·3`); `yaw = kInitYaw`; `pitch = 0.3 rad` (D14). **Reuse the existing degenerate/NaN guards** from `SetAsset`: `!(diagonal > eps)` → fallback radius `1.0`; non-finite center → `vec3(0)`. Store `frameRadius = 0.5·diagonal` (the Zoom clamp reference).

- [x] **Task 2 — Renderer owns the camera; rebuild the view every frame (AC: 1, 3, 4)**
  - [x] Add `OrbitCamera cam_;` to `Renderer` and a non-const accessor `OrbitCamera& Camera() { return cam_; }` (the window proc calls `g_renderer.Camera().Orbit(...)` etc.). Add `void ResetCamera();` that calls `cam_.Reset(asset_.aabbMin, asset_.aabbMax)` and recomputes near/far (so the Reset button has a one-call entry point).
  - [x] **`SetAsset`**: replace the inline `glm::lookAt` framing block ([renderer.cpp:158-174](../../src/renderer.cpp#L158-L174)) with `cam_.Reset(asset_.aabbMin, asset_.aabbMax);` — i.e. auto-fit-on-load **is** Reset (AC4: the first frame a user sees is unchanged). **Keep** the per-asset near/far derivation (`near = radius·0.01`, `far = radius·100`) — it is strictly better than D14's literal fixed `0.01/1000` for files authored in mm or km; compute `radius` the same way (with the same NaN guard) or read `cam_.frameRadius`.
  - [x] **`RenderFrame`**: the view now changes every frame (during a drag), so rebuild it each frame instead of caching it. Set `view_ = cam_.ViewMatrix(); view_pos_ = cam_.Eye();` each frame, keep the **projection** cached on aspect change (projection doesn't depend on the camera), and compute `view_proj_ = proj * view_` each frame. This is a couple of `mat4` multiplies — **no heap, no GL state churn** (D2 hot-path discipline; the 1.2 review's zero-alloc-in-`RenderFrame` rule still holds). Drop `view_`/`view_pos_`/`view_proj_` from `SetAsset`'s responsibilities (they're now derived).
  - [x] The no-asset path (`asset_.meshes.empty()` → clear and return) is unchanged; `cam_` sits at its default state, harmless.

- [x] **Task 3 — Wire mouse + wheel input and the Reset button in the window proc (AC: 1, 2, 3, 5, 6)**
  - [x] In `viewer_window.cpp`, add `#include <windowsx.h>` (for `GET_X_LPARAM` / `GET_Y_LPARAM`). Add a tiny file-local input state: an active-drag enum (`None/Orbit/Pan`) and `last_x/last_y`.
  - [x] **Orbit** — `WM_RBUTTONDOWN`: `SetCapture(hwnd)`, `SetFocus(hwnd)` (so the wheel reaches us, see §D), record point, drag = Orbit. `WM_RBUTTONUP`: `ReleaseCapture()`, drag = None, **return 0** (do not fall through to `DefWindowProc`, or it synthesizes `WM_CONTEXTMENU` — AC6).
  - [x] **Pan** — `WM_MBUTTONDOWN` / `WM_MBUTTONUP`: same capture/focus/release pattern, drag = Pan.
  - [x] **`WM_MOUSEMOVE`**: if a drag is active, `dx = x − last_x`, `dy = y − last_y`; call `g_renderer.Camera().Orbit(dx,dy)` or `.Pan(dx,dy)`; update `last_x/last_y`. Mutate camera state only — **never render synchronously here** (the ~64 Hz timer already redraws, which is what makes manipulation continuous, FR26).
  - [x] **`WM_MOUSEWHEEL`**: `delta = GET_WHEEL_DELTA_WPARAM(wp) / 120.0f`; `g_renderer.Camera().Zoom(delta)`.
  - [x] **`WM_CAPTURECHANGED`**: clear the drag enum (capture can be yanked away by the system; leaving the enum set would make a later move orbit without a button down).
  - [x] **Reset button** — create a `L"BUTTON"` child (`WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON`, caption `L"Reset View"`, small rect e.g. `{8,8,96,26}`, parent `hwnd`, `hmenu = (HMENU)kResetButtonId`) inside `StartRendering` (or `WM_CREATE` after it succeeds). Optionally `WM_SETFONT` with `GetStockObject(DEFAULT_GUI_FONT)` for a non-bold caption. It is a child of `g_hwnd` → destroyed automatically by `DestroyWindow` on the close/unload path (null the global on `WM_DESTROY`; no separate teardown). Handle **`WM_COMMAND`**: `if (LOWORD(wp) == kResetButtonId) { g_renderer.ResetCamera(); SetFocus(hwnd); }` (restore focus so the wheel keeps working after a click).
  - [x] Keep every new handler inside the existing AR18 posture: these are pure state mutations + GL-free Win32 calls — they don't throw, and the `ToggleViewerWindow` / `WM_CREATE` try/catch boundaries already wrap the lifecycle. No new `console_log` lines are needed on the input path (camera moves never "fail").

- [x] **Task 4 — Extend the Phase 1 validator gate with Story 2.4 rows (AC: 1, 2, 3, 4)**
  - [x] Append a **"## 9. Story 2.4 — acceptance checks (camera controls)"** section to `docs/PHASE1_VALIDATOR_GATE.md` (continue the numbering — 2.3 ended at row 18). Rows:
    - **Orbit (FR22)** — right-click-drag inside the panel rotates the view around the model; the model doesn't flip/gimbal at the top/bottom of the arc (pitch clamp).
    - **Zoom (FR23)** — scroll wheel moves the camera in/out smoothly; you can't zoom *through* the model to nothing or lose it to infinity (distance clamp).
    - **Pan (FR24)** — middle-click-drag slides the model across the view; pan speed feels consistent at any zoom (distance-scaled).
    - **Reset + Journey 2 recovery (FR25, AC2)** — load a **non-canonical** fixture (a Z-up / cm / sideways file from the 2.1 rows), confirm it frames tilted/odd, orbit away, then click **Reset View** → the model is reframed centered and fully in view.
    - **Continuous update (FR26)** — during an orbit/pan drag the viewport stays responsive and holds **≥60 fps** (no freeze/stutter while dragging). *(FR26's full force — animation not pausing during camera moves — lands in Epic 3 when the rig animates; in Epic 2 the displayed frame is static, so the check is "drag stays smooth and the timer keeps presenting.")*
    - **No-regression** — re-run the 2.1 rows 1–9, 2.2 rows 10–14, 2.3 rows 15–18; the initial on-open framing is unchanged (auto-fit = Reset).
    - **Single-DLL / warning-free still hold** — only `reaper_animviewer.dll`; clean at `/W3 /permissive-`.
  - [x] Add a **sensitivity-tuning note** (mirroring the 2.2 UV-flip and 2.3 specular notes — a "feels-right" judgment confirmable only on the Windows gate): the knobs are `kOrbitSens`, `kZoomSens`, `kPanSens`, the initial `pitch` (0.3) / `yaw`, and the zoom-distance clamp band (`0.05…20 × frameRadius`). If orbit is too fast/slow or zoom too coarse, these are one-line constant tweaks — flag for a quick follow-up rather than guessing the feel blind on Linux.

## Dev Notes

### Critical orientation — the camera is currently static; this story makes it live

The renderer already does *all* the camera math — but only **once**, in `SetAsset`. [renderer.cpp:158-174](../../src/renderer.cpp#L158-L174) builds a fixed `glm::lookAt(eye, center, +Y)` from the AABB and stores `view_`/`view_pos_`; `RenderFrame` then caches `view_proj_` and rebuilds it **only on an aspect change** ([renderer.cpp:199-204](../../src/renderer.cpp#L199-L204)). There is **no** mouse handling in `WindowProc` ([viewer_window.cpp:344-405](../../src/viewer_window.cpp#L344-L405)) — `WM_RBUTTONDOWN`, `WM_MOUSEMOVE`, `WM_MOUSEWHEEL`, `WM_COMMAND` are all unhandled and fall to `DefWindowProcW`. So this story is **purely additive on the input side** and a **factor-out-then-drive** refactor on the render side; it changes no loader, no shader, no material, no GL resource code.

As in 2.1–2.3, **trust the live tree over `architecture.md`'s stale names** — the architecture still says "ReaImGui panel" / "sokol_gfx" / "FBO bridge" (e.g. its D11–D14 per-frame sequence and the `viewer_panel.h` path in the file tree), all **superseded by the Spike-0 direct-render GL window** (Spec Change Log 2026-06-23). `src/` is flat; the viewport is `viewer_window.cpp`; there is no ImGui. **D14's camera *math* is current and authoritative; D14's framing of where the button/input lives is not** — input is Win32 messages on the GL child, not `ImGui::IsItemHovered`/`IsMouseDragging`.

### §A — The `OrbitCamera` recipe (Task 1)

`src/camera.h`, header-only, `#ifdef _WIN32` guarded:

```cpp
// SPDX-License-Identifier: MIT
#pragma once
#ifdef _WIN32
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
namespace rav {

// D14 orbit camera: state is {target, distance, yaw, pitch}; the view is derived
// each frame. Camera state is panel state, NOT Asset state (survives a reload, D4),
// so it lives here in the renderer, not on the Asset.
struct OrbitCamera {
    glm::vec3 target{0.0f};
    float distance   = 3.0f;
    float yaw        = 0.6f;   // initial 3/4 view; tune on the gate
    float pitch      = 0.3f;   // D14 initial elevation
    float frameRadius = 1.0f;  // set at Reset; the scale ref for the zoom clamp

    glm::vec3 Eye() const {
        const float cp = std::cos(pitch);
        return target + distance * glm::vec3(cp * std::sin(yaw),
                                             std::sin(pitch),
                                             cp * std::cos(yaw));
    }
    glm::mat4 ViewMatrix() const {
        return glm::lookAt(Eye(), target, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    void Orbit(int dx, int dy) {
        yaw   += dx * 0.01f;
        pitch += dy * 0.01f;
        const float lim = 1.55f;                   // ~pi/2 - eps; no pole flip
        pitch = std::clamp(pitch, -lim, lim);
    }
    void Zoom(float delta) {
        distance *= std::exp(-delta * 0.1f);       // exponential feel (D14)
        if (!std::isfinite(distance)) distance = frameRadius * 3.0f;
        distance = std::clamp(distance, frameRadius * 0.05f, frameRadius * 20.0f);
    }
    void Pan(int dx, int dy) {
        const glm::vec3 fwd   = glm::normalize(target - Eye());
        const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 1, 0)));
        const glm::vec3 up    = glm::cross(right, fwd);
        target += (-right * float(dx) + up * float(dy)) * distance * 0.002f;
    }
    void Reset(const glm::vec3& aabbMin, const glm::vec3& aabbMax) {
        glm::vec3 center = 0.5f * (aabbMin + aabbMax);
        float diag = glm::length(aabbMax - aabbMin);
        // `!(diag > eps)` (not `<`) so a NaN — false against everything — also hits
        // the fallback instead of producing a NaN camera (mirrors SetAsset's guard).
        if (!(diag > 1e-4f)) diag = 2.0f;
        if (!(std::isfinite(center.x) && std::isfinite(center.y) && std::isfinite(center.z)))
            center = glm::vec3(0.0f);
        target      = center;
        frameRadius = 0.5f * diag;
        distance    = diag * 1.5f;       // == the pre-2.4 radius*3 framing
        yaw         = 0.6f;
        pitch       = 0.3f;
    }
};

}  // namespace rav
#endif  // _WIN32
```

Sensitivity constants are inline literals here for brevity — pull them to named `constexpr` (`kOrbitSens` etc.) if you prefer; they're the gate-tunable knobs (Task 4 note). `<algorithm>` is for `std::clamp`, `<cmath>` for `std::exp`/trig/`std::isfinite`.

### §B — Renderer integration (Task 2), exact deltas

- `renderer.h`: add `#include "camera.h"`, add member `OrbitCamera cam_;`, add `OrbitCamera& Camera() { return cam_; }` and `void ResetCamera();` to the public API. `view_`/`view_proj_`/`view_pos_` stay as members but become **derived each frame** (no longer set in `SetAsset`).
- `renderer.cpp::SetAsset`: replace lines 158–174 (the center/radius/`dir`/`eye`/`lookAt` block) with `cam_.Reset(asset_.aabbMin, asset_.aabbMax);`. **Keep** lines 161-165's `radius` computation feeding near/far (or read `cam_.frameRadius`): `near_plane_ = radius * 0.01f; far_plane_ = radius * 100.0f;`. Keep `cached_aspect_ = -1.0f;`.
- `renderer.cpp::ResetCamera` (new): `cam_.Reset(asset_.aabbMin, asset_.aabbMax);` + the same near/far recompute, so a Reset after the user zoomed way in/out restores correct depth range too.
- `renderer.cpp::RenderFrame`: after the aspect-cached projection rebuild, set `view_ = cam_.ViewMatrix(); view_pos_ = cam_.Eye(); view_proj_ = proj * view_;` **every frame**. (Currently `view_proj_` is only rebuilt inside the `if (aspect != cached_aspect_)` block — move the `proj * view_` out so it tracks camera motion. Keep `proj` itself inside the aspect guard.) Everything downstream (`mvp`, `u_viewPos`) already reads these.

### §C — The Reset button on a bare GL window (Task 3)

There is no UI toolkit in the viewport (ReaImGui arrives in Epic 5), so the "toolbar button" is a **native Win32 child `BUTTON`** overlaid on the GL surface:
- **Why it stays visible over GL:** the window is created with `WS_CLIPCHILDREN` ([viewer_window.cpp:470](../../src/viewer_window.cpp#L470)) and `CS_OWNDC` ([viewer_window.cpp:413](../../src/viewer_window.cpp#L413)). `WS_CLIPCHILDREN` excludes child-window rects from the parent's (GL) DC clip region, so `glClear`/draw/`SwapBuffers` never paint over the button; USER32 paints the button itself. This is exactly what `WS_CLIPCHILDREN` is for — no GL-side compositing needed, no text renderer needed.
- **Lifecycle:** create it in `StartRendering` (or in `WM_CREATE` right after `StartRendering` succeeds) as a child of `hwnd`. Since it's a child of `g_hwnd`, the existing `DestroyWindow(g_hwnd)` on the close/unload path tears it down automatically (NFR-R3 symmetric teardown is inherited — no new explicit destroy, just null any stored `HWND` global on `WM_DESTROY`). It survives undock/redock the same way the GL child does (Reaper reparents `g_hwnd`; the button rides along as its child).
- **Command routing:** `WM_COMMAND` arrives on the **parent** (`g_hwnd`'s `WindowProc`), identified by the control id `kResetButtonId`. Handle it → `g_renderer.ResetCamera()`. Re-`SetFocus(hwnd)` after so the wheel keeps targeting the viewport (§D).
- It does **not** need its own message loop or subclassing; a plain push button is enough. Keep the rect small and anchored top-left; no `WM_SIZE` reposition is required (top-left is stable). Repositioning on resize is optional polish.

### §D — The `WM_MOUSEWHEEL` focus gotcha (Task 3)

`WM_MOUSEWHEEL` is delivered to the window **with keyboard focus**, not the one under the cursor. A docked child often doesn't hold focus, so the wheel can be swallowed by Reaper. Mitigation already in the task list: **`SetFocus(hwnd)` on any mouse-button-down in the viewport** (orbit/pan already do this via their down-handlers), and re-`SetFocus` after a Reset-button click. In practice users right- or middle-drag before they zoom, so the viewport has focus by then; the explicit `SetFocus` makes first-touch zoom reliable too. Do **not** `SetFocus` on every `WM_MOUSEMOVE` — stealing focus continuously from Reaper's controls is hostile. If the gate finds wheel-while-merely-hovering still misses, the fallback is handling `WM_MOUSEWHEEL` via a hover-set-focus on `WM_MOUSEMOVE`-enter only — flag it, don't pre-build it.

### §E — Boundary & threading discipline (unchanged invariants)

- **AR18 no-throw / main-thread:** all input runs on Reaper's main pump (the same thread as the render timer and `WM_CREATE`); the handlers are arithmetic + `SetCapture`/`SetFocus`/`ReleaseCapture` — none throw. The `ToggleViewerWindow` and `WM_CREATE` try/catch boundaries already guard the lifecycle; no new boundary is required, but **don't** introduce an allocation that could throw on the per-message path.
- **D2 hot-path:** the only per-frame change is two extra `mat4` ops in `RenderFrame` (view + view_proj). No heap, no new GL objects, no new uniforms. The 1.2 review's zero-alloc-in-`RenderFrame` rule is preserved.
- **AR15 symmetric register/unregister:** the Reset button registers no Reaper API surface (it's a pure Win32 child) and is freed with the parent — nothing new to deregister at unload. No new `rec->Register`.
- **AR16 console-only:** unchanged — camera moves produce no diagnostics; the only feedback is the moving viewport. Don't add per-drag log spam.

### Scope fences — what this story does NOT do

- **No camera persistence.** `{target, distance, yaw, pitch}` is panel state that survives a *reload* (it's not on the Asset), but serializing it into the **project file** is FR31/D9 → **Epic 6, Story 6.1**. Do not touch `SaveState`/`LoadState` (no PCM_source exists yet — that's Epic 4).
- **No animation / transport coupling.** FR26 "continuous update" here just means the viewport keeps rendering during a drag; the playhead-driven frame and "don't pause animation on interaction" land in Epic 3/4. No transport code.
- **No zoom-to-cursor, no focus-point picking, no inertia/smoothing, no orthographic mode.** D14 zooms along the eye→target axis; keep it. These are post-MVP polish.
- **No keyboard camera controls / no arcball/trackball quaternion camera.** D14 is an explicit yaw/pitch/distance orbit — implement that, not a quaternion arcball.
- **No renderer feature changes** — no shader edit, no material change, no new uniform, no GL resource. The view/projection matrices already feed the shader; this story only changes *what view matrix* is fed and *when* it's rebuilt.
- **No second panel / no ReaImGui** — the browser and any ImGui toolbar are Epic 5. The Reset button is a single native child control, deliberately minimal.
- **No reposition of the GL window, no new dock behavior** — docking, hide/park, and teardown are all Epic-1 settled; don't perturb the `FrameTimerProc` park-on-hide logic or the `WM_DESTROY`/`CloseViewerWindow` teardown.

### Project Structure Notes

- **New files:** `src/camera.h` (header-only `OrbitCamera`; **no `CMakeLists.txt` change** — only `.cpp` files are enumerated, [CMakeLists.txt:67-73](../../CMakeLists.txt#L67-L73)).
- **Modified:**
  - `src/renderer.h` / `src/renderer.cpp` — own `OrbitCamera`, `Camera()` accessor + `ResetCamera()`, `SetAsset` auto-fit → `cam_.Reset`, per-frame view rebuild in `RenderFrame` (Task 2).
  - `src/viewer_window.cpp` — `#include <windowsx.h>`, mouse/wheel handlers, `WM_CAPTURECHANGED`, the Reset child button + `WM_COMMAND` (Task 3).
  - `docs/PHASE1_VALIDATOR_GATE.md` — §9 Story 2.4 rows + sensitivity-tuning note (Task 4).
  - `_bmad-output/implementation-artifacts/sprint-status.yaml` — status tracking (ready-for-dev → … → done).
  - **Unchanged:** `asset_loader.cpp/.h`, `scene.h`, `gl_loader.*`, `gpu_resources.h`, `console_log.*`, `plugin_main.cpp`, `reaper_api.*`, the shader — confirm their diff is empty.
- **Naming** (architecture line 517-524): types/functions **PascalCase**, locals **snake_case**, file-internal helpers in the anonymous namespace (the input state + button id belong in `viewer_window.cpp`'s existing `namespace {`). SPDX header on the new file. **Comment WHY only** — the pitch clamp (pole flip), the distance clamp (zoom-through), the `WS_CLIPCHILDREN` reason, and the wheel-focus gotcha are the non-obvious lines worth one comment each. No story/commit IDs in comments.
- **Untracked `build_spike.bat` / `build_forcefail.bat`** must stay **out** of this story's commits (carried forward from 2.1–2.3 intelligence — they are local build aids, not part of the change).

### Previous Story Intelligence (Stories 2.1–2.3 and Epic 1)

- **The seam was left open for exactly this story.** 2.1's `SetAsset` comment literally says the auto-fit framing is "recovered later by Reset Camera (Story 2.4)" ([renderer.cpp:158-160](../../src/renderer.cpp#L158-L160)) and the asset comment notes the AABB feeds "camera auto-fit / Reset (D14)" ([scene.h:83](../../src/scene.h#L83)). The AABB, near/far derivation, and NaN guards you need already exist — **reuse them, don't reinvent**.
- **NaN discipline is a standing review theme** (2.1: NaN-AABB guard + `!(radius > eps)` fallback; 2.3: `std::isfinite` shininess/metallic clamps). The Edge Case Hunter *will* probe `Zoom`/`Reset` with degenerate/NaN bounds and a malformed AABB — the `§A` recipe's `isfinite`/`!(diag>eps)` guards and the distance clamp are there to pass that pass on the first review, not the second.
- **Hot-path zero-alloc (1.2 review, 10 findings):** `RenderFrame` must stay allocation-free. The per-frame view rebuild is stack `mat4`s only.
- **Park-on-hide is load-bearing (1.3):** the `FrameTimerProc` renders only while `IsWindowVisible` and parks otherwise. Input messages don't arrive while hidden anyway, but **do not** touch that logic; the camera just sits at its last state when the panel is re-shown (which is the correct, expected behavior).
- **No-throw at the host edge (1.4 / 2.1 / 2.3):** the `WM_CREATE` and `ToggleViewerWindow` try/catch already wrap creation; your new `StartRendering` button-create lives inside that boundary, so a failed `CreateWindowExW` for the button must **not** bail the whole viewport — log nothing or one line and continue (a viewport without a Reset button is still a working viewport). Prefer: if the button create fails, just `LogInfo` once and carry on; don't return false from `StartRendering` over a missing button.
- **`/W3 /permissive-` cleanliness (NFR-R5):** watch the usual MSVC `/W3` snags — signed/unsigned in `LOWORD`/`HIWORD`, `GET_WHEEL_DELTA_WPARAM` is `short`, `float`↔`int` in the sensitivity math (explicit casts). The Linux box won't catch these; the Windows gate build will.

### Testing standards

No automated test harness exists (architecture lines 933-935); validation is the **per-phase validator gate run by Antho in real Reaper** (AR19), and per project memory `feedback_trust_ingame_validation`, when Antho validates in-Reaper and it works, **that IS the gate**. Dev-side verification on this Linux/WSL box is limited to `cmake -S . -B /tmp/rav_build` configuring clean (the `else()` branch stubs the Windows target — the C++ compile, `/W3` cleanliness, single-DLL, ≥60 fps, and **all** interaction ACs are confirmed only on Antho's Windows gate) plus source greps (confirm only `camera.h`/`renderer.*`/`viewer_window.cpp`/gate changed; confirm the shader/loader/scene are byte-for-byte unchanged). The camera ACs are **inherently interactive** (drag/scroll/click) and **visual** (Journey-2 reframe) — Task 4's gate rows are the real test. Recommended fixtures: any 2.1/2.2/2.3 model for orbit/zoom/pan; a **non-canonical** fixture (the Z-up / cm / sideways file from the 2.1 rows) for the Reset/Journey-2 recovery row.

### Latest tech notes

- **GLM 1.0.3** (vendored, `extern/glm/`) — `glm::lookAt`, `glm::cross`, `glm::normalize` are all in `<glm/glm.hpp>` + `<glm/gtc/matrix_transform.hpp>` (already included by `renderer.cpp`). No new dependency, no CMake change.
- **Win32 input** — `GET_X_LPARAM`/`GET_Y_LPARAM` need `<windowsx.h>`; `GET_WHEEL_DELTA_WPARAM` is in `<winuser.h>` (already via `windows.h`). `SetCapture`/`ReleaseCapture`/`WM_CAPTURECHANGED` are the standard drag-tracking trio so a drag that leaves the window keeps reporting. `WM_CONTEXTMENU` is generated by `DefWindowProc` from `WM_RBUTTONUP` — consuming `WM_RBUTTONUP` (returning 0 without forwarding) suppresses it (AC6).
- **D14 vs. shipped near/far** — D14 literally says "near=0.01, far=1000," but 2.1 already ships the **better** per-asset `radius·0.01 / radius·100` (depth precision at any authored unit scale). Keep the shipped derivation; D14's fixed numbers are a simplification it itself caveats ("units depend on the file's authored scale"). This is not a deviation worth a Spec Change Log entry — it's the existing, already-reviewed behavior, retained.

### References

- Story + ACs: [epics.md#Story 2.4](../planning-artifacts/epics.md) (lines 382–394); Epic 2 intro (336–338); Camera & Viewport FR map FR22–FR26 (lines 54–58, 192–196).
- **D14 camera controller** (state, orbit/zoom/pan/reset math, continuous-update, view derivation): [architecture.md](../planning-artifacts/architecture.md) lines 409–422. Camera-tolerance-by-camera (AR13 / Journey-2 recovery): lines 90, 231–233. Camera state is panel state not Asset state (survives reload, D4): line 420, 248. Persisted camera fields (Epic 6): lines 303, 590–591.
- AR9 single coord convention (Y-up world): [epics.md] line 145. AR15 symmetric register / AR16 console-only / AR18 no-throw+main-thread: lines 154–157. AR19 gate: line 161.
- Live code to change: [renderer.cpp:154-181](../../src/renderer.cpp#L154-L181) (`SetAsset` framing → `cam_.Reset`), [renderer.cpp:183-261](../../src/renderer.cpp#L183-L261) (`RenderFrame` per-frame view rebuild), [renderer.h:48-71](../../src/renderer.h#L48-L71) (members/API), [viewer_window.cpp:344-405](../../src/viewer_window.cpp#L344-L405) (`WindowProc` — add input + `WM_COMMAND`), [viewer_window.cpp:242-326](../../src/viewer_window.cpp#L242-L326) (`StartRendering` — add the button), [scene.h:83-84](../../src/scene.h#L83-L84) (AABB feeding Reset). Gate to extend: [docs/PHASE1_VALIDATOR_GATE.md](../../docs/PHASE1_VALIDATOR_GATE.md) (continue after §8 / row 18).
- Prior stories (seam, the auto-fit that becomes Reset, the review patterns to satisfy): [2-1-load-and-display-a-static-gltf-glb-mesh.md](2-1-load-and-display-a-static-gltf-glb-mesh.md), [2-3-multi-material-rendering-with-per-material-specular.md](2-3-multi-material-rendering-with-per-material-specular.md). Epic 1 viewport/teardown facts: [1-3-dockable-gl-window-opened-by-a-reaper-action.md](1-3-dockable-gl-window-opened-by-a-reaper-action.md).

### Review Findings

_BMAD 3-layer code review (Blind Hunter / Edge Case Hunter / Acceptance Auditor), 2026-06-24. Acceptance Auditor: all 6 ACs + 4 tasks satisfied (no AC violation). Findings below._

- [x] [Review][Patch] Infinite AABB magnitude bypasses the degenerate-bounds guard in `Reset` [src/camera.h:84] — **FIXED** (guard now `!(std::isfinite(diag) && diag > 1e-4f)`). — `if (!(diag > 1e-4f)) diag = 2.0f;` catches NaN, zero, and tiny diagonals, but `+inf > 1e-4f` is `true`, so the fallback is skipped for an infinite diagonal. A corrupt asset with an infinite vertex coord then yields `frameRadius = inf` and `distance = inf` → `Eye()`/`glm::lookAt` produce a NaN view matrix and `ResetCamera`'s near/far go infinite, rendering blank with no recovery (Reset re-derives the same inf). The `isfinite` check below guards only `center`, not the diagonal. Extend the guard to reject non-finite diag (mirrors the standing NaN-discipline pattern from the 2.1/2.3 reviews).

**Gate-tuning note (not a defect — confirm on the Windows gate):** Vertical orbit (`pitch += dy`, [camera.h:54](../../src/camera.h#L54)) direction is a "feels-right" call that, unlike the horizontal axis, was **not** explicitly tuned in-Reaper (the Change Log records only the `dx` negation). Pan is internally consistent with the drag-the-model convention (drag-down → model-down). When you next run the gate, sanity-check that vertical orbit drags the way you expect — if inverted it's a one-line sign flip on the `pitch += dy` term, same as the `dx` fix. This is exactly the Task-4 sensitivity row, surfaced here so it isn't forgotten.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Claude Opus 4.8, 1M context) — BMAD dev-story workflow.

### Debug Log References

- `cmake -S . -B /tmp/rav_build` → configures clean (exit 0); non-Windows platform stubs the build target as expected (the C++ compile, `/W3 /permissive-` cleanliness, single-DLL, ≥60 fps, and all interactive ACs are confirmed only on Antho's Windows gate per AR19).
- `git diff --stat` over `asset_loader.*`, `gl_loader.*`, `gpu_resources.h`, `console_log.cpp`, `plugin_main.cpp`, `reaper_api.cpp`, `scene.h`, `CMakeLists.txt`, and the shader → **empty** (byte-for-byte unchanged), confirming the scope fence (loader/shader/scene/CMake untouched; header-only camera = no CMake change).

### Completion Notes List

- **Task 1 — `src/camera.h` (new, header-only, `#ifdef _WIN32`):** D14 `OrbitCamera { target, distance, yaw, pitch, frameRadius }` with inline `Eye()` / `ViewMatrix()` / `Orbit` / `Zoom` / `Pan` / `Reset`. Pitch clamped to ±1.55 rad (no pole flip), distance clamped to `[frameRadius·0.05, ·20]` with a non-finite guard (no zoom-through), and `Reset` reuses the `!(diag > eps)` + `isfinite(center)` degenerate/NaN guards from the old `SetAsset`. Sensitivity knobs pulled to named `constexpr` (`kOrbitSens`/`kZoomSens`/`kPanSens`/`kInitYaw`/`kInitPitch`) for one-line gate tuning. No `CMakeLists.txt` change (headers aren't enumerated).
- **Task 2 — renderer owns the camera, view rebuilt per frame:** `renderer.h` adds `#include "camera.h"`, `OrbitCamera cam_`, `OrbitCamera& Camera()`, `void ResetCamera()`, and a cached `proj_` member. `SetAsset` now just calls `ResetCamera()` (auto-fit-on-load **is** Reset → unchanged first frame, AC4). `ResetCamera` re-frames `cam_` and recomputes per-asset near/far from `cam_.frameRadius` (kept from 2.1, better than D14's fixed 0.01/1000). `RenderFrame` caches only the projection on aspect change and derives `view_`/`view_pos_`/`view_proj_` from `cam_` every frame — two stack `mat4` ops, zero heap (D2 hot-path preserved).
- **Task 3 — Win32 input + Reset button:** `viewer_window.cpp` adds `#include <windowsx.h>`, a file-local `DragMode { None, Orbit, Pan }` + `g_last_x/y`, and handlers for `WM_RBUTTONDOWN`/`WM_MBUTTONDOWN` (SetFocus+SetCapture, §D wheel-focus fix), `WM_R/MBUTTONUP` (ReleaseCapture, **return 0 without DefWindowProc** → no `WM_CONTEXTMENU`, AC6), `WM_MOUSEMOVE` (state-only mutation, no synchronous render — FR26), `WM_MOUSEWHEEL` (notch/120 → `Zoom`), `WM_CAPTURECHANGED` (clear drag), and `WM_COMMAND` (Reset button → `ResetCamera` + restore focus). The **Reset View** button is a native child `BUTTON` created in `StartRendering`; visible over GL via the existing `WS_CLIPCHILDREN`; a failed create is non-fatal (logs once, viewport still works); destroyed with its parent (`g_reset_button` nulled on `WM_DESTROY`). All handlers are pure state mutations + GL-free Win32 calls inside the existing AR18 boundary.
- **Task 4 — gate §9:** appended `## 9. Story 2.4` to `docs/PHASE1_VALIDATOR_GATE.md` (rows 19–26: orbit/zoom/pan/reset+Journey-2/continuous-update/no-context-menu/no-regression/single-DLL) plus a sensitivity-tuning note flagging `kOrbitSens`/`kZoomSens`/`kPanSens`/initial pitch-yaw/clamp band as the Windows-gate-tunable knobs.
- **Scope fences honored:** no persistence (Epic 6), no transport coupling (Epic 3/4), no zoom-to-cursor/inertia/ortho, no keyboard/arcball, no shader/material/GL-resource change, no second panel/ReaImGui, no dock-behavior change. `build_spike.bat` / `build_forcefail.bat` kept out of the change.
- **Pending:** all camera ACs are inherently interactive (drag/scroll/click) and visual (Journey-2 reframe) — confirmed only on Antho's in-Reaper Windows gate (§9 rows). `/W3 /permissive-` cleanliness verified only there; watched the known snags (explicit `float(dx)` casts in Pan, `short`-typed `GET_WHEEL_DELTA_WPARAM / 120.0f`, `WORD`→`int` promotion in `LOWORD(wp) == kResetButtonId`).

### File List

- `src/camera.h` — **new.** Header-only D14 `OrbitCamera` (state + orbit/zoom/pan/reset math + sensitivity `constexpr`s).
- `src/renderer.h` — modified. `#include "camera.h"`, `cam_` member + `proj_` cache, `Camera()` accessor, `ResetCamera()` declaration; `view_`/`view_pos_`/`view_proj_` become per-frame-derived.
- `src/renderer.cpp` — modified. `SetAsset` → `ResetCamera()`; new `ResetCamera()` (re-frame + near/far); `RenderFrame` rebuilds view from `cam_` each frame, projection cached on aspect change.
- `src/viewer_window.cpp` — modified. `#include <windowsx.h>`; drag state + Reset-button globals; mouse/wheel/command/capture handlers in `WindowProc`; Reset button create in `StartRendering`; null button on `WM_DESTROY`.
- `docs/PHASE1_VALIDATOR_GATE.md` — modified. §9 Story 2.4 acceptance rows (19–26) + sensitivity-tuning note.
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — modified. 2-4 status ready-for-dev → in-progress → review.

## Change Log

| Date | Change |
|---|---|
| 2026-06-24 | Story 2.4 drafted (ready-for-dev) — D14 orbit camera (`OrbitCamera` in new `src/camera.h`), right-drag orbit / scroll zoom / middle-drag pan wired in `WindowProc`, native Win32 child **Reset View** button (FR22–FR26). Renderer factors the static auto-fit into `cam_.Reset` and rebuilds the view each frame for continuous update. Loader/shader/scene unchanged. Gate §9 to be appended. |
| 2026-06-24 | Gate feedback (Antho, in-Reaper): horizontal orbit was reversed — negated `dx` in `OrbitCamera::Orbit` (`yaw -= dx·kOrbitSens`) so a rightward drag rotates the model rightward. One-line sensitivity tweak; pitch unchanged. |
| 2026-06-24 | Story 2.4 → done. Antho validated the camera ACs in-Reaper **during dev** (the horizontal-orbit fix in the prior log entry came from that in-Reaper session) — orbit/zoom/pan/Reset+Journey-2 confirmed working. Per AR19 / `feedback_trust_ingame_validation`, in-Reaper validation IS the gate. Code review (BMAD 3-layer) then applied 1 cold-path patch (Reset rejects infinite AABB diagonal) — corrupt-asset hardening that does not require re-validation. |
| 2026-06-24 | Story 2.4 implemented (review) — new header-only `src/camera.h` `OrbitCamera` (D14 state/math, pitch+distance clamps, NaN-safe `Reset`); renderer owns `cam_`, `SetAsset`→`ResetCamera`, per-frame view rebuild in `RenderFrame` (D2 zero-alloc preserved); `viewer_window.cpp` right-drag orbit / middle-drag pan / wheel zoom / `WM_CAPTURECHANGED` / native **Reset View** child button + `WM_COMMAND`, right-up consumed so no `WM_CONTEXTMENU` (AC6). Gate §9 rows 19–26 + tuning note appended. Loader/shader/scene/gl/CMake byte-for-byte unchanged (header-only camera → no CMake change). Linux cmake configures clean; all interactive/visual ACs + `/W3` cleanliness pend Antho's Windows gate (AR19). |
