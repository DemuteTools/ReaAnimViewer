---
baseline_commit: 7658c73a5df6473e7e81cc43da30eb307ad7674b
---

# Story 6.5.3: Viewport tool sidebar + Light tool

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want a top-right tool menu in the viewport, starting with a light control,
so that I can adjust how the model is lit without leaving Reaper.

## Acceptance Criteria

1. **Given** the viewport is open
   **When** it renders
   **Then** a **vertical icon strip** appears in the **top-right** corner of the GL window, built to host multiple tools (extensible — the light tool is the first of several; floor 6.5.4 and perf 6.5.5 will dock into the same strip), using the existing **Win32-child / GL-overlay** pattern from the "Reset View" button — **no ReaImGui** (FR49). [Source: epics.md#Story-6.5.3 / FR49; pattern: [viewer_window.cpp:349-362](../../src/viewer_window.cpp#L349-L362)]

2. **Given** the icon strip
   **When** I click the **light** icon
   **Then** a **flyout** expands offering (a) a **light colour** chooser via the native colour picker (`ChooseColorW`) and (b) a **light position around the origin** control (azimuth/elevation), and both **live-update** the render by driving the `u_lightColor` / `u_lightDir` uniforms introduced in Story 6.5.1 (FR50). [Source: epics.md#Story-6.5.3 / FR50; uniforms: [renderer.cpp:340-341](../../src/renderer.cpp#L340-L341), members [renderer.h:110-111](../../src/renderer.h#L110-L111)]

3. **Given** the new sidebar/flyout
   **When** I interact with it (or it fails to create)
   **Then** the existing **"Reset View"** control keeps working, the **orbit/zoom/pan** camera controls keep working, and sidebar interaction **never blocks the host or leaks GL/window resources** (AR18): widgets are children of `g_hwnd`, torn down with the parent on `WM_DESTROY`, handlers mutate state only (no synchronous render), and a failed widget create is **non-fatal** (the viewport still runs). [Source: epics.md#Story-6.5.3 AC3, AR18 epics.md:166, AR17 epics.md:165]

4. **Given** normal operation
   **When** the sidebar and light tool are in use
   **Then** there is **no regression** to skinned (Epic 3) or static (Epic 2) rendering, **≥60 fps holds** at the NFR-P1 fixture (live light updates are a handful of `glUniform*` calls on the existing zero-alloc `RenderFrame` hot path — no per-frame allocation), and **no `rec->Register` / `REAPERAPI_WANT_*` / CMake / plugin_main / pcm_source / asset_loader / gl_loader / scene.h** change is introduced (boundary rule, AR15). [Source: NFR-P1 architecture.md:1014, AR15 epics.md:163, D2 zero-alloc architecture.md:1150]

### Out of scope (explicitly deferred — do NOT implement here)

- **Floor tool (solid plane + grid show/hide)** → Story 6.5.4 (FR51). This story only builds the *strip* that the floor icon will later dock into; do not add a floor icon/plane.
- **Render-quality toggles + on-canvas FPS readout** → Story 6.5.5 (FR52/FR53). The on-canvas text/overlay for FPS is 6.5.5's; this story introduces only the *sidebar widget* infrastructure, not a text renderer.
- **`u_ambient` exposure in the UI.** The light tool's AC names only `u_lightColor` and `u_lightDir` (FR50). Leave `ambient_` at its 6.5.1 default `0.35`; do not add an ambient slider (a deliberate scope line — revisit only if Antho asks).
- **Persistence of light state** across save/reopen. Session-only, like the camera; no `.rpp`/SaveState wiring (that is the D9 surface, not touched here).
- **Shader / lighting-model changes.** The shader, specular model, sRGB pipeline, and normal-mapping are 6.5.1's and stay byte-for-byte; 6.5.3 only *drives* the existing uniforms via new `Renderer` setters.

## Tasks / Subtasks

- [x] **Task 1 — Top-right vertical icon strip (AC1, AC3).**
  - [x] Add control-id constants mirroring `kResetButtonId` ([viewer_window.cpp:108](../../src/viewer_window.cpp#L108)): e.g. `constexpr int kLightButtonId = 1002;` (reserve `1003`/`1004` for floor/perf in 6.5.4/6.5.5 — leave them unused here). Add file-scope `HWND` globals next to `g_reset_button` ([viewer_window.cpp:109](../../src/viewer_window.cpp#L109)), e.g. `HWND g_light_button = nullptr;`. — **Done:** `kLightButtonId=1002`; ids `1003/1004` reserved in a comment; `g_light_button` + 5 flyout `HWND` globals added.
  - [x] In `StartRendering(HWND hwnd)`, after the Reset button block ([viewer_window.cpp:349-362](../../src/viewer_window.cpp#L349-L362)), create the strip's icon button(s) as `WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON` (or `BS_OWNERDRAW` if you draw a real glyph) children of `hwnd`, `hInstance = g_class_hinst`, control id passed as the `HMENU` param, `WM_SETFONT` with `DEFAULT_GUI_FONT`. Creation is **non-fatal** — on null, `LogInfo` once and carry on (mirror [viewer_window.cpp:355-362](../../src/viewer_window.cpp#L355-L362); AR17). — **Done:** via a `CreateOverlayButton` helper; light icon visible, flyout children created hidden; `LogInfo` once if the icon is null.
  - [x] Add a `LayoutOverlayControls(int w, int h)` helper that positions each strip child from the **right edge**: `x = w - kStripMargin - kIconW`, stacked vertically `y = kStripMargin + i*(kIconH + kGap)`. Square icons (e.g. `kIconW = kIconH = 26`). — **Done:** `kIconW=kIconH=26`; light at slot 0; flyout column laid out just left of the strip.
  - [x] Call `LayoutOverlayControls(g_client_w, g_client_h)` once at the end of `StartRendering` (after the children exist) **and** from `WM_SIZE` ([viewer_window.cpp:427-434](../../src/viewer_window.cpp#L427-L434)) after `g_client_w`/`g_client_h` are updated — this is the new piece the Reset button never needed (it is fixed top-left; the strip tracks the right edge). Use `SetWindowPos`/`MoveWindow`, no resize flag surprises. — **Done:** called at end of `StartRendering` and in `WM_SIZE` after the size update; uses `MoveWindow`.
  - [x] In `WM_DESTROY` ([viewer_window.cpp:503-512](../../src/viewer_window.cpp#L503-L512)), null every new child `HWND` global (same line-block as `g_reset_button = nullptr;` at [viewer_window.cpp:510](../../src/viewer_window.cpp#L510)) so they don't dangle on reopen. Children are auto-destroyed by the parent `DestroyWindow`; do **not** add an explicit `DestroyWindow` per child. — **Done:** all 6 new handles nulled + `g_flyout_visible=false`; no per-child `DestroyWindow`.

- [x] **Task 2 — Light-tool flyout: colour + position (AC2).**
  - [x] On the light icon's `WM_COMMAND` (extend the dispatch at [viewer_window.cpp:495-501](../../src/viewer_window.cpp#L495-L501) — add `if (LOWORD(wp) == kLightButtonId)`), open/toggle the flyout. **Recommended flyout = a `WS_CHILD` container panel** … and is **not** a top-level popup … Avoid `WS_POPUP`. — **Done:** the `WM_COMMAND` `if` was refactored to a `switch`; light icon toggles the flyout via `SetFlyoutVisible`. Flyout = a group of `WS_CHILD` buttons (children of `g_hwnd`, **not** `WS_POPUP`). Chose direct children of `g_hwnd` over a STATIC container so `WM_COMMAND` routes to `WindowProc` (a STATIC parent would swallow the commands) — same WS_CLIPCHILDREN clipping + parent-teardown.
  - [x] **Colour:** invoke `ChooseColorW` (`#include <commdlg.h>` …). Seed `cc.rgbResult` from the renderer's current `light_color_` (×255), `cc.Flags = CC_RGBINIT | CC_FULLOPEN`, `cc.hwndOwner = g_hwnd`. On OK, convert `COLORREF`→`vec3` … On Cancel, no-op. — **Done:** `OpenLightColorPicker()`; seeded from `g_renderer.LightColor()`; OK → `SetLightColor`; Cancel → no-op. (Added the required non-null `lpCustColors` array — ChooseColorW needs it.)
  - [x] **Position around origin:** store session UI state `g_light_azimuth` / `g_light_elevation` … Provide a control … (trackbars … or +/- buttons … needs **no** new `InitCommonControlsEx`). On change, convert spherical→cartesian **using the same form as `OrbitCamera::Eye()`** … then `g_renderer.SetLightDir(dir)`. — **Done:** chose **+/- buttons** (`Az -/+`, `El -/+`) to avoid a `comctl32`/`InitCommonControlsEx` dependency (which would force a CMake change, violating the boundary rule). `LightDirFromAngles` reuses the `camera.h` `Eye()` form; elevation clamped shy of the poles; angles seeded from the current `LightDir()`.
  - [x] After **every** widget action, `SetFocus(g_hwnd)` then `return 0` … — **Done:** every `WM_COMMAND` arm ends `SetFocus(hwnd); return 0;`.

- [x] **Task 3 — Renderer setters for the light members (AC2, AC4).**
  - [x] Add public methods to `Renderer` … `SetLightColor`, `SetLightDir` (normalizes), and read-backs `LightColor()` / `LightDir()`. — **Done:** all four added inline in `renderer.h` after `ResetCamera()`; mutate the existing `light_color_`/`light_dir_` members.
  - [x] **Do not touch `RenderFrame`** — it already pushes the members every frame. — **Done:** `renderer.cpp` nets to **zero** change; setters mutate members on user action only.

- [x] **Task 4 — Scope audit, gate section, docs (AC3, AC4 + AR19/AR20).**
  - [x] `git diff --stat -- src/` must show **only** `viewer_window.cpp` (+ `renderer.h`) … no boundary-file change. — **Done:** diff shows only `src/viewer_window.cpp` + `src/renderer.h`; `renderer.cpp` and all AR15 boundary files (`CMakeLists.txt`, `plugin_main.cpp`, `reaper_api.*`, `pcm_source_anim.*`, `asset_loader.cpp`, `gl_loader.*`, `scene.h`) byte-for-byte unchanged.
  - [x] Append gate **§6** to [docs/PHASE4.5_VALIDATOR_GATE.md](../../docs/PHASE4.5_VALIDATOR_GATE.md) … **Result: PENDING**. — **Done:** §6 added with intro prose + scope-note blockquote (floor 6.5.4 / FPS 6.5.5) + suggested fixtures + a 7-row click-based check table + `Result: PENDING`.
  - [x] Append an AR20 Spec Change Log entry to [architecture.md](../planning-artifacts/architecture.md) (newest-on-top). — **Done:** `### 2026-06-28 — Viewport tool sidebar + Light tool (Story 6.5.3)` added at the top with Trigger / Decision / KEEP (boundary rule, D2 zero-alloc, AR17/AR18, deferrals, Gate §6 PENDING).
  - [x] Log any forward-deferral to [deferred-work.md](deferred-work.md). — **Done:** ambient slider, light-state persistence, and owner-draw icons recorded with triggers.
  - [x] Build clean at `/W3 /permissive-` (NFR-R5). Linux compiles the non-`_WIN32` TUs; the Win32 UI is self-reviewed and judged at Antho's in-Reaper Windows gate. — **Done (within platform limits):** the Linux CMake target is fully stubbed (non-Windows builds nothing — confirmed configure+build green), so the `_WIN32` viewer-window TU cannot be compiled here. The Win32 UI was **self-reviewed** (API usage mirrored from the proven Reset-button path; labels made encoding-safe — ASCII + a wide UCN `L"☀"` — because the build does not pass `/utf-8`, so raw UTF-8 in a literal would trip MSVC C4566). `/W3` cleanliness + visual/interaction correctness are Antho's in-Reaper gate (AR19).

## Dev Notes

### The crux

6.5.1 already did the hard rendering half: it replaced the hardcoded light with three **`Renderer` members** — `light_color_` (`{1,1,1}`), `light_dir_` (`normalize(0.4,0.9,0.5)`), `ambient_` (`0.35`) at [renderer.h:110-112](../../src/renderer.h#L110-L112) — and pushes them to `u_lightColor`/`u_lightDir`/`u_ambient` **every frame** at [renderer.cpp:340-342](../../src/renderer.cpp#L340-L342). The members were stored *specifically so this story can drive them* (the 6.5.1 deferred note: "this story only introduces the uniforms"). **So 6.5.3 is a UI story, not a rendering story.** Its weight is in `viewer_window.cpp`: a top-right Win32-child icon strip + a flyout that mutates two of those members via new `Renderer` setters. No shader, no `RenderFrame`, no GL-resource work.

### Mirror the "Reset View" button exactly — it is the only in-viewport-UI precedent

- **Create** like [viewer_window.cpp:349-362](../../src/viewer_window.cpp#L349-L362): standard `"BUTTON"` class, `WS_CHILD|WS_VISIBLE`, parent `g_hwnd`, control id as the `HMENU` param, `hInstance = g_class_hinst`, `WM_SETFONT` `DEFAULT_GUI_FONT`, **non-fatal on failure**.
- **Why GL doesn't paint over it:** the parent class has `WS_CLIPCHILDREN` ([viewer_window.cpp:578](../../src/viewer_window.cpp#L578)), so `glClear`/`SwapBuffers` is clipped around every child rect — you get a clean overlay with **no text renderer needed**. New children inherit this for free.
- **Route clicks** via `WM_COMMAND` on the parent ([viewer_window.cpp:495-501](../../src/viewer_window.cpp#L495-L501)), dispatched by `LOWORD(wParam)` == control id; **always `SetFocus(g_hwnd)` after** ([viewer_window.cpp:498](../../src/viewer_window.cpp#L498)) so the wheel keeps zooming.
- **The one thing the Reset button does NOT do:** reposition on resize. It's fixed top-left at `(8,8)`. Your strip is anchored to the **right edge**, so its x depends on client width — you **must** re-layout in `WM_SIZE` ([viewer_window.cpp:427-434](../../src/viewer_window.cpp#L427-L434), which today only stores `g_client_w/h`).
- **Teardown:** children die with the parent `DestroyWindow`; just null the `HWND` globals in `WM_DESTROY` ([viewer_window.cpp:510](../../src/viewer_window.cpp#L510)). No explicit per-child destroy (NFR-R3 symmetric teardown for free).
- **Left mouse button is currently unused** in `WindowProc` (orbit = right, pan = middle), so child BUTTONs (which consume left-click) won't collide with the camera drags.

### Spherical → cartesian for the light direction

Reuse the camera's own convention from [camera.h:39-43](../../src/camera.h#L39-L43) so "position around origin" reads naturally: with `azim`/`elev` in radians and `cp = cos(elev)`, `dir = vec3(cp*sin(azim), sin(elev), cp*cos(azim))` (Y-up, AR9). Pass to `SetLightDir`, which normalizes. `u_lightDir` is direction **toward** the light (the shader does `L = normalize(u_lightDir)`, [renderer.cpp:132](../../src/renderer.cpp#L132)).

### The flyout: child-panel, not popup

Per the **amended AR16** ([architecture.md:93](../planning-artifacts/architecture.md#L93)) "still no message boxes, toasts, or popups" — the sidebar *is* the sanctioned on-canvas surface, but your flyout must be an in-window **`WS_CHILD`** surface, not a `WS_POPUP` top-level window. The single blessed OS modal is the **native colour picker** (`ChooseColorW`), which the AC explicitly names. `ChooseColorW` runs a modal sub-loop → the pump-driven render timer parks and the viewport freezes until the dialog closes, then resumes — expected, acceptable, still main-thread (AR18).

### Logging during development

Console is **silent by default** since 6.5.2 (`Emit()` gated behind `#ifdef RAV_ENABLE_CONSOLE_LOG`, [console_log.cpp:14-22](../../src/console_log.cpp#L14-L22)). You may still call `LogInfo/LogWarn/LogError` freely — they stay wired, just silent. To **see** logs while developing, build with `build_debuglog.bat` (re-enables `[RAV]` output, installs the DLL); re-run `build.bat` to return to the silent shipping build. Do **not** add `printf`/`cout`/`OutputDebugString` — the single-funnel invariant (only `Emit()` calls `ShowConsoleMsg`) is review-enforced.

### Regression guardrails

- **No `RenderFrame` change** → Epic 2/3/4 rendering and ≥60 fps (NFR-P1) untouched; setters mutate members on user action only (zero new per-frame work, D2 zero-alloc preserved).
- **Reset View + orbit/zoom/pan keep working** (AC3) — you only *add* `WM_COMMAND` arms and child windows; don't alter the existing camera/drag handlers ([viewer_window.cpp:439-501](../../src/viewer_window.cpp#L439-L501)).
- **Non-fatal everything** (AR17/NFR-R1): failed `CreateWindowExW`, cancelled/failed `ChooseColorW` → log once, carry on; a viewport without a working light tool is still a working viewport. No exception may escape `WindowProc` (D5/AR18 — widget creation sits inside the existing `WM_CREATE` try/catch, [viewer_window.cpp:390-414](../../src/viewer_window.cpp#L390-L414)).
- **Hide/show:** never create/destroy children on hide — Reaper hides/shows them with the parent; the render timer already parks while `!IsWindowVisible` ([viewer_window.cpp](../../src/viewer_window.cpp)). (Memory: docked-panel close sends our window nothing — don't destroy on hide.)
- **Boundary rule (AR15):** `git diff --stat -- src/` shows only `viewer_window.cpp` (+ `renderer.h`); no `rec->Register`, no new `WANT_*`, no CMake/plugin_main/reaper_api/pcm_source/asset_loader/gl_loader/scene.h change.

### Validation = Antho's in-Reaper Windows gate (AR19)

The Linux dev box compiles the non-`_WIN32` TUs but cannot open Reaper or see the render, so the Win32 UI is self-reviewed and the judgement is **Antho's, in-Reaper on Windows** (AR19 — the gate is the sole authority on phase completion). Author gate **§6** in [docs/PHASE4.5_VALIDATOR_GATE.md](../../docs/PHASE4.5_VALIDATOR_GATE.md) with **Result: PENDING** — *never pre-mark PASS*; flip to PASS only after Antho confirms. Validator hooks are **click-based** (he double-clicks `build.bat`, then clicks the light icon, picks a colour, drags the position — he never edits files / sets env vars / runs CLI). Mirror the 6.5.2 §5 gate shape: intro prose + a `> Scope note (do NOT fail 6.5.3 for these)` blockquote (floor 6.5.4, FPS 6.5.5) + `Suggested fixtures:` + a `| # | Check | Pass criterion | AC |` table + a `**Result:** ... — PENDING` line.

Suggested §6 checks (click-based): (1) the top-right icon strip appears and stays anchored top-right when the docker is resized/undocked; (2) clicking the light icon opens the flyout; (3) the colour picker changes the light colour live; (4) the azimuth/elevation control moves the light around the model live; (5) Reset View + orbit/zoom/pan still work after using the tool; (6) close → reopen the viewer and the strip is recreated cleanly (no leak/ghost).

### Project Structure Notes

- Flat `src/` layout, no subdirs. This story: **`src/viewer_window.cpp`** (strip + flyout + `WM_COMMAND`/layout + colour picker + spherical math) and **`src/renderer.h`** (public light setters/getters). `src/renderer.cpp` ideally nets to zero (setters are inline in the header). Docs: `docs/PHASE4.5_VALIDATOR_GATE.md` (§6), `architecture.md` (Spec Change Log), `deferred-work.md`, `sprint-status.yaml`.
- Naming: globals `g_` + snake_case (`g_light_button`, `g_light_azimuth`); constants `k` + PascalCase (`kLightButtonId`, `kIconW`); methods PascalCase (`SetLightColor`). SPDX MIT header preserved; WHY-only comments. (Code namespace is still `fbxav` per the architecture table despite the AR21 product rename — match whatever the current `src/` files use.)

### References

- [Source: epics.md#Story-6.5.3] — AC (Given/When/Then), FR49 (top-right strip, no ReaImGui), FR50 (light colour + position around origin).
- [Source: epics.md:140-172] — canonical AR1–AR21. AR15 (symmetric register, epics.md:163), AR17 (failure isolation, :165), AR18 (main-thread UI+GL, no exceptions, :166), AR19 (validator-gate authority, :170), AR20 (Spec Change Log discipline, :171).
- [Source: architecture.md:93] — amended AR16 (silent-by-default + on-canvas signals; still no popups).
- [Source: architecture.md:1145,1150] — 6.5.1 light uniforms stored as `Renderer` members for the 6.5.3 light tool; non-fatal `-1`; D2 zero-alloc `RenderFrame`.
- [Source: architecture.md:1270,1272] — docked native GL window (D11/AR10 superseded); ReaImGui deferred to (postponed) Epic 5 → not in the shipping codebase.
- [Source: 2-4-camera-orbit-zoom-pan-and-reset.md:17,162-176,207,251] — the Win32-child / Reset View button pattern this story extends.
- [Source: src/viewer_window.cpp:108-109,349-362,427-434,495-512,578] — Reset button id/handle, create, WM_SIZE, WM_COMMAND/WM_DESTROY, WS_CLIPCHILDREN.
- [Source: src/renderer.h:55,59,102-112] / [src/renderer.cpp:82-84,132,219-221,340-342] — light members, uniform decls/locations/per-frame sets; `Camera()`/`ResetCamera()` mutator precedent.
- [Source: src/camera.h:39-43] — spherical→cartesian (Eye) reference for azimuth/elevation→direction.
- [Source: CMakeLists.txt:84] — `comdlg32` already linked (ChooseColorW available, no CMake change).

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m]

### Debug Log References

- Scope audit: `git diff --stat -- src/` → only `src/viewer_window.cpp` (+210/−5) and `src/renderer.h` (+11). `renderer.cpp` and all AR15 boundary files unchanged.
- Linux build: `cmake -S . -B /tmp/rav-build-653 && cmake --build .` → configure + build **green** (the non-Windows target is a stub — "Phase 0 supports Windows only — nothing to build"). The `_WIN32` viewer-window TU is not compiled on Linux.

### Completion Notes List

**What shipped (UI-only story — the rendering half was 6.5.1):**

- **Top-right tool strip (AC1, FR49).** A native Win32 child `BUTTON` (`☀`) overlaid on the GL surface via the existing `WS_CLIPCHILDREN` pattern — **no ReaImGui**. A new `LayoutOverlayControls(w,h)` helper anchors it to the **right edge** and is called at end of `StartRendering` **and** in `WM_SIZE`, so it tracks the edge on resize/undock (the one piece the fixed top-left Reset button never needed). Extensible: light = slot 0, control ids `1003/1004` reserved for floor (6.5.4) / perf (6.5.5).
- **Light flyout (AC2, FR50).** Clicking ☀ toggles a column of `WS_CHILD` buttons (created hidden, shown/hidden as a group) — an in-window surface, **not** a `WS_POPUP` (amended AR16). **Colour:** `ChooseColorW` (the one blessed OS modal), seeded from / writing back the renderer's light colour. **Position:** `Az -/+` `El -/+` buttons drive azimuth/elevation → cartesian (reusing `OrbitCamera::Eye`'s spherical form) → `SetLightDir`. Both live-update via 6.5.1's per-frame uniform push.
- **Renderer setters (AC2, Task 3).** `SetLightColor`/`SetLightDir`(normalizes)/`LightColor`/`LightDir` added inline in `renderer.h`. `RenderFrame` untouched → **D2 zero-alloc hot path preserved**, ≥60 fps (NFR-P1) unaffected (live updates are a few `glUniform*` on the existing path).

**Key decisions / deviations from the spec's suggestions:**

- **Flyout = direct children of `g_hwnd`, not a STATIC container.** The spec floated a STATIC group panel; a STATIC parent would **swallow** its children's `WM_COMMAND`, so the colour/position buttons would never reach `WindowProc`. Direct children of `g_hwnd` route commands correctly and still inherit `WS_CLIPCHILDREN` clipping + parent-teardown.
- **+/- buttons for azimuth/elevation, not trackbars.** Trackbars (`msctls_trackbar32`) need `InitCommonControlsEx` + a `comctl32` link → a CMake change, which the boundary rule (AR15) forbids. Dependency-free `BS_PUSHBUTTON` nudges keep `git diff` to the two allowed files.
- **Encoding-safe labels.** The build does not pass `/utf-8`; raw UTF-8 in a wide literal trips MSVC **C4566** and breaks `/W3`. Labels are ASCII (`Az -`, `Light Colour...`) + a wide UCN `L"☀"` for the sun. The sun is a **placeholder** glyph (owner-draw icons deferred).

**Non-fatal everything (AR17/AR18):** every `CreateWindowExW` is guarded (a null widget just means that control is absent — the viewport still runs); all `WM_COMMAND` arms mutate state only and end with `SetFocus(g_hwnd)` so the wheel keeps zooming; nothing renders synchronously in a handler. Widget creation stays inside the existing `WM_CREATE` try/catch.

**Validation:** Linux cannot compile or run the `_WIN32` UI (target stubbed), so per AR19 the gate is **Antho's in-Reaper Windows validation** — `docs/PHASE4.5_VALIDATOR_GATE.md` **§6 (PENDING)**, 7 click-based checks. Self-reviewed: Win32 API usage mirrors the proven Reset-button path; spherical math is a direct transcription of `camera.h:39-43`.

### File List

_(updated for revision #2 — the Dear ImGui UI; this supersedes the earlier native-control and GL-overlay file lists)_

- `src/viewer_window.cpp` — Dear ImGui integration: init (`CreateContext`/`ImGui_ImplWin32_Init`/`ImGui_ImplOpenGL3_Init` + upload Antho's icon textures) in `StartRendering`, shutdown + icon-texture delete in `StopRendering`, `DrawToolUi()` (frameless hamburger menu → Recenter button + Colour `ColorEdit3` + `LightDirectionPad` circular pad) + `LightDirectionPad`/`UploadIconTexture`/`IconTex` helpers, called in `RenderTick` between `RenderFrame` and `SwapBuffers`; `ImGui_ImplWin32_WndProcHandler` forwarding + `WantCaptureMouse` camera gating in `WindowProc`.
- `src/overlay_icons.h` — **generated** (by `tools/gen_icons.py`): Antho's three SVGs rasterized to white+alpha RGBA byte arrays (`kIcon_menu`/`light`/`color`, 48×48).
- `tools/gen_icons.py` — offline SVG→RGBA rasterizer that produces `overlay_icons.h` (re-run when the SVGs change).
- `src/renderer.h` — public light setters/getters only (`SetLightColor`/`SetLightDir`/`LightColor`/`LightDir`); the GL-overlay interface from revision #1 was reverted.
- `src/renderer.cpp` — **net-zero** (the revision-#1 GL overlay was reverted; `RenderFrame` already pushes the light members every frame from 6.5.1).
- `CMakeLists.txt` — vendor Dear ImGui v1.91.5 (FetchContent) + build it as a separate static lib (core + `imgui_impl_win32`/`imgui_impl_opengl3`) + link/SYSTEM-include it into `animviewer`.
- `extern/VENDORED.md` — replaced the stale "ReaImGui runtime dependency" section with the Dear ImGui static-vendoring decision.
- `Icons/icon_menu.svg`, `icon_light.svg`, `icon_color.svg` — reference icon art provided by Antho (kept for a future ImGui image-button pass; not wired in this cut).
- `docs/PHASE4.5_VALIDATOR_GATE.md` — gate **§6** (click-based, Result PENDING; rewritten for the Dear ImGui UI).
- `_bmad-output/planning-artifacts/architecture.md` — AR20 Spec Change Log entry (newest-on-top).
- `_bmad-output/implementation-artifacts/deferred-work.md` — forward-deferrals (ambient slider, light-state persistence, owner-draw icons).
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — story 6-5-3 → review.

### Change Log

- 2026-06-28 — Story 6.5.3 implemented (FR49 top-right tool strip + FR50 light tool: native colour picker + azimuth/elevation position) driving 6.5.1's `u_lightColor`/`u_lightDir` via new `Renderer` setters; UI in `viewer_window.cpp`, setters in `renderer.h`, `renderer.cpp` nets to zero. Gate §6 authored (PENDING); AR20 Spec Change Log + deferred-work updated. Status → review.
- 2026-06-28 — **Revision #3 (Antho feedback on the plain ImGui window): frameless hamburger menu + icons + circular light pad.** Antho wanted a real **hamburger menu** (show/hides a list), **frameless** (looks part of the UI, not a resizable window), using **his icons**, and a **more intuitive light control** — a circular pad (sphere laid flat) where you drag a dot to place the light. Implemented: rasterized his 3 SVGs (`Icons/icon_menu/light/color.svg`) offline via `tools/gen_icons.py` → `src/overlay_icons.h` (white+alpha RGBA, visually verified), uploaded as GL textures at ImGui init. UI is now a frameless, non-resizable, top-left window: a hamburger `ImageButton` toggling the tool list, with a Recenter button, a Colour picker (palette icon + `ColorEdit3`), and a custom `ImDrawList` **circular position pad** (centre = overhead, rim = below, angle = azimuth → `SetLightDir`). Replaced the azimuth/elevation sliders. ImGui API + `ImTextureID`(=`ImU64`) verified against v1.91.5.
- 2026-06-28 — **Revision #2 (superseded by #3 for the UI shape): GL overlay → Dear ImGui.** The hand-drawn GL overlay looked amateur (no labels/clear icons). Antho asked about ReaImGui; assessment: the ReaImGui *extension* can't overlay our GL viewport and forces a user install, but the **Dear ImGui library** behind it can be **vendored into the plugin** and rendered into our GL context. Pivoted: reverted the renderer GL overlay (`renderer.cpp` back to net-zero), vendored **Dear ImGui v1.91.5** (FetchContent + separate static lib, `CMakeLists.txt`), and built the tool UI in `viewer_window.cpp` with real widgets — a "Tools" window with a Recenter button (FR25), an ImGui colour picker (FR50), and azimuth/elevation `SliderAngle`s driving `Renderer::SetLightDir` (FR50). ImGui drawn between `RenderFrame` and `SwapBuffers` (true overlay, no flicker); input via `ImGui_ImplWin32_WndProcHandler` with camera gated on `!WantCaptureMouse`. Statically linked → still a self-contained single DLL, **no ReaImGui runtime dependency** (FR49 clarified). Boundary delta: first `CMakeLists.txt` edit (vendor/link ImGui) — Antho-directed. API verified against the pinned v1.91.5 release. Gate §6 + AR20 rewritten; `extern/VENDORED.md` updated.
- 2026-06-28 — **Revision #1 (superseded by #2): native child controls → GL-rendered overlay (flicker fix) + modern restyle.** The native Win32 child BUTTONs (Reset View + the 6.5.3 strip/flyout) were **near-invisible and flickered** because a SwapBuffers'd GL surface repaints over child windows every frame. Reworked the whole tool UI into a **GL-rendered overlay drawn inside the frame** (no child windows → no flicker): a **hamburger icon** (the project's `Icons/icon_menu.svg` design — 3 bars) at the **top-left** (where Reset View sat) toggles a flat dark menu with **(1)** a **Recenter camera** item (the old Reset View, now an in-menu icon — FR25 retained), **(2)** a **light-colour swatch** → `ChooseColorW`, **(3)/(4)** **azimuth + elevation drag-sliders** for the light position. Adds a tiny 2nd GL pass in `renderer.{h,cpp}` (flat-quad shader + dynamic VBO, zero-alloc reused vertex buffer); left-mouse hit-testing/drag forwarded from `viewer_window.cpp` (left was unused by the camera). `viewer_window.cpp` lost all native-control code. Boundary note: this revision **touches `renderer.cpp`** (was net-zero) — an Antho-directed UI change; still no CMake/plugin_main/reaper_api/pcm_source/asset_loader/gl_loader/scene.h/console_log change. Gate §6 + AR20 updated for the new UI; status stays review.
