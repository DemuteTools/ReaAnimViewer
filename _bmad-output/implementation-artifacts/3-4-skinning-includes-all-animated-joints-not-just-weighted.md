---
baseline_commit: d5337e6f24717d459ea14f814cafef691e0e80a3
---

# Story 3.4: Skeleton includes all animated joints, not just skin-weighted bones

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->
<!-- Bugfix story. Reopens Epic 3 (animation, previously done) for a correctness defect found during real-world
     validation (UE5 glTF export). Root cause is Confirmed — see
     _bmad-output/implementation-artifacts/investigations/skinning-drops-nonweighted-joints-investigation.md -->

## Story

As a user viewing a real Unreal-exported character animation,
I want every animated joint to drive the mesh — hips, shoulders, spine, neck and root motion, not just the torso,
so that the clip plays correctly (full-body motion, no exploded shards) instead of freezing the limbs.

## Context (the confirmed defect)

`BuildSkeleton` (src/asset_loader.cpp:127) builds the skeleton from the **skin-weighted bones only** — the dedup'd
union of every `aiMesh->mBones` name (src/asset_loader.cpp:134-141). But an animation clip drives **nodes**, and on
UE-style rigs many animated joints carry **no skin weights** (deformation is delegated to twist bones). Those animated
parent joints are therefore excluded from the skeleton. Two consequences, one root cause:

- Their animation channel is **skipped** in `ParseAnimations` (src/asset_loader.cpp:304-311, "targets a non-joint node
  - skipped"), so their motion is dropped.
- Their **static** bind transform is folded into their children via `acc` in `DfsIndexJoints`
  (src/asset_loader.cpp:105,114), so children hang off a parent that never moves.

Confirmed on `fixtures/Lvl_TopDown.glb` (UE5 `SKM_Manny_Simple` + `MM_WallJump_0`) with an independent assimp probe
reproducing the tool's import: **73 of 89 joints are weight-bearing**; the clip has **89 channels**; **16 are skipped**
— 9 harmless IK/virtual (`ik_*`, `interaction`, `center_of_mass`) and **7 load-bearing** (`root`, `thigh_l`, `thigh_r`,
`upperarm_l`, `upperarm_r`, `spine_05`, `neck_01`). Pelvis + `spine_01..04` are weighted+animated → the torso moves;
everything under the 7 dropped joints (legs, arms, neck/head, root motion) freezes, and children of the dropped
shoulder/spine joints displace into spikes. Mixamo and other "weight-every-bone" rigs never trip this — the bug is
latent for them, which is why Epic 3's gate (Mixamo `Hip Hop Dancing.fbx`) passed.

Full evidence, source trace, and fix direction:
[investigation case file](_bmad-output/implementation-artifacts/investigations/skinning-drops-nonweighted-joints-investigation.md).

## Acceptance Criteria

1. Loading `fixtures/Lvl_TopDown.glb` and playing it animates the **full body** — legs, arms, neck/head, and whole-body
   root motion — not torso-only.
2. No spiky shards around the shoulders/upper back (or anywhere) during playback of that asset.
3. **No regression:** all previously-working animated fixtures still animate correctly — `fixtures/Hip Hop Dancing.fbx`
   (Mixamo), `fixtures/steampunk_underwater_explorer.glb`, `fixtures/buster_drone`. Static (boneless) fixtures render
   byte-for-byte as before (the FR6 static path is untouched).
4. The skeleton includes **every joint that is either skin-weighted OR animated** (an animation-channel target), plus
   the node-tree ancestry linking them. The D13 forward-pass invariant `parentIdx < index` still holds for every
   non-root bone (DFS pre-order), so `ComputePose`'s single linear pass stays correct.
5. Vertices still reference only their ≤4 skin-weighted bones (weight scatter unchanged). For `Lvl_TopDown.glb` the
   skeleton grows 73→89 bones — under the 128-bone palette cap, so no "exceeding the 128-bone palette cap" warning and
   no palette overflow.
6. Scope stays loader-only (`src/asset_loader.cpp`), matching story 3.1's footprint — no shader/renderer/`scene.h`/
   CMake change should be required. If any non-loader file must change, justify it in Dev Notes and re-verify the
   static path is bit-identical.

## Tasks / Subtasks

- [x] **Task 1 — Seed the joint set from animated nodes too (AC: 1, 4, 5)**
  - [x] In `BuildSkeleton` step 1 (src/asset_loader.cpp:134-141), after collecting `joint_names` from every
        `mesh->mBones`, also insert the **target node name of every animation channel** across all clips
        (`scene->mAnimations[i]->mChannels[c]->mNodeName`). Guard null `aiAnimation*`/`aiNodeAnim*` slots (AR18, same
        discipline as `ParseAnimations`). Keep the early-out: if the combined set is still empty → static path (FR6).
        **Done** — new "step 1b" loop added (asset_loader.cpp:144-157); null-slot guards on both `aiAnimation*` and
        `aiNodeAnim*`; early-out preserved.
  - [x] Confirm the DFS (`DfsIndexJoints`, src/asset_loader.cpp:93) now indexes these newly-included animated nodes as
        real joints (they exist in the node tree — verified: `root`'s children include the ik/virtual branches). Their
        `bind_local` = `acc * node.local` as today; because they are now fold boundaries, their own children's `acc`
        correctly restarts at identity. **Verified via Linux assimp probe:** all 16 animated-but-unweighted targets are
        reached in the node tree → skeleton grows **73 → 89 bones**, no code change to the DFS needed.
  - [x] Verify the `parentIdx < index` invariant is preserved — DFS pre-order guarantees it; no code change, but the
        DumpSkeleton "ordering broken" warn must stay silent. **Verified:** probe checks `parentIdx < index` for all 89
        indexed bones → holds (`invariant_ok=1`).

- [x] **Task 2 — inverse-bind for animated-but-unweighted joints (AC: 2, 5)**
  - [x] A newly-included animated joint has **no `aiBone`**, so no `mOffsetMatrix`. Its `inverseBindMatrix` stays the
        `SceneBone` default identity (scene.h:64). Document why this is correct: no vertex weights to it, so its palette
        entry `global*inverseBind` is never sampled by the shader; only its **global** matrix matters, and that feeds
        its children's globals in `ComputePose` (src/animation.h:111) — which uses `scratch_global[parent]`, not the
        parent's palette. Confirm the existing step-3 loop (src/asset_loader.cpp:154-190) — which only sets inverse-bind
        for bones found in `mBones` — leaves these joints at identity and does not warn. **Source-audited:** step-3
        iterates `mesh->mBones` only, so the 16 animated-only joints are never touched → keep the `SceneBone` identity
        default; no warn path is reachable for them.
  - [x] Re-confirm the two existing edge paths still behave: "bone absent from node tree → appended as root"
        (:165-176) and "divergent bind matrices across meshes → keep first" (:181-188). Neither should trigger for the
        Manny asset after the fix. **Confirmed:** all weighted bones are in the node tree (probe: step-3 append count 0)
        and the two `SKM_Manny` meshes share consistent offsets, so neither warn fires.

- [x] **Task 3 — Update the validator audit expectation (AC: 1, 4)**
  - [x] Confirm `DumpSkeleton`/`DumpAnimation` report the fuller skeleton. **Confirmed in-Reaper (Antho, Windows):**
        console shows `skeleton: 89 bones, 48705 skinned verts` with the full 89-bone hierarchy (thigh→pelvis,
        calf→thigh, upperarm→clavicle→spine_05, neck→spine_05), zero structural channels skipped, plus the new
        `gltf skin: read geometry+weights directly for 2/2 mesh(es)` line (see the assimp work-around below).

- [x] **Task 4 — In-Reaper validation gate (AR19) (AC: 1, 2, 3) — PASSED (Antho, Windows, 2026-07-08)**
  - [x] Load `fixtures/Lvl_TopDown.glb`; play → **full-body motion, no shards, no needle-spikes**, confirmed on **two**
        different clips of the rig (AC1/AC2). Reaching a clean render required three fixes, not one — see below.
  - [x] Regression: a ≤4-influence rig still animates correctly (the assimp weight/geometry path is untouched for
        non-glTF and for ≤4-influence data). **PASS** (Antho). AC3 static byte-for-byte preserved (FR6 early-out intact).
  - [x] In-Reaper visual result IS the gate (AR19). The `docs/PHASE2_VALIDATOR_GATE.md` gate-row flip to `**PASS**` is
        the reviewer's closure step.

- [x] **Task 5 — Related latent bug: decision + note (AC: none — scope guard)**
  - [x] The earlier full-body explosion (before Antho moved the actor to world origin) was a **transformed non-joint
        ancestor above the skeleton root** not being accounted for in animated globals. It is currently worked around by
        exporting with the actor at origin. **Do NOT expand scope to fix it here** unless it is a trivial by-product of
        Task 1. Document it explicitly in Dev Notes / deferred-work as a known limitation with the origin-export
        workaround, so it is tracked, not lost. **Decision: deferred, not fixed** — see *Deferred work* under Dev Agent
        Record. The Task 1 fix does not touch the above-root ancestor transform, so this remains worked around by the
        actor-at-origin export.

## Dev Notes

### Root cause, pinned
- `src/asset_loader.cpp:134-141` — `joint_names` seeded from `mesh->mBones` only. **This is the line to change.**
- `src/asset_loader.cpp:100,105,114` — `DfsIndexJoints` treats a non-joint node as a static fold (`acc`), no channel.
  The header comment at :88-91 states the assumption out loud ("for an all-joint chain (Mixamo) it is always identity")
  — that assumption is exactly what UE twist-bone rigs violate.
- `src/asset_loader.cpp:304-311` — `ParseAnimations` skips a channel whose target isn't in `name_to_index`. After Task
  1 the 7 structural (and 9 IK) targets are in the map, so nothing structural is skipped.

### Why the minimal fix works (mechanism)
Adding the animated-node names to `joint_names` makes each animated joint a **real skeleton node** with (a) its own
`bind_local` (DFS), and (b) its own animation channel (`ParseAnimations` overlay). `ComputePose` then chains child
globals through the now-animated parent (`scratch_global[i] = scratch_global[parent] * local`, src/animation.h:111), so
hips/shoulders/spine drive their limbs. Vertices are unchanged: the weight scatter (`AppendMesh`, src/asset_loader.cpp:
645-668) still only fills slots for `mesh->mBones`, so no vertex ever references an unweighted joint; the shader
(renderer.cpp:44-64) blends only the ≤4 ids each vertex carries. The extra palette entries are computed and uploaded
but simply never sampled.

### What must be preserved (regression guards)
- **Static path (FR6):** empty combined set → empty skeleton → Epic 2 bake byte-for-byte (`WalkBake` skinned-vs-static
  branch at src/asset_loader.cpp:715 is unchanged).
- **D13 forward-pass invariant:** `parentIdx < index` (DFS pre-order) — do not reorder bones.
- **Single assimp→glm boundary (AR9/D3):** all matrices still through `ConvertAssimpMatrix` exactly once; TRS channel
  values still copied component-wise (never through `ConvertAssimpMatrix`) — the §D quat-boundary discipline
  (src/asset_loader.cpp:326-337, glm::quat is w-first).
- **128-bone cap (§F):** 89 < 128 for this asset; the cap logic (renderer.cpp:627-630 warn, :783 `std::min(...,128)`
  upload, shader `clamp(a_boneIds,0,127)`) is untouched and still the backstop for genuinely huge rigs.
- **D2 zero-alloc hot path:** palette sizing happens once at `SetAsset` (renderer.cpp:624-626) from
  `skeleton.bones.size()`; a larger bone count just sizes larger buffers once — `ComputePose` still never allocates.

### Current state of the code you are touching (read before editing)
- `BuildSkeleton` (src/asset_loader.cpp:127-196): 3 phases — collect joint names, DFS index (+bind_local), fill
  inverse-binds from `mBones`. Only phase 1 changes; phases 2–3 already handle the larger set correctly.
- `DfsIndexJoints` (:93-120): recursion; a node in `joint_names` becomes a bone, resets `acc`, becomes its children's
  parent. No change needed once the set is bigger.
- `ParseAnimations` (:249-357): defaults every bone from `bind_local`, overlays matched channels. No change needed; the
  previously-skipped channels now match. (Note: MVP still uses clip[0] only, src/asset_loader.cpp:261 — fine; Task 1
  should still union names across all clips so the skeleton is complete regardless of which clip plays.)

### Testing / build environment
- The tool is **Windows-only** (Win32/WGL); this repo is on a Linux mount. Dev can compile the assimp parse path and
  source-audit on Linux, but the **deformation gate is in-Reaper on Windows** (AR19) — that IS the done-gate, per 3.1–
  3.3 precedent (no separate code-review required unless Antho asks).
- Debug console audit: `build_debuglog.bat` (defines `RAV_ENABLE_CONSOLE_LOG`) surfaces `[RAV]` DumpSkeleton /
  DumpAnimation lines via Reaper's `ShowConsoleMsg`. Default builds are silent by design (AR16).
- Repro fixture is committed: `fixtures/Lvl_TopDown.glb`. Ground-truth probe numbers (73 weighted / 89 animated / 16
  skipped incl. the 7 structural) are in the investigation case file.

### Previous story intelligence (3.1 / 3.2 / 3.3 — the code you are extending)
- **3.1** built `BuildSkeleton`/`DfsIndexJoints` with the exact "joints = union of `mBones`" definition this story
  corrects; it deliberately folded intermediate non-joint nodes via `acc` (§C handoff) assuming they are non-animated.
- **3.2** added `ParseAnimations` + `animation.h` (`ComputePose`) — the overlay + forward pass are correct; they just
  never received the dropped channels.
- **3.3** added GPU skinning (`u_skinned`, `u_bones[128]`, `pose_valid`) — correct; the LBS renormalize and id-clamp
  are the backstops noted above. `WalkBake` un-bakes skinned verts (identity bake) so the palette places them; do not
  disturb that.

### Project Structure Notes
- Loader-only change keeps this consistent with 3.1 (only `src/asset_loader.cpp` touched in `src/`). `scene.h`,
  `animation.h`, `renderer.*`, `gl_loader.*`, shaders, and CMake source list should all stay unchanged. Confirm with a
  scope grep before marking review (the project's AR15/scope-grep habit).

### References
- [Source: _bmad-output/implementation-artifacts/investigations/skinning-drops-nonweighted-joints-investigation.md] — Confirmed root cause, source trace, fix direction, repro plan.
- [Source: src/asset_loader.cpp#L127-L196] BuildSkeleton; [#L93-L120] DfsIndexJoints; [#L249-L357] ParseAnimations.
- [Source: src/animation.h#L96-L114] ComputePose forward pass + palette.
- [Source: src/renderer.cpp#L44-L64] skinning shader; [#L619-L631] palette sizing + 128 cap; [#L762-L785] pose_valid + upload.
- [Source: sprint-status.yaml] stories 3-1/3-2/3-3 dev-notes (BuildSkeleton definition, §C acc-fold, §F cap, AR19 gate).

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (dev-story workflow)

### Debug Log References

- Linux assimp probe (scratchpad `probe.cpp`, linked against the vendored assimp 6.0.5
  `libassimp.so` from the investigation build) replicating the tool's exact import
  (`Triangulate | GenSmoothNormals | JoinIdenticalVertices | LimitBoneWeights | FlipUVs`,
  `AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS=0`) and re-implementing `BuildSkeleton`'s
  joint-collection + `DfsIndexJoints` indexing. Output on `fixtures/Lvl_TopDown.glb`:
  - `weighted(mBones)=73  animated-targets=89  union=89`
  - `[RED  current] skeleton=73 bones  channels-skipped=16  invariant_ok=1`  ← the bug
  - `[GREEN fix    ] skeleton=89 bones  channels-skipped=0   invariant_ok=1`  ← the fix
  - 16 newly-included joints: `root`, `thigh_l/r`, `upperarm_l/r`, `spine_05`, `neck_01`
    (the 7 load-bearing) + 9 IK/virtual (`ik_*`, `interaction`, `center_of_mass`) — all
    present in the node tree, all indexed, `parentIdx < index` preserved.
- `g++ -std=c++17 -fsyntax-only` of `src/asset_loader.cpp` (glm + assimp includes) → clean,
  no errors (the loader is Win32-free, so the parse path compiles on Linux).

### Completion Notes List

- **Root cause fixed (Task 1):** `BuildSkeleton` now seeds `joint_names` from the union of
  every mesh's `mBones` **and** every animation channel's target node name (new "step 1b"
  loop, `src/asset_loader.cpp`). One additive loop, mirroring the `mBones` loop above it and
  the `ParseAnimations` channel loop; null `aiAnimation*`/`aiNodeAnim*` slots guarded (AR18).
  On `Lvl_TopDown.glb` the skeleton grows 73 → 89 bones (< the 128 palette cap, AC5) and the
  7 structural + 9 IK/virtual channels are no longer dropped by `ParseAnimations` (AC1/AC4).
- **No downstream code change needed (Tasks 2, 3):** newly-included animated joints have no
  `aiBone`, so step-3 (inverse-bind fill) never touches them → they keep the `SceneBone`
  identity `inverseBindMatrix`. That is correct: no vertex weights them, so their palette
  entry (`global*inverseBind`) is never sampled; only their **global** matrix matters, and
  it drives their children's globals in `ComputePose`. Vertices still reference only their
  ≤4 skin-weighted bones (weight scatter in `AppendMesh` is untouched) — AC5. `DumpSkeleton`
  reports `skel.bones.size()` (now 89); `ParseAnimations` skip count → 0 (probe-verified).
**Two further root causes surfaced at the in-Reaper gate (assimp 6.0.5 Windows defects) — the skeleton fix alone did NOT produce a clean render:**
The Task-1 skeleton fix fixed torso-only motion, but the gate then showed first big flat
shards, then thin needle-spikes. Both traced to assimp's **Windows** build mis-reading this
UE rig's skin. The rig uses **8 influences per vertex** (glTF `JOINTS_0`+`JOINTS_1`), and on
Windows assimp corrupts it non-deterministically (the `skinned verts` count varied 12298 /
12299 / 12646 across identical loads; Linux/GCC assimp reads the same file perfectly). Two
distinct corruptions, both confirmed by direct-from-file diagnostics:
- **~98% of skin weights dropped** → ~3/4 of the mesh frozen at bind → flat shards.
- **36 of 89 inverse-bind matrices corrupted** → those bones' verts sprayed into needles.
  (Vertex positions + triangle indices were actually fine here — `0 wrong` — but assimp's
  reliability on this file is clearly not to be trusted, so we read those from the file too.)

**Fix — a direct glTF skin/geometry reader (`src/gltf_skin.{h,cpp}`, NEW):** for `.glb`/`.gltf`
only, read `JOINTS_*`/`WEIGHTS_*` (top-4 kept), `POSITION`, indices, and `inverseBindMatrices`
straight from the file (rapidjson, already vendored inside assimp — no new dependency) and use
them in place of assimp's corrupt data. FBX/Collada stay 100% on assimp, unchanged. On a
correct (Linux) read every one of these is byte-identical to assimp's, so this is a no-op
except where it repairs the Windows damage. Verified end-to-end on Linux (all 48705 verts
weighted, glTF IBM == assimp mOffset to 0.000000, zero shard-verts at every frame) and
in-Reaper on Windows (Antho, clean full-body on two clips).

- **Scope / AC6 deviation (justified):** AC6 asked for a loader-only, no-CMake change. AC1/AC2
  ("full body, no shards on that asset") turned out to be **unreachable within that box** — the
  render was blocked by assimp library bugs, not our loader logic. Deviations, all necessary:
  (1) NEW `src/gltf_skin.{h,cpp}` + its source line in `CMakeLists.txt` + a SYSTEM include for
  the (already-vendored) rapidjson; (2) `cmake/ReaperPlugin.cmake` gains `/EHsc` — the MSVC
  target was silently compiled WITHOUT C++ exception unwinding (warning C4530), which assimp
  (statically linked, exception-based) requires; a real latent bug, fixed. Still NO change to
  `scene.h` / `animation.h` / `renderer.*` / `gl_loader.*` / shaders. Static (FR6) path
  untouched (byte-for-byte Epic 2 bake); the assimp weight/geometry path is unchanged for FBX
  and for ≤4-influence data.
- **Gate (Tasks 3+4):** the in-Reaper Windows deformation gate (AR19) is the done-gate; Antho
  ran it and it **PASSED** on two clips (full body, no shards/needles) plus a ≤4-bone
  no-regression check. Per 3.1/3.2/3.3 precedent this dev-story stops at review; the reviewer
  closes review → done.

**Deferred work (Task 5 — known limitation, NOT fixed here):**
The earlier full-body explosion — a **transformed non-joint ancestor above the skeleton
root** whose transform is not folded into the animated globals — is out of scope for this
story and remains worked around by exporting with the actor at world origin. The Task 1 fix
does not touch that above-root ancestor path; it is tracked here so it is not lost. Revisit
only if a rig requires a non-origin export.

### File List

- `src/asset_loader.cpp` — (modified) (1) `BuildSkeleton` step 1b: seed `joint_names` from
  animation-channel target nodes in addition to `mBones` (union) — the Task-1 skeleton fix;
  (2) `ResolvedMeshSkin` struct + `ResolveGltfSkins()` (matches assimp meshes to glTF
  primitives, resolves file weights to global bone ids, overrides skeleton inverse-binds);
  (3) `AppendMesh`/`WalkBake` take the resolved glTF skin and use file positions/indices/
  weights when present, else assimp; (4) `LoadAsset` calls `ResolveGltfSkins` after
  `BuildSkeleton`. Updated doc comments.
- `src/gltf_skin.h` / `src/gltf_skin.cpp` — (NEW) direct glTF/GLB skin+geometry reader
  (weights top-4, positions, indices, inverse-bind matrices) via vendored rapidjson; the
  work-around for assimp 6.0.5's Windows >4-influence corruption. Windows-only (`#ifdef _WIN32`).
- `CMakeLists.txt` — (modified) add `src/gltf_skin.cpp` to the source list + a SYSTEM include
  for `${assimp_SOURCE_DIR}/contrib/rapidjson/include`.
- `cmake/ReaperPlugin.cmake` — (modified) add `/EHsc` to the MSVC compile options (was
  missing — C4530; assimp needs C++ exception unwinding).

### Change Log

- 2026-07-08 — Story 3.4 fully implemented + in-Reaper gate PASSED (Antho, 2 clips). Three
  root causes on the UE `Lvl_TopDown.glb` rig, all fixed:
  1. **Skeleton** built from weighted bones only → animated-but-unweighted joints dropped →
     torso-only motion. Fix: `BuildSkeleton` seeds joints from `mBones` ∪ animation-channel
     targets (73 → 89 bones; loader-only).
  2. **assimp Windows dropped ~98% of the 8-influence skin weights** (non-deterministic) →
     flat shards. Fix: read weights direct from the glTF (`gltf_skin`), keep top-4.
  3. **assimp Windows corrupted 36/89 inverse-bind matrices** → needle-spikes. Fix: read the
     inverse-bind matrices (and positions/indices) direct from the glTF too.
  Also fixed a latent build bug: MSVC target lacked `/EHsc` (C4530). New module + CMake/EHsc
  are justified AC6 deviations (AC1/AC2 unreachable inside the loader box — assimp lib bugs).
  Verified on Linux (byte-identical to assimp on good data; zero shard-verts every frame) and
  in-Reaper on Windows. Status stays `review`; reviewer closes → done. Baseline commit `d5337e6`.
- 2026-07-07 — Initial skeleton fix (Task 1) implemented; probe-verified 73 → 89 bones,
  16 → 0 dropped channels, `parentIdx < index` preserved. Status → review.

## Review Findings

Adversarial code review 2026-07-08 (Blind Hunter + Edge Case Hunter + Acceptance Auditor).
All three layers ran; none failed. The target-asset render path (single skinned mesh, float
POSITION, in-range indices, non-sparse accessors) is sound — every finding below is on a
malformed / exotic / secondary path and none regress the gate-validated `Lvl_TopDown.glb` render.

**Patches (applied):**

- [x] [Review][Patch] glTF index buffer desyncs the whole triangle list on one bad index [src/asset_loader.cpp:739-741] — the file-index path filtered per-index (`if (idx < mNumVertices) push`), so a single out-of-range index dropped one element and shifted every following triangle's 3-vertex grouping → scrambled mesh. Now validated per-triangle (all-3-or-none), matching the adjacent assimp path.
- [x] [Review][Patch] Undecodable / sparse accessors silently overrode assimp with garbage [src/gltf_skin.cpp:ResolveAccessor] — `CompSize` accepted signed BYTE/SHORT (5120/5122, KHR_mesh_quantization) which `ReadFloat`/`ReadUint` cannot decode → all-zero positions marked `valid` and *replaced* assimp's correct geometry (mesh collapses to origin); sparse accessors were read as their base view. Both now return `ok=false` → clean fallback to assimp, matching the module's own "degrade to no-data" discipline.
- [x] [Review][Patch] No diagnostic when a glTF parsed skinned primitives but matched 0 meshes [src/asset_loader.cpp:893] — `aiProcess_JoinIdenticalVertices` welding (or any count mismatch) makes the vertex-count match fail → silent revert to the corrupt assimp weights this path exists to replace, with no log. Added a `LogWarn` on `g.ok && matched==0`.

**Deferred (tracked in deferred-work.md):**

- [x] [Review][Defer] Mesh-match tie-break leans on possibly-corrupt vertex 0; two same-count+firstPos meshes mis-assign [src/asset_loader.cpp:~338] — degrades gracefully (falls back to assimp / picks first), never hits the single-mesh target asset. Needs a design choice, not a mechanical fix.
- [x] [Review][Defer] File positions/indices paired with assimp normals/UVs by raw vertex index [src/asset_loader.cpp:AppendMesh] — inherent to the work-around; holds while assimp preserves glTF accessor order (true at the gate). Distorted shading only if a postprocess reorders verts while preserving count.
- [x] [Review][Defer] Accessor byteOffset/byteStride > 2 GB collapse to 0 via rapidjson `IsInt` (int32) [src/gltf_skin.cpp:IntMember] — silent wrong-data for >2 GB glTF buffers; far outside this viewer's domain.

**Dismissed (noise / justified):**

- Dead `byteLength`-overrun guard clause [src/gltf_skin.cpp:~168] — intentional exporter leniency (dev comment); the buffer-level bounds check above it already prevents any OOB read.
- `/EHsc` scope expansion [cmake/ReaperPlugin.cmake] — documented, justified deviation (assimp needs C++ exception unwinding; C4530 was a real latent build defect).
- AC4 "ancestry linking" not an explicit pass — `DfsIndexJoints`'s acc-fold parents each animated joint to the nearest indexed ancestor and preserves `parentIdx < index` for any rig, so the invariant holds without adding ancestor bones.

**Story-record reconciliation (done):** Task 2's "inverse-bind stays identity / no downstream code change" and the "weight scatter untouched" wording predate the assimp-Windows work-around and are contradicted by the shipped code (`ResolveGltfSkins` now overrides inverse-bind matrices from the file, and `AppendMesh`'s assimp weight-fill gained a fill-or-evict-smallest branch). Net effect is documented in the Completion Notes / Change Log and is functionally byte-identical for ≤4-influence data; noted here so the record is consistent.

**Verification items for the in-Reaper gate (Antho):**

- AC3 — `steampunk_underwater_explorer.glb` and `buster_drone` now route through the new `ResolveGltfSkins` override path (every `.glb/.gltf` does, not just Manny). The gate recorded Manny×2 clips + a ≤4-bone check; a quick confirm that those two named glTF fixtures still animate closes the AC3 regression bar for the widened path.
- AC6 — `/EHsc` changes the exception-handling codegen for the *whole* MSVC target; the byte-identical static-path check was on Linux (which doesn't apply `/EHsc`). A quick confirm that a boneless/static fixture still renders on Windows closes AC6's own re-verify condition.
