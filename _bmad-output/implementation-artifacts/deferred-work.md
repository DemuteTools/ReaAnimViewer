# Deferred Work

Review findings and follow-up items surfaced during quick-dev iterations
that are intentionally deferred to a later phase. Each item has a clear
trigger condition for when it should be picked up.

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

- **`hIcon` and `hIconSm` on the window class** — currently the viewer
  window shows the generic Windows icon. Cosmetic, not a correctness
  issue. Pick up **at Phase 5 (polish)** with a Demute-branded icon.

- **`SetForegroundWindow` Win10/11 rate-limiting** — the OS may reject
  the foreground change if the calling thread doesn't currently have
  input focus, in which case the window flashes in the taskbar instead
  of foregrounding. Currently unhandled. Pick up **at Phase 5** with a
  fallback chain: `BringWindowToTop` + `AllowSetForegroundWindow` +
  `FlashWindowEx`.

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

- **`plugin_register("accelerator", ...)`** — Phase 0 doesn't need
  keyboard input in the viewer. Phase 1+ may want camera control via
  keyboard, in which case Reaper's accelerator routing must be hooked
  to prevent the host eating our keys. Pick up **when keyboard input is
  needed in the viewer** (likely Phase 2-3 for camera).
