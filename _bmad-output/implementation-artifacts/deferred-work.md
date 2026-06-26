# Deferred Work

Review findings and follow-up items surfaced during quick-dev iterations
that are intentionally deferred to a later phase. Each item has a clear
trigger condition for when it should be picked up.

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
