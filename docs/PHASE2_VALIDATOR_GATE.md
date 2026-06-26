# Phase 2 — Validator Gate

Step-by-step acceptance test for **Phase 2** (Epic 3 — watch the rig animate; the
dominant technical risk). Written for Antho (the validator) to run on Windows. This
file is the **sole authority on Phase 2 completion** (AR19) and grows as Epic 3
progresses: **Story 3.1** seeds the rows below (skeleton & skin parse — *data + audit
only*); Stories 3.2 (sample channels → per-frame bone matrices) and 3.3 (GPU vertex
skinning — the first *moving* rig) append their own.

**You are testing the right thing if**, for Story 3.1, a skinned `.glb`/`.gltf`/`.fbx`
rig loads and prints a **legible skeleton dump** to the console (`[RAV] info:
skeleton: N bones, …`) whose **bone count and parent links match Blender / FBX
Review**, the rig still **renders in bind pose exactly as in Epic 2** (no deformation
yet — that is 3.3), and a **static (boneless) file prints no skeleton noise**.

---

## Prerequisites

- Reaper 7.x (`caller_version == 0x20E`, current SDK targets 7.72)
- Visual Studio 2022 with **Desktop development with C++**
- CMake ≥ 3.20 and Git for Windows on PATH
- The same assimp 6.0.5 + GLM 1.0.3 vendored stack as Phase 1 — **no new dependency**
  in this story (no new importer, no stb call). First configure is slow; later builds
  reuse the cache.

> Phase 2 still has **no ReaImGui dependency** and **no offscreen FBO** — the viewport
> is the same native OpenGL docked window shipped in Epic 1, rendering the same
> Blinn-Phong material path from Epic 2. Story 3.1 is **loader-only and purely
> additive**: it parses the skeleton and per-vertex skin weights into the `Asset`,
> sets the `skinned` flag, and dumps the skeleton to the console. **No shader,
> renderer, GL-resource, header, or CMake change** — so every Epic 2 fixture renders
> **byte-for-byte identically**.

## 1. Build & install

Identical to the Phase 1 gate (§1, §3):

```cmd
cd \path\to\ReaAnimViewer
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
copy build\Release\reaper_animviewer.dll "%APPDATA%\REAPER\UserPlugins\"
```

**Expected:** **zero MSVC warnings at `/W3 /permissive-`** for our own sources, and a
single DLL at `build\Release\reaper_animviewer.dll` with **no `assimp*.dll`** beside it
(assimp is statically linked; stb is header-only). Close and reopen Reaper to re-scan.

## 2. Choosing the file under test

Same as Phase 1: run `RAV: Open Viewer`, a Windows **"Open file" dialog** pops up, pick
the model. To test another file, close the viewer and run the action again. **No
command line, no rebuild, no env-var.** The skeleton audit is read in **Reaper's
console** (the `[RAV]` channel) — open it via the Reaper action list / ReaScript
console if it is not already visible.

**Suggested fixtures** (Khronos **glTF-Sample-Assets** cover most rows):
- *Skinned rig:* `RiggedSimple`, `RiggedFigure`, `CesiumMan`, `BrainStem`, or `Fox`
  (each ships a real joint hierarchy). A **Mixamo** `.fbx`/`.glb` export also works.
- *Skeleton cross-reference:* the same file open in **Blender** (Outliner → Armature)
  or **FBX Review**, to read off the expected bone count and parent links.
- *Non-English bone names:* any rig re-exported with renamed (accented / CJK) bones,
  or a fixture that already ships them (FR5).
- *Static (boneless):* any Epic 2 fixture (`Box`, `Duck`, `BoxTextured`) — used to
  confirm the **no-skeleton-noise** and **no-regression** rows.

## 3. Story 3.1 — skeleton & skin parse (audit)

Story 3.1 fills the **data** Stories 3.2 and 3.3 consume. It parses the flat
`SceneSkeleton` (bones with `parentIdx` + `inverseBindMatrix`, ordered
parent-before-child by a node-tree DFS), scatters per-vertex `boneIds`/`boneWeights`
(global-index remap, ≤4 influences), sets `SceneMesh::skinned = true` on skinned
meshes, and prints a **console skeleton dump** — the click-based validator hook (load
a file → read the console). **Nothing reads `skinned`/`skeleton`/`boneIds` yet** (that
is 3.3), so the rendered image is unchanged from Epic 2: a skinned rig still draws in
**baked bind pose**.

The dump looks like:

```text
[RAV] info: skeleton: 27 bones, 1842 skinned verts
[RAV] info:   [ 0] Hips                 parent  -1 (root)
[RAV] info:   [ 1] Spine                parent   0 (Hips)
[RAV] info:   [ 2] Chest                parent   1 (Spine)
...
```

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Bone count matches | Load a skinned glTF/GLB/FBX rig. The console `skeleton: N bones` count **equals** the joint/bone count shown in Blender (Outliner → Armature) or FBX Review. | AC1 |
| 2 | Parent links match | Spot-check several bones' `parentIdx` / parent-name in the dump against the Blender bone hierarchy. The **root** bone shows `parent -1 (root)`; every other bone's `parentIdx` is **less than its own index** (no `ordering broken` warning appears). | AC1 / AC2 |
| 3 | Bind matrices sane | No `[RAV] warn:` lines about absent / divergent bind matrices for a normal rig. *(Optional dev-discretion proxy: a logged inverse-bind matrix re-multiplied by the bone's bind global ≈ identity.)* | AC1 / AC2 |
| 4 | Non-English bone names intact | Load a rig with non-ASCII bone names. They print in the dump **un-mangled** (raw UTF-8), not transliterated or replaced with `?`. | AC1 (FR5) |
| 5 | Single matrix boundary | *(Source audit, confirmable on the dev box.)* `mOffsetMatrix` is converted **only** via `ConvertAssimpMatrix`, **exactly once**, with **no** `glm::transpose` / `glm::make_mat4` on the bind path (the §D rule, AR9). | AC2 |
| 6 | Zero render regression — skinned | The skinned rig still renders in **bind pose exactly as in Epic 2** — same shading, orientation, auto-fit framing, and camera behaviour. Re-run the Phase 1 gate rows 1–26; all still pass. | implied AC3 |
| 7 | Zero render regression — static | A **static (boneless)** file still loads via the empty-skeleton path: it renders identically to Epic 2 and prints **no skeleton dump** (the console stays clean — only the usual `loaded …` line). | implied AC3 (FR6) |
| 8 | Survivable on bad rigs | A corrupt/over-limit/odd rig degrades to a console line and a best-effort load — it **never throws across the boundary and never crashes Reaper** (re-use the Phase 1 corrupt-file row). | implied AC5 (NFR-R1/AR17) |
| 9 | Single-DLL / warning-free | `build\Release\` contains only `reaper_animviewer.dll` (no new DLL, no CMake change); build is clean at `/W3 /permissive-`. | implied AC6 |

> **Scope note (mirrors the Phase 1 tuning notes):** Story 3.1 delivers **data +
> audit only**. The success signal here is the **skeleton dump matching the source**
> and the **bind-pose render being unchanged** — **not motion**. Deformed playback is
> validated in **Story 3.3** (GPU vertex skinning), and animation channel sampling in
> **Story 3.2**. If a skinned rig looks visually identical to how it looked in Epic 2,
> that is **correct** for this story — the rig is not yet driven by the skeleton. A few
> things are deliberately deferred and may surface as known, non-blocking notes:
> vertex space vs. node baking (3.3's call — baking is left unchanged here),
> intermediate non-joint node transforms (3.2 accumulates them), and multi-mesh
> shared-bone offset divergence (3.1 keeps the first + warns).

## 4. Recording the result

Per project convention (`feedback_trust_ingame_validation`): when these checks pass in
real Reaper, **that is the gate** — note the date and the fixtures used here, and the
story moves to done. The skeleton-dump audit, non-ASCII names, bind-pose
no-regression, single-DLL and warning-free build are validator-confirmed on Windows;
they are not re-litigated on the Linux dev box (which can only confirm the source
audits — scope, the single matrix boundary, that nothing reads `skinned` — and a CMake
configure that is host-stubbed on non-Windows).

**Result:** Story 3.1 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-26).
Fixture: Mixamo `Hip Hop Dancing.fbx` (2 meshes). Console dump — **99 bones, 8928
skinned verts**; root `[0] mixamorig:Hips parent -1`; every `parentIdx < index`
(no `ordering broken` warning), hierarchy coherent with the Mixamo rig (spine→head,
eyes on head, full finger chains, legs/toes, custom horn + skirt bones); **no
absent/divergent bind-matrix warnings**; load clean, no crash. Bind-pose render
unchanged from Epic 2 (skinned data drives nothing until 3.3). Non-ASCII-name and
`/W3`/single-DLL rows ride the same clean build; FBX even loads through the shared
glTF path already (Mixamo `PreservePivots=0`, Spike Finding 3).
