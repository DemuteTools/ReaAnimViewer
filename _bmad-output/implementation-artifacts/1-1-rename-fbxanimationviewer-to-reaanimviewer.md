# Story 1.1: Rename FBXAnimationViewer to ReaAnimViewer

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As the maintainer,
I want the project, binary, Reaper Action, manifest, and docs renamed from **FBXAnimationViewer** to **ReaAnimViewer**,
so that the tool's name reflects that it handles glTF/GLB (primary) and FBX — not FBX alone.

This is the **first story of Epic 1 (Phase 0.5)** and runs against the **Phase 0 scaffolding on `main`** (the `spike/0-1-feasibility` branch is throwaway and was never merged — do not touch it). It is a pure rename: no behavior changes, no new features. The Phase 0 extension still does exactly what it does today (registers one Action that opens an empty WGL window); only its *identity* changes.

## Acceptance Criteria

1. **(AC1 — identity renamed everywhere)** Given the Phase 0 codebase named FBXAnimationViewer, when the rename is applied, then:
   - the built artifact is **`reaper_animviewer.dll`**;
   - the registered Reaper Action id/label is **`RAV: Open Viewer`**;
   - source namespaces / SPDX headers, the `reapack/index.xml` manifest, `docs/`, and repo/folder identity all use **"ReaAnimViewer"**.
2. **(AC2 — build + load still clean)** And the build still produces **zero MSVC `/W3 /permissive-` warnings** (NFR-R5) and the extension still **loads in Reaper in under 2 s** (NFR-P4). Renaming must not change build flags, link libraries, or runtime behavior.
3. **(AC3 — no stale strings)** And **no stale `"FBXAnimationViewer"` / `"FBX-only"` (or `FBXAV` / `fbxav` / `reaper_fbxanimationviewer`) string remains** in shipped code, the manifest, or user-facing strings. Planning artifacts under `_bmad-output/` **intentionally retain** the historical names and must **not** be edited.

## Tasks / Subtasks

- [x] **Task 1 — Rename the build target & project (AC: 1, 2)**
  - [x] In [CMakeLists.txt](../../CMakeLists.txt): `project(fbxanimationviewer …)` → `project(reaanimviewer …)`.
  - [x] `add_reaper_extension(fbxanimationviewer …)` → `add_reaper_extension(animviewer …)`. **CRITICAL:** the helper sets `OUTPUT_NAME "reaper_${target}"` (see [cmake/ReaperPlugin.cmake:26](../../cmake/ReaperPlugin.cmake#L26)), so the target name must be **`animviewer`** to produce `reaper_animviewer.dll`. Using `reaanimviewer` would wrongly yield `reaper_reaanimviewer.dll`; using `reaper_animviewer` would yield `reaper_reaper_animviewer.dll`.
  - [x] Update `target_link_libraries(fbxanimationviewer …)` → `target_link_libraries(animviewer …)` (line 24).
  - [x] Update the non-Windows `message(STATUS …)` text and the `add_custom_target(fbxanimationviewer …)` name + echo text (lines 27, 33, 35) to the new identity (target `animviewer`; human text "ReaAnimViewer").
  - [x] Do **not** change `VERSION`, `DESCRIPTION` semantics beyond removing "FBX-only" framing if present (the current description "loads glTF/FBX rigs" is fine — keep glTF/FBX, it is not "FBX-only"), compile flags, or the SOURCES list. — left untouched; description already non-"FBX-only".
  - [x] Update the usage-example comment in [cmake/ReaperPlugin.cmake:13-14](../../cmake/ReaperPlugin.cmake#L13) (`add_reaper_extension(fbxanimationviewer …)` → `animviewer`) for repo hygiene.
- [x] **Task 2 — Rename source code identifiers & strings (AC: 1, 2, 3)**
  - [x] [src/plugin_main.cpp](../../src/plugin_main.cpp): `namespace fbxav` → `namespace rav` (lines 9, 31, 37); `kCommandName` `"FBXAV_OPEN_VIEWER"` → `"RAV_OPEN_VIEWER"` (line 12); `kActionDesc` `"FBXAV: Open Viewer Window"` → `"RAV: Open Viewer"` (line 13); console string `"[FBXAV] extension loaded (Phase 0)\n"` → `"[RAV] extension loaded (Phase 0)\n"` (line 70). Update the file's top comment if it names the old product. — top comment does not name the product; left as-is. Register/unregister structure (AR15) untouched.
  - [x] [src/viewer_window.cpp](../../src/viewer_window.cpp): `namespace fbxav` → `namespace rav` (lines 13, 192); `kWindowClassName` `L"FBXAnimationViewer.ViewerWindow"` → `L"ReaAnimViewer.ViewerWindow"` (line 16); `kWindowTitle` `L"FBX Animation Viewer"` → `L"ReaAnimViewer"` (line 17); all three console strings `"[FBXAV] …"` → `"[RAV] …"` (lines 91, 155, 168). — `kWindowClassName` confirmed single source of truth for all three Win32 uses.
  - [x] [src/viewer_window.h](../../src/viewer_window.h): `namespace fbxav` → `namespace rav` (lines 6, 16).
  - [x] [src/reaper_api.h](../../src/reaper_api.h) / [src/reaper_api.cpp](../../src/reaper_api.cpp): **no rename needed** — they contain only the `SPDX-License-Identifier: MIT` tag (which stays) and no product name or namespace. Confirm by grep; do not edit gratuitously. — confirmed by grep, not edited.
  - [x] Keep all `SPDX-License-Identifier: MIT` lines exactly as-is — SPDX *license* identifiers are not the product name. — all SPDX lines unchanged.
- [x] **Task 3 — Rename the ReaPack manifest (AC: 1, 3)**
  - [x] [reapack/index.xml](../../reapack/index.xml): `<index … name="FBXAnimationViewer">` → `name="ReaAnimViewer"` (line 10); `<reapack name="FBX Animation Viewer" …>` → `name="ReaAnimViewer"` (line 21); the comment "ReaPack manifest skeleton for the FBX Animation Viewer extension" (line 3); both description CDATA blocks (lines 13, 23-26) — keep "glTF, FBX" wording but drop the "FBX Animation Viewer" product name; the changelog string `"FBXAV: Open Viewer Window"` → `"RAV: Open Viewer"` (line 33); and the DLL filename in the TODO comment + `<source>` example `reaper_fbxanimationviewer.dll` → `reaper_animviewer.dll` (line 39). — 2nd CDATA block (23-26) had no product name to drop; left intact.
  - [x] **Leave the GitHub URLs `https://github.com/demute/asset-animation-viewer` unchanged** (lines 17, 18, 28, 39) — the GitHub repo is a separate, not-yet-public identity (`asset-animation-viewer`, which is neither "FBXAnimationViewer" nor "ReaAnimViewer"). See Open Questions; renaming the actual repo is an out-of-band decision for Antho, not a code change. — URLs left unchanged.
- [x] **Task 4 — Rename user-facing docs (AC: 1, 3)**
  - [x] [README.md](../../README.md): title `# FBX Animation Viewer` → `# ReaAnimViewer` (line 1); clone dir `FBXAnimationViewer` (lines 25, 26) → `ReaAnimViewer`; output path `reaper_fbxanimationviewer.dll` (lines 31, 43) → `reaper_animviewer.dll`; "the FBXAnimationViewer ReaPack listing" (line 41) → "ReaAnimViewer"; action search/label `FBXAV` / `FBXAV: Open Viewer Window` (lines 49, 50) → `RAV` / `RAV: Open Viewer`. The `prfaq-FBXAnimationViewer.md` path reference (line 10) points at a real file under `_bmad-output/` — leave the path literal as-is (the file keeps its historical filename). — prfaq path left literal.
  - [x] [docs/PHASE0_VALIDATOR_GATE.md](../../docs/PHASE0_VALIDATOR_GATE.md): product name (line 4); clone dir `FBXAnimationViewer` (line 31); DLL paths `reaper_fbxanimationviewer.dll` (lines 39, 55, 86) → `reaper_animviewer.dll`; console-log expectation `[FBXAV] extension loaded (Phase 0)` (line 66) → `[RAV] …`; action filter/row `FBXAV` / `FBXAV: Open Viewer Window` (lines 67, 68) → `RAV` / `RAV: Open Viewer`; window title `FBX Animation Viewer` (line 68) → `ReaAnimViewer`.
  - [x] **Scope guard:** Update **only the identity strings** in these docs. Do **not** rewrite the README's ReaImGui-panel narrative (lines 35, 41, 52) — the spike retired the ReaImGui approach for Epic 1, but correcting that narrative is the job of Stories 1.2/1.3, not this rename. Touch those lines only insofar as a name token appears in them. — ReaImGui narrative left intact; only the name token on line 41 changed.
- [x] **Task 5 — Verify no stale strings & build/config sanity (AC: 2, 3)**
  - [x] Run the residual-string grep (see Dev Notes → "Verification"). It must return **zero hits outside `_bmad-output/` and `extern/`**. — shipped code/manifest/docs return only README.md:10 (the intentional `prfaq-FBXAnimationViewer.md` path). Remaining hits are confined to `_bmad/` tooling config, `_bmad-output/` planning artifacts, and `.claude/` local settings — none are shipped code/manifest/user-facing per AC3.
  - [x] On this Linux/WSL box, run `cmake -B build` to confirm the CMake script still configures cleanly with the renamed target (the non-Windows branch builds a stub — this only proves CMake syntax, not the MSVC build). — `cmake` configures with exit 0; non-Windows stub emits `animviewer: … Build target stubbed.` (in-tree `build/` hit a sandbox FS restriction; an out-of-sandbox configure to a tmp dir succeeded cleanly).
  - [x] Hand off the **Windows MSVC build + Reaper load** verification (AC2: zero `/W3` warnings, `RAV: Open Viewer` appears, window opens, `< 2 s` load) to Antho per [docs/PHASE0_VALIDATOR_GATE.md](../../docs/PHASE0_VALIDATOR_GATE.md) — the dev environment here cannot run MSVC or Reaper. Note this explicitly in the Completion Notes. — handed off; see Completion Notes.

## Dev Notes

### Canonical naming map (the heart of this story)

| Aspect | Old | New |
|---|---|---|
| Product / display name | `FBXAnimationViewer` / "FBX Animation Viewer" | **ReaAnimViewer** |
| CMake `project()` name | `fbxanimationviewer` | `reaanimviewer` |
| `add_reaper_extension` target | `fbxanimationviewer` | **`animviewer`** ← yields the DLL below |
| Built DLL | `reaper_fbxanimationviewer.dll` | **`reaper_animviewer.dll`** |
| C++ namespace | `fbxav` | **`rav`** |
| Action command token (`command_id`) | `FBXAV_OPEN_VIEWER` | `RAV_OPEN_VIEWER` |
| Action label (`gaccel` desc) | `FBXAV: Open Viewer Window` | **`RAV: Open Viewer`** |
| Console log tag | `[FBXAV]` | `[RAV]` |
| Win32 window class | `FBXAnimationViewer.ViewerWindow` | `ReaAnimViewer.ViewerWindow` |
| Win32 window title | `FBX Animation Viewer` | `ReaAnimViewer` |
| ReaPack `<index name>` | `FBXAnimationViewer` | `ReaAnimViewer` |
| ReaPack `<reapack name>` | `FBX Animation Viewer` | `ReaAnimViewer` |

**Why the DLL is `reaper_animviewer`, not `reaper_reaanimviewer`:** Reaper's loader only scans `UserPlugins/` for files matching `reaper_*.dll` ([cmake/ReaperPlugin.cmake:6-10](../../cmake/ReaperPlugin.cmake#L6)). The helper prepends `reaper_` to the target name via `OUTPUT_NAME "reaper_${target}"`. So target `animviewer` → `reaper_animviewer.dll`, which reads naturally as "Reaper AnimViewer". AR21 fixes this exact DLL name. **This is the single easiest thing to get wrong in this story** — the human name is "ReaAnimViewer" but the binary drops the leading "Rea" because Reaper supplies it.

**Why namespace `rav` / tag `[RAV]`:** consistent short form of ReaAnimViewer, mirroring the old short `fbxav`/`[FBXAV]`. This supersedes the architecture doc's `fbxav`/`[FBXAV]` convention (Coding Standards table + D7 console format) for all code written from Epic 1 onward — the architecture is a planning artifact retaining historical names; the live code convention is now `rav`/`[RAV]`.

### Files to touch (complete list — main branch, Phase 0 baseline)

Shipped code + manifest + docs (all under the repo root, **not** `_bmad-output/`):

1. `CMakeLists.txt` — project name, target, link libs, platform messages.
2. `cmake/ReaperPlugin.cmake` — usage-example comment only (lines 13-14).
3. `src/plugin_main.cpp` — namespace, command token, action label, console string, header comment.
4. `src/viewer_window.cpp` — namespace, window class, window title, console strings, header comment.
5. `src/viewer_window.h` — namespace.
6. `reapack/index.xml` — index/reapack names, descriptions, changelog action string, DLL filename.
7. `README.md` — title, clone dir, DLL path, listing name, action strings.
8. `docs/PHASE0_VALIDATOR_GATE.md` — product name, clone dir, DLL paths, console tag, action strings, window title.

**Do NOT touch:** `src/reaper_api.h`, `src/reaper_api.cpp` (no name refs), `LICENSE.md` (generic MIT, no product name — confirmed), `extern/VENDORED.md` (no name refs — confirmed), anything under `extern/reaper-sdk/` (vendored SDK), and **anything under `_bmad-output/`** (planning artifacts deliberately keep historical names per AC3).

### Current state of the files being modified (read before editing)

- **`plugin_main.cpp`** is the extension entry point: registers `command_id` + `gaccel` + `hookcommand` on load, and **symmetrically deregisters** them on the `rec == nullptr` unload path (lines 39-48). The rename changes the *string token* `FBXAV_OPEN_VIEWER`; behavior is identical. AR15 (symmetric register/unregister) must stay intact — do not alter the register/unregister structure, only the strings.
- **`viewer_window.cpp`** owns a single modeless Win32 window + WGL context, RAII-ish teardown, and `UnregisterClassW` on close so a future DLL reload doesn't inherit a stale `lpfnWndProc`. The window-class string is used in three places (constant, `CreateWindowExW`, `UnregisterClassW`) — they reference the single `kWindowClassName` constant, so renaming the constant's value updates all uses. Confirm the constant is the only source of truth (it is).
- **`CMakeLists.txt`** has a Windows branch (real target) and a non-Windows branch (stub `add_custom_target` so `cmake --build` on Linux/macOS emits a clear "Windows only" notice instead of silently no-op'ing). Both branches reference the target name — rename both.

### Verification

Residual-string grep the dev must run before marking review (must be **empty** outside `_bmad-output/` and `extern/`):

```sh
grep -rniE "fbxanimationviewer|reaper_fbxanimationviewer|fbxav|namespace fbxav|FBX Animation Viewer|FBX-only" . \
  | grep -vE "/(\.claude|_bmad|_bmad-output)/" \
  | grep -v "extern/reaper-sdk"
```

Then confirm the new identity is present:

```sh
grep -rniE "reaanimviewer|reaper_animviewer|\[RAV\]|RAV: Open Viewer|namespace rav" \
  src/ CMakeLists.txt reapack/ README.md docs/
```

### Testing standards summary

- There is **no automated test harness** in Phase 0 (the project is scaffolding; spec-phase-0-scaffolding.md defines acceptance as a manual validator gate). Verification for this story = (a) the residual-string grep above, (b) `cmake -B build` configures cleanly on this box, and (c) the Windows validator-gate steps run by Antho.
- **NFR-R5 (zero `/W3 /permissive-` warnings)** and **NFR-P4 (loads `< 2 s`)** are re-verified by Antho on Windows — a rename should not affect either, but they are AC2 and must be confirmed, not assumed.
- Antho's manual gate (adapted from [docs/PHASE0_VALIDATOR_GATE.md](../../docs/PHASE0_VALIDATOR_GATE.md)): build → `reaper_animviewer.dll` exists, zero warnings → copy to `UserPlugins/` → console shows `[RAV] extension loaded (Phase 0)` → Action list filtered by `RAV` shows `RAV: Open Viewer` → running it opens an 800×600 window titled **ReaAnimViewer** with the dark-grey clear color.

### Project Structure Notes

- The rename operates on the **Phase 0 scaffolding baseline on `main`**. The spike (`spike/0-1-feasibility`, Story 0.1) is throwaway, never merged, and deliberately kept `fbxav`/`FBXAV` identifiers — see [0-1-…-measure-fps.md](0-1-render-a-skinned-animation-in-a-reaper-hosted-fbo-panel-and-measure-fps.md#L142) ("Keep the AR21 rename out of scope … The rename to ReaAnimViewer is Epic 1 Story 1.1 and happens on `main`."). **Do not** reference, merge, or modify the spike branch.
- No new files, no moved files, no directory renames inside the repo. The repo *folder* identity (AR21 "repo/folder identity") is satisfied by the README/manifest naming + the clone-dir instructions; the actual on-disk working-copy directory is `/mnt/fbxanimationviewer` and renaming the mount is out of scope (and would break tooling paths — see [reference: mount paths] in project memory).

### References

- [Source: epics.md#Story 1.1: Rename FBXAnimationViewer to ReaAnimViewer](../planning-artifacts/epics.md) — user story + the 3 acceptance criteria (Given/When/Then) transcribed above.
- [Source: epics.md#AR21](../planning-artifacts/epics.md) — the architectural rename requirement: DLL `reaper_animviewer.dll`, Action `RAV: Open Viewer`, namespaces/SPDX/manifest/docs/repo identity.
- [Source: sprint-change-proposal-2026-06-23.md](../planning-artifacts/sprint-change-proposal-2026-06-23.md) — §2 "Story 1.1 (rename) unaffected"; §5 handoff confirms 1.1 runs first and is independent of the viewport rework.
- [Source: cmake/ReaperPlugin.cmake#L16-L30](../../cmake/ReaperPlugin.cmake#L16) — `OUTPUT_NAME "reaper_${target}"` rule that pins the target name to `animviewer`.
- [Source: architecture.md#Coding Standards (namespace `fbxav`) & D7 console format](../planning-artifacts/architecture.md) — historical `fbxav`/`[FBXAV]` convention this story supersedes to `rav`/`[RAV]` for code (architecture doc itself retains historical names per AC3).
- [Source: docs/PHASE0_VALIDATOR_GATE.md](../../docs/PHASE0_VALIDATOR_GATE.md) — Antho's manual acceptance steps, to be re-run with the new identity for AC2.

## Open Questions (for Antho — non-blocking; defaults chosen)

1. **GitHub repo URL.** The manifest/PRFAQ point at `github.com/demute/asset-animation-viewer`. That repo name matches neither old nor new product name and the repo isn't public yet. **Default taken:** leave the URLs untouched (renaming a GitHub repo is an out-of-band action, not a code change). Flag if you want the URL slug changed to `reaanimviewer` now in anticipation of the Phase 5 release.
2. **Namespace/tag form.** Chose short `rav` / `[RAV]` (mirrors old `fbxav`/`[FBXAV]`). Confirm you're happy with `rav` vs. a longer `reaanimviewer` namespace. (Recommend `rav` — terse, matches the Action prefix.)

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Claude Opus 4.8, 1M context)

### Debug Log References

- Residual-string grep (Dev Notes → Verification) over shipped paths `src/ CMakeLists.txt cmake/ reapack/ README.md docs/`: only hit is `README.md:10` — the intentional `prfaq-FBXAnimationViewer.md` path reference. Clean.
- New-identity grep confirms `reaanimviewer` / `reaper_animviewer` / `[RAV]` / `RAV: Open Viewer` / `namespace rav` present across all 8 renamed files.
- `cmake -S . -B <tmp>` → exit 0, "Configuring done / Generating done", non-Windows branch printed `animviewer: non-Windows platform detected — … Build target stubbed.` (proves the renamed target + message text parse and execute). An in-tree `cmake -B build` failed only on a sandbox `configure_file` "Operation not permitted" FS restriction — unrelated to the rename.

### Completion Notes List

- Pure rename, no behavior change. FBXAnimationViewer → **ReaAnimViewer** across build target, binary name, Reaper Action, C++ namespace, Win32 window class/title, ReaPack manifest, and user docs.
- **Key correctness point (AR21):** CMake target is **`animviewer`** (not `reaanimviewer`), because `add_reaper_extension` sets `OUTPUT_NAME "reaper_${target}"` → produces **`reaper_animviewer.dll`**. CMake `project()` name is `reaanimviewer`.
- Namespace `fbxav` → `rav`; console tag `[FBXAV]` → `[RAV]`; Action `FBXAV: Open Viewer Window` → `RAV: Open Viewer`; command token `FBXAV_OPEN_VIEWER` → `RAV_OPEN_VIEWER`; window class `ReaAnimViewer.ViewerWindow`; window title `ReaAnimViewer`.
- AR15 symmetric register/unregister structure in `plugin_main.cpp` left untouched — only string tokens changed. `src/reaper_api.{h,cpp}` confirmed name-free (SPDX only) and not edited. All `SPDX-License-Identifier: MIT` lines preserved verbatim.
- **Intentionally NOT changed:** GitHub URLs `github.com/demute/asset-animation-viewer` (Open Question 1 — out-of-band repo decision); the `prfaq-FBXAnimationViewer.md` path literal (file keeps historical name); everything under `_bmad-output/`, `_bmad/`, `extern/`; README ReaImGui-panel narrative (Stories 1.2/1.3 own that correction).
- **Defaults taken on Open Questions:** (1) GitHub repo URL slug left as-is. (2) Short namespace/tag form `rav`/`[RAV]` adopted (recommended). Flag if either should change.
- ⚠️ **Handoff to Antho (Windows validator gate — required for AC2):** the dev box is Linux/WSL with no MSVC/Reaper, so the build+load checks could not be run here. Per [docs/PHASE0_VALIDATOR_GATE.md](../../docs/PHASE0_VALIDATOR_GATE.md): MSVC build produces `reaper_animviewer.dll` with **zero `/W3 /permissive-` warnings** (NFR-R5); copy to `UserPlugins/`; console shows `[RAV] extension loaded (Phase 0)`; Action list filtered by `RAV` shows `RAV: Open Viewer`; running it opens an 800×600 window titled **ReaAnimViewer** with the dark-grey clear color; extension loads **< 2 s** (NFR-P4).

### File List

- `CMakeLists.txt` — modified (project name `reaanimviewer`, target `animviewer`, link libs, non-Windows message + custom target)
- `cmake/ReaperPlugin.cmake` — modified (usage-example comment only)
- `src/plugin_main.cpp` — modified (namespace `rav`, command token, action label, console tag)
- `src/viewer_window.cpp` — modified (namespace `rav`, window class, window title, 3 console tags)
- `src/viewer_window.h` — modified (namespace `rav`)
- `reapack/index.xml` — modified (index/reapack names, description, changelog action string, DLL filename in source example)
- `README.md` — modified (title, clone dir, DLL paths, listing name, action strings)
- `docs/PHASE0_VALIDATOR_GATE.md` — modified (product name, clone dir, DLL paths, console tag, action strings, window title)
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — modified (story 1-1 → in-progress → review)

## Change Log

| Date | Change |
|---|---|
| 2026-06-23 | Story 1.1 implemented — renamed FBXAnimationViewer → ReaAnimViewer across build target (`animviewer` → `reaper_animviewer.dll`), source namespaces (`fbxav` → `rav`), Reaper Action (`RAV: Open Viewer`), ReaPack manifest, and user docs. Pure rename, no behavior change. Residual grep + `cmake` configure verified on Linux; Windows MSVC build + Reaper load handed off to Antho's validator gate (AC2). Status → review. |
| 2026-06-23 | Added `build.bat` (one-click Windows build + install helper; CRLF, adapted from the spike script). Antho ran the Windows validator gate: MSVC build clean, `RAV: Open Viewer` registers, ReaAnimViewer window opens, load < 2 s — AC1/AC2/AC3 all confirmed. Status → done. |
