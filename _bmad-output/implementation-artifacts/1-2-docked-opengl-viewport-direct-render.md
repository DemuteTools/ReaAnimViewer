# Story 1.2: Docked OpenGL viewport (direct render at 60 fps)

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As the implementing developer,
I want a native OpenGL window that renders its scene **directly** into the window framebuffer and presents at **≥60 fps**, driven by a frame timer independent of Reaper's ~30 Hz UI loop,
so that the revised viewport architecture (D11/AR10 superseded by Spike 0 — see architecture Spec Change Log 2026-06-23) is proven on `main` before Story 1.3 docks it and Epic 2 renders real meshes through it.

This is the **second story of Epic 1 (Phase 0.5)**, on `main`, immediately after the 1.1 rename. It is the **first production code that renders something**: it replaces the Phase 0 window's single clear-color `WM_PAINT` with a continuously-rendered, timer-driven, resizable GL viewport. It does **not** dock the window (that is Story 1.3), does **not** load meshes (that is Epic 2), and does **not** harden the init-failure/teardown path (that is Story 1.4). Its one job is to prove the direct-render-at-60-fps engine with clean resize, and to lay down the renderer / GL-loader / RAII-handle structure that Epics 2–3 build on.

## Acceptance Criteria

These transcribe Story 1.2's BDD from [epics.md §Story 1.2](../planning-artifacts/epics.md) and bind each clause to its verifying mechanism.

1. **(AC1 — direct render on the main thread)** Given a WGL/OpenGL context created on **Reaper's main thread** (AR18), when the renderer draws a scene into the **window's default framebuffer directly** and presents via `SwapBuffers`, then visible, **correctly-oriented** (Y-up, right-handed, depth-tested) content appears in the window. **No offscreen FBO, no `ImGui::Image()` bridge, no CPU readback** — those approaches are superseded (Spike 0 Finding 1).
2. **(AC2 — ≥60 fps, independent timer)** And the per-frame render is driven by a **frame timer independent of Reaper's ~30 Hz UI loop**, and the viewport holds **≥60 fps** on Antho's reference workstation (NFR-P1; the spike measured ~62 fps at 1055×604). A smoothed fps figure is emitted to the Reaper console (`[RAV] info: … fps`) so the rate is observable, not assumed.
3. **(AC3 — resize without leaks)** And the viewport **resizes with the window** — content keeps the correct aspect ratio and re-fills the client area — **without leaking GL resources** (scene GL objects are owned by RAII handles, D2/AR8; resize does not recreate the context or reallocate per-frame GPU objects).
4. **(AC4 — build + load stay clean; no regression)** And the build stays **zero-warning at MSVC `/W3 /permissive-`** (NFR-R5) and the extension still **loads in under 2 s** (NFR-P4); the `RAV: Open Viewer` Action still opens the window (no regression to 1.1); on window close the frame timer is killed and the GL context/renderer are torn down with **no leaked context, timer, or window class** (NFR-R3 — full dock/undock lifecycle is Story 1.3's scope, but the standalone-window teardown must already be clean).

## Tasks / Subtasks

- [x] **Task 1 — Decision: raw GL for the MVP viewport (record it) (AC: 1)**
  - [x] Adopt **raw OpenGL** (modern GL via `wglGetProcAddress`) for the viewport renderer, **not** sokol_gfx. Rationale: Spike 0 Finding 4 — the pinned sokol `85d1f1b` ships a new view-based API diverged from D11/D12; with direct-to-window rendering (no FBO) the raw-GL surface the spike already proved is smaller and faster to stand up. The architecture's own lean is "raw GL for the Windows MVP; revisit sokol only if a Mac/Metal backend is needed post-MVP" ([architecture.md Spec Change Log 2026-06-23](../planning-artifacts/architecture.md#spec-change-log)).
  - [x] Consequence to honor: the architecture's boundary rule "`renderer.cpp` is the only file that calls `sg_*`" becomes "`renderer.cpp` + `gl_loader.cpp` are the only files that call modern GL." `extern/sokol/` is **not** added.
  - [x] Append a one-line confirmation to the architecture Spec Change Log (AR20) — terse, no ceremony (Antho prefers substance over process): "2026-06-2x — Phase 0.5 confirmed raw GL for the MVP viewport; sokol not added (Spike 0 Finding 4)."
- [x] **Task 2 — Introduce the modern-GL function loader (AC: 1, 4)**
  - [x] New `src/gl_loader.h` / `src/gl_loader.cpp` (namespace `rav`). Adapt the spike's [`spike_gl.h`/`spike_gl.cpp`](#references) function-pointer table + `wglGetProcAddress` resolution, **dropping** the spike's hidden-host-window/context code (the viewport window owns the context here) and the FBO entry points (no FBO in direct render). Provide a `bool LoadGlFunctions(std::string& out_error)` that resolves every required pointer and reports the first missing one.
  - [x] Required functions for the 1.2 test scene: shader (`glCreateShader`/`glShaderSource`/`glCompileShader`/`glGetShaderiv`/`glGetShaderInfoLog`/`glCreateProgram`/`glAttachShader`/`glLinkProgram`/`glGetProgramiv`/`glGetProgramInfoLog`/`glDeleteShader`/`glDeleteProgram`/`glUseProgram`/`glGetUniformLocation`/`glUniformMatrix4fv`), buffers (`glGenBuffers`/`glDeleteBuffers`/`glBindBuffer`/`glBufferData`), and VAO (`glGenVertexArrays`/`glDeleteVertexArrays`/`glBindVertexArray`/`glEnableVertexAttribArray`/`glVertexAttribPointer`). Keep the `X`-macro table so Epic 2/3 append textures, `glVertexAttribIPointer`, etc. without churn.
  - [x] Define the GL ≥1.5/3.0 enums/typedefs missing from Windows `<gl/GL.h>` (`GLchar`, `GLsizeiptr`, `GL_VERTEX_SHADER`, `GL_ARRAY_BUFFER`, `GL_STATIC_DRAW`, `GL_COMPILE_STATUS`, …) exactly as the spike header does.
- [x] **Task 3 — Introduce RAII GL resource handles (AC: 3, 4)**
  - [x] New `src/gpu_resources.h` (header-only; namespace `rav`). Move-only RAII wrappers for the raw-GL objects the renderer owns: `GpuBuffer` (VBO/IBO), `GpuVertexArray` (VAO), `GpuProgram` (shader program). Pattern per [architecture.md §Pattern Examples](../planning-artifacts/architecture.md) — `= delete` copy, `noexcept` move that steals + zeroes the source, destructor calls `glDelete*` **only if** the handle `!= 0`.
  - [x] This pulls `gpu_resources.h` in at Phase 0.5 (the architecture tree labels it "Phase 1") and re-bases its wrappers on `GLuint` instead of `sg_*_t`. Note this as a deliberate structure variance (consequence of the raw-GL decision).
- [x] **Task 4 — Introduce the console-log helper (D7 format) (AC: 2, 4)**
  - [x] New `src/console_log.h` / `src/console_log.cpp` (namespace `rav`): `LogInfo/LogWarn/LogError(const char* fmt, …)` that `snprintf` into a stack buffer and funnel through `ShowConsoleMsg`, emitting `[RAV] <level>: <message>\n` (single line per event) per [architecture.md D7 / Format Patterns](../planning-artifacts/architecture.md), using the **`[RAV]`** tag established by Story 1.1 (supersedes the doc's historical `[FBXAV]`).
  - [x] Use it for the once-per-second fps line and any GL-init diagnostic in this story. (Keep it lean — this is the project's single user-feedback channel, AR16.)
- [x] **Task 5 — The renderer: direct-render test scene (AC: 1, 3)**
  - [x] New `src/renderer.h` / `src/renderer.cpp` (namespace `rav`). Interface shaped like the spike's `Renderer` but for direct-to-window: `bool Init(std::string& out_error)` (compiles the shader, builds the test-scene VAO/VBO; requires a current GL context), `void RenderFrame(float time_seconds, int width, int height)` (sets `glViewport`, clears, draws into the **currently-bound default framebuffer** — never binds an FBO), `void Shutdown()` (RAII handles release on destruction; explicit `Shutdown` zeroes them while the context is still current).
  - [x] Test scene: a small, **visibly oriented, animated** scene — recommended a lit/colored **rotating cube above a ground reference** (or a cube + axis triangle) — enough to prove (a) the shader pipeline runs, (b) `GL_DEPTH_TEST` (`GL_LESS`) + backface cull (`GL_CULL_FACE`, CCW front per D12) work, (c) **Y-up is visually unambiguous**, and (d) the rotation, advanced by the timer's elapsed seconds, makes >30 fps smoothness visible to the eye. `#version 330` shaders (the spike's profile — see Task 7 context note).
  - [x] Matrices via **GLM** (Task 6): perspective projection (50° vertical FOV, near 0.01, far 1000 per D14), `lookAt`, model rotation. Recompute projection from the **current width/height** each frame so resize keeps aspect (AC3). Clear color matches Phase 0 (`0.10, 0.10, 0.12, 1.0`).
  - [x] No `new`/allocation in `RenderFrame` (D2/hot-path rule): build buffers once in `Init`; the per-frame matrices are stack locals.
- [x] **Task 6 — Add GLM (matrix math) per AR6 (AC: 1, 4)**
  - [x] The renderer is the project's first matrix consumer and GLM is **absent on `main`**. Add **GLM 1.0.3**. Default per AR6: **vendor** the header tree under `extern/glm/` (consistent with the no-submodule vendoring discipline) and add the include dir in CMake. Acceptable fallback if vendoring the full header tree is impractical on the drvfs/WSL mount: **CMake FetchContent** pinned to the `1.0.3` tag (the spike's proven approach — see its `CMakeLists.txt`). Record whichever in `extern/VENDORED.md` with the pin.
- [x] **Task 7 — Evolve the viewer window: GL-func load + frame timer + present + resize (AC: 1, 2, 3, 4)**
  - [x] Update [src/viewer_window.cpp](../../src/viewer_window.cpp). Keep it a **standalone top-level window** opened by the existing `RAV: Open Viewer` Action (docking is Story 1.3). Keep `CreateGLContextFor` / `EnsureClassRegistered` / the idempotent `OpenViewerWindow` / class-unregister-on-close. Changes:
    - After `wglCreateContext` + `wglMakeCurrent`, call `rav::LoadGlFunctions` and `Renderer::Init`; on failure emit `[RAV] error:` and bail (do not open a broken window). *(Story 1.4 hardens this into the full symmetric-teardown-on-bail path; here a clean log-and-bail is enough.)*
    - **Frame timer:** drive rendering from a **Win32 `SetTimer(nullptr, 0, 15, &TimerProc)`** (~66 Hz), started when the window opens and **`KillTimer`'d** on close/destroy. Reaper's main message pump dispatches `WM_TIMER` on the **main thread** (AR18 ✓). This is the spike-proven mechanism for exceeding Reaper's ~30 Hz extension-timer cap — do **not** use `rec->Register("timer", …)` for the render loop (that rides the ~30 Hz UI loop). The timer callback makes the context current, calls `Renderer::RenderFrame(elapsed, w, h)`, and `SwapBuffers`.
    - **`WM_PAINT`** becomes a no-op validate (`BeginPaint`/`EndPaint`) — the timer renders, not paint. Keep `WM_ERASEBKGND` returning 1 (GL owns the surface).
    - **`WM_SIZE`** stores the new client width/height (clamped ≥1); the timer's next `RenderFrame` picks them up. Do **not** recreate the context or scene buffers on resize (AC3 — direct render to the window default FB means there is nothing GPU-side to reallocate).
    - **vsync:** resolve `wglSwapIntervalEXT`; recommended ship with vsync **on** (interval 1 → tear-free clean 60) but the dev should first measure with it **off** (interval 0, as the spike did) to confirm ≥60 fps headroom, then record the chosen policy. Either satisfies AC2 as long as the timer is >30 Hz.
    - **fps:** count `SwapBuffers` calls, compute fps over a ≥1 s window via `QueryPerformanceCounter`, log via `rav::LogInfo` once per second.
    - On `WM_DESTROY` / `CloseViewerWindow`: `KillTimer`, make context current, `Renderer::Shutdown()`, then the existing `DestroyGLContext` + `UnregisterClassW`. Order matters — GL objects must be deleted while the context is current.
  - [x] Update [src/viewer_window.h](../../src/viewer_window.h) only if the signatures change (they should not — `OpenViewerWindow`/`CloseViewerWindow` stay).
- [x] **Task 8 — Wire up CMake + API surface (AC: 4)**
  - [x] [CMakeLists.txt](../../CMakeLists.txt): add `src/gl_loader.cpp`, `src/renderer.cpp`, `src/console_log.cpp` to the `add_reaper_extension(animviewer …)` SOURCES (explicit list, no GLOB — D-build hygiene). Add the GLM include directory (vendored `extern/glm/` or FetchContent target). Keep `target_link_libraries(animviewer PRIVATE opengl32 gdi32 user32)`. Update the non-Windows stub message only if needed.
  - [x] [src/reaper_api.h](../../src/reaper_api.h): **no new `WANT_*` expected** for 1.2 — the timer is Win32 `SetTimer`, not a Reaper API, and `ShowConsoleMsg` is already wanted. (`DockWindowAddEx` etc. arrive in Story 1.3.) Confirm by grep that no other Reaper symbol is newly called.
- [x] **Task 9 — Verify, seed the Phase 0.5 gate, hand off the Windows run (AC: 2, 3, 4)**
  - [x] On this Linux/WSL box, run `cmake -B build` (out-of-sandbox tmp dir if the in-tree `build/` hits the drvfs `configure_file` restriction noted in Story 1.1) to confirm the renamed/extended target configures and that GLM resolves. The non-Windows branch stubs the target — this proves CMake syntax + dependency wiring only, **not** the MSVC build or the GL runtime.
  - [x] Seed `docs/PHASE0.5_VALIDATOR_GATE.md` (new) with this story's rows (it grows in 1.3/1.4) per AR19 — see "Antho's manual validation" below. Keep it short.
  - [x] **Hand the Windows MSVC build + Reaper run to Antho** (the dev box has no MSVC/Reaper, same constraint as Story 1.1): zero `/W3` warnings, Action opens the window, a visibly-animated correctly-oriented scene renders, console shows `[RAV] info: … fps` at **≥60**, dragging the window edges resizes the content cleanly with no flicker/leak/crash, closing the window leaves Reaper stable, load <2 s. Note this handoff explicitly in Completion Notes.

## Dev Notes

### Why this story exists (and what it is NOT)

Spike 0 proved — on the reference workstation — that a **ReaImGui panel is hard-capped ~30 fps** and **cannot display a foreign GPU texture**, while a **native OpenGL window rendering directly + `SwapBuffers` on an independent timer hit ~62 fps** ([docs/SPIKE0_FINDINGS.md](../../docs/SPIKE0_FINDINGS.md) Finding 1). That retired the dominant architectural risk and rewrote D11/AR10. Story 1.2 re-implements that finding as **production code on `main`** (the spike branch is throwaway and never merged — do not reference or merge `spike/0-1-feasibility`). It is the load-bearing proof: every later epic renders through this engine.

Scope fences (what belongs to neighbouring stories — do not pull them in):
- **Docking via `DockWindowAddEx`, removing the standalone window, full dock/undock/close/reopen lifecycle → Story 1.3.** 1.2 keeps a plain top-level window.
- **Graceful GL-context/init-failure handling with symmetric register-reversal on bail → Story 1.4.** 1.2 does a clean log-and-bail, no broken window, but the full NFR-R3/AR15 teardown-on-failure choreography is 1.4's.
- **Loading/skinning/displaying a real mesh → Epic 2/3.** 1.2 renders a built-in test scene only. The renderer interface is shaped so Epic 2 adds `SetAsset(...)` and swaps the test scene for the loaded `Asset` without restructuring.

### The two things this story must get right

1. **The frame timer must beat Reaper's UI loop.** Use `SetTimer(nullptr, 0, 15, &TimerProc)` (~66 Hz), dispatched by Reaper's message pump on the main thread. `rec->Register("timer", …)` rides the ~30 Hz UI cadence and will NOT reach 60 fps — the spike proved the timer source is what matters (it fed even ReaImGui at 66 Hz, but ReaImGui's *presentation* stayed capped; a direct-render window has no such cap). `SetTimer`'s ~15.6 ms floor yields ~64 fps, comfortably ≥60.
2. **Direct render, never an FBO.** Always render into framebuffer 0 (the window default FB). The whole point of the architecture change is removing the offscreen-FBO + readback path. If you find yourself calling `glGenFramebuffers`, you have drifted out of scope.

### Current state of the files being modified (read before editing)

- **[`src/viewer_window.cpp`](../../src/viewer_window.cpp)** (Phase 0, post-1.1-rename): a modeless top-level Win32 window (`WS_OVERLAPPEDWINDOW`, 800×600, parented to Reaper's main HWND) owning a WGL context (`PIXELFORMATDESCRIPTOR` 32-color/24-depth/8-stencil, `CS_OWNDC`). It renders **once** on `WM_PAINT` (`PaintFrame` → clear `0.10,0.10,0.12` + `SwapBuffers`). `WM_SIZE` just `InvalidateRect`s. `CloseViewerWindow` destroys the window and `UnregisterClassW`s the class (so a DLL reload doesn't inherit a stale `lpfnWndProc`). Namespace `rav`, window class `ReaAnimViewer.ViewerWindow`, title `ReaAnimViewer`. **What 1.2 changes:** `WM_PAINT`→validate-only; add GL-func load + `Renderer` + a `SetTimer` render loop + `KillTimer`/`Shutdown` teardown; `WM_SIZE` stores dimensions instead of invalidating. **What must be preserved:** the idempotent `OpenViewerWindow` (raise-if-open), the `CS_OWNDC` flag (required for stable WGL), the class unregister on close, the dark-grey clear color, parenting to `reaper_main`.
- **[`src/plugin_main.cpp`](../../src/plugin_main.cpp)**: entry point — registers `command_id`+`gaccel`+`hookcommand` (Action `RAV: Open Viewer`, token `RAV_OPEN_VIEWER`), symmetric deregister on the `rec==nullptr` unload path (which already calls `CloseViewerWindow()`). **1.2 should not need to touch this** unless you choose to surface a load-time GL-capability probe (defer that to 1.4). `caller_version == REAPER_PLUGIN_VERSION` (0x20E) guard stays. AR15 symmetric register/unregister stays intact.
- **[`src/reaper_api.h`](../../src/reaper_api.h)**: `REAPERAPI_MINIMAL` + `WANT_ShowConsoleMsg` only. 1.2 adds no WANTs (Win32 timer). **[`src/reaper_api.cpp`](../../src/reaper_api.cpp)**: the single `REAPERAPI_IMPLEMENT` TU — do not touch.
- **[CMakeLists.txt](../../CMakeLists.txt)** / **[cmake/ReaperPlugin.cmake](../../cmake/ReaperPlugin.cmake)**: target is `animviewer` → `reaper_animviewer.dll` (helper prepends `reaper_`; do not rename). MSVC `/W3 /permissive-`, C++17, `UNICODE`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN` already set. Windows branch links `opengl32 gdi32 user32`; non-Windows branch is a stub.

### Naming, namespace, console (live conventions — supersede the architecture doc's historical names)

- **Namespace `rav`**, console tag **`[RAV]`** (Story 1.1 superseded the architecture's `fbxav`/`[FBXAV]` for all code from Epic 1 onward). Console format: `[RAV] <level>: <message>` single-line, levels `info|warn|error` only (D7).
- Types PascalCase (`Renderer`, `GpuBuffer`), functions PascalCase (`RenderFrame`, `LoadGlFunctions`), locals/members snake_case, file-scope globals `g_` + snake_case, compile-time constants `k` + PascalCase, file-internal symbols in anonymous namespaces. SPDX `// SPDX-License-Identifier: MIT` + one-line purpose on every new file. Comment only non-obvious WHY (e.g. the timer-source rationale, the Y-up convention) — never narrate WHAT. ([architecture.md §Naming/Structural/Comment patterns](../planning-artifacts/architecture.md))

### Coordinate / orientation reference (so the scene looks right — AC1)

Render **as-authored**: column-major, **right-handed, Y-up, CCW front-facing** (D3). GLM is column-major right-handed by default — matches. Backface cull with CCW front (D12). The test scene must make "up" obvious so a wrong projection/winding is visible at a glance. (Epic 2 introduces `convertAssimpMatrix` for loaded files; 1.2 has no loaded geometry so no conversion boundary yet.)

### File structure after this story

New: `src/gl_loader.{h,cpp}`, `src/renderer.{h,cpp}`, `src/gpu_resources.h`, `src/console_log.{h,cpp}`, `extern/glm/` (or a FetchContent pin), `docs/PHASE0.5_VALIDATOR_GATE.md`.
Modified: `src/viewer_window.cpp` (+ maybe `.h`), `CMakeLists.txt`, `extern/VENDORED.md`, architecture Spec Change Log (one line).
Variances from the [architecture.md project tree](../planning-artifacts/architecture.md): (a) `gl_loader.{h,cpp}` is new — the tree assumed sokol provided GL access; raw-GL needs an explicit loader. (b) `gpu_resources.h` arrives at Phase 0.5 not Phase 1, and wraps `GLuint` not `sg_*_t`. (c) `extern/sokol/` is not added; `extern/glm/` is. (d) The window file keeps the name `viewer_window.{h,cpp}` rather than the doc's planned `viewer_panel.{h,cpp}` (it is a GL window, not a ReaImGui panel; 1.3 may revisit the name when it docks). All are direct consequences of the Spike-0 viewport change — note, don't agonize.

### Testing standards

- **No automated test harness** (consistent with the whole project — validation is by per-phase validator gate in real Reaper; [architecture.md §Test organization](../planning-artifacts/architecture.md)). 1.2's verification = (a) `cmake -B build` configures cleanly + GLM resolves on this box; (b) zero-warning MSVC build; (c) Antho's manual run on the reference workstation.
- NFR-P1 (≥60 fps) and NFR-P4 (<2 s load) are **measured by Antho on Windows** — the dev box cannot run MSVC/Reaper/GL.

### Antho's manual validation (seed `docs/PHASE0.5_VALIDATOR_GATE.md` with these rows)

Build (`cmake --build build --config Release`) → `reaper_animviewer.dll`, **zero `/W3 /permissive-` warnings** → copy to `%APPDATA%\REAPER\UserPlugins\` → restart Reaper. Then:
1. Console shows `[RAV] info: extension loaded …` and load is <2 s (NFR-P4).
2. Action list filtered `RAV` → run `RAV: Open Viewer` → the window opens with an **animated, correctly-oriented** scene (the rotating reference scene spins smoothly; up is up).
3. Console prints `[RAV] info: … fps` once per second at **≥60** (NFR-P1).
4. Drag the window edges/corner → the scene **resizes**, keeps aspect, re-fills the client area, **no flicker / no stretch / no crash**.
5. Close the window → Reaper stays stable; reopening via the Action works again (no stale class / no leaked context — the deeper dock/undock cycling is Story 1.3's gate row).

### References

- [Source: epics.md §Story 1.2: Docked OpenGL viewport (direct render at 60 fps)](../planning-artifacts/epics.md) — user story + the 3 AC clauses transcribed above; Epic 1 intro (revised per Spike 0).
- [Source: epics.md §FR27/FR28, AR10, AR18, AR16, AR15](../planning-artifacts/epics.md) — FR27 reinterpreted (docked GL viewport); AR10 superseded; AR18 main-thread GL; AR16 console-only; AR15 symmetric register.
- [Source: architecture.md §Spec Change Log 2026-06-23](../planning-artifacts/architecture.md) — D11/AR10 superseded → native docked GL window; D12 amended → direct-to-window FB, no FBO/MSAA-resolve/readback; sokol re-pin-or-drop (lean: raw GL); ReaImGui deferred to Epic 5.
- [Source: architecture.md §D12 render pipeline / D13 skinning / D14 camera](../planning-artifacts/architecture.md) — forward/opaque/depth-`GL_LESS`/backface-CCW; 50° FOV, near 0.01 far 1000 (camera math arrives fully in Epic 2, but the projection constants apply now).
- [Source: architecture.md §D2 GPU resource ownership + §Pattern Examples (RAII handle), §D7 console format, §Process Patterns (hot-path/no-alloc, build hygiene)](../planning-artifacts/architecture.md) — RAII move-only handles, `[RAV] <level>:` logging, no per-frame allocation, explicit CMake source list.
- [Source: docs/SPIKE0_FINDINGS.md](../../docs/SPIKE0_FINDINGS.md) — Finding 1 (docked GL window ~62 fps vs ReaImGui ~30 fps cap), Finding 4 (sokol `85d1f1b` new view API → raw GL), measured numbers.
- Spike reference implementation (read for technique, do **not** merge — branch `spike/0-1-feasibility`): `src/spike_gl.{h,cpp}` (modern-GL function table + `wglGetProcAddress` loader, legacy-context-suffices note), `src/spike_glwindow.{h,cpp}` (`WS_CHILD` GL window, `SwapBuffers`, `wglSwapIntervalEXT(0)`, `WM_SIZE`-stores-size, QPC fps), `src/spike_main.cpp` (`SetTimer(nullptr,0,15,&proc)` render-loop drive). Retrieve via `git show spike/0-1-feasibility:src/<file>`.
- [Source: 1-1-rename-fbxanimationviewer-to-reaanimviewer.md](1-1-rename-fbxanimationviewer-to-reaanimviewer.md) — namespace `rav` / tag `[RAV]` adopted (supersedes doc `fbxav`/`[FBXAV]`); the `cmake -B build` drvfs `configure_file` sandbox caveat; the Windows-validator handoff pattern this story repeats.
- [Source: architecture.md AR6 / extern/VENDORED.md](../../extern/VENDORED.md) — GLM 1.0.3 vendoring discipline (no submodules; drvfs rationale); GLM absent on `main` → this story adds it.

## Previous Story Intelligence (Story 1.1 + Spike 0)

- **From 1.1:** the rename is done — code is `rav` / `[RAV]` / `RAV: Open Viewer` / `reaper_animviewer.dll`. Build it on `main` after 1.1. The dev environment is **Linux/WSL with no MSVC/Reaper/GL** → all runtime ACs are handed to Antho's Windows validator gate; the dev verifies `cmake` configure + zero-warning intent only. In-tree `cmake -B build` may hit a drvfs `configure_file` "Operation not permitted" error — configure to an out-of-sandbox tmp dir instead (1.1 did this successfully).
- **From Spike 0:** the docked-GL-window approach is proven at ~62 fps; the readback/FBO path is dead; `SetTimer`-driven render on the main thread is the mechanism; legacy `wglCreateContext` granted a high-enough compatibility profile for `#version 330` (if shader compile fails on the reference workstation, the fix is a `wglCreateContextAttribsARB` 3.3-core context — flag it, don't pre-build it). The spike rendered with raw GL because sokol's pin diverged — 1.2 makes that the production choice.

## Git Intelligence

Recent `main`/branch commits confirm the sequencing: `1809be7 feat(epic-1): rename … (story 1.1)` (the immediate predecessor — pure rename, no behavior change), preceded by `51f22cb docs(planning): correct-course — fold Spike 0 findings into architecture + Epic 1` (the source of the D11/AR10 supersession this story implements). No production rendering code has landed on `main` yet — 1.2 is the first. The throwaway `spike/0-1-feasibility` branch carries the reference implementation but is intentionally unmerged.

## Latest Technical Information

- **GLM 1.0.3** — current stable; header-only; column-major right-handed by default (matches D3). Use `glm/gtc/matrix_transform.hpp` for `perspective`/`lookAt`/`rotate`. No API surprises vs the spike's usage.
- **Modern GL on Windows** — `opengl32.dll` still exports only GL 1.1; everything ≥1.5 (VAO/VBO/shaders) is resolved at runtime via `wglGetProcAddress` against a *current* context (the loader pattern from `spike_gl.cpp`). `wglSwapIntervalEXT` (vsync) is itself a `wglGetProcAddress` extension entry — resolve it defensively (may be null on exotic drivers; absence just means you cannot toggle vsync, not a failure).
- **Reaper SDK** — pinned `31234f3…`, Reaper 7.72+, `caller_version 0x20E` (unchanged from Phase 0).

## Project Context Reference

No `project-context.md` exists in this repo (the activation glob found none); conventions live in `architecture.md` (decisions D1–D18 + Implementation Patterns) and are summarised inline above. Binding cross-cutting invariants for this story: **AR18** (UI + GL on Reaper's main thread only), **AR16** (console-only diagnostics, `[RAV]`), **AR15** (symmetric register/unregister — touched lightly here; full failure-path symmetry is 1.4), **NFR-R3** (no leaked GL context / window class / timer), **NFR-R5** (`/W3 /permissive-` warning-free), **NFR-P1** (≥60 fps), **NFR-P4** (<2 s load), **D2/AR8** (RAII GPU handles), **AR20** (deviations get a Spec Change Log line — the raw-GL confirmation).

## Story Completion Status

Ultimate context engine analysis completed — comprehensive developer guide created. Status: **ready-for-dev**.

## Open Questions (for Antho — non-blocking; defaults chosen)

1. **GLM delivery: vendor vs FetchContent.** Default taken: **vendor `extern/glm/`** per AR6's no-submodule discipline; FetchContent (pinned `1.0.3`) is the sanctioned fallback if vendoring the full header tree is awkward on the drvfs mount. Either is fine — purely internal, no user-facing effect. (Recommend FetchContent if the vendored tree bloats the repo; vendor if you want offline-clean clones per the VENDORED.md ethos.)
2. **vsync policy.** Default: measure with vsync **off** to confirm ≥60 fps headroom, then **ship with vsync on** (tear-free clean 60). Flag if you'd rather keep it off for a visible fps-headroom readout in the console. No user-facing difference beyond tearing.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Claude Opus 4.8, 1M context) — BMad dev-story workflow.

### Debug Log References

- `cmake -S . -B /tmp/rav_build` → configures clean (Linux stub branch; proves CMake syntax + source-list wiring). The `if(WIN32)` GLM/FetchContent branch is not exercised on Linux.
- Standalone FetchContent probe (`/tmp/glmcheck`) → GLM **1.0.3** clones and resolves on the drvfs/WSL mount (`GLM: Version 1.0.3`); de-risks Antho's first Windows configure (network needed once).
- GLM API compile-test (`g++ -std=c++17 -Wall -Wextra` against the fetched tree) of the renderer's matrix calls (`perspective`/`lookAt`/`rotate`/`value_ptr`/`mat3`/`offsetof`) → **compiles clean**.
- `grep` of new sources for newly-called Reaper symbols → only `ShowConsoleMsg` (already `WANT`ed); the lone `Register` hit is a code comment, not a call. **No new `WANT_*` added** (confirms Task 8 / reaper_api.h unchanged).

### Completion Notes List

**Acceptance Criteria — validated (dev impl + Windows validator gate passed on the reference workstation, 2026-06-23):**
- ✅ **AC1 (direct render, correct orientation)** — `Renderer::RenderFrame` draws into framebuffer 0 only (no `glGenFramebuffers` anywhere; no ImGui bridge, no readback); WGL context created on Reaper's main thread; Y-up depth-tested cube-above-ground scene. Gate: correctly-oriented scene renders.
- ✅ **AC2 (≥60 fps, independent timer)** — `SetTimer(nullptr,0,15,&FrameTimerProc)` ~66 Hz dispatched by Reaper's main pump (not `rec->Register("timer")`); QPC present-rate fps logged `[RAV] info: … fps` once/sec. Gate: ≥60 fps observed in console.
- ✅ **AC3 (resize without leaks)** — `WM_SIZE` stores clamped client w/h; projection recomputed each frame from live size; scene GL objects owned by move-only RAII handles; no context/buffer recreation on resize. Gate: resize re-fills, keeps aspect, no flicker/garbage/crash.
- ✅ **AC4 (clean build/load + no regression + clean teardown)** — explicit CMake source list, GLM as SYSTEM include (zero-warning intent at `/W3 /permissive-`); `RAV: Open Viewer` Action unchanged (no 1.1 regression); symmetric `StopRendering` (KillTimer → make-current → `Renderer::Shutdown` → `DestroyGLContext`) + window-class unregister on close. Gate: build zero-warning, load < 2 s, Action opens window, close leaves Reaper stable.

All four ACs satisfied. `code-review` run 2026-06-23 (high-effort, recall-biased) → 10 findings, **all corrected** (see Code Review Record below); build re-confirmed compiling. Story → **done**.

**Implemented (dev-complete on `main`):**
- ✅ Raw-GL decision recorded — architecture Spec Change Log got the terse Phase-0.5 confirmation line; `extern/sokol/` not added (Task 1).
- ✅ `src/gl_loader.{h,cpp}` — `rav` namespace, X-macro modern-GL table (shader/buffer/VAO only; FBO entry points dropped — direct render), `bool LoadGlFunctions(std::string&)` reporting the first missing symbol; missing GL enums/typedefs defined for `<gl/GL.h>` (Task 2).
- ✅ `src/gpu_resources.h` — header-only move-only RAII wrappers `GpuBuffer`/`GpuVertexArray`/`GpuProgram` over `GLuint` (`= delete` copy, `noexcept` move-steal-and-zero, `glDelete*` only if handle ≠ 0) (Task 3).
- ✅ `src/console_log.{h,cpp}` — `LogInfo/LogWarn/LogError` funnelling through `ShowConsoleMsg` in the D7 `[RAV] <level>: <message>` single-line format (Task 4).
- ✅ `src/renderer.{h,cpp}` — direct-to-framebuffer-0 renderer (never binds an FBO). Test scene: a lit **rotating cube above a grey ground**, distinct per-face colours, **+Y face bright green** so Y-up is unambiguous; `GL_DEPTH_TEST`(`GL_LESS`) + back-face cull (`GL_CULL_FACE`, `GL_CCW` front). GLM matrices, 50° FOV / near 0.01 / far 1000 (D14); the view-projection is rebuilt only when the aspect changes (resize keeps aspect, AC3 — see post-review note). No allocation in `RenderFrame` (buffers built once in `Init`; per-frame matrices are stack locals) (Tasks 5–6).
- ✅ GLM 1.0.3 via FetchContent pin (sanctioned fallback — full-tree vendoring impractical on drvfs), added as a **SYSTEM** include so its headers can't trip `/W3 /permissive-`; recorded in `extern/VENDORED.md` (Task 6).
- ✅ `src/viewer_window.cpp` evolved — GL-func load + `Renderer::Init` after `wglMakeCurrent` (clean **log-and-bail**, no broken window, on failure); **`SetTimer(nullptr,0,15,&FrameTimerProc)`** ~66 Hz render loop on the main thread (AR18), `KillTimer` on close/destroy; `WM_PAINT`→validate-only; `WM_ERASEBKGND`→1; `WM_SIZE` stores clamped client size (no context/buffer recreation); **vsync OFF** (interval 0) so the ≥60 fps headroom is measurable; QPC present-rate fps logged once/sec; symmetric `StopRendering` (KillTimer → make-current → `Renderer::Shutdown` → `DestroyGLContext`) on `WM_DESTROY`/`CloseViewerWindow`. `viewer_window.h` signatures unchanged (Task 7).
- ✅ CMake — added `gl_loader.cpp`/`renderer.cpp`/`console_log.cpp` to the explicit SOURCES list (no GLOB), GLM SYSTEM include, link line unchanged; `reaper_api.h` untouched (Task 8).
- ✅ `docs/PHASE0.5_VALIDATOR_GATE.md` seeded with Story 1.2's rows (grows in 1.3/1.4) (Task 9).

**Decisions taken (story delegated these to the dev):**
- **GLM delivery → FetchContent pin `1.0.3`** (not in-tree vendoring): the full GLM header tree is impractical to hand-vendor on the drvfs mount; FetchContent is the story-sanctioned fallback and the spike's proven approach.
- **vsync → OFF (interval 0)** for this proving story: makes the ≥60 fps headroom observable in the console (AC2 wants the rate observed, not assumed), matching the spike's ~62 fps measurement. Story 1.3 can flip to interval 1 for tear-free docked presentation.

**⚠️ Windows validation handed to Antho (the dev box has no MSVC/Reaper/GL — same constraint as Story 1.1).** All runtime ACs — zero `/W3 /permissive-` warnings, the Action opening the window, the animated correctly-oriented scene, console `[RAV] info: … fps` at **≥60**, clean resize, clean close, load < 2 s — are verified on the reference workstation via **`docs/PHASE0.5_VALIDATOR_GATE.md`**. Dev-side verification covered CMake configure + GLM resolution + GLM-API compile + zero-warning intent only. If shaders fail to compile/link on the reference workstation, the fix is a `wglCreateContextAttribsARB` 3.3-core context (flagged in the gate, not pre-built).

### Code Review Record (2026-06-23)

`/code-review 1-2` — high-effort, recall-biased (7 finder angles × verify pass). 10 findings surfaced; the one false positive (a "DC leak" on the WM_CREATE-failure path) was **REFUTED** at verify (`CS_OWNDC` makes `ReleaseDC` a no-op; the private DC is freed with the window). All 10 actionable findings were corrected on `main`:

**Correctness:**
- **First-frame flash** — `StartRendering` now paints one frame (`RenderTick()`) before returning, so the window shows real content the instant `ShowWindow` reveals it instead of ~1 timer-tick (~15 ms) of undefined GL framebuffer. (`WM_PAINT` stayed validate-only; the fix is the eager first paint.)
- **`wglMakeCurrent` unchecked + redundant** — removed the per-frame `wglMakeCurrent` from `RenderTick`; the context is made current once (and checked) in `StartRendering` and stays current on the single render thread. Fixes both the ignored-return correctness risk and the per-frame driver-call cost.
- **Normal matrix** — vertex shader now uses a dedicated `uniform mat3 u_normal = transpose(inverse(mat3(model)))` (computed CPU-side per draw) instead of `mat3(u_model)`, which is correct under non-uniform scale/skinning (forward-proofs Epic 2). Added `glUniformMatrix3fv` to the GL loader table; dropped the now-unused `u_model` uniform. `Init` now fails with a diagnostic if `u_mvp`/`u_normal` don't resolve.
- **Modal-loop render pause** — documented as a KNOWN LIMITATION in the timer comment (pump-driven render pauses during host menu/modal sub-loops; inherent to a single-thread loop, **not** the NULL-hwnd timer choice — a window-bound timer stalls identically; a dedicated render thread is the only real fix, out of MVP scope). No code change.

**Efficiency / altitude:**
- **Fixed GL state** (`GL_DEPTH_TEST`/`GL_LESS`/`GL_CULL_FACE`/`GL_BACK`/`GL_CCW`) moved from per-frame `RenderFrame` to one-time `Init` — no per-frame state churn; future passes that toggle state won't be silently stomped each frame.
- **Camera matrices cached** — `view` is a constant computed once in `Init`; `view_proj` is rebuilt only when the aspect ratio actually changes (resize), not every frame (was a full `perspective`+`lookAt`+multiply at ~66 Hz). AC3 still satisfied.
- **`DrawRange` abstraction** — the cube's hardcoded byte-offset arithmetic in `RenderFrame` replaced by `{count, offset}` ranges precomputed once in `Init` (Epic-2-ready seam for a per-asset range list).
- **RAII consolidation** — `GpuBuffer`/`GpuVertexArray`/`GpuProgram` (three byte-identical wrappers) collapsed to one `GpuHandle<Deleter>` template + three delete policies in `gpu_resources.h`. Ownership contract now lives in one place.
- **`g_renderer_ready` removed** — redundant second readiness flag; `RenderTick` guards on `g_hglrc` (the timer only runs once everything is up). One source of truth.
- **Log channel unified** — `plugin_main.cpp`'s load message switched from a hand-formatted `ShowConsoleMsg` to `LogInfo`, so every console line shares the D7 `[RAV] <level>:` format.

Dev-side re-verification: GLM matrix expressions (`lookAt`/`perspective`/`transpose(inverse(mat3))`/`value_ptr`) and the consolidated RAII template each compile + run clean under `g++ -std=c++17 -Wall -Wextra`; reference sweep confirms no dangling `g_renderer_ready`/`u_model`/`*_index_count_`/direct-`ShowConsoleMsg` symbols. **Windows MSVC build re-confirmed compiling by Antho.** The behavioural fixes (eager first paint, etc.) are low-risk improvements over the already-gate-passed build; a quick re-run of the Phase 0.5 gate on the reference workstation is recommended but not blocking.

### File List

**New:**
- `src/gl_loader.h`
- `src/gl_loader.cpp`
- `src/gpu_resources.h`
- `src/console_log.h`
- `src/console_log.cpp`
- `src/renderer.h`
- `src/renderer.cpp`
- `docs/PHASE0.5_VALIDATOR_GATE.md`

**Modified:**
- `src/viewer_window.cpp`
- `src/plugin_main.cpp` (code-review: load message routed through `LogInfo`)
- `CMakeLists.txt`
- `extern/VENDORED.md`
- `_bmad-output/planning-artifacts/architecture.md` (Spec Change Log — one raw-GL confirmation entry)
- `_bmad-output/implementation-artifacts/sprint-status.yaml` (status → review → done)

## Change Log

| Date | Change |
|---|---|
| 2026-06-23 | Story 1.2 drafted (ready-for-dev). Implements the Spike-0 viewport supersession on `main`: native GL window, direct render into the window FB, `SetTimer`-driven ~66 Hz render loop (independent of Reaper's ~30 Hz UI loop), `SwapBuffers`, clean resize, RAII GL handles. Introduces `renderer`, `gl_loader`, `gpu_resources`, `console_log`, and GLM; adopts raw GL over sokol (Finding 4). Docking → 1.3; init-failure hardening → 1.4; mesh loading → Epic 2. |
| 2026-06-23 | Story 1.2 implemented (→ review). New `gl_loader`, `gpu_resources`, `console_log`, `renderer`; `viewer_window.cpp` rebuilt around a ~66 Hz `SetTimer` direct-render loop with QPC fps + symmetric teardown; GLM 1.0.3 wired via FetchContent (SYSTEM include); raw-GL line added to the architecture Spec Change Log; `PHASE0.5_VALIDATOR_GATE.md` seeded. Dev-verified: CMake configures, GLM resolves + its API compiles clean. **Windows MSVC build + runtime ACs (≥60 fps, resize, teardown, <2 s load) handed to Antho's Phase 0.5 validator gate.** |
| 2026-06-23 | `code-review` (high-effort, recall-biased) → 10 findings, all corrected (→ **done**). Correctness: eager first-frame paint (no open flash), per-frame `wglMakeCurrent` removed, proper `u_normal` normal matrix (+ `glUniformMatrix3fv` in loader), modal-loop pause documented as a known limitation. Cleanup/altitude: fixed GL state moved to `Init`, camera matrices cached (rebuild only on aspect change), `DrawRange` ranges precomputed, three RAII wrappers consolidated to one `GpuHandle<Deleter>` template, redundant `g_renderer_ready` removed, `plugin_main` load message unified onto `LogInfo`. One "DC leak" finding refuted (`CS_OWNDC`). Build re-confirmed compiling on Windows. See Code Review Record. |
