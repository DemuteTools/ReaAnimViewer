# Investigation: Skinned animation drops non-weighted parent joints (UE Manny "only torso moves" + shoulder spikes)

## Hand-off Brief

1. **What happened.** A UE5-exported glTF (`fixtures/Lvl_TopDown.glb`, SKM_Manny_Simple + MM_WallJump clip) renders a correct bind pose but on playback only the torso animates while limbs/head freeze and shards spike around the shoulders/upper back. **Confirmed** root cause: the viewer builds its skeleton from only the weight-bearing bones (73 of 89), so animated but non-skinning parent joints (`root`, `thigh_l/r`, `upperarm_l/r`, `spine_05`, `neck_01`) are excluded, their animation channels are dropped, and their motion is baked as static into their children's bind-local.
2. **Where the case stands.** Root cause Confirmed by an independent assimp probe that reproduces the tool's import path exactly (same flags, same `BuildSkeleton`/`ParseAnimations` logic). Pinned to `src/asset_loader.cpp` skeleton construction.
3. **What's needed next.** Fix direction identified (seed the joint set from animated nodes + ancestor chain, not just `mesh->mBones`). Ready for a story/quick-dev fix; no further diagnosis required.

## Case Info

| Field            | Value |
| ---------------- | ----- |
| Ticket           | N/A |
| Date opened      | 2026-07-07 |
| Status           | Concluded |
| System           | Reaper extension (Windows/Win32/WGL); repro asset exported from UE 5.8 glTF Exporter v1.3.1 |
| Evidence sources | `fixtures/Lvl_TopDown.glb` (direct glTF JSON inspection); independent assimp v6 probe compiled from the vendored assimp source on Linux; source read of `src/asset_loader.cpp`, `src/animation.h`, `src/renderer.cpp` |

## Problem Statement

Testing the viewer with a real Unreal asset. After working around an earlier full-body explosion (caused by a world-placement ancestor node — actor moved to world origin), two defects remained:
- **BUG 2:** on playback only the torso moves; limbs and head stay static, though the clip animates all bones.
- **BUG 3:** a few bones render as spiky shards around the upper back/shoulders during playback.
Bind pose (t=0) renders correctly.

## Evidence Inventory

| Source | Status | Notes |
| ------ | ------ | ----- |
| `fixtures/Lvl_TopDown.glb` glTF JSON | Available | 90 nodes, 89 joints, single scene root, 1 clip `MM_WallJump_0`, 267 channels = T/R/S on all 89 joints; all joints named, unique; inverse-bind unit-scale. Data is textbook-valid. |
| assimp v6 import probe (Linux, vendored source) | Available | Reproduces `Importer` flags + `BuildSkeleton` DFS + `ParseAnimations` name-match. Ground truth for what the tool actually sees. |
| `src/asset_loader.cpp` | Available | `BuildSkeleton` (:127), `DfsIndexJoints` (:93), `ParseAnimations` (:249), `WalkBake` (:692), `AppendMesh` (:606). |
| `src/animation.h` `ComputePose` | Available | Palette math correct given a correct skeleton. |
| `src/renderer.cpp` skinning shader + upload | Available | LBS + weight renormalize + id clamp correct; 128-bone cap not hit (89 < 128). |
| Reaper `[RAV]` console audit (`build_debuglog.bat`) | Not collected | Would independently confirm on Antho's Windows build; not needed — probe already confirms. |

## Confirmed Findings

### Finding 1: The exported animation data is valid; all 89 joints are animated
**Evidence:** glTF JSON of `fixtures/Lvl_TopDown.glb` — `animations[0]='MM_WallJump_0'`, 89 channels, all target named, unique joint nodes; T/R/S present; single scene root; inverse-bind matrices unit-scale.
**Detail:** The defect is not in the export. No export setting change will fix it.

### Finding 2: Bind pose renders correctly → skeleton/inverse-bind/weights are individually correct
**Evidence:** User screenshot after the origin fix — character stands, mostly correct; spikes/freeze appear only on playback.
**Detail:** Isolates the defect to the *animation overlay / hierarchy*, not to mesh binding.

### Finding 3 (ROOT CAUSE): The skeleton is built from only weight-bearing bones, excluding 16 animated joints
**Evidence:** assimp probe on `Lvl_TopDown.glb`:
- assimp splits the mesh by material into 2 aiMesh (27 + 62 bones); **union of `mesh->mBones` = 73** joint names.
- `DfsIndexJoints` indexes 73 bones; 0 absent from the node tree.
- Clip has **89 channels**; **73 match a skeleton joint, 16 are skipped** because their target bone is not weight-bearing:
  `center_of_mass, interaction, ik_hand_r/l/gun/root, ik_foot_r/l/root` (9 IK/virtual — harmless) **plus `root, spine_05, neck_01, thigh_l, thigh_r, upperarm_l, upperarm_r` (7 structural, animated, load-bearing).**
- Code: `BuildSkeleton` collects joint names ONLY from `mesh->mBones` — `src/asset_loader.cpp:134-141`.
**Detail:** The 7 structural skipped bones are ancestors of weighted bones. `thigh_l/r` drive the legs, `upperarm_l/r` the arms, `spine_05`/`neck_01` the neck/head, `root` the whole-body root motion. Their rotation is the hip/shoulder/spine swing of the jump.

## Deduced Conclusions

### Deduction 1: Skipped animated parents freeze limbs (BUG 2)
**Based on:** Findings 2, 3 + `DfsIndexJoints` fold logic.
**Reasoning:** A node not in the (weight-derived) joint set is treated as a non-joint. `DfsIndexJoints` folds its **static** `node->mTransformation` (bind pose) into descendants' bind-local via `acc` (`src/asset_loader.cpp:105,114`) and assigns it no channel (`ParseAnimations` skips it, `:304-311`). So a skipped **animated** joint contributes only its *rest* transform; its animation is silently dropped. Legs (under `thigh_l/r`), arms (under `upperarm_l/r`), neck/head (under `spine_05`/`neck_01`), and root motion (under `root`) therefore do not move. Pelvis + `spine_01..04` **are** weight-bearing and animated → the torso moves. This is exactly "only the torso moves."
**Conclusion:** BUG 2 = dropped animation on non-weighted parent joints.

### Deduction 2: The same drop mis-places children → shoulder/back spikes (BUG 3)
**Based on:** Finding 3 + the `acc` static fold.
**Reasoning:** A weighted child whose ancestor chain crosses a skipped **animated** joint carries that joint's *static* transform baked into its bind-local, but at playback its own animated local is applied on top of a hierarchy that assumes the parent never moved. The mismatch displaces the child. The densest cluster of skipped structural joints is the shoulder/upper-spine region (`upperarm_l/r`, `clavicle` children, `spine_05`, `neck_01`) — matching the observed spiky shards there.
**Conclusion:** BUG 3 is the same root cause; its severe cases surface where skipped animated joints are concentrated.

### Deduction 3: Why other fixtures animate fine
**Based on:** Finding 3 + rig conventions.
**Reasoning:** Mixamo/other rigs weight vertices to every FK bone, so `mesh->mBones` ≈ the full animated set and nothing is skipped (the code comment at `src/asset_loader.cpp:88-91` even assumes skipped intermediates are identity/non-animated). UE's Manny uses twist bones for deformation and leaves the main FK bones (`thigh`, `upperarm`, …) as non-skinning drivers — so those animated bones fall outside `mesh->mBones` and get dropped. The bug is latent for weight-everything rigs and triggered by UE-style twist-bone rigs.

## Missing Evidence

| Gap | Impact | How to Obtain |
| --- | ------ | ------------- |
| `[RAV]` console audit on Windows | Independent cross-check (probe already Confirms) | Build `build_debuglog.bat`, load the glb, read Reaper console (`skeleton: N bones`, `channel '…' targets a non-joint node - skipped`). Optional. |

## Source Code Trace

| Element | Detail |
| ------- | ------ |
| Error origin | `src/asset_loader.cpp:134-141` — `BuildSkeleton` seeds `joint_names` only from `mesh->mBones` (weight-bearing bones). |
| Trigger | `src/asset_loader.cpp:100,105,114` — `DfsIndexJoints` treats any node outside that set as a non-joint, folding its **static** transform into descendants and giving it no channel. |
| Condition | Rig with animated joints that carry no skin weights (UE twist-bone skeletons): `root`, `thigh_l/r`, `upperarm_l/r`, `spine_05`, `neck_01`. |
| Related files | `src/asset_loader.cpp` (`ParseAnimations:304-311` skip path), `src/animation.h` (`ComputePose` — correct given a correct skeleton), `src/renderer.cpp` (shader/upload — correct). |

## Conclusion

**Confidence:** High — Confirmed root cause via an independent probe that reproduces the tool's import; deterministic; pinned to source lines.

The viewer's skeleton is the set of *skin-weighted* bones, not the set of *animated* bones. Any animated joint without direct vertex weights (standard in UE twist-bone rigs) is excluded, its animation dropped, and its rest transform baked statically into its children — freezing every limb/segment it should drive (BUG 2) and displacing children hung off it (BUG 3, spikes).

## Recommended Next Steps

### Fix direction
Build the joint set from the **union of (a) `mesh->mBones` names and (b) every animation channel's target node name**, then include the node-tree ancestry chain linking those joints up to their common root — instead of `mesh->mBones` alone (`src/asset_loader.cpp:134-141`). Every animated joint then becomes a real skeleton node with its own channel and contributes its animated transform to `ComputePose`'s hierarchy; vertices still reference only their ≤4 weighted bones, but the palette globals are now correct. Bone count rises 73→89 (< 128 cap, no palette-size impact). Revisit the `acc` static-fold assumption at `DfsIndexJoints` (`:88-91,105,114`): it is only safe for genuinely non-animated intermediate nodes.
- Related latent bug (already worked around by moving the actor to origin): a transformed non-joint ancestor above the skeleton root is not accounted for in animated globals — worth folding into the same fix.

### Diagnostic
Optional confirmation on Windows: `build_debuglog.bat` → load `fixtures/Lvl_TopDown.glb` → console shows `skeleton: 73 bones` and 7 structural `... targets a non-joint node - skipped` lines.

## Reproduction Plan
1. Export any UE5 Manny/Metahuman skeletal mesh + animation as `.glb` (actor at world origin).
2. Load in the viewer; play.
3. Observe torso-only motion + shoulder/upper-back spikes.
Repro asset committed at `fixtures/Lvl_TopDown.glb`.

## Side Findings
- assimp splits a multi-material glTF mesh into one aiMesh per material (2 here: 27 + 62 bones) — expected; not a defect.
- The 9 IK/virtual skipped channels (`ik_*`, `interaction`, `center_of_mass`) are correctly harmless (no weighted descendants); a fix that adds all animated joints will include them too, which is fine (they just won't influence vertices).
