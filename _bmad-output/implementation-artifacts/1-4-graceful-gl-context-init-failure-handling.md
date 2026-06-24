# Story 1.4: Graceful GL context / init failure handling

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer on an unusual GPU / driver,
I want a clear console message instead of a crash if the viewport can't initialize,
so that I know what went wrong (and that Reaper itself is fine) instead of losing my session.

This is the **fourth and final story of Epic 1 (Phase 0.5)**, on `main`, after Story 1.3 docked the proven 60 fps GL engine into Reaper. Stories 1.2/1.3 deliberately fenced *init-failure hardening* out and kept only a minimal "log-and-bail" — they each say **"Story 1.4 hardens this"** ([viewer_window.cpp:19](../../src/viewer_window.cpp#L19), [viewer_window.cpp:194](../../src/viewer_window.cpp#L194)). Story 1.4's one job is to make the **failure path** as disciplined as the success path: a user-readable diagnostic, a fully symmetric teardown-on-bail (no leaked GL context / DC / timer / window class — NFR-R3, AR15), and a no-exception boundary so a failed open can **never** crash the host (AR17, AR18). It changes **nothing** about the success path (the engine, docking, toggle, fps all stay exactly as 1.3 shipped). It is **not** ReaImGui-missing handling (that moved to Epic 5) and does **not** load meshes (Epic 2).

## Acceptance Criteria

These transcribe Story 1.4's BDD from [epics.md §Story 1.4](../planning-artifacts/epics.md#story-14-graceful-gl-context--init-failure-handling) and bind each clause to its verifying mechanism.

1. **(AC1 — graceful diagnostic + clean bail on init failure)** Given the OpenGL context or required modern-GL functions fail to initialize, when the user triggers the `RAV: Open Viewer` Action, then the extension emits a **user-readable `ShowConsoleMsg` diagnostic** naming *what* failed and *what to do* (e.g. driver too old / GPU unsupported), and **bails cleanly** — no broken/blank window is left docked, and **Reaper does not crash or hang** (AR16 console-only, AR17 failure isolation). The Action remains runnable afterward (a retry is allowed to re-attempt the open).
2. **(AC2 — fully symmetric teardown on the bail path)** And every resource/registration acquired *during the failed open* is symmetrically reversed, leaving **no dangling pointers or leaked OS/GL objects** (NFR-R3, AR15): the WGL context and its `HDC`, the frame timer, and — on a hard create-failure — the window class are each released on the way out. **Conversely**, the plugin-load registrations (`command_id` / `gaccel` / `hookcommand` / `toggleaction`) are *not* touched by a failed open — the extension stays loaded and the Action stays registered (reversing those is the unload path's job, not a window-open failure's).
3. **(AC3 — no exception escapes into the host)** And the Action's entry path (`OnHookCommand` → `ToggleViewerWindow` → `OpenViewerWindow`) is a **no-throw boundary**: any unexpected exception is caught and converted to a console diagnostic rather than propagating into Reaper's command dispatch (AR18 "no exceptions in the plugin entry path", AR17 no-host-crash).
4. **(AC4 — success path & build stay clean; no regression)** And the **success path is byte-for-byte behaviourally unchanged** — when init succeeds the panel docks, animates at ≥60 fps, toggles, resizes, and tears down exactly as Story 1.3 shipped; the build stays **zero-warning at MSVC `/W3 /permissive-`** (NFR-R5), loads in **under 2 s** (NFR-P4), and adds **no new Reaper API symbols and no ReaImGui** (Epic 1 has no ReaImGui dependency — AC5 of the re-scope).
5. **(AC5 — ReaImGui-missing handling is explicitly out)** And the former "ReaImGui absent" diagnostic + graceful bail (FR30) is **not** implemented here — it is relocated to Epic 5, where the animation browser introduces the first ReaImGui panel and the `cfillion/reaimgui` dependency (AR3, NFR-C2). Do not add any ReaImGui detection, header, or call.

## Tasks / Subtasks

- [x] **Task 1 — User-readable, actionable init-failure diagnostics (AC: 1)**
  - [x] The failure points already exist in [src/viewer_window.cpp `StartRendering`](../../src/viewer_window.cpp#L195-L240) and `CreateGLContextFor`; each currently logs a terse `LogError`. **Upgrade the messages** so a non-technical user understands the cause and the remedy. Target a single greppable `[RAV] error:` line per cause (D7 — no multi-line; Reaper's console concatenates poorly):
    - WGL context creation failure (`ChoosePixelFormat`/`SetPixelFormat`/`wglCreateContext` → `CreateGLContextFor` returns false): e.g. `failed to create an OpenGL context — your GPU/driver may not support OpenGL or no display is available; the viewer did not open (Reaper is unaffected)`.
    - `wglMakeCurrent` failure: name it as a context-activation failure with the same "viewer did not open, Reaper unaffected" tail.
    - **Missing modern-GL function** (the common "old driver" case): [gl_loader.cpp:20](../../src/gl_loader.cpp#L20) already returns `"missing GL function: <name>"`. **Add a GL-version probe** *before* `LoadGlFunctions` runs (the context is current at that point): read `glGetString(GL_VERSION)` / `GL_RENDERER` (both are GL 1.1, always available via `opengl32`, no loader needed) and, if the per-symbol load then fails, emit one line like `OpenGL 3.3+ required but your driver reports "<GL_VERSION>" on "<GL_RENDERER>" — update your GPU driver; the viewer did not open (Reaper is unaffected)`. Keep the raw `missing GL function: X` line too (it is the precise diagnostic for log-mining), but lead with the human-readable cause.
  - [x] `renderer.Init` failure and `SetTimer` failure already `LogError`; reword to the same "what failed + viewer did not open, Reaper unaffected" shape. Do **not** invent new failure modes — only reword existing ones and add the version probe.
  - [x] **No message boxes / dialogs** — `ShowConsoleMsg` via `LogError` is the only channel (AR16). The console helpers in [console_log.h](../../src/console_log.h) are the only allowed surface.
- [x] **Task 2 — Make the teardown-on-bail fully symmetric (AC: 2)**
  - [x] Walk **every** resource the open path can acquire and confirm each is released on the failure branch. The existing pieces: `WM_CREATE → StartRendering` bail already calls `StopRendering()` ([viewer_window.cpp:262-265](../../src/viewer_window.cpp#L262-L265)) which kills the timer, makes-current, `Renderer::Shutdown`, and `DestroyGLContext` (context + DC). `DestroyGLContext` already recovers the owning HWND via `WindowFromDC` when `g_hwnd` is still null during `WM_CREATE` ([viewer_window.cpp:92-99](../../src/viewer_window.cpp#L92-L99)) — this is the low-sev dangle the 1.3 review explicitly **deferred to 1.4** ([1-3 Review Findings](1-3-dockable-gl-window-opened-by-a-reaper-action.md#review-findings)). **Verify it, keep it, and own it as this story's.**
  - [x] **Close the one remaining asymmetry — the window class.** When `OpenViewerWindow` bails because `CreateWindowExW` returned null (WM_CREATE returned -1 after a GL init failure), `EnsureClassRegistered` has already set `g_class_registered = true` ([viewer_window.cpp:331-334](../../src/viewer_window.cpp#L331), [L348-353](../../src/viewer_window.cpp#L348-L353)) but the class is left registered with no window. **On the hard create-failure branch, unwind it** (`UnregisterClassW` + reset `g_class_registered`/`g_class_hinst`) so a failed open leaves *nothing* behind — symmetric with the success path's "class registered ↔ unregistered on close/unload". A subsequent Action retry simply re-registers it (idempotent). Add a one-line WHY comment.
  - [x] **Do NOT over-reverse.** A failed *window open* must leave the plugin-entry registrations (`command_id`/`gaccel`/`hookcommand`/`toggleaction` in [plugin_main.cpp:72-79](../../src/plugin_main.cpp#L72-L79)) intact — the extension stays loaded and the Action stays usable. Those are reversed only on the `rec == nullptr` unload path ([plugin_main.cpp:48-58](../../src/plugin_main.cpp#L48-L58)). Confirm by inspection that nothing in the open/bail path calls `g_register("-...")`.
  - [x] Confirm the WM_CREATE-bail does **not** rely on `WM_DESTROY` firing (Windows does **not** send `WM_DESTROY` when `WM_CREATE` returns -1 — only `WM_NCDESTROY`), which is exactly why `WM_CREATE` calls `StopRendering()` directly. `StopRendering()` is idempotent, so even if a `WM_DESTROY` *did* arrive the second teardown is a no-op. Add/confirm a WHY comment so a future reader doesn't "simplify" the explicit `StopRendering()` away.
- [x] **Task 3 — No-throw boundary at the Action entry (AC: 3)**
  - [x] AR18 mandates "no exceptions in the plugin entry path." The Action dispatch is `OnHookCommand` → `ToggleViewerWindow` → `OpenViewerWindow`. Today nothing wraps these; a `std::bad_alloc` from the `std::string err` buffer (or any future throw) would propagate into Reaper's command dispatcher (UB / host crash). **Wrap the open path in a `try { … } catch (const std::exception& e) { LogError("viewer open failed: %s (Reaper is unaffected)", e.what()); } catch (...) { LogError("viewer open failed: unknown error (Reaper is unaffected)"); }`** boundary. Place it at the smallest sensible scope — wrapping the body of `ToggleViewerWindow` (or `OnHookCommand` in [plugin_main.cpp](../../src/plugin_main.cpp)) so **both** the open and the close/toggle legs are covered. On a caught throw, also ensure no half-built window survives (call the same teardown — `CloseViewerWindow()` is idempotent and safe to invoke from the catch).
  - [x] Keep the boundary **thin and explicit** — do not sprinkle try/catch through the renderer/loader (those already use `bool`+`out_error`, not exceptions, per D5/D6). This is one guard at the host boundary only.
- [x] **Task 4 — A provable failure path: forced-failure test hook (AC: 1, 2, 3)**
  - [x] AC1–AC3 are **unverifiable on working hardware** — Antho's reference GPU initializes fine, so the graceful path never triggers naturally. Add a **compile-time forced-failure switch** so the path can be exercised on demand, e.g. a `#ifdef RAV_FORCE_INIT_FAILURE` guard that makes `CreateGLContextFor` (or `LoadGlFunctions`) return false immediately with a clear `// TEST-ONLY` comment. Default build leaves it **OFF** (no `#define`); the validator flips it on for one run, confirms the diagnostic + clean bail + Reaper-stays-alive, then rebuilds without it.
  - [x] Document the exact flip in the validator gate (Task 6). Keep the switch zero-cost and invisible in normal builds (no runtime branch, no warning under `/W3`).
  - [x] **Do not** ship a runtime UI/menu for this — it is a build-define test aid only (AR16 keeps the console as the sole user channel; this is a developer/validator affordance).
- [x] **Task 5 — Wire-up sanity: no new symbols, no ReaImGui, CMake unchanged (AC: 4, 5)**
  - [x] `grep` the changed sources: **no new Reaper API symbol** is needed (the diagnostics use `ShowConsoleMsg` already wanted; `glGetString` is core GL 1.1 from `opengl32`, already linked). Confirm `reaper_api.h` is **unchanged**.
  - [x] `grep -rniE 'imgui|reaimgui' src/` → **none** (AC5 — Epic 1 has no ReaImGui dependency; do not add detection/header/call).
  - [x] No `CMakeLists.txt` change expected (no new TUs). On this Linux/WSL box run `cmake -S . -B /tmp/rav_build` (out-of-sandbox — the in-tree `build/` hits the drvfs `configure_file` restriction known since Story 1.1) to confirm the target still **configures** (non-Windows branch stubs the target; this proves CMake syntax only, not the MSVC build or the GL runtime).
- [x] **Task 6 — Extend the Phase 0.5 validator gate; hand off the Windows run (AC: 1, 2, 3, 4)**
  - [x] Append a **Story 1.4** section to [docs/PHASE0.5_VALIDATOR_GATE.md](../../docs/PHASE0.5_VALIDATOR_GATE.md) (it already announces "Story 1.4 (init-failure hardening) append their own"). Cover: (a) **forced-failure run** — build with `RAV_FORCE_INIT_FAILURE`, trigger the Action, confirm a clear console diagnostic appears, **no window docks**, Reaper stays fully responsive, and re-running the Action does not crash; (b) **leak check on the bail** — after the forced failure, no orphan window/tab, no stuck render (no fps lines), Reaper exits clean; (c) **success path regression** — rebuild *without* the define and re-run the full Story 1.3 checklist (dock / drag to each edge / float / ≥60 fps docked / resize / toggle close+reopen / dock persists across restart / clean exit) to prove 1.4 changed nothing on the happy path. Keep it short.
  - [x] **Hand the Windows MSVC build + Reaper runs to Antho** (the dev box has no MSVC/Reaper/GL — same constraint as Stories 1.1–1.3): zero `/W3 /permissive-` warnings, the forced-failure diagnostic is readable and Reaper survives, the success-path regression passes, load < 2 s. Note this handoff explicitly in Completion Notes.

## Dev Notes

### Why this story exists (and what it is NOT)

Epic 1's success path is done: 1.2 stood up the 60 fps direct-render engine, 1.3 docked it and made the dock/undock/close/toggle lifecycle leak-free. **Both stories explicitly deferred the *failure* path to 1.4** — the source says so in two places ([viewer_window.cpp:19](../../src/viewer_window.cpp#L19) "Init-failure hardening is Story 1.4"; [viewer_window.cpp:194](../../src/viewer_window.cpp#L194) "Story 1.4 hardens this into the full symmetric teardown-on-bail path"). So 1.4 is **hardening + diagnostics + a no-throw boundary on an already-working code path**, not new feature code. Most of the skeleton is present (every `StartRendering` step already returns `bool` and logs; `WM_CREATE` already bails via `StopRendering()`+return -1). The dev's job is to (1) make the diagnostics genuinely user-readable, (2) close the *one* real symmetry gap (the window class on a hard create-failure), (3) add the host-boundary try/catch, (4) make the path testable via a forced-failure define. **Resist the urge to rewrite the lifecycle** — it was hard-won across 1.3's long debugging journey (see 1.3 Change Log) and the success path must not regress.

Scope fences (do not pull these in):
- **ReaImGui-missing handling → Epic 5.** Explicitly relocated (AC5, [epics.md:331](../planning-artifacts/epics.md#story-14-graceful-gl-context--init-failure-handling)). No `ImGui_*`, no `extern/reaimgui/`, no detection.
- **Mesh / asset-load failure handling → Epic 2/3 + Epic 6 (FR34–FR37, Story 6.4).** 1.4 still renders the 1.2 built-in test scene; there is no file loading to fail yet. The graceful-degradation/per-item-isolation work is a later epic.
- **The docker-X "parks not frees" behavior is NOT a leak and is out of scope.** It was settled in 1.3 (the panel lingers as a dormant hidden window until toggled/unloaded — see [project memory: reaper-docked-close] and 1.3 Completion Notes). Do not re-litigate it here; 1.4 is about *init* failure, not *close* behavior.

### Current state of the files being modified (read before editing)

- **[`src/viewer_window.cpp`](../../src/viewer_window.cpp)** (post-1.3) — owns the whole engine + lifecycle. The init-failure-relevant surface:
  - `CreateGLContextFor(hwnd)` ([L102-130](../../src/viewer_window.cpp#L102)) — `GetDC` → `ChoosePixelFormat`/`SetPixelFormat` (32/24/8) → `wglCreateContext`; each failure releases the DC and returns false. **Add the version-probe context here is already current after `wglMakeCurrent` in `StartRendering`, so probe in `StartRendering` between make-current and `LoadGlFunctions`, not here.**
  - `StartRendering(hwnd)` ([L195-240](../../src/viewer_window.cpp#L195)) — the failure funnel: `CreateGLContextFor` → `wglMakeCurrent` → `LoadGlFunctions` → `g_renderer.Init` → `SetTimer`; each returns false with a `LogError`. **This is where the diagnostics get upgraded and the version probe is inserted.**
  - `StopRendering()` ([L245-256](../../src/viewer_window.cpp#L245)) — idempotent symmetric teardown (KillTimer → make-current → `Renderer::Shutdown` → `DestroyGLContext`). **Already correct; verify, don't change.**
  - `DestroyGLContext()` ([L85-100](../../src/viewer_window.cpp#L85)) — releases context + DC; the `WindowFromDC` fallback for the `g_hwnd==null` (WM_CREATE-bail) case is the 1.3-review dangle fix folded here. **Own it.**
  - `WM_CREATE` ([L261-266](../../src/viewer_window.cpp#L261)) — `if (!StartRendering) { StopRendering(); return -1; }`. **The bail spine; keep its shape.**
  - `OpenViewerWindow` ([L322-360](../../src/viewer_window.cpp#L322)) — class-register → `CreateWindowExW` (`WS_CHILD`) → on null bail with `LogError("CreateWindowExW failed")` → else `DockWindowAddEx`+`DockWindowActivate`. **The class-unwind gap (Task 2) and the reworded bail message live here.** Note: `DockWindowAddEx` is only reached *after* a fully successful create, so there is no "docked-then-failed" half-state to reverse.
  - `ToggleViewerWindow` / `CloseViewerWindow` ([L367-398](../../src/viewer_window.cpp#L367)) — the toggle + owned-close paths; `CloseViewerWindow` is idempotent and is the right thing to call from the Task-3 catch.
  - **What must be preserved (do NOT touch):** the entire success-path render machinery (`RenderTick`/`FrameTimerProc`/timer/fps/QPC), the `IsWindowVisible`-gated park-not-destroy hide handling, vsync-off policy, `CS_OWNDC`, the toggle/`DockWindowActivate` reopen, `DockWindowRemove`-on-owned-close. 1.4 only edits the *failure* branches + adds one try/catch.
- **[`src/gl_loader.cpp`](../../src/gl_loader.cpp)** ([L13-24](../../src/gl_loader.cpp#L13)) — `LoadGlFunctions(out_error)` resolves each modern-GL symbol via `wglGetProcAddress`; the first miss returns `"missing GL function: <name>"`. **Keep this; the human-readable "driver too old" line is composed in `StartRendering` around it (using the version probe), not inside the loader** (the loader stays the single, terse modern-GL surface — architecture's "only gl_loader/renderer call modern GL" rule).
- **[`src/console_log.h`](../../src/console_log.h)** — `LogInfo/Warn/Error`, the **only** allowed feedback channel (AR16, D7 `[RAV] <level>: <message>`, single-line). No new logging API needed.
- **[`src/plugin_main.cpp`](../../src/plugin_main.cpp)** — `OnHookCommand` ([L24-30](../../src/plugin_main.cpp#L24)) dispatches the Action to `ToggleViewerWindow`; symmetric register at load / deregister at unload. **Candidate home for the Task-3 try/catch boundary** (wrap `OnHookCommand`'s call, or wrap inside `ToggleViewerWindow` — dev's call; wrapping the toggle covers both open and close legs in one place).
- **[`src/reaper_api.h`](../../src/reaper_api.h)** — `WANT_ShowConsoleMsg` + the three `DockWindow*` + `RefreshToolbar`. **No change** (1.4 adds no Reaper symbols). **[`reaper_api.cpp`](../../src/reaper_api.cpp)** — the single `REAPERAPI_IMPLEMENT` TU; do not touch.

### Naming, namespace, console (live conventions)

Namespace **`rav`**, console tag **`[RAV]`**, format `[RAV] <level>: <message>` single-line (D7). Types/functions PascalCase, locals/members snake_case, file-scope globals `g_`+snake_case, constants `k`+PascalCase, file-internal symbols in anonymous namespaces. SPDX header already on every file — keep it. Comment only non-obvious WHY (the class-unwind-on-bail, the "no WM_DESTROY after WM_CREATE returns -1" fact, the version-probe-for-readable-diagnostic intent) — never narrate WHAT. ([architecture.md §Naming/Structural/Comment patterns](../planning-artifacts/architecture.md))

### Threading / lifecycle invariants this story must honor

- **AR18 — main-thread only, no exceptions in the entry path.** The Action callback and the GL/teardown calls all run on Reaper's main thread (unchanged). The new mandate this story enforces is the **no-exception** half: the host-boundary try/catch (Task 3) is the literal implementation of "no exceptions in the plugin entry path."
- **AR17 — failure isolation / no host crash.** A failed viewer open must never take Reaper down. This is the through-line of AC1+AC3.
- **AR16 — console-only diagnostics.** Every diagnostic is a `ShowConsoleMsg` line via `LogError`. No dialogs/toasts/boxes — ever.
- **AR15 / NFR-R3 — symmetric register/unregister, no leaks.** On the bail path: timer killed, GL context deleted while current, DC released, window class unregistered (Task 2). On the *non*-bail-but-failed-open: plugin-entry registrations stay (don't over-reverse). NFR-R3's "clean unload" is unchanged from 1.3 and must keep passing.

### Win32 facts that matter here

- **`WM_CREATE` returning -1 → no `WM_DESTROY`.** Windows aborts `CreateWindowEx` (returns null) and sends `WM_NCDESTROY` but *not* `WM_DESTROY`. That is exactly why `WM_CREATE` calls `StopRendering()` itself before returning -1 — do not assume `WM_DESTROY` will clean up the partial init. `StopRendering()` is idempotent, so a stray second call is harmless.
- **`glGetString(GL_VERSION)` / `GL_RENDERER`** are GL 1.1 entry points exported directly by `opengl32.dll` — available the moment a context is current, with **no** `wglGetProcAddress` loader needed. Safe to call in `StartRendering` right after `wglMakeCurrent` succeeds and before `LoadGlFunctions`. They return `const GLubyte*` (cast to `const char*` for `%s`); guard against a null return.
- **`CS_OWNDC`** owns the DC for the window's lifetime; `ReleaseDC` is still correct on our acquired `GetDC` handle and the class is reclaimed by `UnregisterClassW`. The forced-failure test must not leave the class registered (Task 2 covers the real path; the test path goes through the same `OpenViewerWindow` bail, so it is covered automatically).

### File structure after this story

Modified: `src/viewer_window.cpp` (diagnostics + class-unwind-on-bail + version probe + forced-failure guard), `src/gl_loader.cpp` *(optional — only if you choose to host the forced-failure guard there)*, `src/plugin_main.cpp` *or* `viewer_window.cpp` (the try/catch boundary — dev's placement call), `docs/PHASE0.5_VALIDATOR_GATE.md`. **No new files**, no CMake change, no new dependency, no new Reaper symbol. `viewer_window.h` signatures stay unchanged.

### Testing standards

- **No automated test harness** (whole-project posture — validation is the per-phase validator gate in real Reaper; [architecture.md §Test organization](../planning-artifacts/architecture.md)). Dev-side verification = `cmake -S . -B /tmp/rav_build` configures cleanly + the no-new-symbol / no-ReaImGui greps. Everything runtime is Antho's Windows gate.
- The init-failure ACs are **only** observable via the **forced-failure build define** (Task 4) — there is no working-hardware way to trigger them. The gate (Task 6) documents the flip-build-run-revert loop. NFR-P1 (≥60 fps docked, success-path regression) and the leak-free bail are **measured by Antho on Windows** — the dev box cannot run MSVC/Reaper/GL.

### Antho's manual validation (append these rows to `docs/PHASE0.5_VALIDATOR_GATE.md`)

**A — forced-failure path** (build *with* `RAV_FORCE_INIT_FAILURE`, e.g. add `/D RAV_FORCE_INIT_FAILURE` or a CMake `-D`):
1. Build → `reaper_animviewer.dll`, **zero `/W3 /permissive-` warnings** → copy to `%APPDATA%\REAPER\UserPlugins\` → restart Reaper.
2. Run `RAV: Open Viewer` → the **console shows a clear, readable diagnostic** (what failed + "viewer did not open, Reaper is unaffected"), and **no panel docks**.
3. Reaper stays **fully responsive** (no freeze, no crash dialog); run the Action again → still no crash, same diagnostic.
4. No stray fps lines appear afterward (the render loop never started / was torn down) — no orphan tab/window in any docker.
5. Close Reaper → exits clean, **no crash dialog**, no orphan window (NFR-R3).

**B — success-path regression** (rebuild *without* the define):
6. Re-run the full Story 1.3 checklist: Action docks the panel, drag into each docker edge + float, console fps reads **≥60 docked**, resize is clean, toggle closes + reopens, dock position **persists across a Reaper restart**, Reaper exits clean. **Nothing about the happy path may have changed.**

### References

- [Source: epics.md §Story 1.4: Graceful GL context / init failure handling](../planning-artifacts/epics.md#story-14-graceful-gl-context--init-failure-handling) — user story + the 3 BDD clauses (diagnostic+clean bail, symmetric register reversal, ReaImGui-relocation); Epic 1 intro honoring AR15/AR16/AR18.
- [Source: epics.md §AR15/AR16/AR17/AR18](../planning-artifacts/epics.md#L152-L155) — symmetric register; console-only diagnostics; failure isolation (no host crash); main-thread GL + **no exceptions in the plugin entry path**.
- [Source: epics.md §NFR-R1/NFR-R3/NFR-R5/NFR-P4](../planning-artifacts/epics.md#L107-L109) — zero host crashes; clean unload / no leaked GL contexts·classes·timers·pointers; `/W3 /permissive-` warning-free; <2 s load.
- [Source: architecture.md §Cross-cutting concerns / Implementation patterns](../planning-artifacts/architecture.md) — `plugin_main.cpp` is the *only* file that calls `rec->Register` (symmetric-register auditability, [L874](../planning-artifacts/architecture.md)); `log.h`/`console_log` is the *only* `ShowConsoleMsg` wrapper ([L875](../planning-artifacts/architecture.md), D7 format [L290](../planning-artifacts/architecture.md)); no-throw boundary / RAII / symmetric register as the consistency contract ([L92-93](../planning-artifacts/architecture.md), [L610](../planning-artifacts/architecture.md), [L1115](../planning-artifacts/architecture.md)).
- [Source: architecture.md §Spec Change Log 2026-06-23](../planning-artifacts/architecture.md#L1136) — ReaImGui dependency + FR30 (graceful ReaImGui-missing) deferred to Epic 5; Epic 1 has no ReaImGui dependency.
- [Source: 1-3-dockable-gl-window-opened-by-a-reaper-action.md §Review Findings](1-3-dockable-gl-window-opened-by-a-reaper-action.md#review-findings) — the `g_hdc`-dangle-on-`StartRendering`-failure finding, fixed in 1.3 and flagged "equally valid to fold into 1.4's hardening" → **owned here**.
- [Source: src/viewer_window.cpp](../../src/viewer_window.cpp) — the live failure surface: `CreateGLContextFor`, `StartRendering`, `StopRendering`, `DestroyGLContext`, `WM_CREATE` bail, `OpenViewerWindow` class-register/create-bail.
- [Source: src/gl_loader.cpp:13-24](../../src/gl_loader.cpp#L13) — `LoadGlFunctions` per-symbol `"missing GL function: X"` diagnostic (the "old driver" failure mode the version probe humanizes).
- [Source: src/plugin_main.cpp:24-30](../../src/plugin_main.cpp#L24) — `OnHookCommand` Action dispatch (no-throw-boundary candidate site); [L48-58](../../src/plugin_main.cpp#L48-L58) symmetric unload deregister (do not duplicate on a failed open).

## Previous Story Intelligence (Stories 1.2 / 1.3 + Spike 0)

- **1.2/1.3 explicitly teed up 1.4.** Both left in-code markers: "Init-failure hardening is Story 1.4" and "Story 1.4 hardens this into the full symmetric teardown-on-bail path." The failure skeleton (bool-returning steps, `WM_CREATE`→`StopRendering`+return -1) is already in place — 1.4 finishes it, it does not start it.
- **The `g_hdc` dangle is already half-handled.** 1.3's code review found `DestroyGLContext` left `g_hdc` non-null on a `StartRendering` failure during `WM_CREATE`; the fix (recover the HWND via `WindowFromDC`, always null `g_hdc`) was **applied in 1.3** and the review said folding it into 1.4 was "equally valid." **It is in the tree now ([viewer_window.cpp:92-99](../../src/viewer_window.cpp#L92)) — verify and claim it as 1.4's; do not re-fix.**
- **The lifecycle is fragile-by-history — change only the failure branches.** 1.3's Change Log records a long, painful journey to the current close/hide behavior (4+ reverted attempts: dialog wrapper, screenset, `DockIsChildOfDock`, destroy-on-hide white-husk bugs). The *success and close* paths are settled and validator-confirmed by Antho 2026-06-24. **1.4 must not touch them** — it edits only the init-*failure* branches + adds a host-boundary try/catch.
- **Dev box is Linux/WSL — no MSVC/Reaper/GL.** All runtime ACs go to Antho's Windows gate; in-tree `cmake -B build` hits a drvfs `configure_file` error, so configure to `/tmp/rav_build`. Same constraint as 1.1/1.2/1.3.
- **Code-review discipline.** 1.2 had 10 findings, 1.3 had 1 (this story's dangle). Expect a `code-review` pass after dev-story; keep the diff minimal and the WHY-comments tight to make the review clean.

## Git Intelligence

Recent `main`: `19860e9 feat(epic-1): direct-render GL viewport docked into Reaper (stories 1.2 + 1.3)`, `1809be7 feat(epic-1): rename … (story 1.1)`. Story 1.3 is currently `review` (impl done; 1 low-sev patch applied; awaiting Antho's Windows gate re-run). 1.4 is a **small, surgical change** layered on the same two-to-three source files — diagnostics + one symmetry fix + one try/catch + a test define + the gate doc. **No new TUs, no new dependency, no CMake change, no new Reaper symbol.** The `spike/0-1-feasibility` branch stays unmerged.

> Sequencing note: 1.3 is still in `review` pending Antho's Windows re-run. 1.4's code can be written now (it is independent of the 1.3 gate result), but the 1.4 *Windows validation* should run after 1.3's gate clears, since both share the same `viewer_window.cpp` and Antho validates them on the same build.

## Latest Technical Information

- **`glGetString` (GL 1.1)** — `glGetString(GL_VERSION)` returns a driver string like `"3.3.0 NVIDIA 552.44"` or `"1.1.0"` on a fallback/software path; `glGetString(GL_RENDERER)` names the GPU/driver (e.g. `"GDI Generic"` on the Microsoft software rasterizer — the classic "no real GL" case worth naming in the diagnostic). Both are exported by `opengl32.dll`, need only a current context, and return `const GLubyte*` (may be null — guard it). This is the cheap, dependency-free way to turn `"missing GL function: glCreateShader"` into `"OpenGL 3.3+ required, your driver reports 1.1.0 (GDI Generic) — update your GPU driver"`.
- **No new build inputs** — `opengl32`/`gdi32`/`user32` already linked (1.2); GLM already wired; no ReaImGui in Epic 1; `ShowConsoleMsg` already wanted. The forced-failure switch is a pure preprocessor `#define`, zero runtime/link cost.
- **Reaper command dispatch is not exception-safe.** Reaper calls `OnHookCommand` directly from its main message pump; a C++ exception crossing that boundary is undefined behavior in the host. The Task-3 try/catch is the contractual fix (AR18). Reaper 7.x / `caller_version 0x20E` — unchanged.

## Project Context Reference

No `project-context.md` exists in this repo (the activation glob found none); cross-cutting conventions live in `architecture.md` (decisions D1–D18 + Implementation Patterns) and are summarised inline above. Binding invariants for this story: **AR18** (main-thread + **no exceptions in the entry path** — the new enforcement here), **AR17** (failure isolation — a failed open never crashes the host), **AR16** (console-only diagnostics, `[RAV]`, single-line D7), **AR15** (symmetric register/unregister — class register↔unregister on the bail; do **not** reverse plugin-entry registrations on a failed open), **NFR-R3** (no leaked GL context / DC / timer / window class across the failure path), **NFR-R1** (zero host crashes), **NFR-R5** (`/W3 /permissive-` warning-free), **NFR-P4** (<2 s load), **AR20** (note any deviation).

Relevant project memory: *Reaper docked-panel close sends no message* (the docker-X "parks not frees" design — settled in 1.3, **out of scope** for 1.4) and *Windows .bat build lessons* (CRLF, flat errorlevel gotos, close-Reaper-before-copy) for whatever build aid the forced-failure run uses.

## Story Completion Status

Ultimate context engine analysis completed — comprehensive developer guide created. Status: **ready-for-dev**.

## Open Questions for Antho (non-blocking — sensible defaults chosen)

1. **Forced-failure switch mechanism** — defaulting to a CMake/compiler `-D RAV_FORCE_INIT_FAILURE` build define (clean, leaves zero trace in normal builds). If you'd rather have a dedicated `build_forcefail.bat` alongside `build.bat` so you can one-click the test build, say so and the dev will add it. *(Default: the `-D` define documented in the validator gate.)*
2. **Diagnostic wording** — the dev will write plain, non-jargon lines ("update your GPU driver", "Reaper is unaffected"). If you have a preferred house phrasing for user-facing console text, drop it and it'll be matched; otherwise the dev's wording stands.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Opus 4.8, 1M context) — dev-story workflow, 2026-06-24.

### Debug Log References

- `cmake -S . -B /tmp/rav_build` → configures cleanly, exit 0 (non-Windows branch stubs the target — proves CMake syntax only, as expected on this Linux/WSL box; the MSVC build + GL runtime are Antho's Windows gate).
- `grep -rniE 'imgui|reaimgui' src/` → none (AC5 honored).
- `git status` on `src/reaper_api.h` / `src/reaper_api.cpp` / `CMakeLists.txt` → unchanged (no new Reaper symbol, no build change). The only GL added is `glGetString(GL_VERSION/GL_RENDERER)` — core GL 1.1 from the already-linked `opengl32`.

### Completion Notes List

**What was implemented (failure-path-only; success path untouched):**

- **AC1 — readable diagnostics + GL version probe.** Reworded every `StartRendering` failure `LogError` into a single-line, non-jargon `[RAV] error:` message ending in "the viewer did not open (Reaper is unaffected)" (D7 single-line). Added a GL-version probe (`glGetString(GL_VERSION)`/`GL_RENDERER`, null-guarded) right after `wglMakeCurrent` and before `LoadGlFunctions`, so the common "old driver" failure now leads with a human remedy ("OpenGL 3.3+ required but your driver reports … — update your GPU driver") *and* keeps the precise `missing GL function: X` line for log-mining. ([src/viewer_window.cpp](../../src/viewer_window.cpp) `StartRendering`)
- **AC2 — fully symmetric teardown-on-bail.** Closed the one real asymmetry: on a hard create-failure (`CreateWindowExW` null after WM_CREATE returned -1) the window class is now unwound via a new idempotent `UnregisterViewerClass()` helper (also used by `CloseViewerWindow`, removing duplicated logic). Verified + claimed the 1.3-deferred `g_hdc`/`WindowFromDC` dangle fix and the idempotent `StopRendering()` in `WM_CREATE`; added a WHY comment on the "no `WM_DESTROY` after `WM_CREATE` returns -1" Win32 fact so it isn't "simplified" away. Confirmed by inspection nothing on the open/bail path calls `g_register("-…")` — the plugin-entry registrations stay intact on a failed open (not over-reversed).
- **AC3 — no-throw host boundary.** Wrapped the body of `ToggleViewerWindow` in `try { … } catch (const std::exception&) { … } catch (…) { … }`, covering both the open and close/toggle legs in one thin guard; each catch logs a console diagnostic and calls the idempotent `CloseViewerWindow()` so no half-built window survives. Added `#include <exception>`.
- **AC4 — success path unchanged.** No edits to any success-path machinery (render tick / timer / fps / QPC / vsync / docking / hide-park / toggle). Diff is failure-branches + one try/catch + one test define only.
- **AC5 — ReaImGui stays out.** No ReaImGui detection/header/call added (`grep` clean).
- **Task 4 — forced-failure switch.** `#ifdef RAV_FORCE_INIT_FAILURE` at the top of `LoadGlFunctions` returns false immediately (default build never defines it → zero runtime cost, no `/W3` warning). Injecting at the GL-load step also drives the new version-probe diagnostic. Flip: `-DCMAKE_CXX_FLAGS="/D RAV_FORCE_INIT_FAILURE"`.

**Handoff to Antho (Windows MSVC + Reaper — this dev box has no MSVC/Reaper/GL, same as 1.1–1.3):** run the new §5 of `docs/PHASE0.5_VALIDATOR_GATE.md` — (A) forced-failure build → confirm readable diagnostic + no panel docks + Reaper survives + clean exit, zero `/W3 /permissive-` warnings; (B) rebuild without the define → re-run the §4 Story 1.3 checklist to confirm the happy path is unchanged. Per the sequencing note, run 1.4's Windows validation after 1.3's gate clears (shared `viewer_window.cpp`, same build).

**Deviations:** none (AR20). Extracted `UnregisterViewerClass()` as a small DRY helper for the now-two unregister sites — behaviour identical to 1.3.

### File List

- `src/viewer_window.cpp` (modified) — readable diagnostics + GL version probe; `UnregisterViewerClass()` helper + class-unwind on create-failure bail; WM_CREATE WHY comment; no-throw try/catch boundary in `ToggleViewerWindow`; `#include <exception>`.
- `src/gl_loader.cpp` (modified) — `RAV_FORCE_INIT_FAILURE` test-only forced-failure guard in `LoadGlFunctions`.
- `docs/PHASE0.5_VALIDATOR_GATE.md` (modified) — new §5 Story 1.4 init-failure gate (forced-failure run + success-path regression) + Windows handoff.
- `build_forcefail.bat` (new) — one-click forced-failure build+install (`RAV_FORCE_INIT_FAILURE`) so Antho can run the §5 test without typing CMake flags; pairs with the normal `build.bat` for the revert step.

No new files, no CMake change, no new dependency, no new Reaper API symbol; `viewer_window.h` / `reaper_api.h` unchanged.

### Change Log

| Date | Change |
|---|---|
| 2026-06-24 | Story drafted (create-story workflow) — init-failure hardening: user-readable diagnostics + GL version probe, symmetric teardown-on-bail (window-class unwind, owns the 1.3 `g_hdc` dangle), no-throw Action boundary (AR18), forced-failure test define, validator-gate §5. ReaImGui-missing explicitly out (Epic 5). Status → ready-for-dev. |
| 2026-06-24 | Antho Windows validator gate §5 PASSED — (A) forced-failure build (`build_forcefail.bat`, `RAV_FORCE_INIT_FAILURE`) → clear console diagnostic, no panel docks, Reaper stays stable; (B) normal rebuild (`build.bat`) → success path unchanged. Added `build_forcefail.bat` (one-click forced-failure build+install) per open-question option. Remaining before `done`: code-review. |
| 2026-06-24 | Dev implementation (dev-story workflow). All 6 tasks complete: readable `StartRendering` diagnostics + GL-version probe; `UnregisterViewerClass()` helper + class-unwind on hard create-failure bail + WM_CREATE WHY comment (AC2); no-throw try/catch boundary in `ToggleViewerWindow` (AC3); `RAV_FORCE_INIT_FAILURE` test guard in `gl_loader.cpp` (Task 4); validator-gate §5 added. CMake configures clean; no new symbols/ReaImGui/CMake change. Windows MSVC build + Reaper runtime gate handed to Antho. Status → review. |

## Review Findings

Code review (bmad-code-review, 3 parallel layers: Blind Hunter + Edge Case Hunter + Acceptance Auditor), 2026-06-24. Acceptance Auditor verdict: **contract met — all of AC1–AC5 satisfied, no forbidden touches** (no new Reaper symbol, no ReaImGui, no CMake change, success path untouched).

- [x] [Review][Patch] AR18 no-throw boundary has a hole: an exception from `StartRendering` (the `std::string err` alloc / `g_renderer.Init`) runs inside `WM_CREATE`, i.e. synchronously **behind `CreateWindowExW`/USER32**, so it cannot reliably reach the `try/catch` in `ToggleViewerWindow` — under MSVC `/EHsc`, throwing across the Win32 callback boundary is UB / `std::terminate` (a host crash, violating AR17). This is exactly the `std::bad_alloc`-from-`err` case AR18 names. **FIXED 2026-06-24**: wrapped the `StartRendering` call in `WM_CREATE` in its own `try/catch (std::exception / ...)` that logs + `StopRendering()` + `return -1`, so no exception ever crosses the USER32 frame. [src/viewer_window.cpp:284-309]
- [x] [Review][Defer] DC leak on a *stale* `g_hwnd`: `CloseViewerWindow`'s `!IsWindow(g_hwnd)` branch → `StopRendering` → `DestroyGLContext`, where `WindowFromDC(g_hdc)` returns null for an already-destroyed window, so `ReleaseDC(null, g_hdc)` silently fails and the DC leaks. [src/viewer_window.cpp:96] — deferred, pre-existing (the `WindowFromDC` fallback ships in HEAD/1.3; narrow path — Reaper would have to destroy our child window out from under us, and it occurs only at unload where the single-DC leak is harmless).

**Dismissed as noise (3):**
- *C4702 "unreachable code" from the `RAV_FORCE_INIT_FAILURE` early-`return false`* — C4702 is an MSVC `/W4`-only warning; the build is `/W3 /permissive-`, and Antho already compiled the forced-failure config clean for the §5 gate. No `/W3` warning.
- *The `catch` handlers' own `CloseViewerWindow()`/`LogError()` could throw and escape* — neither allocates: `CloseViewerWindow` is pure Win32 + `UnregisterViewerClass`, and `LogError` is a variadic `vsnprintf`→`ShowConsoleMsg` ([console_log.h](../../src/console_log.h)). No realistic throw source in the cleanup path.
- *Catch-block wording `"viewer open/close failed: …"` differs from the spec's suggested string* — compliant and more accurate (the guard wraps both legs); still single-line D7 with the "Reaper is unaffected" tail.

> Hygiene note (not a code finding): `build_spike.bat` is untracked on this branch and is **not** part of Story 1.4's File List — keep it out of the 1.4 commit (stage only `src/viewer_window.cpp`, `src/gl_loader.cpp`, `docs/PHASE0.5_VALIDATOR_GATE.md`, `build_forcefail.bat`, and the story/sprint files).

| 2026-06-24 | Code review (bmad-code-review, 3 layers). Acceptance Auditor: contract met (AC1–AC5). 1 patch applied — AR18 no-throw boundary extended into `WM_CREATE` (exception can no longer cross the `CreateWindowExW`/USER32 frame; AR17 host-crash hole closed). 1 defer (pre-existing 1.3 DC-leak on stale `g_hwnd` → deferred-work.md). 3 dismissed. Status → done. **Carry-along for next Windows build:** the new `WM_CREATE` try/catch (mirrors the existing `ToggleViewerWindow` guard) should ride Antho's next MSVC rebuild to confirm `/W3 /permissive-` zero-warning — low risk, no behaviour change on the success path. |
