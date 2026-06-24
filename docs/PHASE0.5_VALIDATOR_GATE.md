# Phase 0.5 — Validator Gate

Step-by-step acceptance test for **Phase 0.5** (Epic 1 — the viewer lives inside
Reaper). Written for Antho (the validator) to run on Windows. This file grows as
Epic 1 progresses: **Story 1.2** seeds the rows below; Story 1.3 (docking) and
Story 1.4 (init-failure hardening) append their own.

**You are testing the right thing if** the extension still loads, the
`RAV: Open Viewer` action opens a window that shows an **animated, correctly
oriented** 3D scene at **≥60 fps**, resizing it stays clean, and closing it
leaves Reaper stable.

---

## Prerequisites

- Reaper 7.x (`caller_version == 0x20E`, current SDK targets 7.72)
- Visual Studio 2022 with **Desktop development with C++**
- CMake ≥ 3.20 and Git for Windows on PATH
- A network connection on the **first** configure (CMake `FetchContent` clones GLM 1.0.3)

> Epic 1 still has **no ReaImGui dependency** (deferred to Epic 5 per the Spike 0
> Spec Change Log). The viewport is a native OpenGL window.
>
> Build under test = the **post-code-review** `main` (2026-06-23): eager first-frame
> paint (no open flash), cached camera matrices, correct normal matrix, fixed GL
> state set once. Behaviour is unchanged for these checks; re-running this gate
> just re-confirms the corrected build.

## 1. Build

```cmd
cd \path\to\ReaAnimViewer
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**Expected:** GLM 1.0.3 is fetched on first configure, **zero MSVC warnings at
`/W3 /permissive-`**, and a DLL at `build\Release\reaper_animviewer.dll`.

## 2. Install

```cmd
copy build\Release\reaper_animviewer.dll "%APPDATA%\REAPER\UserPlugins\"
```

Close and reopen Reaper so it re-scans extensions.

## 3. Story 1.2 — acceptance checks

| # | Check | Pass criterion | AC / NFR |
|---|---|---|---|
| 1 | Load is fast | Console shows `[RAV] info: extension loaded …`; Reaper is responsive in **< 2 s** after launch. | AC4 / NFR-P4 |
| 2 | Action opens viewer | `Actions → Show action list…`, filter `RAV`, run `RAV: Open Viewer`. A window titled `ReaAnimViewer` opens. | AC1 / AC4 |
| 3 | Scene is correct & animated | A **rotating cube above a grey ground** is visible. It **spins smoothly**, the **top face (green) stays up**, faces are depth-sorted (no see-through), lighting reads as 3D. "Up" is unambiguous. | AC1 |
| 4 | Frame rate ≥ 60 fps | Console prints `[RAV] info: NN.N fps  (WxH)` once per second, at **≥ 60**. (Vsync ships **off** so the headroom is visible — a number well above 60 is expected.) | AC2 / NFR-P1 |
| 5 | Independent of UI rate | The fps line stays ≥ 60 even while Reaper's UI is idle — the render is timer-driven, not tied to Reaper's ~30 Hz loop. | AC2 |
| 6 | Resize stays clean | Drag the window edges/corner. The scene **re-fills** the client area and **keeps aspect** (the cube is not stretched). **No flicker, no garbage, no crash.** | AC3 |
| 7 | Close is clean | Close the window. Reaper stays stable. Re-run the action → a fresh window opens and animates again (no stale class, no leaked context/timer). | AC4 / NFR-R3 |
| 8 | Shutdown is clean | Close Reaper. It exits with **no crash dialog**, no orphan window. | AC4 / NFR-R3 |

If **any** row fails, capture the Reaper version, the console text, and a
screenshot (for visual failures), and report back.

> **Known fallback (Spike 0 note):** if the window opens but the scene is missing
> and the console shows a shader **compile/link** error, the fix is a
> `wglCreateContextAttribsARB` 3.3-core context (the legacy `wglCreateContext`
> profile was sufficient on the spike's reference workstation). Flag it — don't
> work around it.

## What this gate proves (Story 1.2)

- The direct-render-into-the-window engine reaches ≥ 60 fps on a timer
  independent of Reaper's UI loop — the load-bearing proof every later epic
  renders through (Spike 0 Finding 1, now production code on `main`).
- The renderer / GL-loader / RAII-handle / console-log structure that Epics 2–3
  build on is in place and leak-free across open/close.

---

## 4. Story 1.3 — docking acceptance checks

The viewport is now a **dockable panel** (a `WS_CHILD` window handed to Reaper's
docker via `DockWindowAddEx`), not a free-floating top-level window. Build +
install exactly as in §1–2, then run `RAV: Open Viewer` and check:

| # | Check | Pass criterion | AC / NFR |
|---|---|---|---|
| 1 | Action docks the panel | Run `RAV: Open Viewer`. The viewport appears as a **tab in a Reaper docker** (not a separate floating top-level window). | AC1 / AC2 |
| 1b | Action is a toggle | Run `RAV: Open Viewer` **again while the panel is open** → it **closes**. Run it once more → it **re-opens / re-docks**. In the action list the entry shows a **checkmark (on/off)** that tracks the panel; a toolbar button assigned to it lights up while open. | AC1 |
| 2 | No standalone window | There is **no** free-floating `ReaAnimViewer` top-level window anywhere — the viewport exists only as the docked panel. | AC2 |
| 3 | Docks into any docker | Drag the panel into **each** docker edge — top, bottom, left, right — and **float** it. It renders correctly in every position and floats fine. | AC1 |
| 4 | ≥ 60 fps while docked | With the panel docked, the console `[RAV] info: NN.N fps (WxH)` line still reads **≥ 60**. (Vsync ships **off** — a number well above 60 is expected, at the real docked size.) | AC4 / NFR-P1 |
| 5 | Resize via docker stays clean | Drag the docker splitter (or the float's edges). The scene **re-fills + keeps aspect** — **no flicker, garbage, or crash**. | AC4 |
| 6 | Docker-X close parks | Close the panel via the **docker tab's close button (X)**. The console prints `[RAV] info: panel hidden — render paused` and the `fps` lines **stop** — no rendering, no fps, no GPU work (Reaper hides the panel and sends us no message, so we park rather than destroy). Reaper stays stable. Re-run `RAV: Open Viewer` → the panel **re-shows** and resumes (`panel shown — render resumed`). To fully destroy + free the GL context, run the Action while the panel is open (the **toggle** — see row 1b). Repeat a few times. | AC3 / NFR-R3 |
| 6b | Minimize / tab-switch survive | With the panel docked, **minimize Reaper** then restore it (and/or, if it shares a docker with other tabs, switch away and back). The render parks while hidden and **resumes** when shown — the panel is **never** destroyed or left blank/white. | AC3 / NFR-R3 |
| 7 | Dock position persists | With the panel docked, **restart Reaper**. Re-run the Action → the panel returns to its **last dock position** (the `identstr` persistence round-trip). | AC1 |
| 8 | Shutdown is clean | Close Reaper with the panel open. It exits with **no crash dialog**, no orphan window. | AC3 / NFR-R3 |
| 9 | Load stays fast | Console still shows `[RAV] info: extension loaded …`; Reaper responsive in **< 2 s** (no regression from 1.1/1.2). | AC4 / NFR-P4 |

If **any** row fails, capture the Reaper version, console text, and a screenshot,
and report back.

### What this gate proves (Story 1.3)

- The proven 1.2 render engine now lives **inside Reaper as a dockable panel**
  (FR27 reinterpreted: a docked GL viewport via `DockWindowAddEx`, FR28/FR29) —
  the user-facing point of Epic 1.
- The dock / undock / docker-X-close / reopen / unload lifecycle is **leak-free**
  on the main thread (NFR-R3 / AR15 / AR18): the engine's ≥ 60 fps, clean resize,
  and leak-free teardown all survive the move into the docker.

---

## 5. Story 1.4 — init-failure hardening acceptance checks

Story 1.4 makes the **failure** path as disciplined as the success path: a
user-readable console diagnostic, a fully symmetric teardown-on-bail (no leaked
GL context / DC / timer / window class), and a no-throw boundary so a failed open
can never crash Reaper. It changes **nothing** on the success path.

These ACs **cannot** be triggered on working hardware (your reference GPU
initializes fine), so the failure path is exercised with a **compile-time
forced-failure switch**, `RAV_FORCE_INIT_FAILURE`. The gate is a
build-with → test → rebuild-without loop.

> **Note on the forced diagnostic:** with the switch on, the failure is injected
> at the modern-GL load step, so the console will show the readable
> *"OpenGL 3.3+ required but your driver reports `<X>` on `<Y>` — update your GPU
> driver…"* line. On your real GPU the reported version (`<X>`) is your **actual**
> driver version — that's expected and fine; the point being tested is that a
> **clear diagnostic appears, no window docks, and Reaper survives**, not the
> literal version number.

### A — forced-failure path (build *with* the switch)

```cmd
cmake -B build-forcefail -G "Visual Studio 17 2022" -A x64 -DCMAKE_CXX_FLAGS="/D RAV_FORCE_INIT_FAILURE"
cmake --build build-forcefail --config Release
copy build-forcefail\Release\reaper_animviewer.dll "%APPDATA%\REAPER\UserPlugins\"
```

Close & reopen Reaper, then:

| # | Check | Pass criterion | AC / NFR |
|---|---|---|---|
| 1 | Build is clean | **Zero `/W3 /permissive-` warnings** even with the switch on; DLL produced. | AC4 / NFR-R5 |
| 2 | Clear diagnostic on failure | Run `RAV: Open Viewer`. The console shows a **readable** `[RAV] error:` line naming what failed and ending with **"the viewer did not open (Reaper is unaffected)"**. | AC1 / AR16 |
| 3 | No broken window docks | **No** viewport panel/tab appears in any docker — nothing half-built is left behind. | AC1 / AC2 |
| 4 | Reaper survives | Reaper stays **fully responsive** — no freeze, no crash dialog. Run the Action **again** → same diagnostic, still no crash (the Action stays registered and re-runnable). | AC1 / AC3 / AR17 |
| 5 | No stray render | **No** `[RAV] info: NN.N fps` lines appear afterward (the render loop never started / was torn down). | AC2 / NFR-R3 |
| 6 | Clean shutdown after a failed open | Close Reaper → exits with **no crash dialog**, no orphan window (the bail leaked no context / DC / timer / window class). | AC2 / NFR-R3 |

### B — success-path regression (rebuild *without* the switch)

Rebuild the **normal** way (no `/D`, as in §1) and re-run the **entire Story 1.3
checklist (§4, rows 1–9)**: the panel docks, drags into each docker edge + floats,
reads **≥ 60 fps** docked, resizes cleanly, toggles closed + reopens, the dock
position **persists across a Reaper restart**, and Reaper exits clean. **Nothing
about the happy path may have changed** — 1.4 only touched the failure branches.

Delete the throwaway `build-forcefail\` directory when done.

### What this gate proves (Story 1.4)

- A failed viewer open emits a **clear, actionable console diagnostic** and
  **never crashes or hangs Reaper** (AR16 / AR17 / AR18 — the no-throw boundary).
- The bail path is **fully symmetric**: no leaked GL context, DC, frame timer, or
  window class, while the plugin-entry registrations stay intact so the Action
  remains usable (NFR-R3 / AR15).
- The success path is **byte-for-byte unchanged** (the §4 regression), closing
  Epic 1 / Phase 0.5.
