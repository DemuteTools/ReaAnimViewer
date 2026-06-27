# Deferred Work

Review findings and follow-up items surfaced during quick-dev iterations
that are intentionally deferred to a later phase. Each item has a clear
trigger condition for when it should be picked up.

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
