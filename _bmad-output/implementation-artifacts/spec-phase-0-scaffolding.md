---
title: 'Phase 0 — Repo scaffolding, build system, Reaper extension boilerplate, empty viewer window'
type: 'feature'
created: '2026-05-09'
status: 'in-progress'
baseline_commit: 'NO_VCS'
context:
  - '{project-root}/_bmad-output/planning-artifacts/prfaq-FBXAnimationViewer.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** The FBXAnimationViewer project has no code, no build system, and no proven path from `git clone` to a Reaper extension that loads. Without that foundation, every later phase (mesh rendering, skinning, transport sync) blocks on infrastructure debugging instead of feature work. Risk #0 in the PRFAQ is exactly this: Reaper Extension SDK first-run friction.

**Approach:** Stand up a CMake project that vendors `reaper-sdk` and `WDL` as git submodules, builds a single `reaper_fbxanimationviewer.dll` against MSVC on Windows x64, and ships a minimal extension whose only job is (a) register an Action in Reaper and (b) when triggered, open a SWELL/Win32 window hosting an empty WGL context. Add a ReaPack manifest skeleton for later distribution. No rendering yet — just the plumbing proven end-to-end.

## Boundaries & Constraints

**Always:**
- DLL name **must** start with `reaper_` (Reaper's loader scans for that prefix).
- Plugin entry point is `REAPER_PLUGIN_ENTRYPOINT`; honor the `caller_version` check; return 0 cleanly when `rec == nullptr` (unload path).
- Build is cross-platform-portable in CMake structure (no hard-coded MSVC-only logic at top level), but Phase 0 only needs to compile on Windows x64 — Linux/macOS targets may stub.
- Vendor SDKs as git submodules under `extern/` — never copy sources into the tree.
- C++17, no exceptions in plugin entry path (Reaper is a hostile host — crash = lose the project).
- License headers: short SPDX-style line on each new source file (`// SPDX-License-Identifier: MIT`).

**Ask First:**
- Anything that requires creating an external GitHub repo or pushing a remote.
- Adding any dependency not listed in the PRFAQ build stack (sokol_gfx, assimp, GLM are Phase 1+ — do not pull yet).
- Any deviation from the submodule strategy (e.g. switching to FetchContent).

**Never:**
- Do not implement any rendering, mesh loading, animation, or transport sync — those are Phases 1–3.
- Do not import Autodesk FBX SDK — assimp covers FBX (PRFAQ explicit).
- Do not link against `WDL` libraries we don't need yet — only what SWELL/Win32 window opening requires.
- Do not commit binaries, build outputs, or VS solution files (`.gitignore` covers this).
- No `--no-verify` git commits, no `git push --force` anywhere.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|---|---|---|---|
| Reaper loads plugin (happy path) | DLL placed in `%APPDATA%\REAPER\UserPlugins\`, Reaper started | Plugin registers, action `FBXAV: Open Viewer Window` appears in Action List | N/A |
| Action triggered | User runs the registered action | An empty top-level window opens with a valid WGL context, GL viewport cleared to a fixed color (e.g. dark grey 0.1, 0.1, 0.12) | If GL context creation fails: write error via `ShowConsoleMsg`, do not crash |
| Plugin unload | Reaper shuts down (rec == nullptr second call) | Window destroyed, GL context released, action unregistered | N/A |
| Stale caller_version | Reaper version mismatch | Return 0 from entry point, no registration, no window | N/A |
| Re-trigger action while window already open | Action fired a second time | Bring existing window to front (do not open a duplicate) | N/A |

</frozen-after-approval>

## Code Map

- `CMakeLists.txt` -- top-level project, MSVC + cross-platform stubs, defines `reaper_fbxanimationviewer` shared lib target
- `cmake/ReaperPlugin.cmake` -- helper function `add_reaper_extension(target ...)` enforcing `reaper_` prefix and platform suffix
- `extern/reaper-sdk/` -- submodule from `https://github.com/justinfrankel/reaper-sdk` (provides `reaper_plugin.h`, `reaper_plugin_functions.h`)
- `extern/WDL/` -- submodule from `https://github.com/justinfrankel/WDL` (provides SWELL headers; on Windows mostly passthrough)
- `src/plugin_main.cpp` -- `REAPER_PLUGIN_ENTRYPOINT` impl: API loader, action registration, hook for action callback
- `src/plugin_main.h` -- shared types, action ID, version constants
- `src/reaper_api.cpp` / `.h` -- thin wrapper to load the subset of Reaper API functions we need (ShowConsoleMsg, plugin_register, AddRemoveReaScript-style action hook)
- `src/viewer_window.cpp` / `.h` -- creates Win32 window (Phase 0: Windows direct; SWELL macros allowed but not required), creates WGL context, runs message pump, on close releases GL context
- `reapack/index.xml` -- ReaPack manifest skeleton (one extension entry, version `0.0.1-phase0`, no source URL — placeholder)
- `.gitignore` -- VS build artifacts, `build/`, `out/`, `.vs/`, `*.user`
- `.gitmodules` -- pins for reaper-sdk and WDL
- `LICENSE.md` -- MIT, holder "Demute Studio"
- `README.md` -- 1-page install/build guide for Antho's validator gate
- `docs/PHASE0_VALIDATOR_GATE.md` -- step-by-step Reaper test instructions

## Tasks & Acceptance

**Execution (one atomic commit per task):**
- [ ] `git init` in repo root, default branch `main`, then `.gitignore`, `LICENSE.md`, `README.md` -- bootstrap version control + legal/docs surface so subsequent commits are diffable
- [ ] `extern/reaper-sdk/` and `extern/WDL/` -- add as submodules pinned to a known-good commit; document the pinned commit hashes in `README.md`
- [ ] `CMakeLists.txt` + `cmake/ReaperPlugin.cmake` -- define `reaper_fbxanimationviewer` SHARED target; on Windows force `reaper_` filename prefix and `.dll` extension; expose `REAPER_SDK_DIR`/`WDL_DIR` cache vars; require C++17
- [ ] `src/plugin_main.{h,cpp}` -- entry point, `caller_version` check, API loader bootstrap, `plugin_register("hookcommand", ...)` for the open-window action
- [ ] `src/reaper_api.{h,cpp}` -- minimal API loader: `ShowConsoleMsg`, `AddExtensionsMainMenu`, `plugin_register`, `KBD_OnMainActionEx` symbol resolution from `rec->GetFunc`
- [ ] `src/viewer_window.{h,cpp}` -- `OpenViewerWindow()` / `CloseViewerWindow()`; Windows: `RegisterClass`, `CreateWindowEx`, `wglCreateContext`, message pump on a worker thread or modeless via Reaper's main loop (prefer modeless + Reaper hook); clear color on `WM_PAINT`; singleton-guard re-trigger
- [ ] `reapack/index.xml` -- minimal `<index>` with one `<reapack type="extension">` entry, version `0.0.1-phase0`, MIT metadata, no real source URL yet (comment placeholder)
- [ ] `docs/PHASE0_VALIDATOR_GATE.md` -- Antho-facing checklist: clone, submodule init, cmake configure, build, copy DLL path, Reaper actions test

**Acceptance Criteria:**
- Given a fresh clone of the repo on Windows with VS2022 installed, when Antho runs `git submodule update --init --recursive` then `cmake -B build -G "Visual Studio 17 2022" -A x64` then `cmake --build build --config Release`, then a file `build/Release/reaper_fbxanimationviewer.dll` exists and links cleanly with zero MSVC warnings at level `/W3`.
- Given the built DLL is copied to `%APPDATA%\REAPER\UserPlugins\` and Reaper is launched, when Antho opens the Action List and searches "FBXAV", then the action `FBXAV: Open Viewer Window` is listed.
- Given the action is run, when no viewer window is currently open, then a top-level window appears titled `FBX Animation Viewer` with a non-black GL clear color visible inside the client area.
- Given the action is run a second time while the window is already open, when the action fires, then the existing window is brought to the foreground (no duplicate window).
- Given Reaper is closed, when the user has interacted with the viewer window, then Reaper exits cleanly with no crash dialog and no stuck child windows.
- Given the repo is opened on Linux or macOS with CMake, when `cmake -B build` runs, then configuration succeeds (build target may be a stub or no-op — Phase 0 only requires Windows to actually link).

## Spec Change Log

## Design Notes

**Why direct Win32 over SWELL on Windows for Phase 0:** On Windows SWELL is mostly a passthrough — direct Win32 + WGL is fewer moving parts for the validator gate. The window code stays in one file (`viewer_window.cpp`) so swapping it for SWELL macros later (when Linux/macOS port lands) is a single-file refactor. Choosing SWELL prematurely would force us to debug both layers if the gate fails.

**Window lifecycle / threading:** Reaper owns the main message pump. Creating a separate thread for the GL window is tempting but historically fragile in Reaper extensions (timer/UI APIs are main-thread-only). Phase 0 uses a modeless top-level window registered via Reaper's `Main_OnCommand` hook and pumped through the host's message loop. Worker-thread rendering is a Phase 1+ concern.

**API loading pattern:** Reaper does not link against an import lib — every API function is resolved at load time via `rec->GetFunc("FunctionName")`. Centralize this in `reaper_api.cpp` with a `LOAD_API(name)` macro so Phase 1 can extend it without touching `plugin_main`.

**Submodule pinning:** Pin to specific commits (not branches) so Antho's clone is reproducible. README documents the SHAs. If `reaper-sdk` upstream breaks, we control the upgrade cadence.

## Verification

**Commands (run by Antho on Windows after clone):**
- `git submodule update --init --recursive` — expected: `extern/reaper-sdk` and `extern/WDL` populated
- `cmake -B build -G "Visual Studio 17 2022" -A x64` — expected: configure succeeds, no missing-dependency errors
- `cmake --build build --config Release` — expected: `build/Release/reaper_fbxanimationviewer.dll` produced, zero warnings at `/W3`

**Manual checks (validator gate — Antho in Reaper):**
- DLL copied to `%APPDATA%\REAPER\UserPlugins\` → Reaper launched → no crash dialog, no error in Reaper console.
- Action List → search "FBXAV" → `FBXAV: Open Viewer Window` is present.
- Trigger action → window opens with clear color visible (not black, not white).
- Trigger action again → no second window, existing one comes forward.
- Close Reaper → clean shutdown, no orphan window.

**Sanity check available to me on WSL:**
- `cmake -B build-linux` from project root — expected: configure succeeds (Linux build may be stub-target or skipped). Catches CMake script errors early without needing Windows turnaround.
