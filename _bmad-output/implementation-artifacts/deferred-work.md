# Deferred Work

Review findings and follow-up items surfaced during quick-dev iterations
that are intentionally deferred to a later phase. Each item has a clear
trigger condition for when it should be picked up.

---

## Deferred from: story 6.3 (cross-machine portability via native Reaper media) — 2026-06-27

- **A custom relative-path / missing-media remap engine is explicitly NOT built (the defining boundary of 6.3)** [`src/pcm_source_anim.cpp`; would-have-been `project_state.{h,cpp}`]. FR46 was reformulated 2026-06-27 **off** a home-grown "relative path + missing-media remap" plan **onto** Reaper's **native** copy-into-project + relink (the path rides our file-backed `PCM_source` via `GetFileName`/`SetFileName` — already in place). So 6.3 deliberately builds **no** path-rewriting, search-path resolver, or "locate missing media" dialog of our own — cross-machine portability is native. **Trigger:** there is **no trigger to build a remap engine** — native copy/relink is the sanctioned mechanism. The only sanctioned code is the **only-if-gap** native-participation fix below; anything beyond "make the source participate in the *native* mechanism" is out of scope for the MVP.
- **A "locate missing media" UX is post-release** [`src/viewer_window.cpp`; `src/asset_loader.cpp`]. An unresolved path today surfaces a **console diagnostic** (`LoadAsset`→`LogError`, AR16) and isolates the failure (the poll advances the gate on failure so the rest of the session keeps playing — AR17 / FR37); there is **no** interactive relink/browse dialog. **Trigger:** if validators want a user-facing way to relocate a missing file from inside the viewer, design it post-release (Epic 8 robustness) on top of the existing `LoadResult{category,message}` path — still **not** a remap engine, and Reaper's own "locate media" already covers native relink.
- **Native-participation fix in `pcm_source_anim.cpp` fires only if the in-Reaper gate finds a gap** [`src/pcm_source_anim.cpp` `GetFileName`/`IsAvailable`]. The verify-first hypothesis (AR20) is that "copy media into project directory" already embeds our file-backed items and native relink already finds a moved file → **expected zero-code**. **Trigger:** if `docs/PHASE4_VALIDATOR_GATE.md` §6 row 2 shows a moved-folder reopen does **not** relink + play our items, diagnose *which* native step failed — (a) "copy media" did not copy our files, or (b) relink did not call `SetFileName` on the moved path — and apply the **minimal** fix so Reaper treats the source as copyable/relinkable (`GetFileName`/`IsAvailable` reporting), behind the AR18 no-throw boundary, **without** weakening the `file=` `only-if-empty` guard (that guard keeps the native/relinked path authoritative — the load-bearing 6.1↔6.3 interaction). Re-run §6 after any fix.
- **Per-item camera framing + `time_offset`/`time_scale` reaffirmed-deferred (unchanged from 6.1/6.2)** [`src/pcm_source_anim.cpp`; `src/renderer.*`]. 6.3 persists nothing new: camera is **global** today (no per-item source of truth), and `time_offset`/`time_scale` are **native** take state (`D_STARTOFFS`/`D_PLAYRATE`) Reaper persists and relocates itself. **Trigger:** unchanged from the 6.1 entries below (`D_PLAYRATE` per its 4.4 trigger).

---

## Deferred from: story 6.2 (panel/viewport dock-state persistence) — 2026-06-27

- **Per-item camera framing is still NOT persisted (reaffirmed from 6.1)** [`src/renderer.*` `OrbitCamera`; `src/pcm_source_anim.cpp` `SaveState`/`LoadState`]. The epic AC for 6.2 names "an adjusted camera" in its *Given*, but the camera is **global** today (the renderer owns one `OrbitCamera`, reset on every `SetAsset` — Story 2.4), so there is **no per-item camera source of truth** to persist; the testable *Then* for 6.2 is **dock position only**. `cam_*` keys are **not** added to `SaveState`/`LoadState`. **Trigger:** unchanged from the 6.1 entry below — make the camera per-item (per-item `AnimSource` fields, viewer writes on change / reads+applies on item switch), then add the `cam_*` keys; the versioned `rav_ver`/`key=value` framework already carries them.
- **Auto-reopen the viewer panel on project load is NOT implemented** [`src/viewer_window.cpp`; `src/plugin_main.cpp`]. 6.2 relies on the **dock position** (native docker / `kDockIdent` in `reaper.ini`), but it does **not** make the viewer panel **auto-open** when a project that had it open is reopened — the user re-runs the `RAV: Open Viewer` toggle action, and the panel returns to its remembered dock. Dock *position* is global (`reaper.ini`); "was the panel open in *this* project" is per-project state we do not currently record. **Trigger:** if validators want the panel to reappear automatically on project load, persist a per-project "viewer was open" flag (project ext-state or a `ProjectStateContext` hook) and, on a project-load notification, call `OpenViewerWindow` while keeping the toggle/`RefreshToolbar` state coherent (AR15/AR18) — **never** ReaImGui, **never** a custom screenset engine.
- **Global ext-state dock fallback fires only if the in-Reaper gate finds a gap** [`src/viewer_window.cpp`; `src/reaper_api.h`]. The verify-first hypothesis (AR20) is that the native docker (`DockWindowAddEx` + stable `kDockIdent`) already round-trips the dock position (dock state is Reaper-global / `reaper.ini`, not per-project, per D9) → **expected zero-code**. **Trigger:** if `docs/PHASE4_VALIDATOR_GATE.md` §5 row 1/2 shows the panel does **not** return to its remembered dock, add a **minimal** global `SetExtState`/`GetExtState` save/restore in `viewer_window.cpp` (+ `REAPERAPI_WANT_SetExtState`/`WANT_GetExtState` in `reaper_api.h`, resolved via the existing `REAPERAPI_LoadAPI` — no new `rec->Register`), behind the AR18 no-throw boundary, and re-run §5.

---

## Deferred from: story 6.1 (per-item state persistence) — 2026-06-27

- **Per-item camera framing is NOT persisted (camera is global today)** [`src/pcm_source_anim.cpp` `SaveState`/`LoadState`; `src/renderer.*` `OrbitCamera`]. The D9 `key=value` format names optional `cam_target` / `cam_distance` / `cam_yaw` / `cam_pitch` keys ([architecture.md:589-596](../planning-artifacts/architecture.md#L589)), but Story 6.1 persists **none** of them, because there is **no per-item camera state to persist**: the renderer owns **one** `OrbitCamera` (`cam_`), reset on every `SetAsset` (Story 2.4), shared across all items. Writing `cam_*` now would have no source of truth. **Trigger:** if per-item camera framing is wanted, (1) add per-item camera fields to `AnimSource`, (2) have the viewer **write** the current camera into the current item's source on change and **read+apply** it on item switch (the same poll/load path 4.3–4.5 uses), then (3) add the `cam_*` keys to `SaveState`/`LoadState` — the versioned `rav_ver`/`key=value` framework is already in place to carry them (forward-compat: an older extension ignores the new keys).
- **`time_offset` / `time_scale` are native take state, not ours to write** [`src/pcm_source_anim.cpp` `GetCurrentAnimItem`]. The D9 format also lists optional `time_offset` / `time_scale` keys, deliberately **not** persisted by 6.1: left-trim offset is take **`D_STARTOFFS`** — Reaper persists it **natively** and 4.4's `GetCurrentAnimItem` already consumes it; writing our own `time_offset` would duplicate native state and risk the same override hazard the `file=` `only-if-empty` guard exists to prevent. Playrate is take **`D_PLAYRATE`** — **already deferred from Story 4.4** (cross-reference the 2026-06-27 story-4.4 entry below; not duplicated here). **Trigger:** none from 6.1 — these stay native; only revisit `D_PLAYRATE` per its existing 4.4 trigger (retime the animation by multiplying the item-relative term by `D_PLAYRATE`, reconciled with 4.2 item-sizing).

---

## Deferred from: code review of story-4.5 (2026-06-27)

- **Half-open span `[ip, ip+il)` — playhead parked exactly on a lone item's end shows nothing** [`src/pcm_source_anim.cpp` `GetCurrentAnimItem`, the `pos >= ip + il` continue]. Pre-existing 4.3/4.4 boundary, unchanged by 4.5's selection rework. For two abutting items the half-open span is correct (the next item owns that sample); but for a **single** item, when the cursor lands exactly on `ip+il` the function returns false. With the startup fixture gone (if the `viewer_window.cpp` removal is kept), the first-ever interaction parking precisely on an end edge shows a blank panel instead of the last frame. **Trigger:** if validators report a one-sample blank at item ends, decide whether the end sample should hold the item (close the high end with `pos <= ip + il` for the lone-item case, or hold-last-frame in RenderTick).
- **Current item deleted/moved → viewer holds a stale frame of a no-longer-present asset** [`src/viewer_window.cpp` `RenderTick` hold branch + the `g_transport_driven` latch]. Once any RAV item drives the view, `g_transport_driven` latches true forever; deleting the current item (or removing the topmost overlapping item with nothing else spanning) makes `GetCurrentAnimItem` return false → RenderTick HOLDs the deleted item's last frame, never reverting to the idle/blank background. Pre-existing (AC3 "freeze on leave"), but it conflicts with the "viewport idle until an item is current" narrative introduced by the picker/fixture removal. **Trigger:** if a validator finds a deleted animation still frozen on screen and wants it cleared, reset `g_transport_driven`/clear the displayed asset when no item has been current for N ticks (or on a project-change notification).
- **"Ours"-typed topmost item with an empty source path masks a valid lower-track item** [`src/pcm_source_anim.cpp` winner-emit, `out_path = fn ? fn : ""`]. 4.5 selects the winner by track number only; a topmost RAV item whose `GetFileName()` is null/empty would set `out_path=""` and suppress an otherwise-renderable lower-track item (the old first-match code had the same exposure only for the first item). Realistically unreachable — our `PCM_source` always carries `m_path` — and `LoadAsset("")` fails+logs once (path-change gate, no per-frame storm). **Trigger:** if an empty-path "ours" source ever becomes possible, skip empty-path candidates in the walk so a valid lower-track item can win.

*(The same-track Z-order tie-break and the duplicate-file asset cache are recorded under the story-4.5 implementation deferrals below, not duplicated here.)*

---

## Deferred from: story 4.5 (multi-item / current-item selection) — 2026-06-27

- **Same-track overlap front-most / Z-order precedence is not honored** [`src/pcm_source_anim.cpp` `GetCurrentAnimItem`]. Story 4.5 resolves overlap by **track** priority (the topmost track's spanning RAV item wins — lowest `IP_TRACKNUMBER`), satisfying FR14's "highest-priority **track**". But when **two** RAV items overlap on the **same** track (same `IP_TRACKNUMBER`), the selection ties and the walk keeps the **first walk-order** item (strict `<`), which is deterministic but is **not** necessarily Reaper's visual **front-most / Z-order** item. **Trigger:** if a sound designer stacks RAV items on a single track (e.g. in lanes / fixed-item-lanes) and expects the visually front-most to display, read the item Z-order / lane info and add it as a secondary tie-break in `GetCurrentAnimItem` (after the `IP_TRACKNUMBER` compare).
- **No asset cache to share VRAM across duplicate files** [`src/pcm_source_anim.cpp` / `src/viewer_window.cpp` reload path]. The viewer lazily loads **one** asset — the item under the playhead — and reloads on a path change (Story 4.3), so NFR-P6 (≥10 items) holds with VRAM at ~one asset regardless of item count. There is **no** shared cache, so the same `.glb` placed on two tracks would be re-loaded each time the playhead crosses from one to the other (a per-path **cold reload**, the Story 4.3 cost — bounded by the path-change gate, not by item count). **Trigger:** if rapid scrubbing across many distinct-then-repeated items makes per-boundary load time an issue, add the shared `Asset` cache (the D2 factory pattern) keyed by file path (architecture.md:220/1101 — post-MVP). *(Take `D_PLAYRATE` is **already** logged from 4.4 above — not duplicated here; the priority walk does not change that deferral.)*

---

## Deferred from: story 4.4 (native-media coexistence) — 2026-06-27

- **Take playrate (`D_PLAYRATE`) is not honored** [`src/pcm_source_anim.cpp` `GetCurrentAnimItem`]. Story 4.4 makes an animation item behave like native media for **move / resize / color / rename** and honors the take's `D_STARTOFFS` (left-trim → `animTime = (pos − itemStart) + D_STARTOFFS`), but a change to the take's **playrate** does **not** time-stretch the animation in this MVP — the displayed time advances at 1.0× regardless of `D_PLAYRATE`. **Trigger:** if sound designers need to retime an animation item via playrate, multiply the item-relative term by `D_PLAYRATE` in `GetCurrentAnimItem` (`animTime = D_STARTOFFS + (pos − itemStart) * D_PLAYRATE`, one more `GetMediaItemTakeInfo_Value` call) **and** reconcile it with Story 4.2's `GetLength` / `ProbeAnimationDuration` item-sizing, which currently assumes playrate 1.0 — changing a take's playrate makes Reaper auto-resize the item, so the displayed-time mapping and the item-length contract must move together. That cross-story coupling (4.2 ↔ 4.4) is why playrate is deferred while `D_STARTOFFS`, which has no such coupling (a left trim leaves item length unchanged), ships in 4.4.

---

## Deferred from: code review of story-3.3 (2026-06-26)

- **Palette uploaded without an `isfinite` screen** [`src/renderer.cpp` `RenderFrame`]. The per-frame `ComputePose` output is uploaded verbatim via `glUniformMatrix4fv`; inputs are guarded (3.1 weight `isfinite`, 3.2 tps-finite + quat-normalize) but a degenerate zero-scale/sheared bind bone could still compose a NaN into the palette → on-screen NaN-explosion vs AC7. **Trigger:** if a rig explodes/vanishes at the AR19 visual gate, add a cheap finite-screen (or log `palette_[0]` + AABB) as a §C diagnostic; a per-frame scan over ≤128 mat4 is the cost to weigh.
- **Clip-less / non-canonical skinned rig misplaced via the static fallback** [`src/renderer.cpp` + `src/asset_loader.cpp` §C]. When `pose_valid` is false (no clip / no-op), a skinned mesh draws through the static path at mesh-local space, which equals bind pose ONLY for canonical (identity-bind-palette) skins. A rig with a non-identity mesh→armature bind renders displaced/mis-scaled. **Trigger:** the AR19 in-Reaper visual gate — if the validated FBX comes out displaced, walk the §C knob order (`globalInverse` premultiply first for FBX). This is the dominant-risk gate item, not a Linux-side fix.

---

## Deferred from: code review of story-3.2 (2026-06-26)

- **`ComputePose` size-mismatch no-op has no diagnostic** [`src/animation.h`]. On a buffer/channel size mismatch, `ComputePose` returns without writing, so a caller's pre-zeroed palette/scratch read as a *valid-looking* all-zero pose (0 is finite, root tx (0,0,0)) — silently masking a wiring bug. Harmless in 3.2 (the only caller, `DumpAnimation`, always pre-sizes to `bones.size()`). **Trigger:** Story 3.3, when a live per-frame caller (`Animator`/`PoseBuffer` sized at `SetAsset`) is wired into the render loop — add an assert/once-warn on mismatch so a mis-sized buffer is loud, not a frozen rig.

---

## Architectural decision recorded 2026-05-10 (PRD step-04 round)

**Decision: viewer is rendered inside a ReaImGui dockable panel, not a standalone Win32 window.**

The Phase 0 standalone Win32 viewer window (`viewer_window.cpp`) is superseded by a ReaImGui-driven panel that docks into Reaper's docker system. This was raised by Antho during PRD User Journey discovery (step-04) — *"j'aimerai qu'il soit embedded dans une fenetre ReaImGui comme ca on peut Docker la fenetre dans l'interface Reaper. C'est important."*

**Consequences:**

- A new **Phase 0.5** is inserted in the build plan (~1.5–2 days, slot in Week 2): refactor `src/viewer_window.cpp` into `src/imgui_panel.cpp`. Keep WGL context creation (sokol_gfx still needs a GL context) and the action registration. Remove `RegisterClassExW`, `CreateWindowExW`, `WindowProc` — ReaImGui replaces all of that.
- Rendering pattern: sokol_gfx renders the 3D scene to an off-screen FBO; the resulting texture handle is passed to `ImGui::Image()`. ReaImGui handles panel chrome, docking, resize, and input.
- ReaImGui (cfillion's extension, ReaPack `cfillion/reaimgui`) becomes a runtime dependency. The ReaPack manifest must declare it as an auto-install dependency.
- Cross-platform window management is now ReaImGui's problem, not ours. The original Phase 0 SWELL-port plan (deferred from `extern/WDL`) is rendered moot — ReaImGui internally uses SWELL on Mac/Linux.
- Three earlier deferred items become **superseded** (see annotations in the Phase 0 review section below).

**Triggered by:** PRD step-04 architectural input.
**Pick up:** Phase 0.5 must precede Phase 1 (since Phase 1 renders into the panel).

---

## From Phase 0 review (2026-05-09)

### Build hardening

- **/Zc:__cplusplus flag** — without it, MSVC reports `__cplusplus = 199711L`
  even with `CXX_STANDARD 17`. No vendored SDK header currently guards on
  `__cplusplus >= 201703L`, so this is dormant. Pick up **when adding any
  third-party header that uses `if __cplusplus >=` guards** (likely Phase 1
  with assimp/glm/sokol_gfx).

- **/WX warnings-as-errors** — current build sets `/W3` but not `/WX`, so
  warnings still produce a successful build. AC1 requires "zero warnings"
  but enforcement relies on the validator manually inspecting the build
  log. Pick up **when CI is added** (Phase 5) or earlier if a regression
  ever sneaks a warning past the gate.

- **CMAKE_SOURCE_DIR → PROJECT_SOURCE_DIR** in `cmake/ReaperPlugin.cmake`
  for the SDK include path. Phase 0 doesn't compose, but if this project
  ever becomes an `add_subdirectory()` of a larger CMake build the SDK
  path will resolve to the parent's source dir. Pick up **if/when this
  project is consumed as a subdirectory**.

### Window UX polish

- **~~`hIcon` and `hIconSm` on the window class~~** — **SUPERSEDED 2026-05-10 by ReaImGui decision**: the window class no longer exists in Phase 0.5+; ImGui panels don't carry HWND-level icons.

- **~~`SetForegroundWindow` Win10/11 rate-limiting~~** — **SUPERSEDED 2026-05-10 by ReaImGui decision**: foreground/focus handling now goes through Reaper's docker; the OS rate-limit issue doesn't apply to docked panels.

### Pixel format leanness

- **`pfd.cDepthBits = 24; pfd.cStencilBits = 8`** — Phase 0 only clears
  the framebuffer, never tests depth or stencil. Currently we request
  depth/stencil pessimistically. Some integrated GPUs may not match a
  24/8 format and fall back via `ChoosePixelFormat`. Pick up **at the
  start of Phase 1 (mesh rendering)** when actual depth-test policy is
  decided — the pixel format choice depends on whether we go forward-
  rendered with z-buffer or deferred.

### Reaper API loader extension

- **Capture more API functions** as later phases need them: `Main_OnCommand`,
  `GetPlayPosition2Ex`, `GetSetMediaTrackInfo_Value`, `EnumProjects`,
  project state hooks for camera persistence, etc. Add `REAPERAPI_WANT_*`
  defines in `src/reaper_api.h` as needed. Pick up **incrementally per
  phase**.

### Modeless dialog wiring

- **~~`plugin_register("accelerator", ...)`~~** — **SUPERSEDED 2026-05-10 by ReaImGui decision**: ReaImGui handles keyboard input routing through its own accelerator hooks; no manual `plugin_register("accelerator", ...)` needed from us.

---

## Deferred from: code review of story-1.4 (2026-06-24)

- **DC leak on a stale `g_hwnd`** ([src/viewer_window.cpp:96](../../src/viewer_window.cpp#L96)) — `CloseViewerWindow`'s `!IsWindow(g_hwnd)` branch routes through `DestroyGLContext`, where `WindowFromDC(g_hdc)` returns null for an already-destroyed window, so `ReleaseDC(null, g_hdc)` silently fails and the DC leaks. Pre-existing (the `WindowFromDC` fallback shipped in 1.3/HEAD, owned by 1.4 per the story). Narrow trigger: Reaper would have to destroy our child window out from under us, and it only happens at unload, where the single-DC leak is harmless. **Pick up if** a future story sees Reaper externally destroying the docked HWND (e.g. screenset/layout changes), or if a leak audit flags it.

---

## Deferred from: code review of story-2.3 (2026-06-24)

- **Textured colored metal derives a white specular highlight** ([src/asset_loader.cpp:100-103](../../src/asset_loader.cpp#L100-L103)) — `ConvertMaterial` tints the specular toward `baseColorFactor`, but a gold/copper metal commonly authors `baseColorFactor=(1,1,1)` with the actual color in the diffuse *texture*. The loader sees only the factor (texture binding is a separate step), so the derived specular is white instead of base-color-tinted — a fidelity gap on exactly the metallic-roughness case FR16 targets. Correct handling needs the albedo texel, i.e. texture-aware / true PBR shading (a metalness uniform threaded into the shader, applied post-texture). That is explicitly out of Story 2.3's scope (Dev Notes §C: Blinn-Phong approximation only, no PBR BRDF). **Pick up as** optional Epic-6 polish if the gate finds metallic highlights read wrong, alongside the deferred metal-diffuse-suppression knob (§C).

---

## Deferred from: code review of story-3.1 (2026-06-26)

- **Same-named (or empty-named) distinct joints collapse to one global index** ([src/asset_loader.cpp](../../src/asset_loader.cpp) `DfsIndexJoints`) — the D1 skeleton keys bones by name (§B mandates dedup-by-name, the assimp bone↔node matching is name-based), so two distinct nodes sharing a name (common in mis-authored FBX left/right chains, or assimp's unnamed intermediate nodes → empty name) collapse to a single index. The second subtree's weights silently scatter onto the first bone and its `parentIdx` links point at the wrong parent, with no warning. Truly fixing it requires an identity model beyond name-matching (e.g. node-path keys) — out of 3.1's name-keyed model. **Pick up** in 3.3 if a real Demute fixture exhibits duplicate joint names; a cheap interim mitigation is a warn-once when `DfsIndexJoints` sees a name already indexed.
- **No >128-bone palette cap** ([src/asset_loader.cpp](../../src/asset_loader.cpp) `BuildSkeleton`) — `boneIds` can exceed the D13 std140 128-mat4 palette ceiling for a large rig, silently overflowing 3.3's GPU palette. Spec §F explicitly defers the `kMaxBones`/cap decision to 3.3 (validated Mixamo fixture = 99 bones, within ceiling). **Pick up** in 3.3 when the skinning shader + palette UBO are built — clamp/warn at parse or size the palette to the rig.
