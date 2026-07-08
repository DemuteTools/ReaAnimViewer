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

## 4. Story 3.2 — animation sampling & per-frame matrices (audit)

Story 3.2 fills the **motion** Story 3.3 will draw. It parses the first animation clip
into the already-declared `SceneAnimation` (per-bone TRS keyframes, indexed by bone
global index, **ticks→seconds** at the boundary), and adds a **pure-glm sampler**
(`src/animation.h`) that, given a time `t`, composes the **per-frame global bone
matrices** + skinning palette in one linear forward pass. Like 3.1 it is **purely
additive and changes no rendered pixel** — nothing consumes `asset.animations` yet, so
a skinned rig still draws in **baked bind pose**. The deliverable is **parsed channels +
a deterministic sampler + a console audit**, proving the math before 3.3's shader
touches it. **No shader, renderer, GL-resource, `scene.h`, or CMake change** (the sampler
is header-only like `camera.h`), so every Epic 2 / Story 3.1 fixture renders identically.

The audit (emitted only on an **animated** load, right after the 3.1 skeleton dump) looks
like:

```text
[RAV] info: animation: 1 clip, dur 3.467s (tps 30.0), 65/99 bones animated, 8421 keys
[RAV] info:   probe t=0.000  root tx (   0.00,   0.00,   0.00)  palette finite=yes
[RAV] info:   probe t=1.733  root tx (   0.00,  12.40,   3.10)  palette finite=yes  maxDelta(t0->mid)=41.70
[RAV] info:   probe t=3.467  root tx (   0.00,   0.05,  -0.20)  palette finite=yes
```

`root tx` is the first parentless bone's global-space translation (root-motion evidence,
FR7); `finite` checks all 16 floats of every palette matrix (AC3 boundary validity);
`maxDelta(t0->mid)` is the largest bone-position change from `t=0` to mid (proves the
pose varies with time — the sampler is live, not frozen).

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Clip parsed (ticks→seconds) | Load a Demute/Mixamo animated rig. The console reports **≥1 clip** with a `dur Ns` that **matches the clip length** in Blender (Dope Sheet / NLA) or FBX Review. A wrong unit shows up as an obviously-wrong duration (e.g. 100× off). | AC1 |
| 2 | Root motion present | For a clip **with root motion**, the probe's `root tx` **changes across `t=0 → mid → end`**; for an **in-place** clip it stays ~constant. | AC1 (FR7) |
| 3 | Pose varies with time | `maxDelta(t0->mid)` is **clearly non-zero** for an animated rig (sampling is live). | AC1 |
| 4 | Boundary poses valid | Every palette matrix is `finite=yes` at **`t=0` and `t=duration`** (AC3 clamp); the loader never warns of NaN/explosion. Sampling slightly past `duration` clamps (the probe at `t=duration` is the end clamp). | AC3 |
| 5 | Rotation-only & full-TRS both pose | The full-TRS Mixamo fixture probes finite/sensible. If a **rotation-only** fixture is on hand, its probe is finite too (T & S fall back to bind-local). *(If only Mixamo is available, record rotation-only as best-effort and note it.)* | AC2 |
| 6 | Quaternion order correct | *(Source audit, dev-box confirmable.)* rotation keys convert **w-first**: `glm::quat(q.w, q.x, q.y, q.z)`, **exactly once**, with **no** `ConvertAssimpMatrix` on any TRS value (vectors/quats are not matrices, §D). | AC2 |
| 7 | Zero render regression | Re-run the §3 / Phase-1 rows: the **bind-pose render is unchanged**, a **static (boneless) file** still loads with **no animation dump**, and a skinned-but-**clip-less** rig prints the skeleton dump but **no animation line**. | implied AC4 |
| 8 | Single-DLL / warning-free | `build\Release\` holds only `reaper_animviewer.dll` (no new DLL, **no CMake change** — `animation.h` is header-only); build is clean at `/W3 /permissive-`. | implied AC6 |

> **Scope note (mirrors §3):** Story 3.2 delivers **sampled data + audit only** —
> **deformed playback is validated in Story 3.3.** The success signal here is the
> **console probe** (clip duration matches the source, root motion + pose vary with
> time, all matrices finite at the clamps) **and the bind-pose render being unchanged** —
> **not motion on screen.** The probe validates *sampling correctness and motion*, not
> yet *visual alignment* (vertex-space vs. node-baking reconciliation is 3.3's call;
> baking is left unchanged here). Deliberately deferred, may surface as known non-blocking
> notes: intermediate non-joint *animated* nodes (warn+skip — char clips animate joints
> directly), multi-clip selection / looping / cubic interpolation (MVP = clip 0, linear
> + slerp, clamp), and the GPU palette upload + skinning shader + transport-driven `t`
> (all 3.3 / Epic 4).

## 5. Story 3.3 — GPU vertex skinning (the moving rig)

Story 3.3 is the **payoff of Epic 3 and the first story whose success signal is motion
on screen.** It consumes 3.1's skin data and 3.2's `ComputePose` unchanged: every frame
the renderer computes the **skinning palette** (`palette[i] = globalMat[i] *
inverseBindMatrix[i]`) into pre-sized buffers (D2 zero-alloc), uploads it as a
`uniform mat4 u_bones[128]`, and the vertex shader **skins each vertex** by its bone
ids/weights (D13 linear blend skinning). `t` is driven by the **free-running frame
clock** (`fmod(time, duration)`), so the rig **loops continuously** — transport-driven
`t` is Epic 4. The one structural change is **vertex-space reconciliation**: skinned-mesh
vertices are now stored **un-baked** (mesh-local), so the palette — not the node bake —
places and deforms them; the static path stays world-baked and **byte-for-byte**
unchanged (§C / AC3).

This is the **dominant-risk gate**: the Linux dev box can compile and source-audit but
**cannot see the deformation** — the visual match to Blender / FBX Review is yours to
judge in Reaper, and **1–2 iterations at this gate are expected** (see §C "If it's
wrong": the `globalInverse` knob is the first thing to try for a displaced FBX rig).

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Rig deforms | Load the validated animated rig (Mixamo `Hip Hop Dancing.fbx` and/or a Demute clip). The mesh **visibly animates and loops** — limbs follow the skeleton, no frozen, exploded, or origin-collapsed geometry. | AC1 |
| 2 | Matches the reference | The deformation **matches Blender / FBX Review** for the same fixture: correct **scale, orientation, placement**, and joint motion. **Not** mirrored, not 100× too big/small, not collapsed at the origin (AC4 — a rig that "deforms but wrong" is a FAIL). | AC1 / AC4 |
| 3 | ≥60 fps | While animating at the ~20k-tri / 4-material / ~50-bone target, the docked viewport's `[RAV] … fps` line holds **≥60 fps** on the reference workstation. Record the actual fps + the fixture's tri/bone count. | AC2 (NFR-P1) |
| 4 | Static unchanged | An Epic 2 static fixture (`Box`, `Duck`, `BoxTextured`) renders **exactly as before** — it takes `u_skinned=0`, the skinning shader branch is dead, baked verts unchanged. No regression. | AC3 |
| 5 | Bone cap survivable | A >128-bone rig (if available) logs the cap warning **once** at load and renders best-effort **without crashing or garbage**. Note if no such fixture is on hand (Mixamo standard = 65, the test rig = 99 — both under the cap). | AC6 |
| 6 | Degenerate-clip survivable | A skinned-but-**clip-less** rig renders in **bind pose** (no motion, no crash, via the static path); a malformed rig (empty/size-mismatched clip) degrades to a console line and **Reaper survives** — no NaN explosion on screen. | AC7 |
| 7 | Single-DLL / warning-free | `build\Release\` holds only `reaper_animviewer.dll` (no new DLL); clean at `/W3 /permissive-`; **no new dependency**; the GL loader gains **exactly one** row (`glVertexAttribIPointer`) and the **CMake source list is unchanged** (no new `.cpp`). | AC8 |

> **Scope note (mirrors §3/§4):** Story 3.3 **closes Epic 3** — the rig now **moves**. The
> success signal is **correct deformation matching the reference**, not just "different".
> `t` is a **free-running loop** here; **transport-driven `t`** (`playhead − itemStart`,
> clamped, via a `PCM_source`) is **Epic 4** (FR10). Deliberately deferred, may surface
> as known non-blocking notes: **multi-clip selection / looping modes / blending /
> cubic-step interpolation** (MVP = clip 0, linear T/S + slerp R, clamp-then-loop); a
> **>128-bone remap** (3.3 caps + warns; a UBO/SSBO partition is post-MVP);
> **per-mesh / divergent-bind palettes** (one shared palette per skeleton is the MVP);
> and **exact normal matrices under heavy non-uniform bone scale** (`mat3(skin)` is
> correct for rigid/uniform-scale bones — the rigs in practice).

## 6. Recording the result

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

**Result:** Story 3.4 — **PASS** (Antho, in-Reaper Windows validation, 2026-07-08).
Bugfix reopening Epic 3 for UE twist-bone rigs (animated-but-unweighted joints). Fixture:
`Lvl_TopDown.glb` (UE5 `SKM_Manny_Simple` + two clips). Full-body motion — legs, arms,
neck/head, whole-body root motion — **no shards, no needle-spikes** on either clip.
Console: **89 bones, 48705 skinned verts**, zero structural channels skipped, plus the new
`gltf skin: read geometry+weights directly for 2/2 mesh(es)` line. Three root causes fixed:
skeleton = `mBones ∪ animation-channel targets` (73→89, <128 cap, `parentIdx<index` held);
a direct glTF skin/geometry read side-stepping assimp 6.0.5's Windows >4-influence weight +
inverse-bind corruption; and the missing MSVC `/EHsc` (C4530). A ≤4-influence rig still
animates (no regression); the static/FR6 path stays byte-for-byte. Code review (BMAD 3-layer)
applied 3 defensive patches to the new direct-glTF reader on malformed/exotic/secondary paths
only — none affect this validated render — and flagged a secondary-fixture re-check that Antho
confirmed OK at the gate.
