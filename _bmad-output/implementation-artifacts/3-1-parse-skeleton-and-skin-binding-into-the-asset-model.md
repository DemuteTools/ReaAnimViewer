# Story 3.1: Parse skeleton and skin binding into the Asset model

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As the implementing developer,
I want the bone hierarchy and per-vertex skin weights loaded into the `Asset`,
so that the data needed for deformation exists in a single, auditable structure.

This is **Story 1 of Epic 3 (Phase 2 — the dominant technical risk, 7–10 days)**. Epic 2 ships a textured, multi-material, camera-controllable **static** mesh; a skinned glTF/GLB today loads fine but through the **static path** — [asset_loader.cpp:245](../../src/asset_loader.cpp#L245) literally says `// boneIds / boneWeights stay zero-filled — static path (FR6)`, [asset_loader.cpp:341](../../src/asset_loader.cpp#L341) hard-codes `out.skinned = false`, and `Asset::skeleton` / `Asset::animations` are left empty. The rig renders correctly in **bind pose** because `WalkBake` bakes every node's world transform into the vertex positions.

Story 3.1 fills the **data** that Stories 3.2 (sample channels → per-frame bone matrices) and 3.3 (GPU vertex skinning) will consume. It is **loader-only and purely additive**: it parses the flat `SceneSkeleton` (bones with `parentIdx` + `inverseBindMatrix`), scatters per-vertex `boneIds`/`boneWeights`, sets `SceneMesh::skinned = true` on skinned meshes, and emits a **console skeleton dump** so the validator can cross-check bone count / parent links / bind matrices against Blender or FBX Review. **It deliberately changes no shader, no renderer, and no GL resource code** — `skinned` drives nothing until 3.3 (nothing in `renderer.cpp` / `viewer_window.cpp` reads it yet), so the rendered output is **byte-for-byte identical to Epic 2** for every fixture. The deformation itself (and any vertex-space rework that skinning requires) belongs to 3.3.

Two facts decide the shape of the work:

- **The target structs already exist.** [scene.h](../../src/scene.h) declared `SceneVertex.boneIds/boneWeights`, `SceneBone{parentIdx, inverseBindMatrix, name}`, `SceneSkeleton`, and the keyframe/animation types back in 2.1 so the `Asset` shape would be final and the VBO layout would never churn. 3.1 **populates** them; it does **not** author or move any type, and adds **no CMake change** (no new `.cpp`).
- **The matrix boundary already exists and is the heart of this story.** [`ConvertAssimpMatrix`](../../src/asset_loader.cpp#L57-L63) is the single row-major→column-major transpose (AR9/D3). `aiBone::mOffsetMatrix` (the inverse bind matrix) **must** pass through it **exactly once** — not a hand-rolled transpose, not a second `glm::transpose`, not a double-convert. The architecture's own warning: *"once a single mat4 transposes wrongly at the assimp boundary, debugging a skinned rig is hell"* ([architecture.md](../planning-artifacts/architecture.md) Decision Priority Analysis).

## Acceptance Criteria

From [epics.md#Story 3.1](../planning-artifacts/epics.md) (lines 408–411), verbatim BDD:

**Given** a skinned glTF/GLB rig
**When** the loader populates `SceneSkeleton` (bones flat with `parentIdx`, `inverseBindMatrix`) and per-vertex `boneIds`/`boneWeights` (D1)
1. **Then** the bone count, parent links, and bind matrices match the source as cross-checked against Blender/FBX Review for at least one Demute fixture
2. **And** matrices pass through the single `convertAssimpMatrix` boundary (row-major → column-major) **exactly once** (AR9).

Implied, non-negotiable (the system must stay working end-to-end — requirements even though not in the AC text):

3. **Zero render regression — static AND skinned files render exactly as in Epic 2.** Because no consumer reads `skinned`/`skeleton`/`boneIds` yet, the rendered image, the auto-fit framing, the camera, and every 2.1–2.4 gate row must be unchanged. A skinned rig still renders in baked bind pose; a static (no-bone) file still takes the `skinned=false` path with empty skeleton and zero-filled bone attributes (FR6). **`WalkBake`'s vertex-position baking is unchanged in this story** (see Dev Notes §F — vertex-space rework, if any, is 3.3's call).
4. **The skeleton must be auditable without a renderer.** Since the rig is not yet drawn deformed (that is 3.3), the only way to satisfy AC1 is a **console diagnostic dump** through the existing D7/AR16 funnel ([console_log.h](../../src/console_log.h), `[RAV]` prefix): on every successful load of a skinned file, log the bone count and, per bone, `index`, `name`, and `parentIdx` (and parent name). This is the click-based validator hook (load a file → read the console), no env-var/CLI. See Dev Notes §E.
5. **No-throw host boundary preserved (AR18).** All new parsing stays inside the existing single `try/catch` in `LoadAsset`; all `aiScene`/`aiMesh`/`aiBone` access stays inside `asset_loader.cpp` (the only TU that includes `<assimp/...>`, D5). A corrupt/over-limit rig degrades to a `LoadResult` category or a console line — it never throws across the boundary and never crashes Reaper (NFR-R1/AR17).
6. **The deliverable remains a single `reaper_animviewer.dll`**, builds **warning-free under `/W3 /permissive-`** for our own sources (NFR-R5), and adds **no new dependency** (assimp + glm already vendored; no new importer, no stb call).

## Tasks / Subtasks

- [x] **Task 1 — Build the flat `SceneSkeleton` from the node tree + skin joints (AC: 1, 2)**
  - [x] In `asset_loader.cpp` (anonymous namespace), add a helper that produces a `SceneSkeleton` plus a **`bone name → global index` map** from the `aiScene`. The skeleton's bones are the **skin joints** — the union of every `aiMesh->mBones[*]->mName` across all meshes (deduplicated by name). See Dev Notes §B for the exact algorithm.
  - [x] **Ordering = node-tree DFS pre-order**, restricted to nodes that are skin joints. This guarantees **parent-before-child** so 3.2's single linear global-matrix pass is valid (D1/D13). Assign each joint its global index in this DFS order.
  - [x] **`parentIdx` = the global index of the nearest ancestor node that is itself a skin joint**, or `-1` if none (root joint). Intermediate non-joint nodes between two joints are skipped here; their transforms are 3.2's concern (Dev Notes §F).
  - [x] **`inverseBindMatrix` = `ConvertAssimpMatrix(bone->mOffsetMatrix)`** — through the one boundary, once (AC2). Take it from the bone's **first occurrence**; if the same bone name recurs across meshes with a *different* offset matrix, keep the first and `LogWarn` once (multi-mesh shared-bone divergence is a 3.3 concern, §F).
  - [x] `name` = `bone->mName.C_Str()` **preserved as-authored, including non-ASCII** (FR5/AR13) — `std::string` is UTF-8, no transliteration, no normalization.
  - [x] Populate `asset.skeleton.bones`. If **no mesh has bones**, leave `asset.skeleton.bones` empty — the FR6 static path, unchanged.

- [x] **Task 2 — Scatter per-vertex `boneIds` / `boneWeights`; set the `skinned` flag (AC: 1, 3)**
  - [x] Thread the skinned data into the existing CPU-mesh build. `AppendMesh` currently zero-fills bone attributes ([asset_loader.cpp:245](../../src/asset_loader.cpp#L245)); give it (and `PendingMesh`) access to the per-mesh→global bone-index remap and the `aiMesh` bone weights so it can fill the slots. See Dev Notes §C for the scatter recipe.
  - [x] For each `aiBone` `b` on the mesh (local index `lb`), look up its **global** skeleton index `gb` via the §B name map. For each `aiVertexWeight {mVertexId, mWeight}`, write `gb`/`mWeight` into the **next free slot** (0..3) of that vertex's `boneIds`/`boneWeights`. `aiProcess_LimitBoneWeights` (already set, [asset_loader.cpp:392](../../src/asset_loader.cpp#L392)) guarantees ≤4 influences and renormalizes them to sum 1.0 — but **guard against >4** defensively (skip the overflow slot, don't UB-write past index 3).
  - [x] Unused slots stay `boneId=0, weight=0` (the zero init `SceneVertex v{}` already does this). A zero-weight slot contributes nothing in 3.3's `Σ w·palette`, so a leftover `boneId=0` is harmless.
  - [x] Set `out.skinned = (mesh->mNumBones > 0)` in `UploadMesh` (replacing the hard-coded `false` at [asset_loader.cpp:341](../../src/asset_loader.cpp#L341)). **This flag drives no code path in this story** — it is data for 3.3's path selection (D13/FR6). Confirm by grep that nothing in `renderer.*`/`viewer_window.*` reads it (it doesn't, today).
  - [x] The interleaved VBO already uploads the whole `SceneVertex`, so the bone attributes reach the GPU automatically — **no renderer VAO change** here (binding attribute locations 3/4 is 3.3, when the skinning shader needs them).

- [x] **Task 3 — Console skeleton dump (the validator hook) (AC: 1, 4)**
  - [x] After a successful load, when `asset.skeleton.bones` is non-empty, emit a `LogInfo` summary via [console_log.h](../../src/console_log.h): a header line with the bone count, then one line per bone: `idx | name | parentIdx (parent name)`. Also log total skinned-vertex / influence sanity (e.g. count of vertices with ≥1 non-zero weight). Keep it to the D7 one-line-per-event spirit but a compact table is acceptable for a load-time audit. Exact format in Dev Notes §E.
  - [x] This is the artifact AC1 is cross-checked against — make names + parent links legible enough to diff by eye against Blender's Outliner / FBX Review's skeleton tree.

- [x] **Task 4 — Create the Phase 2 validator gate with Story 3.1 rows (AC: 1, 2, 3)**
  - [x] Create **`docs/PHASE2_VALIDATOR_GATE.md`** (new — Epic 3 is Phase 2; the pattern is `docs/PHASE<N>_VALIDATOR_GATE.md`, AR19, the sole authority on phase completion). Mirror the structure of `docs/PHASE1_VALIDATOR_GATE.md`. Add a **"## 1. Story 3.1 — skeleton & skin parse (audit)"** section with rows:
    - **Bone count matches** — load a Demute skinned glTF/GLB; the console bone count equals the joint/bone count shown in Blender (Outliner → Armature) or FBX Review.
    - **Parent links match** — spot-check several bones' `parentIdx`/parent-name against the Blender bone hierarchy (root has `parentIdx = -1`).
    - **Bind matrices sane** — the logged inverse-bind matrices are finite and, where checkable, consistent with the source (a quick proxy: re-multiplying by the bone's bind global ≈ identity — optional, dev's discretion).
    - **Non-English bone names intact** — a fixture with non-ASCII bone names (FR5) logs them un-mangled.
    - **Zero render regression** — the skinned rig still renders in bind pose exactly as in Epic 2; re-run the Phase 1 gate's 2.1–2.4 rows — all still pass; a static (no-bone) file still loads via the empty-skeleton path with no skeleton dump.
    - **Single-DLL / warning-free** — only `reaper_animviewer.dll`; clean at `/W3 /permissive-`.
  - [x] Add a **scope note** (mirroring the Phase 1 tuning notes): Story 3.1 delivers *data + audit only*; deformed playback is validated in 3.3. The bind-pose render being unchanged is the success signal here, not motion.

## Dev Notes

### Critical orientation — this is data-plumbing, not rendering

Trust the **live tree over `architecture.md`'s stale names** (same caveat as every Epic 1/2 story): the doc still says "ReaImGui panel" / "sokol_gfx" / "FBO bridge" and uses the old `fbxav` namespace and `[FBXAV]` console prefix. All superseded — the live code is a **direct-render native GL child window**, namespace **`rav`**, console prefix **`[RAV]`** (Spec Change Log 2026-06-23 + AR21 rename). `src/` is flat. **D1's data model and D3/AR9's matrix boundary are current and authoritative; D13's per-frame skinning math is the forward contract that fixes the *shape* of the data 3.1 produces but is NOT implemented here.**

What 3.1 touches: **only [`src/asset_loader.cpp`](../../src/asset_loader.cpp)** (+ a new `docs/PHASE2_VALIDATOR_GATE.md`). It does **not** edit `scene.h` (types already final), `renderer.*`, `viewer_window.*`, any shader, `gl_loader.*`, `gpu_resources.h`, or `CMakeLists.txt`. If you find yourself changing the renderer, you have left this story's scope — stop.

### §A — The existing load flow you are extending

`LoadAsset` ([asset_loader.cpp:371](../../src/asset_loader.cpp#L371)) already: opens the file (UTF-8/`u8path`), sets `AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS=0` (Spike Finding 3 — Mixamo rigs stay in bind pose without it; harmless for glTF, the loader is shared), imports with `aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights` (note: **no** `PreTransformVertices` — it would destroy the node hierarchy 3.1 needs, and **no** `MakeLeftHanded` — render as-authored), then `WalkBake` recurses the node tree baking world transforms into `PendingMesh` CPU arrays, resolves materials/textures, and `UploadMesh`es each into a `SceneMesh`. Your skeleton parse slots in **after** `scene` is validated and **before/around** the mesh walk; the per-vertex scatter happens **inside** the mesh build (you need the bone-index map first, so build the skeleton before walking meshes, or pass the map down).

Recommended order inside `LoadAsset` (after the `pending.empty()` guard or just before the mesh walk):
1. Build skeleton + `name→globalIdx` map (Task 1 / §B). Cheap, allocates at load time only (fine — hot-path alloc discipline is a *per-frame* rule, this is load).
2. Walk/append meshes as today, but the scatter (Task 2 / §C) consults the map.
3. Set `asset.skeleton = std::move(skeleton)`.
4. Dump (§E).

`Asset` stays move-only and movable — your added `std::vector<SceneBone>`/`std::string` members are trivially movable, so no special handling for the D4 reload-swap.

### §B — Skeleton build algorithm (parent-before-child, dedup by name)

assimp exposes joints as **`aiMesh->mBones[]`** (per-mesh), each `aiBone` carrying `mName`, `mOffsetMatrix` (= inverse bind), and `mWeights[]`. The **parent/child** relationship is **not** on `aiBone` — it lives in the **`aiNode` tree** (`scene->mRootNode` → `mChildren`), matched to bones by **name**. So:

```text
1. SET joint_names = { b->mName  for every mesh m, for every m->mBones[b] }   // dedup
2. DFS the aiNode tree from scene->mRootNode, pre-order, carrying nearest_joint_ancestor_index (start -1):
     if node->mName is in joint_names AND not already indexed:
         gidx = bones.size()
         name_to_index[node->mName] = gidx
         bones.push_back({ parentIdx = nearest_joint_ancestor_index, name = node->mName, inverseBindMatrix = <filled in step 3> })
         recurse children with nearest_joint_ancestor_index = gidx
     else:
         recurse children with nearest_joint_ancestor_index unchanged
3. Fill inverseBindMatrix: iterate meshes' mBones; for each, gidx = name_to_index[bone->mName];
     if bones[gidx].inverseBindMatrix not yet set: bones[gidx].inverseBindMatrix = ConvertAssimpMatrix(bone->mOffsetMatrix)
     else if it differs meaningfully: LogWarn once (shared-bone offset divergence — §F), keep first.
```

DFS pre-order gives **parent-before-child ordering for free** (a node is pushed before its descendants), which is exactly the invariant D13's `globalMat[i] = globalMat[parent[i]] * localMat[i]` single forward pass relies on. **`parentIdx < i` for every non-root bone** — assert/verify this in the dump; if it ever fails, the DFS was wrong.

Edge cases: a joint that appears in `mBones` but **not** in the node tree (malformed file) → it won't get indexed by the DFS; fall back to appending it with `parentIdx = -1` and `LogWarn`, so its weights still resolve. A bone whose `mOffsetMatrix` is never set stays at the `SceneBone` default `mat4(1.0f)` identity (the struct default) — acceptable, log it.

### §C — Per-vertex weight scatter

Bone indices stored in `SceneVertex::boneIds` must be **global skeleton indices** (the §B `name_to_index` values), because 3.3 indexes its matrix palette as `paletteMat[boneId[k]]` and the palette order == `skeleton.bones` order. assimp's per-mesh `mBones` local index is **not** the global index — you must remap.

Recipe, per mesh (do this where `AppendMesh` fills vertices; `base` is the vertex offset already used for indices):

```text
for lb in [0 .. mesh->mNumBones):
    aiBone* b = mesh->mBones[lb]
    int gb = name_to_index[b->mName]          // global skeleton index
    for w in [0 .. b->mNumWeights):
        v = base + b->mWeights[w].mVertexId    // assimp vertex id is mesh-local; AppendMesh offsets by `base`
        float wt = b->mWeights[w].mWeight
        // find next free slot in verts[v].boneIds/boneWeights (slot with weight==0), up to 4
        if a free slot exists: boneIds[slot]=gb; boneWeights[slot]=wt;
        else: skip (LimitBoneWeights should prevent this; guard so we never write index>3)
```

Important interaction with `aiProcess_JoinIdenticalVertices` (active): assimp assigns weights in the **joined** vertex index space, the same space `mesh->mVertices` / `mFaces` use — so `mWeights[].mVertexId` lines up with the vertices `AppendMesh` is pushing. No special handling, just don't reorder vertices. Keep the existing `base` offset logic so multi-mesh assets stay consistent.

Weights from `LimitBoneWeights` are already normalized to sum 1.0; do **not** add a renormalization pass unless a fixture's dump shows sums materially off 1.0 (flag it if so — that would be a story-spec judgment call, not a pinned decision).

### §D — The one matrix rule (AC2, the explicit success criterion)

`aiMatrix4x4` is **row-major**; `glm::mat4` is **column-major**. [`ConvertAssimpMatrix`](../../src/asset_loader.cpp#L57-L63) transposes exactly once and is the **only** place any assimp matrix becomes a `glm::mat4`. For 3.1 that means `mOffsetMatrix` → `ConvertAssimpMatrix(...)` → `SceneBone::inverseBindMatrix`, full stop. **Do not** also `glm::transpose` it, **do not** build it with `glm::make_mat4` (that assumes column-major source and would give the wrong result), **do not** convert it in two places. The node-transform path already uses `ConvertAssimpMatrix` ([asset_loader.cpp:282](../../src/asset_loader.cpp#L282)); your bind-matrix path uses the same function. One boundary, audited by eye in the §E dump.

### §E — Console dump format (Task 3)

Use the `[RAV]` info channel. Suggested shape (legible to diff against Blender):

```text
[RAV] info: skeleton: 27 bones, 1842 skinned verts
[RAV] info:   [ 0] Hips                 parent -1 (root)
[RAV] info:   [ 1] Spine                parent  0 (Hips)
[RAV] info:   [ 2] Chest                parent  1 (Spine)
...
```

`LogInfo` is printf-style/variadic ([console_log.h](../../src/console_log.h)); one call per line is simplest and matches the D7 one-line-per-event format. Non-ASCII bone names print through `ShowConsoleMsg` as their UTF-8 bytes — that is the FR5 check (they must come through un-mangled). Only dump when `skeleton.bones` is non-empty (a static file produces no skeleton noise — preserves the clean Epic 2 console).

### §F — Explicitly deferred to 3.2 / 3.3 (do NOT do these here, but set them up cleanly)

These are real and known; flagging them now prevents a 3.3 ambush. **None block 3.1's ACs**, which validate parsed data + bind-pose-unchanged render, not deformation.

- **Vertex space vs. node baking.** `WalkBake` bakes each mesh's node world transform into vertex positions. For correct LBS, skinned vertices live in **skin/mesh space** matching the inverse-bind matrices, and the skinned mesh node transform is typically *not* baked (glTF ignores it; the joints carry placement). Reconciling this — likely storing skinned vertices un-baked and letting the palette place them — is **3.3's** decision when the skinning shader and the no-static-regression AC are on the table. 3.1 leaves baking **unchanged** so the bind-pose render stays identical to Epic 2.
- **Intermediate non-joint nodes** between two joints (their local transforms) must be folded into the child joint's local transform when 3.2 composes global matrices. 3.1's `parentIdx` points to the nearest *joint* ancestor; the skipped nodes' transforms are 3.2's to accumulate.
- **Multi-mesh shared-bone offset divergence** (§B step 3) — if real fixtures hit it, 3.3 may need per-mesh palettes. 3.1 keeps the first offset + warns.
- **Channels / animation sampling** (`SceneAnimation`, TRS keyframes) — 3.2. 3.1 leaves `asset.animations` empty.
- **GPU palette upload, VAO attribute locations 3/4, skinning vertex shader, `skinned`-flag path selection in the renderer** — 3.3.

### Project Structure Notes

- **Files touched:** `src/asset_loader.cpp` (parse + scatter + dump), new `docs/PHASE2_VALIDATOR_GATE.md`. No header, no CMake, no other `.cpp`.
- **Conventions** ([architecture.md](../planning-artifacts/architecture.md) Naming/Standards): namespace `rav`; types/functions PascalCase (`ConvertAssimpMatrix`), locals/members snake_case (`name_to_index`, `bone_count`), constants `k`-PascalCase (e.g. `kMaxBones = 128` if you introduce a cap constant — the D13 std140 ceiling is 128 mat4s; NFR-P1 targets 50). C++17, MSVC `/W3 /permissive-` warning-free, x64. MIT SPDX header already on `asset_loader.cpp` — don't duplicate; the new gate `.md` needs none.
- **Comment discipline:** WHY-only, no WHAT-narration, no story/commit IDs in code. The architecture's own example WHY-comment is literally about skinning: *"glTF inverseBindMatrix is already in mesh-relative space, so we don't premultiply by the scene-root transform here."*
- **Boundary rule (D5):** all `<assimp/...>` access stays in `asset_loader.cpp`; nothing assimp leaks into `scene.h` consumers.

### References

- [epics.md](../planning-artifacts/epics.md#L400-L411) — Story 3.1 BDD ACs; Epic 3 framing (lines 396–398); FR5/FR6/FR7/FR15 inventory.
- [architecture.md](../planning-artifacts/architecture.md) — **D1/AR7** scene data model + flat bones + skinned flag; **D3/AR9** coordinate & `convertAssimpMatrix` boundary; **D5/D6** assimp boundary, error categories, no-throw; **D13/AR12** GPU skinning forward contract (palette math, 4-weights/vertex, 128-bone std140 cap, parent-before-child single pass); **AR13** convention tolerance by camera; **AR17/AR18** failure isolation + no-throw/main-thread; Spec Change Log — FBX `PreservePivots=0`, raw-GL supersedes sokol, `rav` rename.
- [scene.h](../../src/scene.h#L27-L58) — `SceneVertex`, `SceneBone`, `SceneSkeleton`, `SceneMesh.skinned`.
- [asset_loader.cpp](../../src/asset_loader.cpp) — `ConvertAssimpMatrix` (57–63), `AppendMesh` zero-fill (232–255), `WalkBake` (278–302), `UploadMesh` skinned=false (341), `LoadAsset` import flags (389–392).
- [console_log.h](../../src/console_log.h) — `LogInfo/LogWarn` D7/AR16 funnel, `[RAV]` format.
- [docs/PHASE1_VALIDATOR_GATE.md](../../docs/PHASE1_VALIDATOR_GATE.md) — gate-doc structure to mirror for Phase 2.
- Memory: validator hooks must be **click-based** (load file → read console), never env-var/CLI.

### Review Findings

BMAD 3-layer code review 2026-06-26 (Blind Hunter + Edge Case Hunter + Acceptance Auditor). Acceptance Auditor: **all 6 ACs + scope PASS** (AC2 single matrix boundary grep-clean, AC3 no `skinned` consumer, scope = loader + gate doc only). Findings below are cold-path hardening — the validated Mixamo `Hip Hop Dancing.fbx` gate (clean rig) never exercises them, so the AR19 in-Reaper gate stands and `done` is not demoted.

- [x] [Review][Patch] Non-finite (NaN/Inf) bone weight poisons the skin scatter — the `vw.mWeight <= 0.0f` guard lets NaN through (NaN<=0 is false); the free-slot sentinel `boneWeights[s]==0.0f` then reads the NaN-occupied slot as "occupied", so a corrupt weight uploads to the GPU (NaN-deformed verts in 3.3) and the `skinned_verts` audit counts the poisoned vertex. **FIXED** — scatter guard now `!std::isfinite(vw.mWeight) || vw.mWeight <= 0.0f`, matching the existing position-finite discipline. [src/asset_loader.cpp AppendMesh scatter]
- [x] [Review][Patch] Null `aiBone*` dereferenced without guard in all three bone loops — a null `mBones[i]` slot on a malformed file is a raw deref = SEH access-violation (UB), **not** a C++ exception, so the enclosing `catch(...)` cannot convert it and Reaper crashes (violates AR18/NFR-R1). The codebase already null-guards `mesh` one line up; mirror it for bones. **FIXED** — `if (!b) continue;` added to BuildSkeleton phase 1 + phase 3 and the AppendMesh scatter loop. [src/asset_loader.cpp BuildSkeleton ×2 + AppendMesh scatter]
- [x] [Review][Defer] Same-named (or empty-named) distinct joints collapse to one global index — the D1 model keys bones by name (§B mandates dedup-by-name), so two distinct nodes sharing a name are fundamentally ambiguous; the second subtree's weights scatter onto the first bone with no warning. Resolving needs an identity-model change beyond name-matching — 3.3+ concern. [src/asset_loader.cpp DfsIndexJoints] — deferred, design-level (Mixamo fixtures have unique names)
- [x] [Review][Defer] No >128-bone cap — a rig above the D13 std140 128-mat4 palette ceiling would silently overflow in 3.3. Spec §F explicitly defers the cap to 3.3 (validated fixture = 99 bones, within ceiling). [src/asset_loader.cpp BuildSkeleton] — deferred, pre-existing (3.3 GPU-palette scope)

## Dev Agent Record

### Agent Model Used

claude-opus-4-8 (1M context) — BMAD dev-story workflow.

### Debug Log References

- AC2 single-boundary audit (`grep -nE "glm::transpose|make_mat4"` on `asset_loader.cpp`) → **empty**; `mOffsetMatrix` converts only via `ConvertAssimpMatrix`, exactly once ([asset_loader.cpp:139](../../src/asset_loader.cpp#L139)).
- Scope audit (`git diff --name-only -- src/`) → only `src/asset_loader.cpp`. No header / renderer / shader / CMake change.
- AC3 no-consumer audit (`grep -rln "\.skinned\|->skinned" src/` minus `scene.h`/`asset_loader.cpp`) → **none**; the flag drives no render path, so the render is byte-for-byte Epic 2.
- Linux CMake configure reaches the host-stubbed `non-Windows … Build target stubbed` branch as designed; `configure_file` then fails with `Operation not permitted` — a pre-existing WSL2 `/mnt`-mount quirk (hits the untouched system modules too), **not** a project error. The `/W3 /permissive-` compile genuinely pends the Windows gate (AR19), as with every prior story.

### Completion Notes List

- **Loader-only + purely additive, as scoped.** All work lives in the anonymous namespace + `LoadAsset` of [asset_loader.cpp](../../src/asset_loader.cpp). No `scene.h` type was authored or moved (the target structs were final since 2.1), no `.cpp` added → **no CMake change**.
- **Task 1 — `BuildSkeleton` + `DfsIndexJoints`.** Joints = dedup'd union of every `aiMesh->mBones[*]->mName`. Indexed by node-tree DFS pre-order → **parent-before-child** (`parentIdx < index`, the D13 forward-pass invariant; a violation is surfaced as a `[RAV] warn` in the dump). `parentIdx` = nearest *joint* ancestor (-1 = root); intermediate non-joint nodes pass their ancestor index through unchanged (their transforms are 3.2's). `inverseBindMatrix` = `ConvertAssimpMatrix(mOffsetMatrix)` from the bone's **first occurrence**; a divergent shared-bone offset across meshes keeps the first + warns once (§F). A joint in `mBones` but absent from the node tree is appended as a root + warned, so its weights still resolve.
- **Task 2 — scatter + flag.** `AppendMesh` / `WalkBake` now thread the `name→globalIdx` map; the scatter remaps assimp's per-mesh local bone index to the **global** skeleton index and writes `gb`/`weight` into the next free slot (0..3), guarding against >4 (never writes past index 3) and skipping zero-weight influences. `PendingMesh` carries `skinned = (mesh->mNumBones > 0)`, set in `WalkBake`; `UploadMesh` propagates it to `SceneMesh::skinned` (replacing the hard-coded `false`). Bone attributes ride the existing interleaved VBO — no VAO change.
- **Task 3 — `DumpSkeleton`.** On a skinned load only, emits `[RAV] info: skeleton: N bones, M skinned verts` + one legible line per bone (`[idx] name parent P (parent name)`). Static/boneless files print nothing → the Epic 2 console stays clean. Non-ASCII names print as raw UTF-8 (the FR5 check). This is the click-based validator hook (load → read console), no env-var/CLI.
- **Task 4 — `docs/PHASE2_VALIDATOR_GATE.md`** created, mirroring the Phase 1 gate: prerequisites, build/install, fixture guidance, a 9-row Story 3.1 audit table, and a scope note (data + audit only; deformed playback is 3.3, the unchanged bind-pose render is the success signal here).
- **Deferred to 3.2/3.3 (set up cleanly, §F):** vertex-space vs. node baking (baking left **unchanged** → bind-pose render identical), intermediate non-joint node transforms, multi-mesh shared-bone divergence, channel sampling (`asset.animations` left empty), and all GPU/shader/VAO/`skinned`-path-selection work.
- **Validation status:** Linux source audits pass (scope, single matrix boundary, no `skinned` consumer). The `/W3` compile, the console dump's bone-count/parent-link/non-ASCII cross-check vs Blender, and the bind-pose no-regression rows pend Antho's in-Reaper Windows gate (AR19) — recorded in the Phase 2 gate doc §3.

### File List

- `src/asset_loader.cpp` — modified (skeleton parse + per-vertex weight scatter + `skinned` flag + console dump; new includes `<string>`/`<unordered_map>`/`<unordered_set>`).
- `docs/PHASE2_VALIDATOR_GATE.md` — new (Phase 2 validator gate, Story 3.1 audit rows).

## Change Log

| Date | Version | Description |
|---|---|---|
| 2026-06-26 | 0.1 | Story 3.1 implemented — flat `SceneSkeleton` (DFS pre-order, parent-before-child) + per-vertex `boneIds`/`boneWeights` scatter (global remap, ≤4) + `skinned` flag + `[RAV]` console skeleton dump (validator hook); new `docs/PHASE2_VALIDATOR_GATE.md`. Loader-only, purely additive — no header/renderer/shader/CMake change, zero render regression. Status → review. |
| 2026-06-26 | 1.0 | **Status → done.** Antho in-Reaper Windows validation PASSED (Phase 2 gate §3/§4) — Mixamo `Hip Hop Dancing.fbx`: dump shows **99 bones, 8928 skinned verts**, root `parent -1`, every `parentIdx < index` (no ordering-broken warning), coherent Mixamo hierarchy, no bind-matrix warnings, clean load, bind-pose render unchanged. In-Reaper validation IS the gate (AR19); no code-review (per Antho, as 2-2/2-3). |
| 2026-06-26 | 1.1 | **BMAD 3-layer code-review** (Antho-requested). Acceptance Auditor PASS (6 ACs + scope). 2 patches applied (cold-path hardening, never hit by the validated clean Mixamo rig → `done` stands per AR19): (1) non-finite NaN/Inf skin-weight guard via `std::isfinite`; (2) null `aiBone*` guards in all 3 bone loops, closing an SEH/UB host-crash hole (a null deref is **not** caught by `catch(...)`, contra one reviewer's note). 2 deferred (same-name joint collapse → identity-model, 3.3; >128-bone palette cap → 3.3 §F). 7 dismissed (by-design per §C: ≤4-influence OOB-guarded + no-renorm, boneId=0 harmless-with-zero-weight, `skinned`=mNumBones>0; assimp-won't-duplicate; correct zero-weight count). |
