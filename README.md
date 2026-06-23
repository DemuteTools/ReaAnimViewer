# ReaAnimViewer

A Reaper extension that loads glTF and FBX animation files and renders the
animated rig live, synced to the Reaper transport. Designed for game audio
sound designers who currently rely on screen captures of engine playback.

**Status:** Phase 0 — scaffolding. Loads in Reaper and opens an empty viewer
window. No mesh rendering yet.

See `_bmad-output/planning-artifacts/prfaq-FBXAnimationViewer.md` for the
full concept, MVP scope, and 6-week build plan.

## Build (Windows x64, MSVC)

Prerequisites:

- Reaper 7.x installed
- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake 3.20 or newer
- Git (for submodules)

Steps:

```sh
git clone <repo-url> ReaAnimViewer
cd ReaAnimViewer
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/Release/reaper_animviewer.dll`.

## Install

**Prerequisite (Phase 0.5+): ReaImGui must be installed.** The viewer renders as a ReaImGui dockable panel. Install via ReaPack:

1. In Reaper: **Extensions → ReaPack → Browse packages…**
2. Search `reaimgui`, install the `cfillion/reaimgui` package.
3. Restart Reaper.

Once the ReaAnimViewer ReaPack listing is live (Phase 5), ReaImGui will be declared as an auto-install dependency — you won't need this step manually. For the current pre-release build distributed by copying the DLL, install ReaImGui first as described above.

Then copy `reaper_animviewer.dll` into:

```
%APPDATA%\REAPER\UserPlugins\
```

Restart Reaper. Open **Actions → Show action list…** and search `RAV`. You
should see `RAV: Open Viewer`.

> **Phase 0 note (2026-05-09):** the shipped Phase 0 DLL opens a standalone Win32 window (no ReaImGui dependency yet). Phase 0.5 will refactor this into a ReaImGui dockable panel; the ReaImGui prerequisite above takes effect from that build onward.

## Validator Gate (Phase 0)

Detailed Phase 0 acceptance test instructions live in
[`docs/PHASE0_VALIDATOR_GATE.md`](docs/PHASE0_VALIDATOR_GATE.md).

## Vendored dependencies

The Reaper Extension SDK headers required for Phase 0 are committed
directly under `extern/reaper-sdk/sdk/`. No `git submodule init` step is
needed. See [`extern/VENDORED.md`](extern/VENDORED.md) for upstream
source URLs, pinned commits, and the re-vendoring procedure.

WDL/SWELL is referenced by the project's stack but not required for
Phase 0 (Windows uses native Win32 + WGL). It will be vendored when the
Linux/macOS port lands.

## License

MIT — see [`LICENSE.md`](LICENSE.md).

Released free for the game audio community by Demute Studio.
