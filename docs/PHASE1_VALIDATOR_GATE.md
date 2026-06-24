# Phase 1 — Validator Gate

Step-by-step acceptance test for **Phase 1** (Epic 2 — see a textured 3D model with
camera control). Written for Antho (the validator) to run on Windows. This file is
the **sole authority on Phase 1 completion** (AR19) and grows as Epic 2 progresses:
**Story 2.1** seeds the rows below; Stories 2.2 (textures), 2.3 (multi-material
specular) and 2.4 (camera) append their own.

**You are testing the right thing if** the viewer opens a real `.glb`/`.gltf` file
(not the Epic 1 test cube), the mesh appears **correctly lit and oriented**, a
broken file logs **one error line and Reaper survives**, and the deliverable is
**one DLL** with no assimp side-DLL.

---

## Prerequisites

- Reaper 7.x (`caller_version == 0x20E`, current SDK targets 7.72)
- Visual Studio 2022 with **Desktop development with C++**
- CMake ≥ 3.20 and Git for Windows on PATH
- A network connection on the **first** configure (CMake `FetchContent` clones GLM
  1.0.3 **and assimp 6.0.5** — the assimp clone + first compile is **slow**, expect
  several minutes; subsequent builds reuse the cache)

> Phase 1 still has **no ReaImGui dependency** and **no offscreen FBO** — the
> viewport is the same native OpenGL docked window shipped in Epic 1. The model is
> loaded with **assimp**, narrowed to the **glTF + FBX + Collada** importers only
> (AR5). When you run `RAV: Open Viewer` a standard Windows **"Open file" dialog**
> appears — pick the model you want to test. (The full file-browser panel is still
> Epic 5; this is just the minimal native picker.)

## 1. Build

```cmd
cd \path\to\ReaAnimViewer
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**Expected:** GLM 1.0.3 and assimp 6.0.5 are fetched on first configure (slow),
**zero MSVC warnings at `/W3 /permissive-`** *for our own sources* (assimp's own
tree builds with its warnings suppressed and is irrelevant), and a DLL at
`build\Release\reaper_animviewer.dll`.

> If a shader ever fails to compile at `#version 330`, the fix is a
> `wglCreateContextAttribsARB` 3.3-core context — flag it; it is not expected on the
> reference workstation.

## 2. Single-DLL check (D15 / D17)

Look in `build\Release\`. The **only** `reaper_*`/assimp artifact that ships is
`reaper_animviewer.dll`. There must be **no `assimp-vc143-mt.dll`** (or any
`assimp*.dll`) beside it — assimp is statically linked.

| # | Check | Pass criterion | AC / NFR |
|---|---|---|---|
| S1 | No side-DLL | `build\Release\` contains `reaper_animviewer.dll` and **no `assimp*.dll`**. | AC5 / D15 / D17 |
| S2 | Warning-free | The build log shows **0 warnings** for `src\*.cpp` under `/W3 /permissive-`. | NFR-R5 |

## 3. Install

```cmd
copy build\Release\reaper_animviewer.dll "%APPDATA%\REAPER\UserPlugins\"
```

Close and reopen Reaper so it re-scans extensions.

## 4. Choosing the file under test

Just run `RAV: Open Viewer` — a Windows **"Open file" dialog** pops up. Pick the
model and it loads. To test another file: close the viewer, run the action again,
pick the next one. **No command line, no rebuild.**

On load the console prints `[RAV] info: loaded <path> (N meshes)`; if you cancel the
dialog it prints `[RAV] info: no file selected — viewport idle` and shows an empty
panel (this is fine — not a failure).

**Suggested fixtures** (Khronos glTF-Sample-Assets cover most rows):
- *Canonical glTF/GLB:* `Box`, `Duck`, `BoxTextured` (Y-up, meters) — any static one.
- *No-animation file:* any static glTF above (no animation channels).
- *Non-canonical:* a Z-up or centimetre-scale export (e.g. a Blender Z-up `.glb`),
  or a file with non-English bone names.
- *Collada:* any assimp/Khronos `.dae` sample (best-effort, ≥1 public sample).
- *Corrupt:* truncate a `.glb` in a hex editor, or rename a `.txt` to `.glb`.

## 5. Story 2.1 — acceptance checks

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Canonical mesh renders | In the Open-file dialog pick a canonical glTF/GLB. The mesh appears **shaded** (Blinn-Phong: diffuse + a specular highlight), **upright**, and **framed** (auto-fit, neither tiny nor clipping). | AC1 |
| 2 | GLB binary loads | A `.glb` (binary) loads the same as a `.gltf`. | AC1 |
| 3 | Static / no-animation path | A file with **no animation channels** still renders as a static mesh (it does not stay invisible or error). | AC2 (FR6) |
| 4 | Non-canonical as-authored | A **Z-up / centimetre / foreign-bone-name** file **loads without error** and renders **tilted or scaled as-authored** — NOT silently re-oriented upright. (A tilted model here is the *correct* result; Reset Camera will recover it in Story 2.4.) | AC3 |
| 5 | Collada loads | A static `.dae` loads through the same path and renders as-authored. *(Best-effort against one public sample.)* | AC4 |
| 6 | Corrupt file is survivable | Pick a corrupt/unreadable file in the dialog. The console logs **exactly one** `[RAV] error: load failed [...]: …` line, the viewport stays open (blank), and **Reaper does not crash**. | implied AC6 |
| 7 | Close/reopen is clean | Close the viewer and re-run the action (the dialog lets you pick a different file). A fresh model loads; no leak, no stale state, Reaper stable. | implied AC8 |
| 8 | Shutdown is clean | Close Reaper. It exits with **no crash dialog**, no orphan window. | implied AC8 |
| 9 | Non-ASCII path loads | Put a model under a folder whose name has non-ASCII characters (e.g. accented or CJK) and pick it. It loads normally (the path is carried as UTF-8) — **not** misreported as `file-not-found`. | hardening (code review 2026-06-24) |

## 6. Recording the result

Per project convention (`feedback_trust_ingame_validation`): when these checks pass
in real Reaper, **that is the gate** — note the date and the fixtures used here, and
the story moves to done. Visual correctness, orientation, no-crash, single-DLL and
warning-free build are all validator-confirmed on Windows; they are not re-litigated
on the Linux dev box (which can only confirm the CMake configure and source greps).

**Result:** Story 2.1 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-24).

## 7. Story 2.2 — acceptance checks (diffuse textures)

Story 2.2 fills the diffuse-texture seam: the loader resolves each material's base-
color texture through **one unified path** (AR14) covering both glTF packaging styles
— **GLB-embedded** (FR17) and **multi-file glTF with a sibling image** (FR18) —
decodes it (stb_image), uploads a GPU texture, and the shader modulates the flat base
color with it. An unresolvable texture **degrades to the flat color** with one console
warning, never failing the load (AC3).

**Suggested fixtures** — Khronos **glTF-Sample-Assets** `BoxTextured` and `Duck` cover
both variants in one pair: the `.glb` is embedded (FR17); the `glTF/` folder ships the
`.gltf` + a sibling `.png` (FR18). Both show a recognizable texture, not flat grey.

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 10 | GLB-embedded texture | Open an embedded `.glb` (e.g. `BoxTextured.glb` or `Duck.glb`). The surface shows the **texture image**, not a flat grey/colored face. | AC1 (FR17) |
| 11 | glTF-sibling texture | Open the multi-file `BoxTextured/glTF/BoxTextured.gltf` (with its sibling `.png` next to it). The **same** texture displays. | AC2 (FR18) |
| 12 | Missing-texture fallback | Rename/remove the sibling `.png` (or open a glTF whose image is absent), then load it. The console logs **exactly one** `[RAV] warn: texture unresolved for material … - using flat color` line, the model still renders in **flat base color**, the load does **not** fail, and Reaper survives. | AC3 |
| 13 | No-regression (textureless) | A **textureless** static file (Story 2.1's `Box`/`Duck` without a texture) and a `.dae` still render exactly as in Story 2.1 — flat Blinn-Phong, no change. | implied AC (R5/2.1 rows 1–5) |
| 14 | Single-DLL still holds | `build\Release\` still contains only `reaper_animviewer.dll` — **no** `assimp*.dll` and **no** image-decoder DLL (stb is header-only, compiled in). | AC4 / D15 / D17 |

> **UV-orientation note:** if a textured model appears **vertically mirrored**
> (upside-down texture), it is a single-line UV-convention flip, **not** a logic bug.
> The starting position is **no flip** — glTF defines top-left UV origin and we upload
> the image top-row-first, which is self-consistent. If the gate sees a mirrored
> texture, the one-line fix is `stbi_set_flip_vertically_on_load(true)` in
> `asset_loader.cpp` (plus the equivalent flip on the uncompressed-swizzle branch).
> Flag it for a quick follow-up rather than guessing the convention blind — it cannot
> be confirmed on the Linux dev box.

**Result:** Story 2.2 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-24).
