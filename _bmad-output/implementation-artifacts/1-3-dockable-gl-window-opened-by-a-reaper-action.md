# Story 1.3: Dockable GL window opened by a Reaper Action

Status: done
<!-- 2026-06-24: validated in-game on Windows by Antho (docks via the Action, ≥60 fps docked, drag/float across dockers, close/reopen, dock persistence) AND 3-layer adversarial code review clean (see Review Findings). The one low-sev patch applied to viewer_window.cpp touches only DestroyGLContext's init-failure bail path — a cold path the happy-path validation never exercises, so it cannot regress what was tested. -->

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want the viewer to appear as a panel I can **dock anywhere in Reaper** (top, bottom, left, right, or floating), opened from the `RAV: Open Viewer` Action,
so that the preview lives where I already work, with no separate floating window to manage.

This is the **third story of Epic 1 (Phase 0.5)**, on `main`, immediately after Story 1.2 stood up the direct-render-at-60-fps GL engine in a **standalone top-level window**. Story 1.3's one job is to take that proven engine and **hand its window to Reaper's native docker via `DockWindowAddEx`**, replacing the top-level `WS_OVERLAPPEDWINDOW` with a dockable `WS_CHILD` window — without losing the ≥60 fps, the clean resize, or the leak-free teardown 1.2 established. It does **not** harden the init-failure/teardown-on-bail choreography (that is Story 1.4) and does **not** load meshes (that is Epic 2).

## Acceptance Criteria

These transcribe Story 1.3's BDD from [epics.md §Story 1.3](../planning-artifacts/epics.md#story-13-dockable-gl-window-opened-by-a-reaper-action) and bind each clause to its verifying mechanism.

1. **(AC1 — docks via the Action into any docker)** Given ReaAnimViewer is loaded, when I trigger the `RAV: Open Viewer` Action (FR29), then the OpenGL viewport is handed to Reaper's docker via **`DockWindowAddEx`** and appears as a panel that can sit in **any** Reaper docker — top, bottom, left, right, or floating (FR28) — and the user can drag it between dockers using Reaper's native docking gestures. FR27 is reinterpreted: a **docked GL viewport**, not a ReaImGui panel.
2. **(AC2 — the standalone window is gone)** And the Phase 0 / Story-1.2 **standalone top-level `WS_OVERLAPPEDWINDOW` window is removed entirely** — the viewport now exists only as a docked child window. No separate floating top-level window is created.
3. **(AC3 — leak-free dock lifecycle on the main thread)** And the per-frame render still runs on **Reaper's main thread** (AR18, the ~66 Hz `SetTimer` loop from 1.2 is unchanged), and the window **docks / undocks / closes / reopens — including via the docker's own close button** — **without leaking GL contexts, window classes, or timers** (NFR-R3). Closing via the docker X and reopening via the Action both work repeatedly.
4. **(AC4 — engine quality preserved; build + load stay clean; no regression)** And the docked viewport keeps everything 1.2 proved — the **animated, correctly-oriented scene at ≥60 fps** (NFR-P1) and **clean resize** (now driven by the docker resizing the child) — the build stays **zero-warning at MSVC `/W3 /permissive-`** (NFR-R5), the extension still **loads in under 2 s** (NFR-P4), and the `RAV: Open Viewer` Action / load path is otherwise unchanged (no regression to 1.1/1.2).

## Tasks / Subtasks

- [x] **Task 1 — Want the docking API surface (AC: 1, 3)**
  - [x] In [src/reaper_api.h](../../src/reaper_api.h), add three `REAPERAPI_WANT_*` entries next to the existing `WANT_ShowConsoleMsg`: `REAPERAPI_WANT_DockWindowAddEx`, `REAPERAPI_WANT_DockWindowActivate`, `REAPERAPI_WANT_DockWindowRemove`. These are the exact symbols the spike proved (`spike/0-1-feasibility:src/reaper_api.h`). SDK signatures ([extern/reaper-sdk/sdk/reaper_plugin_functions.h](../../extern/reaper-sdk/sdk/reaper_plugin_functions.h)):
    - `void DockWindowAddEx(HWND hwnd, const char* name, const char* identstr, bool allowShow)`
    - `void DockWindowActivate(HWND hwnd)`
    - `void DockWindowRemove(HWND hwnd)`
  - [x] These resolve through the existing `REAPERAPI_LoadAPI(rec->GetFunc)` call in `plugin_main.cpp` — **no new registration**, just more wanted symbols. Do **not** touch `reaper_api.cpp` (the single `REAPERAPI_IMPLEMENT` TU). Confirm the load path already bails cleanly if any wanted symbol fails to resolve (it returns 0 — Story 1.4 enriches this, here the existing behavior is enough).
- [x] **Task 2 — Convert the viewer window from top-level to a dockable child (AC: 1, 2)**
  - [x] In [src/viewer_window.cpp](../../src/viewer_window.cpp), change the `CreateWindowExW` style from **`WS_OVERLAPPEDWINDOW`** to **`WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN`** (the spike-proven style for a docked GL child — see `spike_glwindow.cpp`). Keep parent = `reaper_main`, keep the `CS_OWNDC` class flag (required for stable WGL), keep the initial size.
  - [x] **Remove the self-show**: a docked child must not be shown by us. Delete the `ShowWindow(g_hwnd, SW_SHOW)` + `UpdateWindow(g_hwnd)` calls in `OpenViewerWindow` — Reaper reveals the window when we dock+activate it (Task 3). (The window may be created without `WS_VISIBLE`; `DockWindowActivate` makes it visible inside the docker.)
  - [x] Keep the window class name/title constants; the title string becomes the docker **tab caption** indirectly (the docker uses the `name` passed to `DockWindowAddEx`, Task 3 — the `WS_CHILD` title is not shown as a top-level caption).
- [x] **Task 3 — Dock + activate on open; remove on explicit close (AC: 1, 3)**
  - [x] After the child window is created (and `WM_CREATE`/`StartRendering` succeeded), call **`DockWindowAddEx(g_hwnd, <name>, <identstr>, true)`** then **`DockWindowActivate(g_hwnd)`** (the spike's exact sequence — `spike_main.cpp::StartGlWindow`). `<name>` = the docker tab caption (e.g. `"ReaAnimViewer"`); `<identstr>` = a **stable** dock-persistence key (e.g. `"ReaAnimViewer.Viewer"`) — Reaper remembers the dock position per identstr across sessions, so it must not change between builds.
  - [x] **Idempotent reopen**: in `OpenViewerWindow`, when `g_hwnd` already exists and `IsWindow(g_hwnd)`, replace the old top-level `SetForegroundWindow`/`SW_RESTORE` raise with **`DockWindowActivate(g_hwnd)`** (brings the docked panel's tab forward). Return without recreating.
  - [x] **Explicit close path** (`CloseViewerWindow`): call **`DockWindowRemove(g_hwnd)`** *before* `DestroyWindow(g_hwnd)` when the window is still live, so Reaper drops it from the docker cleanly. Keep the existing `UnregisterClassW` on close. (Note the asymmetry handled in Task 4: the docker-close path does **not** call `DockWindowRemove` — Reaper has already removed it.)
- [x] **Task 4 — Make the dock/undock/close/reopen lifecycle leak-free (AC: 3)**
  - [x] **Docker close button (X):** when the user closes the panel from the docker, Reaper destroys our child → `WM_DESTROY` fires. The existing `WM_DESTROY → StopRendering()` (KillTimer → make-current → `Renderer::Shutdown` → `DestroyGLContext`) from 1.2 already tears the render loop + GL down on the main thread; keep it, and keep nulling `g_hwnd` there. Do **not** call `DockWindowRemove` from `WM_DESTROY` (Reaper is already removing it — calling it during its own teardown is redundant and risks reentrancy). The window **class** is intentionally left registered after a docker-close (it is unregistered on the explicit `CloseViewerWindow` path and on unload — see below); reopening via the Action with the class still registered is correct and cheap.
  - [x] **Unload (`rec == nullptr`):** `plugin_main.cpp` already calls `CloseViewerWindow()` on unload. Verify the full chain leaves nothing behind: if the panel is open → `DockWindowRemove` + `DestroyWindow` (→ `WM_DESTROY` → `StopRendering`) + `UnregisterClassW`; if it was already closed via the docker → `g_hwnd` is null, `CloseViewerWindow` calls `StopRendering()` (idempotent — timer/context already down) then `UnregisterClassW`. Either way: no leaked GL context, no orphan timer, no dangling window class (NFR-R3, AR15).
  - [x] **Undock/redock (drag between dockers):** Reaper reparents the same `HWND` — no destroy/recreate. `CS_OWNDC` keeps the WGL DC valid across reparenting; the timer + context are untouched; `WM_SIZE` from the new docker just updates `g_client_w/h`. No code needed beyond confirming `WM_SIZE` still stores size (it does, from 1.2). Add a one-line WHY comment only if the reparent-survives-context invariant is non-obvious.
- [x] **Task 5 — Vsync / fps observability decision for the docked panel (AC: 4)**
  - [x] Story 1.2 shipped **vsync OFF (interval 0)** so the ≥60 fps headroom is observable in the console. **Keep vsync OFF through Epic 1** so the validator can re-confirm NFR-P1 *when docked* (docking changes window size/compositing vs. the 1.2 top-level window; the spike's ~62 fps was measured docked at 1055×604, so this is the apples-to-apples check). Record the choice; flipping to interval 1 for tear-free presentation is a later-polish item, not Epic 1's job. (If the dev finds docked tearing objectionable enough to flip now, that is an allowed delegated call — note it and keep an observable fps line either way.)
- [x] **Task 6 — Wire-up sanity + no new Reaper symbols beyond docking (AC: 4)**
  - [x] No `CMakeLists.txt` change expected (no new translation units — all edits land in `viewer_window.cpp` + `reaper_api.h`). Confirm by inspection.
  - [x] `grep` the changed sources for newly-called Reaper symbols: the only additions should be `DockWindowAddEx` / `DockWindowActivate` / `DockWindowRemove` (all now wanted). No other Reaper API enters in 1.3.
  - [x] On this Linux/WSL box, run `cmake -S . -B /tmp/rav_build` (out-of-sandbox tmp dir — the in-tree `build/` hits the drvfs `configure_file` restriction noted since Story 1.1) to confirm the target still configures. The non-Windows branch stubs the target, so this proves **CMake syntax only**, not the MSVC build or the docking runtime.
- [x] **Task 7 — Extend the Phase 0.5 validator gate; hand off the Windows run (AC: 1, 2, 3, 4)**
  - [x] Append a **Story 1.3** section to [docs/PHASE0.5_VALIDATOR_GATE.md](../../docs/PHASE0.5_VALIDATOR_GATE.md) (it already announces "Story 1.3 (docking) … append their own"). Add rows covering: Action docks the panel; drag it into each docker edge + float; ≥60 fps still holds **while docked**; resize via the docker handle stays clean; close via the **docker X** leaves Reaper stable; reopen via the Action re-docks; dock position **persists across a Reaper restart** (the identstr round-trip); Reaper exits with no crash/orphan. Keep it short.
  - [x] **Hand the Windows MSVC build + Reaper run to Antho** (the dev box has no MSVC/Reaper/GL — same constraint as Stories 1.1/1.2): zero `/W3` warnings, the Action docks the panel, it can be dragged into any docker and floated, the scene animates at ≥60 fps docked, resize is clean, closing via the docker X and reopening via the Action both work without leaks, dock position survives a Reaper restart, load < 2 s. Note this handoff explicitly in Completion Notes.

## Dev Notes

### Why this story exists (and what it is NOT)

Story 1.2 proved the **direct-render-at-60-fps engine** but kept the window a plain top-level `WS_OVERLAPPEDWINDOW` (docking was explicitly fenced out, [1-2 §Scope fences](1-2-docked-opengl-viewport-direct-render.md)). Story 1.3 makes the viewport **live inside Reaper as a dockable panel** — the user-facing point of Epic 1 ("the viewer lives inside Reaper"). The mechanism is **not** ReaImGui (Spike 0 proved a ReaImGui panel caps at ~30 fps and cannot adopt a GPU texture — [SPIKE0_FINDINGS Finding 1](../../docs/SPIKE0_FINDINGS.md)); it is Reaper's **native docker**, which happily hosts a raw `WS_CHILD` GL window and lets us keep driving our own ~66 Hz present loop. This is exactly what the spike's `spike_glwindow` + `StartGlWindow` demonstrated at ~62 fps docked.

Scope fences (what belongs to neighbouring stories — do not pull them in):
- **Graceful GL-context/init-failure handling with full symmetric register-reversal on bail → Story 1.4.** 1.3 keeps 1.2's clean log-and-bail; it must not regress it, but the full NFR-R3/AR15 teardown-on-failure choreography (and any load-time GL-capability probe) is 1.4's.
- **Loading/skinning/displaying a real mesh → Epic 2/3.** 1.3 still renders the 1.2 built-in test scene (rotating cube above ground). Docking must not change what is rendered.
- **ReaImGui anything → Epic 5.** Epic 1 has **no ReaImGui dependency**; do not add `extern/reaimgui/`, `imgui_api.*`, or any `ImGui_*` call.

### The docking mechanism (spike-proven — copy the technique, not the throwaway code)

The reference is `spike/0-1-feasibility:src/spike_glwindow.cpp` (the child GL window) and `:src/spike_main.cpp::StartGlWindow` / `StopAll` (the dock/undock lifecycle). **Read for technique; do not merge the spike branch.** The essential sequence:

```cpp
// 1. Create the GL window as a CHILD of Reaper's main HWND (not a top-level window):
hwnd = CreateWindowExW(0, kClass, title,
                       WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                       0, 0, w, h, reaper_main, nullptr, hinst, nullptr);
// 2. Bring up the WGL context + GL loader + Renderer + ~66 Hz SetTimer  (all unchanged from 1.2)
// 3. Hand the window to Reaper's docker and reveal it:
DockWindowAddEx(hwnd, "ReaAnimViewer", "ReaAnimViewer.Viewer", true);
DockWindowActivate(hwnd);
// ... teardown (explicit close / unload):
DockWindowRemove(hwnd);   // only on the path WE initiate; NOT from WM_DESTROY
DestroyWindow(hwnd);      // → WM_DESTROY → StopRendering() (KillTimer + GL teardown)
```

Key behavioural facts learned from the spike:
- The docker **reparents** our child into its own host window; we keep ownership of the `HWND`, the WGL context, and the present timer. `CS_OWNDC` keeps the device context valid through reparenting.
- When the user closes the panel from the **docker's tab close button**, Reaper destroys our child → our `WndProc` gets `WM_DESTROY`. The spike just nulled its `HWND` there and let the render guard skip; production must also **fully stop the render loop + GL** in `WM_DESTROY` (1.2 already does this via `StopRendering()`), so a docker-close leaves nothing leaking.
- `DockWindowRemove` is only called on the path **we** initiate (explicit `CloseViewerWindow`, unload). Calling it from inside `WM_DESTROY` when Reaper is already destroying the window is redundant/reentrant — don't.

### Current state of the files being modified (read before editing)

- **[`src/viewer_window.cpp`](../../src/viewer_window.cpp)** (post-1.2): owns the full engine — `CreateGLContextFor` (WGL ctx, `PIXELFORMATDESCRIPTOR` 32/24/8, `CS_OWNDC`), `LoadGlFunctions` + `Renderer::Init`, the **`SetTimer(nullptr,0,15,&FrameTimerProc)` ~66 Hz** render loop, QPC present-rate fps via `LogInfo`, `WM_SIZE`-stores-size, `WM_PAINT`-validate-only, `WM_ERASEBKGND→1`, and **`StartRendering`/`StopRendering`** (symmetric: KillTimer → make-current → `Renderer::Shutdown` → `DestroyGLContext`). `OpenViewerWindow` currently creates a **`WS_OVERLAPPEDWINDOW` top-level** window parented to `reaper_main`, then `ShowWindow`/`UpdateWindow`s it, and raises an already-open window via `SW_RESTORE`/`SetForegroundWindow`. `CloseViewerWindow` `DestroyWindow`s then `UnregisterClassW`s. **What 1.3 changes:** window style → `WS_CHILD`; drop self-`ShowWindow`; add `DockWindowAddEx`+`DockWindowActivate` after create; idempotent-raise → `DockWindowActivate`; `CloseViewerWindow` → `DockWindowRemove` before `DestroyWindow`. **What must be preserved (do not touch):** the entire `StartRendering`/`StopRendering`/`RenderTick`/timer/fps machinery, `CS_OWNDC`, the eager first-frame `RenderTick()` paint, `WM_DESTROY → StopRendering()`, the class-unregister-on-close, vsync-off policy (Task 5), parenting to `reaper_main`.
- **[`src/reaper_api.h`](../../src/reaper_api.h)**: `REAPERAPI_MINIMAL` + `WANT_ShowConsoleMsg` only. 1.3 adds the three `WANT_DockWindow*` entries. **[`src/reaper_api.cpp`](../../src/reaper_api.cpp)**: the single `REAPERAPI_IMPLEMENT` TU — do not touch.
- **[`src/plugin_main.cpp`](../../src/plugin_main.cpp)**: registers `command_id`+`gaccel`+`hookcommand` (`RAV: Open Viewer`), symmetric deregister on the `rec==nullptr` path (which already calls `CloseViewerWindow()`); `REAPERAPI_LoadAPI(rec->GetFunc)` resolves all wanted symbols (the new `DockWindow*` ones flow through automatically). **1.3 should not need to touch this** — the docking calls live in `viewer_window.cpp`. (Optionally update the `"extension loaded (Phase 0)"` log text if it bothers you — cosmetic, not required.)
- **[`CMakeLists.txt`](../../CMakeLists.txt)**: target `animviewer` → `reaper_animviewer.dll`, links `opengl32 gdi32 user32`. **No change** (no new TUs).

### Naming / structure decision: keep `viewer_window.{h,cpp}` (don't rename to `viewer_panel`)

The [architecture project tree](../planning-artifacts/architecture.md) names this file `viewer_panel.{h,cpp}` ([architecture.md L776-777](../planning-artifacts/architecture.md), L1048) — but that name **presumed a ReaImGui panel** that Spike 0 deleted. Story 1.2 deliberately kept `viewer_window.{h,cpp}` and noted 1.3 "may revisit the name when it docks." **Decision: do not rename.** It is still a GL *window* (a `WS_CHILD` HWND we own), merely hosted by the docker; renaming churns includes/CMake for no behavioural gain and the architecture's `viewer_panel` name is now a historical artifact (note this as a deliberate variance, AR20 — no Spec Change Log line needed for a name the doc already flags as superseded by the GL-window change).

### Naming, namespace, console (live conventions)

Namespace **`rav`**, console tag **`[RAV]`**, format `[RAV] <level>: <message>` single-line (D7). Types/functions PascalCase, locals/members snake_case, file-scope globals `g_` + snake_case, constants `k` + PascalCase, file-internal symbols in anonymous namespaces. SPDX header already present on `viewer_window.cpp` — keep it. Comment only non-obvious WHY (the `WS_CHILD`+docker rationale, the "no `DockWindowRemove` from `WM_DESTROY`" asymmetry) — never narrate WHAT. ([architecture.md §Naming/Structural/Comment patterns](../planning-artifacts/architecture.md))

### Threading / lifecycle invariants this story must honor

- **AR18 — main-thread only.** `DockWindowAddEx`/`Activate`/`Remove` and all GL calls run on Reaper's main thread. The Action's `OnHookCommand` and the `WM_TIMER` callback are both main-thread (Reaper's message pump dispatches them) — unchanged from 1.2. No new threads.
- **NFR-R3 — no leaks across the dock lifecycle.** This is AC3 and the heart of the story: every way the panel can go away (Action-driven close, docker X, unload) must end with timer killed, GL context deleted while current, and — on the explicit/unload paths — the window class unregistered. Map all three paths explicitly (Task 4) and convince yourself each is leak-free.
- **AR15 — symmetric register/unregister.** `DockWindowAddEx` is paired with `DockWindowRemove` on the paths we own; the existing Action/gaccel/hookcommand register↔deregister symmetry in `plugin_main.cpp` is untouched.
- **AR16 — console-only diagnostics.** Any docking failure (e.g. `DockWindowAddEx` unavailable) logs via `LogError`; no dialogs.

### Coordinate / orientation / rendering

Unchanged from 1.2 — the renderer and test scene are not touched. Docking only changes *where the window lives and how it is sized*, not what is drawn. The correctly-oriented (Y-up, CCW-front, depth-tested) rotating-cube-above-ground scene must look identical, just inside a docker.

### File structure after this story

Modified only: `src/viewer_window.cpp`, `src/reaper_api.h`, `docs/PHASE0.5_VALIDATOR_GATE.md`. **No new files**, no CMake change, no new dependency. (`src/viewer_window.h` signatures stay — `OpenViewerWindow`/`CloseViewerWindow` are unchanged in shape.)

### Testing standards

- **No automated test harness** (whole-project posture — validation is the per-phase validator gate in real Reaper; [architecture.md §Test organization](../planning-artifacts/architecture.md)). 1.3's dev-side verification = `cmake -S . -B /tmp/rav_build` configures cleanly + the no-new-Reaper-symbol grep; everything runtime is Antho's Windows gate.
- NFR-P1 (≥60 fps **docked**), the dock/undock/close/reopen lifecycle, and dock-position persistence are **measured by Antho on Windows** — the dev box cannot run MSVC/Reaper/GL.

### Antho's manual validation (append these rows to `docs/PHASE0.5_VALIDATOR_GATE.md`)

Build → `reaper_animviewer.dll`, **zero `/W3 /permissive-` warnings** → copy to `%APPDATA%\REAPER\UserPlugins\` → restart Reaper. Then:
1. Run `RAV: Open Viewer` → the viewport appears **docked** (a tab in a Reaper docker), not as a free-floating top-level window.
2. Drag the panel into **each** docker position — top, bottom, left, right — and **float** it. It renders correctly in all (FR28).
3. While docked, the console `[RAV] info: … fps` line still reads **≥60** (NFR-P1 holds docked, not just floating).
4. Resize the docker (or the float) → the scene re-fills + keeps aspect, **no flicker/garbage/crash** (AC4).
5. Close the panel via the **docker's tab close button** → Reaper stays stable; re-run the Action → it **re-docks** and animates again (no stale class, no leaked context — AC3/NFR-R3).
6. With the panel docked, **restart Reaper** → on reopen via the Action it returns to its **last dock position** (the `identstr` persistence round-trip).
7. Close Reaper → exits with **no crash dialog**, no orphan window (NFR-R3).

### References

- [Source: epics.md §Story 1.3: Dockable GL window opened by a Reaper Action](../planning-artifacts/epics.md#story-13-dockable-gl-window-opened-by-a-reaper-action) — user story + the 3 AC clauses transcribed above; Epic 1 intro (revised per Spike 0).
- [Source: epics.md §FR27/FR28/FR29](../planning-artifacts/epics.md) — FR27 reinterpreted (docked GL viewport via `DockWindowAddEx`); FR28 dock into any docker; FR29 the `RAV: Open Viewer` Action. FR30 + AR3/NFR-C2 (ReaImGui) explicitly **moved to Epic 5** — out of scope here.
- [Source: epics.md §AR15/AR16/AR17/AR18, NFR-R3/NFR-R5/NFR-P1/NFR-P4](../planning-artifacts/epics.md) — symmetric register, console-only, failure isolation, main-thread GL; no-leak unload; warning-free; ≥60 fps; <2 s load.
- [Source: architecture.md §Spec Change Log 2026-06-23](../planning-artifacts/architecture.md#spec-change-log) — D11/AR10 superseded → **native OpenGL window docked via `DockWindowAddEx`**; ReaImGui deferred to Epic 5; Epic 1 has no ReaImGui dependency.
- [Source: docs/SPIKE0_FINDINGS.md §Finding 1](../../docs/SPIKE0_FINDINGS.md) — the docked-GL-window-via-`DockWindowAddEx` route hit ~62 fps where ReaImGui capped at ~30; "it still docks like any Reaper panel."
- [Source: extern/reaper-sdk/sdk/reaper_plugin_functions.h L1123-L1158](../../extern/reaper-sdk/sdk/reaper_plugin_functions.h) — exact `DockWindowActivate(HWND)`, `DockWindowAddEx(HWND, const char* name, const char* identstr, bool allowShow)`, `DockWindowRemove(HWND)` prototypes.
- [Source: 1-2-docked-opengl-viewport-direct-render.md](1-2-docked-opengl-viewport-direct-render.md) — the engine this story docks: `SetTimer` render loop, `StartRendering`/`StopRendering`, RAII GL handles, vsync-off-for-observable-fps, the "Story 1.3 can flip vsync / revisit the file name when it docks" notes.
- Spike reference implementation (read for technique, do **not** merge — branch `spike/0-1-feasibility`): `src/spike_glwindow.{h,cpp}` (`WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN` GL child, `WM_DESTROY`-nulls-hwnd), `src/spike_main.cpp` (`StartGlWindow`: `DockWindowAddEx`+`DockWindowActivate`; `StopAll`: `DockWindowRemove`+destroy), `src/reaper_api.h` (the three `WANT_DockWindow*` macros). Retrieve via `git show spike/0-1-feasibility:src/<file>`.

## Previous Story Intelligence (Story 1.2 + Spike 0)

- **From 1.2 (done):** the engine is on `main` — `viewer_window.cpp` holds a `SetTimer`-driven ~66 Hz direct-render loop with QPC fps, symmetric `StartRendering`/`StopRendering`, RAII GL handles, GLM via FetchContent, vsync OFF. Code-review corrected 10 findings (eager first-frame paint, per-frame `wglMakeCurrent` removed, proper normal matrix, fixed GL state moved to `Init`, cached camera matrices, RAII consolidated to one `GpuHandle<Deleter>` template). **1.3 must not regress any of these** — it only swaps top-level→child and adds the dock calls. The dev box is **Linux/WSL with no MSVC/Reaper/GL** → all runtime ACs go to Antho's gate; in-tree `cmake -B build` hits a drvfs `configure_file` error, so configure to `/tmp/rav_build`.
- **1.2 explicitly teed up 1.3:** "Story 1.3 can flip [vsync] to interval 1 for tear-free docked presentation" and "the window file keeps the name `viewer_window.{h,cpp}` … 1.3 may revisit the name when it docks." This story decides: **keep vsync OFF through Epic 1** (observable fps gate, Task 5) and **keep the file name** (no rename, see Dev Notes).
- **From Spike 0:** the exact docking route is proven — `WS_CHILD` window → `DockWindowAddEx(hwnd, name, identstr, true)` → `DockWindowActivate(hwnd)`, torn down with `DockWindowRemove`. The spike hit ~62 fps docked at 1055×604, so NFR-P1 has real headroom when docked. The spike's `WM_DESTROY` only nulled the HWND (it was throwaway); production folds in the full `StopRendering()` teardown there (already present from 1.2).

## Git Intelligence

Recent `main` commits: `1809be7 feat(epic-1): rename … (story 1.1)`, then Story 1.2 landed the render engine (docked-viewport direct-render, `gl_loader`/`renderer`/`gpu_resources`/`console_log`, GLM, code-review fixes — file `1-2-docked-opengl-viewport-direct-render.md` marked **done**). 1.3 is a **focused, low-surface change** on top: it edits two source files (`viewer_window.cpp`, `reaper_api.h`) plus the gate doc, introduces no new TUs, no new dependency, no CMake change. The throwaway `spike/0-1-feasibility` branch carries the dock reference implementation and stays unmerged.

## Latest Technical Information

- **Reaper docking API** — `DockWindowAddEx(hwnd, name, identstr, allowShow)` registers a child window with Reaper's docker; `identstr` is the persistence key Reaper uses to remember the dock position/state across sessions (so it must be stable across builds). `DockWindowActivate(hwnd)` reveals/foregrounds the docked window's tab; `DockWindowRemove(hwnd)` detaches it. All three are plain `rec->GetFunc`-resolved Reaper API (no ReaImGui, no extra dependency). Reaper 7.x / `caller_version 0x20E` — unchanged from Phase 0.
- **Win32 docked child** — a docked GL viewport is a `WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN` window; `CS_OWNDC` keeps its WGL device context valid across the docker's reparenting. The window is *not* self-`ShowWindow`n — `DockWindowActivate` controls visibility inside the docker.
- **No new build inputs** — opengl32/gdi32/user32 already linked (1.2); GLM already wired; no ReaImGui in Epic 1.

## Project Context Reference

No `project-context.md` exists in this repo (the activation glob found none); conventions live in `architecture.md` (decisions D1–D18 + Implementation Patterns) and are summarised inline above. Binding cross-cutting invariants for this story: **AR18** (UI + GL + docking calls on Reaper's main thread only), **AR16** (console-only diagnostics, `[RAV]`), **AR15** (symmetric register/unregister — `DockWindowAddEx`↔`DockWindowRemove` on owned paths), **AR17** (failure isolation — a docking hiccup never crashes the host), **NFR-R3** (no leaked GL context / window class / timer across dock/undock/close/reopen/unload — this is AC3), **NFR-R5** (`/W3 /permissive-` warning-free), **NFR-P1** (≥60 fps, re-verified **docked**), **NFR-P4** (<2 s load), **AR20** (deviations get noted — the kept `viewer_window` file name).

## Story Completion Status

Ultimate context engine analysis completed — comprehensive developer guide created. Status: **ready-for-dev**.

## Resolved Decisions (confirmed by Antho 2026-06-23 — not open)

1. **Vsync policy when docked → vsync stays OFF** through Epic 1. Antho confirmed tearing is a non-issue for now; keeping it off lets the validator read the docked fps and confirm ≥60 (NFR-P1) at a real docked size (matches the spike's measurement method). Tear-free vsync-on is a later-polish flip — **do not implement it in 1.3**.
2. **Docker tab caption → `"ReaAnimViewer"`** (confirmed), with persistence key `identstr = "ReaAnimViewer.Viewer"`. Use these literal strings in `DockWindowAddEx`.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Claude Opus 4.8, 1M context) — dev-story workflow

### Debug Log References

- `cmake -S . -B /tmp/rav_build` → configures cleanly (non-Windows branch stubs the target; proves CMake syntax only, per the drvfs `configure_file` constraint since Story 1.1).
- Symbol grep on changed sources: only new Reaper API calls are `DockWindowAddEx` / `DockWindowActivate` / `DockWindowRemove` (all now `REAPERAPI_WANT_*`); `grep -rniE 'imgui|reaimgui' src/` → none (Epic 1 has no ReaImGui dependency).

### Completion Notes List

- **Task 1** — Added `REAPERAPI_WANT_DockWindowAddEx/Activate/Remove` to `reaper_api.h`. They resolve through the existing `REAPERAPI_LoadAPI(rec->GetFunc)` in `plugin_main.cpp` (which already bails to 0 if any wanted symbol fails) — no new registration, `reaper_api.cpp` untouched.
- **Task 2** — `OpenViewerWindow` window style `WS_OVERLAPPEDWINDOW` → `WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN`, position → 0,0, parent still `reaper_main`, `CS_OWNDC` kept. Removed the self-`ShowWindow(SW_SHOW)` + `UpdateWindow` (the docker owns visibility). Window title constant kept.
- **Task 3** — After create: `DockWindowAddEx(g_hwnd, "ReaAnimViewer", "ReaAnimViewer.Viewer", true)` then `DockWindowActivate`. Idempotent reopen path now calls `DockWindowActivate` (replacing the old `SW_RESTORE`/`SetForegroundWindow` raise). `CloseViewerWindow` calls `DockWindowRemove` before `DestroyWindow` on the live-window (owned) path.
- **Task 4** — `WM_DESTROY` runs `StopRendering()` + nulls `g_hwnd`. (NB: the docker-X does NOT reach `WM_DESTROY` — see the close-handling note below — only our own `CloseViewerWindow`/unload do.) Unload via `CloseViewerWindow`: live → `DockWindowRemove`+`DestroyWindow`+unregister; already-down → idempotent `StopRendering()` + unregister. Undock/redock needs no new code — `CS_OWNDC` keeps the WGL DC valid across Reaper reparenting the same HWND.
- **Task 5** — Vsync stays OFF (interval 0) through Epic 1 (confirmed decision) so the validator can re-confirm NFR-P1 *while docked*. Updated the `TrySetVsync` rationale comment to record this; the observable fps line is unchanged.
- **Task 6** — No `CMakeLists.txt` change (no new TUs). CMake configure clean; symbol grep confirms only the three docking symbols added, no ReaImGui.
- **Task 7** — Appended a Story 1.3 section (9-row acceptance table + "what this gate proves") to `docs/PHASE0.5_VALIDATOR_GATE.md`.
- **Windows-gate fix (AC3 / NFR-R3) — docker-close left the render loop running; root cause proven by a message trace.** Antho's Windows runs: panel docked, ≥60 fps ✓, but closing via the docker X left the console still logging fps (background rendering). **Four event-based attempts were all inert** — `WM_DESTROY` teardown, `WM_SHOWWINDOW` pause, a `screenset` `LOAD_STATE`(null) hide, and finally converting the window to a proper **dialog** (the SWS-canonical type) — none received any close/hide message on tab-X. **Definitive root cause (proven by temporarily logging every window message):** on docker-X close Reaper hides an *ancestor* (the docker host) and sends our window **nothing** — no `WM_DESTROY`/`WM_CLOSE`/`WM_SHOWWINDOW`/`WM_WINDOWPOSCHANGED` — because hiding an ancestor via `SetWindowPos` does not notify child windows. The dialog and screenset hypotheses (from the SWS reference) did not apply to our raw-docked GL window. **Final solution:** detect close by the window's actual **visibility** — `FrameTimerProc` checks `IsWindowVisible(g_hwnd)` each tick and parks the present loop (no SwapBuffers / no fps / no GPU) while hidden, logging `panel hidden — render paused` / `panel shown — render resumed` once per transition; it self-resumes on reopen. This is standard Win32 (not a Reaper hack) and is the *only* mechanism that reflects the docker's ancestor-hide. The dialog wrapper and screenset path were **removed** (their justification — a native close message — was disproven), reverting to the simplest working form: a CS_OWNDC `WS_CHILD` docked directly. `WM_DESTROY` still tears down on the paths we own (explicit close / unload). **Windows-only target → not compilable on the Linux dev box; re-test on Windows (fps STOP on docker-X close was confirmed working in the prior iteration).**
- **Toggle action (Antho request, 2026-06-23).** The `RAV: Open Viewer` action is now a **toggle**: running it while the panel is visible **closes** it (full `CloseViewerWindow` teardown), while running it when absent or docker-hidden **opens/re-shows** it (`ToggleViewerWindow` → `OpenViewerWindow`, whose existing branch handles both create-and-dock and re-show-if-hidden). Added `ViewerWindowIsVisible()` and registered a Reaper **`toggleaction`** callback (`OnToggleAction`) so the action's on/off state drives the toolbar/menu checkmark, with `RefreshToolbar(command_id)` called after each toggle for an immediate update (new wanted symbol `REAPERAPI_WANT_RefreshToolbar`). `toggleaction` is deregistered symmetrically on unload (AR15). Files touched grew to include `src/plugin_main.cpp` and `src/viewer_window.h`.
- **WINDOWS HANDOFF (required):** the dev box is Linux/WSL with no MSVC/Reaper/GL, so all runtime ACs (AC1 docks via Action / AC2 no standalone window / AC3 leak-free dock-undock-close-reopen incl. docker-X / AC4 ≥60 fps docked + clean resize + zero `/W3 /permissive-` warnings + <2 s load + dock-position-persists-across-restart) are verified by Antho on Windows via the new validator-gate §4 rows. Build, run, and confirm there before this story is truly closed.
- **FINAL close/hide behavior (confirmed working by Antho 2026-06-24).** Reaper sends our docked window no message on tab-X close and a close is indistinguishable from a transient hide (minimize keeps WS_VISIBLE; tab-switch hides). Every attempt to auto-DESTROY on hide misfired during minimize/restore (blank white husk). Settled design: `FrameTimerProc` renders while `IsWindowVisible(g_hwnd)`, else **parks** (no fps/GPU) — it never destroys on a hide, so minimize/tab-switch/restore are safe. The window is destroyed only where we know it's a real close: the **toggle** action (Action while open) or unload. Cost (accepted, hard Reaper limitation): an X-closed panel lingers as a dormant hidden window (GL idle, nothing running) until reopened or toggled off. The earlier 4-attempt event/dialog/screenset/DockIsChildOfDock journey is in the Change Log.
- **Variance noted (AR20):** file kept as `viewer_window.{h,cpp}` rather than the architecture's `viewer_panel.{h,cpp}` (the latter name presumed a ReaImGui panel Spike 0 deleted) — deliberate, no Spec Change Log line needed (the doc already flags the name as superseded by the GL-window change).

### File List

- `src/reaper_api.h` — modified (3 `REAPERAPI_WANT_DockWindow*` + `RefreshToolbar`)
- `src/viewer_window.cpp` — modified: CS_OWNDC `WS_CHILD` docked directly via `DockWindowAddEx`; close detected by `IsWindowVisible` gate in `FrameTimerProc` (proven necessary — Reaper sends no message on docker-X); `DockWindowRemove` on owned close; `ToggleViewerWindow`/`ViewerWindowIsVisible`; vsync-off rationale. (Dialog-wrapper and screenset experiments were tried and reverted — see Completion Notes.)
- `src/viewer_window.h` — modified (declare `ToggleViewerWindow` + `ViewerWindowIsVisible`)
- `src/plugin_main.cpp` — modified (action is a toggle via `ToggleViewerWindow`; `toggleaction` callback `OnToggleAction` registered/deregistered; `RefreshToolbar` after toggle)
- `docs/PHASE0.5_VALIDATOR_GATE.md` — modified (added Story 1.3 acceptance-check section + toggle/close rows)
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — modified (1-3 status → in-progress → review)

### Change Log

| Date | Change |
|---|---|
| 2026-06-23 | Implemented Story 1.3: handed the 1.2 GL engine to Reaper's native docker. Converted the viewer from a top-level `WS_OVERLAPPEDWINDOW` to a dockable `WS_CHILD` via `DockWindowAddEx`/`DockWindowActivate`/`DockWindowRemove`; made the dock/undock/docker-X-close/reopen/unload lifecycle leak-free on the main thread; kept vsync OFF for observable docked fps. All 7 tasks complete; runtime ACs handed to Antho's Windows validator gate (§4). Status → review. |
| 2026-06-23 | Windows-gate fix (AC3/NFR-R3): docker-X close hides (not destroys) the panel and sends NO reliable message (`WM_SHOWWINDOW` attempt was inert), so the NULL-hwnd thread timer kept rendering in the background. Final fix: poll `IsWindowVisible` in `FrameTimerProc` and park the render loop while hidden (logs paused/resumed once). Message-independent + self-healing for undock/redock. Corrected `WM_DESTROY` comment; updated validator-gate row 6. Verified on Windows by Antho (fps stops on close). |
| 2026-06-23 | Made the `RAV: Open Viewer` action a **toggle** (Antho request): visible → close, absent/hidden → open. Added `ToggleViewerWindow`/`ViewerWindowIsVisible`, a Reaper `toggleaction` callback for the toolbar/menu checkmark, and `RefreshToolbar` after each toggle (`+REAPERAPI_WANT_RefreshToolbar`). Touched `plugin_main.cpp` + `viewer_window.h`. Re-test on Windows required. |
| 2026-06-23 | Message-trace diagnostic (logged every window message) proved it definitively: on docker-X close Reaper hides an *ancestor* and sends our window NOTHING (no WM_DESTROY/CLOSE/SHOWWINDOW/WINDOWPOSCHANGED). Dialog + screenset hypotheses disproven. Reverted to CS_OWNDC WS_CHILD docked directly + visibility detection in `FrameTimerProc`. Removed the dialog wrapper, screenset path, and the diagnostic. |
| 2026-06-23 | Destroy-on-hide abandoned — it kept corrupting the window on minimize/restore (white husk), because a close is fundamentally indistinguishable from a transient hide (minimize keeps WS_VISIBLE; tab-switch hides; Reaper sends no signal). **Final decision: do NOT destroy on hide.** `FrameTimerProc` simply renders while visible and PARKS while hidden (no fps/GPU). The window is destroyed only by the toggle action (Action while open) or unload — paths where we *know* it's a real close. Removed all close-heuristic code (IsIconic/grace/debounce). Robust, no white husk. Trade-off: an X-closed panel stays as a dormant hidden window (GL idle, nothing running) until reopened or toggled off. |
| 2026-06-23 | Minimize→restore STILL produced a white husk: the grace never armed because a **minimized window keeps WS_VISIBLE** (IsWindowVisible stays TRUE while minimized), so the code — which only checked IsIconic inside the `!IsWindowVisible` branch — never noticed the minimize. Fix: test `IsIconic(GetAncestor(hwnd,GA_ROOT))` **first**, independently of visibility → park + arm the ~1.5 s grace; the brief restore transition then rides out the grace instead of closing. A real X-close (Reaper up, not minimized) still closes. |
| 2026-06-23 | Antho: minimize→restore wrongly closed the panel (white husk + `panel closed — releasing`). First attempt: a ~1.5 s minimize grace window after seeing Reaper minimized. (Superseded same day — the grace never armed; see next row.) |
| 2026-06-23 | Antho's Windows test: `DockIsChildOfDock` stays >= 0 after the X (Reaper does not even un-dock us), so it can't detect a close. Per Antho's suggestion, on the hide we now run the SAME teardown the toggle uses: hidden while Reaper is NOT minimized (debounced 3 ticks, `GetAncestor`+`IsIconic` guards the minimize case) → `CloseViewerWindow()` (DockWindowRemove + DestroyWindow + free GL + UnregisterClass) → "closed" means gone. Minimize → park (survives). Dropped the `DockIsChildOfDock` want. **Known edge:** if the panel shares a docker with other tabs, switching away will close it (indistinguishable from an X-close — Reaper gives no signal); re-open via the Action. Re-test on Windows (close releases + minimize survives + toggle). |
| 2026-06-23 | Screenset attempt was also inert on docker-X. Researched the reference (SWS `sws_wnd.cpp`): a dockable window must be a **dialog**, not a raw `CreateWindowEx` child — Reaper's docker only sends `WM_DESTROY` to docked *dialogs*. **Rewrote `viewer_window.cpp`** to a modeless `DLGTEMPLATE` dialog shell (`ViewerDlgProc`) hosting the unchanged CS_OWNDC GL child (`GlChildProc`); docker-X now fires the dialog `WM_DESTROY` → GL child destroyed → `StopRendering` + `DockWindowRemove`. Removed the poll and the screenset path (dropped those wants). Windows-only target → cannot compile/run on dev box; re-test on Windows (confirm compile + fps STOP on docker-X close). |

### Review Findings

Code review 2026-06-24 (3-layer adversarial: Blind Hunter + Edge Case Hunter + Acceptance Auditor). Scope: the 4 source files 1.3 touched (`viewer_window.cpp`, `viewer_window.h`, `reaper_api.h`, `plugin_main.cpp`), diff vs HEAD (note: Story 1.2 is uncommitted, so 1.2 engine code is bundled in `viewer_window.cpp` but was already reviewed). AC1–AC4 audited as satisfied per the story's own Resolved Decisions / re-scoped close behavior; all runtime ACs remain handed to Antho's Windows validator gate.

- [x] [Review][Patch] `DestroyGLContext` leaves `g_hdc` dangling on a `StartRendering` failure during `WM_CREATE` [src/viewer_window.cpp:92] — **FIXED 2026-06-24**: release via `WindowFromDC(g_hdc)` when `g_hwnd` is still null, and always null `g_hdc`. — the guard `if (g_hdc && g_hwnd)` skips `ReleaseDC` + the `g_hdc = nullptr` reset because the file-scope `g_hwnd` is still null at that point (it is assigned only after `CreateWindowExW` returns, line 336, and `WM_CREATE` runs *inside* that call). Net effect: `g_hdc` is left non-null/dangling after a failed open. **Not an actual GDI leak** (the class is `CS_OWNDC`, so the owned DC is reclaimed when the system destroys the partially-created window) and **not a live functional bug** (the stale `g_hdc` is overwritten by the next `GetDC` before any use, and `RenderTick` guards on `g_hglrc` which is null after teardown). Low severity / hygiene. Unambiguous fix: drop the `&& g_hwnd` clause so `g_hdc` is released/nulled regardless (or release via `WindowFromDC(g_hdc)`). **Overlap note:** this lives in the init-failure path the story explicitly fences to **Story 1.4** ("teardown-on-bail choreography"), and is likely pre-existing from 1.2 — folding the one-liner into 1.4's hardening is an equally valid call.

**Dismissed as noise / handled-elsewhere (12):** queued `WM_TIMER` after `KillTimer` (handled — `FrameTimerProc` guards on null `g_hwnd`, `RenderTick` guards on null `g_hglrc`, single-threaded pump finishes `WM_DESTROY` first); double `StopRendering` on create-failure (idempotent); stale toolbar checkmark after async `DockWindowActivate` (cosmetic, self-corrects on next Reaper poll); `g_renderer` static destructor at DLL unload (neutralized — `Shutdown()` runs on unload and resets the RAII handles to 0, so the dtor is a no-op; pre-existing 1.2 design); no `LogError` on docking-API failure / AR16 (handled by the `REAPERAPI_LoadAPI != 0` load-time bail — `WANT_` symbols cannot be null at call time, and the dock calls return `void`); toggle cannot close a docker-X-hidden husk (documented/accepted design — Resolved Decisions + gate row 6 + project memory); panel parks-not-frees after docker-X (documented; not an NFR-R3 leak — everything is torn down on toggle/unload); `plugin_main.cpp` touched vs the spec's "Modified only" list (authorized scope addition — the toggle was an Antho request, traceable in the File List); plus Blind Hunter's own self-withdrawn items (#3 `WM_SIZE` operators, #8 `wglGetProcAddress` ordering, #9 timer-ms vs comment, #11 QPC-freq ordering, #12 single-thread globals, #13 `WM_PAINT` no-draw).
