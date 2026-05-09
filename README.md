# FBX Animation Viewer

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
git clone <repo-url> FBXAnimationViewer
cd FBXAnimationViewer
git submodule update --init --recursive
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/Release/reaper_fbxanimationviewer.dll`.

## Install

Copy `reaper_fbxanimationviewer.dll` into:

```
%APPDATA%\REAPER\UserPlugins\
```

Restart Reaper. Open **Actions → Show action list…** and search `FBXAV`. You
should see `FBXAV: Open Viewer Window`.

## Validator Gate (Phase 0)

Detailed Phase 0 acceptance test instructions live in
[`docs/PHASE0_VALIDATOR_GATE.md`](docs/PHASE0_VALIDATOR_GATE.md).

## Vendored dependencies

These ship as git submodules under `extern/`:

| Path | Source | Pinned commit |
|---|---|---|
| `extern/reaper-sdk` | https://github.com/justinfrankel/reaper-sdk | _set on submodule add_ |
| `extern/WDL` | https://github.com/justinfrankel/WDL | _set on submodule add_ |

Pinned commit hashes are recorded in `.gitmodules` and locked by the gitlink
in the parent tree. Always run `git submodule update --init --recursive`
after a fresh clone or pull.

## License

MIT — see [`LICENSE.md`](LICENSE.md).

Released free for the game audio community by Demute Studio.
