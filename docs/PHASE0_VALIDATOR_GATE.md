# Phase 0 — Validator Gate

This document is the step-by-step acceptance test for **Phase 0** of the
ReaAnimViewer build plan. It is written for Antho (the validator)
to run on Windows.

**You are testing the right thing if** the extension loads in Reaper, an
action shows up in the Action List, triggering it opens an empty dark
grey window, re-triggering it just raises that window, and closing
Reaper exits cleanly with no crash.

**You are not testing yet** any animation, mesh, or transport features
— those are Phase 1+.

---

## Prerequisites

- Reaper 7.x installed (any 7.x with `caller_version == 0x20E` — current SDK targets 7.72)
- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake ≥ 3.20 on PATH
- Git for Windows

> **Phase 0 only** uses Win32 + WGL directly and has **no ReaImGui dependency**. From Phase 0.5 onward, the viewer refactors to a ReaImGui dockable panel (per PRD architectural decision 2026-05-10) and ReaImGui (`cfillion/reaimgui` via ReaPack) becomes a runtime prerequisite. This document covers the Phase 0 acceptance test only — see the PRD (`_bmad-output/planning-artifacts/prd.md`) for the post-Phase-0.5 install flow.

## 1. Build

Open a Developer Command Prompt for VS 2022 (or any shell where `cmake.exe` and `cl.exe` are on PATH) and run:

```cmd
cd \path\to\ReaAnimViewer
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**Expected:** zero MSVC warnings at `/W3`, and a file at:

```
build\Release\reaper_animviewer.dll
```

**If configure fails** with a missing-module error referencing
`ReaperPlugin`, verify `cmake/ReaperPlugin.cmake` exists in your
working tree.

**If link fails** complaining about `wgl*` / `gl*` symbols, verify the
`opengl32 gdi32 user32` link line in `CMakeLists.txt` survived your
clone.

## 2. Install the DLL into Reaper

Copy the DLL to your user plugins directory:

```cmd
copy build\Release\reaper_animviewer.dll "%APPDATA%\REAPER\UserPlugins\"
```

If Reaper is already running, **close and reopen it** so it re-scans
extensions.

## 3. Acceptance checks

| # | Check | Pass criterion |
|---|---|---|
| 1 | Reaper launches | No crash dialog, no error popup. The Reaper window opens normally. |
| 2 | Console log on load | `Extensions → Show ReaScript console output` (or `View → Show console`) shows `[RAV] extension loaded (Phase 0)`. |
| 3 | Action registered | `Actions → Show action list…` then type `RAV` in the filter. The row `RAV: Open Viewer` is present. |
| 4 | Action opens the window | Double-click that row (or click `Run`). A 800×600 window titled `ReaAnimViewer` appears. The client area is a uniform dark grey (≈ RGB 26,26,31). |
| 5 | Re-trigger raises | With the viewer window open, run the action again. **No second window.** The existing one comes to the foreground. |
| 6 | Resize redraws | Drag the window edge. Background stays uniform dark grey across the new size — no stretched garbage, no flicker, no white flash. |
| 7 | Close window | Click the window's `X`. The window vanishes. Reaper keeps running normally. Run the action again — a fresh window opens. |
| 8 | Reaper shutdown | Close Reaper. Reaper exits cleanly. **No "this program has stopped responding" dialog**, no orphan window left on the desktop, no taskbar ghost. |

If **any** check fails, capture:

- The Reaper version (`Help → About REAPER`)
- The exact text of any error dialog
- The contents of the ReaScript console
- Screenshots if the failure is visual

…and report back. Do **not** continue to Phase 1 until every row above is green.

## 4. Uninstall (if needed)

```cmd
del "%APPDATA%\REAPER\UserPlugins\reaper_animviewer.dll"
```

Then restart Reaper. The action disappears from the Action List.

## What this gate proves

- The build toolchain produces a DLL Reaper can load.
- The plugin entry point honors the `caller_version` handshake.
- The `gaccel` + `command_id` + `hookcommand` registration pipeline is wired correctly.
- The Win32 + WGL window can be created, painted, and destroyed inside Reaper's process without leaks or crashes.

Everything in Phases 1–5 (mesh loading, skinning, transport sync, file
reload, polish) builds on this foundation. If Phase 0 is green, the
risk that the next phase blocks on infrastructure debugging is low.
