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

## 8. Story 2.3 — acceptance checks (multi-material + per-material specular)

Story 2.3 closes the **specular-response** gap. The multi-material render path (one
draw per (node, mesh) with its own material, the per-material `u_specularColor` /
`u_shininess` Blinn-Phong shader) was already built by 2.1/2.2 — this story changes
only the **values** `ConvertMaterial` derives: for plain glTF metallic-roughness
(where assimp leaves `COLOR_SPECULAR` unset) it tints the specular toward the base
color by `metallicFactor` (F0 0.04 dielectric → base-color metal) and clamps the
roughness-derived shininess to `2…1000`, so a matte surface and a polished one show a
**visibly different highlight** (FR16). An explicitly-authored specular (FBX/Collada
Phong, glTF `KHR_materials_specular`, `pbrSpecularGlossiness`) is honored as-is. The
draw loop and shader are **byte-for-byte unchanged**.

**Suggested fixtures** — Khronos **glTF-Sample-Assets**: `MetalRoughSpheres` (a grid
varying metallic × roughness in one frame — the ideal matte↔glossy contrast fixture)
and a multi-material model such as `FlightHelmet`, `BoomBox`, or `DamagedHelmet`.

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 15 | Multi-material renders distinctly | Open a multi-material model (e.g. `FlightHelmet` / `BoomBox` / `DamagedHelmet`). Each sub-mesh shows its **own** diffuse/texture **and** highlight — not one uniform surface. | AC1 (FR20) |
| 16 | Matte vs glossy distinguishable | Open `MetalRoughSpheres`. The low-roughness/metallic spheres show a **tight, bright, base-color-tinted** highlight; the high-roughness ones a **broad, dim** highlight — side by side in one frame. (A two-material custom file, one matte + one glossy, also works.) | AC2 (FR16) |
| 17 | No-regression | Re-run the 2.1 canonical / non-canonical / `.dae` / corrupt rows (1–9) and the 2.2 texture rows (10–14). All still pass unchanged — the draw loop and shader did not change. | AC3 |
| 18 | Single-DLL / warning-free still hold | `build\Release\` still contains only `reaper_animviewer.dll` (no new DLL); build is clean at `/W3 /permissive-`. | AC4 |

> **Specular-tuning note:** the constants are a "looks-right" judgment confirmable
> only on the Windows gate, not the Linux dev box. The starting position is the
> principled default: dielectric **F0 = 0.04**, shininess clamp **2…1000**, and **no**
> metal diffuse suppression. If metals read too flat/too bright or the matte↔glossy
> contrast is too subtle, the knobs are (1) the **F0 constant** (`0.04`), (2) the
> **shininess clamp** (`2 … 1000`), and optionally (3) a mild **metal diffuse
> suppression** (`baseColorFactor *= (1 − k·metallic)`) — but (3) needs a metalness
> uniform threaded into the shader to apply *post-texture* (else a textured metal,
> whose factor is white, goes black), which is a PBR-shader change out of this story's
> scope. Flag any tuning for a one-line follow-up rather than guessing blind.

**Result:** Story 2.3 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-24).

## 9. Story 2.4 — acceptance checks (camera controls)

Story 2.4 turns the fixed three-quarter snapshot into an **interactive viewport**.
The static `glm::lookAt` computed once in `SetAsset` is factored into a D14
`OrbitCamera { target, distance, yaw, pitch }` (new `src/camera.h`) that the renderer
owns and rebuilds the view from **every frame**, so dragging is continuous (FR26).
Right-click-drag **orbits** (FR22), the scroll wheel **zooms** (FR23), middle-click-drag
**pans** (FR24), and a native Win32 child **Reset View** button re-frames the model's
bounding box (FR25) — which is also the documented recovery path (AR13) for a
non-canonical (Z-up / cm / tilted) file that loads "wrong" (Journey 2). Auto-fit-on-load
**is** Reset, so the initial framing a user sees is unchanged from 2.1–2.3. The loader,
shader, materials, and GL resource code are **untouched** — this story only changes
*what* view matrix is fed and *when* it is rebuilt, plus the input handlers.

**Suggested fixtures** — any 2.1/2.2/2.3 model for orbit/zoom/pan; a **non-canonical**
fixture (a Z-up / cm / sideways file from the 2.1 rows) for the Reset / Journey-2 row.

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 19 | Orbit (FR22) | Right-click-drag inside the panel rotates the view around the model. At the top/bottom of the arc the model does **not** flip or gimbal (pitch clamp). | AC1 |
| 20 | Zoom (FR23) | Scroll wheel moves the camera in/out smoothly. You **cannot** zoom *through* the model to nothing, nor lose it to infinity (distance clamp). | AC1 |
| 21 | Pan (FR24) | Middle-click-drag slides the model across the view. Pan speed feels **consistent** at any zoom level (distance-scaled). | AC1 |
| 22 | Reset + Journey 2 recovery (FR25) | Load a **non-canonical** fixture (Z-up / cm / sideways from the 2.1 rows); confirm it frames tilted/odd. Orbit away, then click **Reset View** → the model is reframed **centered and fully in view**. | AC2 |
| 23 | Continuous update (FR26) | During an orbit/pan drag the viewport stays responsive and holds **≥60 fps** — no freeze or stutter while dragging. *(FR26's full force — animation not pausing during camera moves — lands in Epic 3 when the rig animates; in Epic 2 the displayed frame is static, so the check is "drag stays smooth and the timer keeps presenting.")* | AC3 |
| 24 | No right-click context menu | Right-drag inside the panel raises **no** Win32 popup menu (WM_RBUTTONUP is consumed for orbit, suppressing WM_CONTEXTMENU). | AC6 |
| 25 | No-regression | Re-run the 2.1 rows 1–9, 2.2 rows 10–14, 2.3 rows 15–18. All still pass; the **initial on-open framing is unchanged** (auto-fit = Reset). | AC4 |
| 26 | Single-DLL / warning-free still hold | `build\Release\` contains only `reaper_animviewer.dll` (the camera header adds no DLL, no CMake change); build is clean at `/W3 /permissive-`. | AC5 |

> **Sensitivity-tuning note:** the camera "feel" is a judgment confirmable only on the
> Windows gate, not the Linux dev box. The knobs (all one-line `constexpr` in
> `src/camera.h`) are `kOrbitSens` (orbit speed), `kZoomSens` (zoom rate), `kPanSens`
> (pan speed), the initial `kInitPitch` (0.3) / `kInitYaw` (0.6), and the zoom-distance
> clamp band (`0.05 … 20 × frameRadius`). If orbit is too fast/slow, zoom too coarse,
> or the clamp too tight/loose, these are one-line constant tweaks — flag for a quick
> follow-up rather than guessing the feel blind on Linux.

**Result:** Story 2.4 — _pending Antho's in-Reaper Windows validation._
