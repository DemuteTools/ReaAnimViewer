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
