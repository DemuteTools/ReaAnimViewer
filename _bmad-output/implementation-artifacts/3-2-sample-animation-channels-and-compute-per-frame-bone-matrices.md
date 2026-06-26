# Story 3.2: Sample animation channels and compute per-frame bone matrices

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As the implementing developer,
I want TRS channels sampled at an arbitrary time `t` into global bone matrices,
so that the pose at any frame can be computed deterministically.

This is **Story 2 of Epic 3 (Phase 2 — the dominant technical risk, 7–10 days)**. Story 3.1 filled the **static** skin data: a flat `SceneSkeleton` (bones with `parentIdx` + `inverseBindMatrix`) and per-vertex `boneIds`/`boneWeights`, validated in-Reaper against Mixamo `Hip Hop Dancing.fbx` (99 bones / 8928 skinned verts). It deliberately left `asset.animations` **empty** and deferred channel sampling to this story ([3-1…md](3-1-parse-skeleton-and-skin-binding-into-the-asset-model.md) §F).

Story 3.2 fills the **motion**: it parses the assimp animation clip into the already-declared `SceneAnimation` (TRS keyframes per bone), and adds a **pure-glm sampler/composer** that, given a time `t`, produces the **per-frame global bone matrices** (and the skinning palette) that Story 3.3 will upload as a GPU matrix-palette uniform.

Like 3.1, this story is **purely additive and changes no rendered pixel**. Nothing consumes the sampled pose yet — the renderer still draws the **baked bind pose** (3.1's `WalkBake` is unchanged). The deformation itself, the GPU palette upload, the skinning shader, and driving `t` from a clock/transport are **Story 3.3 / Epic 4**. 3.2's deliverable is **parsed channels + a deterministic sampler + a console audit** proving the math is right before a renderer ever touches it.

Three facts decide the shape of the work:

- **The target types already exist and are final.** [scene.h:60-72](../../src/scene.h#L60-L72) declared `KeyframeT{float time; glm::vec3 value;}`, `KeyframeR{float time; glm::quat value;}`, `KeyframeS{...vec3...}`, `AnimChannel{translation, rotation, scale}`, and `SceneAnimation{float duration; std::vector<AnimChannel> channels;}` (**`channels` indexed by bone global index**) back in 2.1. 3.2 **populates** them; it authors **no new type** and **does not edit `scene.h`**.
- **The skeleton is already flat and parent-before-child.** 3.1's `DfsIndexJoints` guarantees `parentIdx < i` for every non-root bone ([asset_loader.cpp:84-103](../../src/asset_loader.cpp#L84-L103)), which is exactly the invariant D13's single linear forward pass relies on: `globalMat[i] = globalMat[parent[i]] * localMat[i]`. 3.2 reuses this ordering; **no graph traversal, one linear pass**.
- **The matrix boundary is already solved — and the animation path must NOT re-cross it.** TRS keyframe *values* are `vec3`/`quat`, not matrices; they are copied component-wise from assimp and the local matrix is **composed on the glm side** (already column-major). `ConvertAssimpMatrix` is for `aiMatrix4x4` only and was already applied to `inverseBindMatrix` in 3.1. **Do not route any composed TRS matrix through `ConvertAssimpMatrix`** (see Dev Notes §D). The 3.2 analog of AC2 is the **quaternion component-order rule**: `aiQuaternion(w,x,y,z) → glm::quat(w, x, y, z)`.

## Acceptance Criteria

From [epics.md#Story 3.2](../planning-artifacts/epics.md#L419-L425) (lines 419–425), verbatim BDD:

**Given** a `SceneAnimation` with translation/rotation/scale channels (and root motion when present)
**When** the animation is sampled at time `t` and the flat bone array is walked once to compose global matrices
1. **Then** all TRS channels are applied including baked root motion (FR7)
2. **And** a clip with rotation-only channels and a clip with full TRS both pose correctly
3. **And** sampling at `t=0` and `t=duration` both produce valid poses (boundary clamp).

Implied, non-negotiable (the system must stay working end-to-end — requirements even though not in the AC text):

4. **Zero render regression — static AND skinned files render exactly as in Epic 2 / Story 3.1.** No consumer reads `asset.animations` or the sampled pose yet, so the rendered image, auto-fit framing, camera, and every 2.1–3.1 gate row must be byte-for-byte unchanged. A skinned rig still renders in baked bind pose; a static (no-bone) file still takes the empty-skeleton/empty-animation path. **`WalkBake`'s vertex baking is unchanged** (vertex-space reconciliation remains 3.3's call — §F).
5. **The sampler must be auditable without a renderer.** Since the rig is not yet drawn deformed (3.3), the only way to satisfy AC1–AC3 now is a **console diagnostic probe** through the existing D7/AR16 funnel ([console_log.h](../../src/console_log.h), `[RAV]` prefix): on a successful load of an *animated* file, log the clip metadata and a **sampled-pose probe** at `t=0`, `t=duration/2`, `t=duration` that evidences (a) root motion, (b) the pose changes over time, (c) all matrices finite at both clamp boundaries. This is the click-based validator hook (load a file → read the console), no env-var/CLI. See Dev Notes §E.
6. **Determinism + zero per-frame allocation (D2).** The same `(asset, t)` produces the same pose every call (no time-of-day, no RNG, no platform-divergent math beyond IEEE glm). The sampler API must let the caller pass **pre-sized output/scratch buffers** so 3.3 can call it every frame at ≥60 fps with **no `new`/`malloc`/`push_back` in the hot path** (D2, architecture.md "no allocation in hot paths after Phase 1"). Load-time allocation (parsing keyframes, building channels) is fine.
7. **No-throw host boundary preserved (AR18).** All new assimp access (`aiAnimation`/`aiNodeAnim`) stays inside the existing single `try/catch` in `LoadAsset` and inside `asset_loader.cpp` (the only TU that includes `<assimp/...>`, D5). A clip with malformed/empty channels degrades to a `LoadResult` category or a console line — it never throws across the boundary and never crashes Reaper (NFR-R1/AR17). Null `aiNodeAnim*`/`aiAnimation*` slots are guarded (mirror 3.1's bone-null discipline — a raw deref is SEH/UB, **not** catchable by `catch(...)`).
8. **Single `reaper_animviewer.dll`**, builds **warning-free under `/W3 /permissive-`** for our own sources (NFR-R5), **no new dependency** (assimp + glm already vendored; no new importer, no stb call), and — preferred — **no CMakeLists change** (see Dev Notes §G: the sampler is a header-only module like `camera.h`).

## Tasks / Subtasks

- [x] **Task 1 — Parse the assimp animation clip into `SceneAnimation` (AC: 1, 2, 7)**
  - [x] In `asset_loader.cpp` (anonymous namespace), add a helper `ParseAnimations(scene, skeleton, name_to_index, bind_local) -> std::vector<SceneAnimation>` (see Dev Notes §B for the exact algorithm). Call it in `LoadAsset` **after** `BuildSkeleton` and **before** the dump, only when `scene->mNumAnimations > 0` *and* the skeleton is non-empty.
  - [x] **MVP = first clip only.** Parse `scene->mAnimations[0]` into one `SceneAnimation`. If `mNumAnimations > 1`, `LogInfo` "N clips, using first" (multi-clip selection is Epic 4+/post-MVP per [scene.h:81](../../src/scene.h#L81) "MVP: first one used").
  - [x] **Size `anim.channels` to `skeleton.bones.size()`** (channels indexed by bone global index — the [scene.h:71](../../src/scene.h#L71) contract). Every bone gets a fully-populated `AnimChannel` so the sampler never branches on "missing channel" (Dev Notes §B step 2 — default keys).
  - [x] **Convert time to SECONDS at the boundary.** `tps = (anim->mTicksPerSecond != 0.0) ? anim->mTicksPerSecond : 25.0;` then every `Keyframe::time = key.mTime / tps` and `SceneAnimation::duration = anim->mDuration / tps`. **Dump `duration` and `tps`** in the audit (§E) — a wrong unit assumption shows up as an obviously-wrong duration at the gate (self-checking; assimp's glTF importer and FBX importer report different `mTicksPerSecond`, so never hard-code a rate).
  - [x] **Per-`aiNodeAnim` channel:** map `channel->mNodeName` → bone global index via 3.1's `name_to_index`. If found, **replace** that bone's default keys with the sampled keyframes. If the node name is **not a joint** (helper/mesh node animation), `LogWarn` once and skip it (intermediate/non-joint node animation is deferred — §F; Mixamo/glTF character clips animate joints directly). Guard null `mChannels[i]` (AR18).
  - [x] **Value conversion (the quaternion boundary, §D):** `aiVectorKey → KeyframeT/S{ time, glm::vec3(v.x, v.y, v.z) }` (component copy — vectors are not matrices, **no `ConvertAssimpMatrix`**). `aiQuatKey → KeyframeR{ time, glm::quat(q.w, q.x, q.y, q.z) }` (**w-first glm ctor** — this is the AC2-analog discipline; no handedness flip, we render as-authored per D3). Preserve keyframe order (assimp emits them time-sorted; the sampler assumes sorted-ascending).
  - [x] Leave `asset.animations` **empty** if there are no clips (a skinned-but-static rig, or any file with `mNumAnimations == 0`) — the FR6/3.1 path, unchanged.

- [x] **Task 2 — Capture each bone's bind-local transform (the unanimated fallback) (AC: 1, 2)**
  - [x] During the skeleton DFS, capture each joint's **bind-local matrix** = the node-tree local transform (`ConvertAssimpMatrix(node->mTransformation)`) **folding any intermediate non-joint ancestor nodes** since the parent joint (Dev Notes §C). This is the rest-pose local the sampler uses for **bones the clip does not animate** and for **components a rotation-only channel omits** (the AC2 "rotation-only … poses correctly" path — T and S come from bind-local).
  - [x] Store bind-local in a **parse-local `std::vector<glm::mat4>` aligned to `skeleton.bones`** (NOT in `SceneBone` — `scene.h` stays frozen; the bind-local is consumed immediately by Task 1's default-key synthesis and never needs to live in the `Asset`). Decompose it into TRS for the default keyframes via the §B step 2 recipe (translation = column 3; rotation = `glm::quat_cast` of the normalized upper-3×3; scale = column lengths). Flag with a one-line `LogWarn` if a bind-local has non-trivial shear/negative scale (decompose is lossy there — acceptable for rigs, surfaced for honesty; a real offender is a §F judgment call, not a silent pass).
  - [x] **Why fold intermediate non-joint nodes here:** 3.1's `parentIdx` points to the *nearest joint* ancestor and skipped the non-joint nodes between joints, explicitly handing their transforms to 3.2 ([3-1…md](3-1-parse-skeleton-and-skin-binding-into-the-asset-model.md) §F). For Mixamo the chain is all-joints (no fold needed → identity accumulator), but glTF rigs can interpose control nodes; folding keeps `globalMat[i]` correct without a graph walk.

- [x] **Task 3 — Header-only sampler/composer module `src/animation.h` (AC: 1, 2, 3, 6)**
  - [x] Create **`src/animation.h`** (NEW, header-only, `inline` free functions in `namespace rav`, pure glm — **no assimp, no GL, no CMake change**; mirrors the [camera.h](../../src/camera.h) precedent). MIT SPDX header; `#ifdef _WIN32` guard to match the codebase (or none if it has no Windows-only deps — match `camera.h`). Dev Notes §F for signatures.
  - [x] `SampleVec3(const std::vector<KeyframeT/S>&, float t) -> glm::vec3`: empty → `vec3(0)` (T) / `vec3(1)` (S); one key → that value; else **binary-search** the bracketing pair, **clamp** `t` below first / above last key, **linear lerp** by the local parameter (AC3 boundary clamp).
  - [x] `SampleQuat(const std::vector<KeyframeR>&, float t) -> glm::quat`: empty → identity `quat(1,0,0,0)`; clamp at ends; **`glm::slerp`** (or normalized-lerp) between the bracketing keys; **normalize** the result. Quaternion interpolation, not component lerp (AC2 "pose correctly").
  - [x] `ComposeLocal(const AnimChannel&, float t) -> glm::mat4` = `glm::translate(T) * glm::mat4_cast(glm::normalize(SampleQuat(...))) * glm::scale(S)` — TRS order, column-major (D3).
  - [x] `ComputePose(const SceneSkeleton&, const SceneAnimation&, float t, std::vector<glm::mat4>& outPalette, std::vector<glm::mat4>& scratchGlobal)`:
    - `t = glm::clamp(t, 0.0f, anim.duration)` (AC3).
    - **Caller pre-sizes** `outPalette`/`scratchGlobal` to `bones.size()` once (D2 zero-alloc — the function must **not** resize; assert/early-return on size mismatch). See §F for an `Animator`/`PoseBuffer` struct that owns the scratch, sized at `SetAsset`, for 3.3's per-frame call.
    - Single forward pass: `for i: local = ComposeLocal(channels[i], t); scratchGlobal[i] = bone.parentIdx < 0 ? local : scratchGlobal[bone.parentIdx] * local; outPalette[i] = scratchGlobal[i] * bone.inverseBindMatrix;`. Valid because **`parentIdx < i`** (3.1 invariant) — parent global is already computed.
  - [x] Guard the degenerate sizes (`channels.size() != bones.size()`, empty skeleton) without UB — return without writing rather than indexing OOB.

- [x] **Task 4 — Console audit probe (the validator hook) (AC: 1, 3, 5)**
  - [x] In `asset_loader.cpp`, after a successful animated load (skeleton non-empty AND `!asset.animations.empty()`), add `DumpAnimation(skeleton, anim)` via [console_log.h](../../src/console_log.h). `#include "animation.h"` for the probe (header-only, pure glm — safe in any TU). Format in Dev Notes §E. Sample at `t=0`, `t=duration/2`, `t=duration` using **load-time-allocated** scratch buffers (alloc OK here — not the hot path).
  - [x] Log: clip count, clip `duration` (s) + `tps`, animated-bone count (bones whose channels came from an `aiNodeAnim`, not default keys) + total keyframes. Then per probe-time `t`: the **root bone's global-space translation** (`scratchGlobal[root][3].xyz` — root-motion evidence, FR7) and a **finite check** across the whole palette. Also log the **max bone-position delta between `t=0` and `t=duration/2`** (proves the pose actually varies with time — the sampler is live, not frozen). Keep it to the D7 one-line-per-event spirit.
  - [x] **Silent for static / boneless / clip-less files** — preserves the clean Epic 2 console and 3.1's skinned-only dump behavior.

- [x] **Task 5 — Extend the Phase 2 validator gate with Story 3.2 rows (AC: 1, 2, 3, 4)**
  - [x] Append a **"## 4. Story 3.2 — animation sampling & per-frame matrices (audit)"** section to existing **[docs/PHASE2_VALIDATOR_GATE.md](../../docs/PHASE2_VALIDATOR_GATE.md)** (created in 3.1; mirror its row style). Rows:
    - **Clip parsed** — load a Demute animated rig; the console reports ≥1 clip with a `duration` (s) that matches the clip length shown in Blender / FBX Review (catches the ticks→seconds unit).
    - **Root motion present** — the probe's root global translation **changes across `t=0 → mid → end`** for a clip with root motion (and stays ~constant for an in-place clip). FR7.
    - **Pose varies with time** — the max bone-position delta `t=0→mid` is clearly non-zero (sampling is live).
    - **Boundary poses valid** — every palette matrix is finite at `t=0` and `t=duration` (AC3 clamp); sampling slightly past `duration` clamps (no NaN/explosion).
    - **Rotation-only & full-TRS both pose** — if a rotation-only fixture is available, its probe is finite/sensible (T & S fall back to bind-local); the full-TRS Mixamo fixture likewise. AC2. (If only Mixamo is on hand, record rotation-only as best-effort and note it.)
    - **Zero render regression** — re-run the 2.1–3.1 gate rows: bind-pose render unchanged, a static file still loads silently with no animation dump.
    - **Single-DLL / warning-free** — only `reaper_animviewer.dll`; clean at `/W3 /permissive-`.
  - [x] Add a **scope note**: 3.2 delivers *sampled data + audit only*; **deformed playback is validated in 3.3**. The unchanged bind-pose render is the success signal here, not motion on screen.

## Dev Notes

### Critical orientation — this is sampling math + data-plumbing, not rendering

Same caveat as every Epic 1/2/3.1 story: **trust the live tree over `architecture.md`'s stale names.** The doc still says "ReaImGui panel" / "sokol_gfx" / "FBO bridge", the old `fbxav` namespace, and the `[FBXAV]` console prefix — **all superseded.** Live code is a **direct-render native GL child window**, namespace **`rav`**, console prefix **`[RAV]`**, flat `src/` (Spec Change Log 2026-06-23 + AR21 rename). The architecture's **D1 data model, D3/AR9 matrix boundary, and D13 skinning forward contract are current and authoritative** for the *math and types*; D13's GPU upload/shader half is **3.3**, not here.

What 3.2 touches: **`src/asset_loader.cpp`** (parse + bind-local capture + audit probe), **new `src/animation.h`** (header-only sampler), and **`docs/PHASE2_VALIDATOR_GATE.md`** (append §4). It does **not** edit `scene.h` (types final), `renderer.*`, `viewer_window.*`, any shader, `gl_loader.*`, `gpu_resources.h`, or `CMakeLists.txt`. **If you find yourself changing the renderer or the shader, you have left this story's scope — stop (that is 3.3).**

### §A — The existing load flow you are extending (3.1's end state)

`LoadAsset` ([asset_loader.cpp:538](../../src/asset_loader.cpp#L538)) already: opens the file (UTF-8/`u8path`), sets `AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS=0` (Spike Finding 3), imports with `aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights` (**no** `PreTransformVertices` — preserves the node hierarchy 3.2 needs for bind-local; **no** `MakeLeftHanded` — render as-authored), then:
1. `BuildSkeleton(scene, bone_index)` → flat `SceneSkeleton` + `name_to_index` map ([asset_loader.cpp:572](../../src/asset_loader.cpp#L572)).
2. `WalkBake(...)` bakes node world transforms into `PendingMesh` CPU arrays + scatters per-vertex weights ([:578](../../src/asset_loader.cpp#L578)).
3. materials/textures, `UploadMesh` each into a `SceneMesh`.
4. `if (!skeleton.bones.empty()) DumpSkeleton(...)` ([:628](../../src/asset_loader.cpp#L628)); `asset.skeleton = std::move(skeleton)` ([:637](../../src/asset_loader.cpp#L637)).

**Your insertions:**
- **Bind-local capture (Task 2):** fold it into `BuildSkeleton`/`DfsIndexJoints` (extend them to also fill a `std::vector<glm::mat4>& bind_local` aligned to `skel.bones`), or do a focused second DFS right after. `DfsIndexJoints` already carries the parent-joint context — the natural place to accumulate the non-joint fold (§C).
- **`ParseAnimations` (Task 1):** call right before the dump, gated on `scene->mNumAnimations > 0 && !skeleton.bones.empty()`. Returns `std::vector<SceneAnimation>` → `asset.animations = std::move(...)`.
- **`DumpAnimation` (Task 4):** after the existing `DumpSkeleton`, gated on `!asset.animations.empty()`.

`Asset` stays move-only/movable — `std::vector<SceneAnimation>` of trivially-movable members needs no special handling for the D4 reload-swap.

### §B — Animation parse algorithm (assimp → SceneAnimation)

assimp exposes clips as `scene->mAnimations[]`. Each `aiAnimation` has `mDuration` (ticks), `mTicksPerSecond`, and `mChannels[]` of `aiNodeAnim`. Each `aiNodeAnim` targets a node by `mNodeName` and carries `mPositionKeys[]` (`aiVectorKey`), `mRotationKeys[]` (`aiQuatKey`), `mScalingKeys[]` (`aiVectorKey`), each `{ mTime (ticks), mValue }`.

```text
ParseAnimations(scene, skeleton, name_to_index, bind_local) -> vector<SceneAnimation>:
  if scene->mNumAnimations == 0 OR skeleton.bones.empty(): return {}     // FR6 / 3.1 path
  if scene->mNumAnimations > 1: LogInfo("N clips, using first")          // MVP: clip 0

  aiAnimation* a = scene->mAnimations[0]; if (!a) return {}              // null guard (AR18)
  double tps = (a->mTicksPerSecond != 0.0) ? a->mTicksPerSecond : 25.0   // seconds boundary
  SceneAnimation out; out.duration = float(a->mDuration / tps)
  out.channels.resize(skeleton.bones.size())                            // indexed by bone idx

  // 1. Default every bone's channel from its bind-local (the unanimated fallback, §C).
  for i in [0 .. bones.size()):
      vec3 T, S; quat R = DecomposeTRS(bind_local[i], &T, &S)            // §C decompose
      out.channels[i].translation = { { 0.0f, T } }                     // single key at t=0
      out.channels[i].rotation    = { { 0.0f, R } }
      out.channels[i].scale       = { { 0.0f, S } }

  // 2. Overlay the clip's animated channels onto the matching joints.
  for c in [0 .. a->mNumChannels):
      aiNodeAnim* ch = a->mChannels[c]; if (!ch) continue               // null guard
      auto it = name_to_index.find(ch->mNodeName.C_Str())
      if (it == name_to_index.end()): LogWarn("anim: channel '%s' targets non-joint - skipped", ...); continue   // §F deferred
      int gi = it->second
      AnimChannel& dst = out.channels[gi]
      if ch->mNumPositionKeys > 0: dst.translation = convert(ch->mPositionKeys, tps)   // KeyframeT{ k.mTime/tps, vec3(v.x,v.y,v.z) }
      if ch->mNumRotationKeys > 0: dst.rotation    = convert(ch->mRotationKeys, tps)   // KeyframeR{ k.mTime/tps, quat(q.w,q.x,q.y,q.z) }   <- §D
      if ch->mNumScalingKeys  > 0: dst.scale       = convert(ch->mScalingKeys,  tps)   // KeyframeS{ k.mTime/tps, vec3(v.x,v.y,v.z) }
      mark gi as "animated" (for the audit count)

  return { std::move(out) }
```

A bone the clip never names keeps its single bind-local key in all three channels → it poses at rest (correct). A rotation-only `aiNodeAnim` overrides only `rotation`; `translation`/`scale` keep the bind-local default → **AC2's "rotation-only poses correctly"** falls out for free.

### §C — Bind-local capture + fold (Task 2) and TRS decompose

`aiNode::mTransformation` is the node's **local** (parent-relative) transform — the rest/bind pose local. For a joint whose parent in the *skeleton* is the nearest-joint ancestor, but which has **intermediate non-joint nodes** between them in the *node tree*, the bind-local must fold those skipped nodes:

```text
bind_local[joint] = (product of skipped non-joint nodes' mTransformation, parent-joint → this, exclusive→inclusive)
                  = accumulated_nonjoint * ConvertAssimpMatrix(jointNode->mTransformation)
```

Carry an `accumulated_nonjoint` matrix through the DFS (identity at each joint boundary):
- entering a **non-joint** node: `acc_child = acc * ConvertAssimpMatrix(node->mTransformation)`, recurse with `acc_child`.
- entering a **joint** node: `bind_local[gidx] = acc * ConvertAssimpMatrix(node->mTransformation)`; recurse children with `acc = identity` (the fold restarts below each joint).

For Mixamo (all-joint chains) `acc` is always identity → `bind_local = ConvertAssimpMatrix(node->mTransformation)`, the simple case. The fold is the documented 3.1→3.2 handoff ([3-1…md](3-1-parse-skeleton-and-skin-binding-into-the-asset-model.md) §F bullet 2).

**Decompose `mat4 → (T, R, S)`** for the default keys (rigs are translate+rotate+~uniform-scale, so this is lossless in practice):
```text
T = vec3(m[3])                                  // translation column
S = vec3(len(m[0]), len(m[1]), len(m[2]))       // column magnitudes
mat3 rot = { m[0]/S.x, m[1]/S.y, m[2]/S.z }     // de-scaled rotation basis
R = glm::quat_cast(rot)                          // (normalize the basis first)
```
(You may use `glm::decompose` from `<glm/gtx/matrix_decompose.hpp>` instead — it also returns skew/perspective you can ignore; the manual form avoids the extra GTX include and the skew/perspective out-params. Either is fine; the manual form is lighter.) `LogWarn` once if any column length is ~0 or negative-determinant (degenerate/mirrored bind — a §F judgment call, surfaced not silently dropped).

### §D — The value-conversion rules (the AC2 analog)

Two boundary disciplines, distinct from 3.1's matrix rule:

- **Vectors (`aiVector3D` → `glm::vec3`):** direct component copy `glm::vec3(v.x, v.y, v.z)`. A `vec3` is not a matrix — **do not** pass it through `ConvertAssimpMatrix`, and **do not** transpose anything.
- **Quaternions (`aiQuaternion` → `glm::quat`):** assimp's `aiQuaternion` stores `{w, x, y, z}`; glm's **`glm::quat(w, x, y, z)`** constructor is **w-first**. Use `glm::quat(q.w, q.x, q.y, q.z)`. Getting the component order wrong silently yields a plausible-but-wrong rotation that only shows as garbage in 3.3 — this is the 3.2 equivalent of "once a single mat4 transposes wrongly, debugging a skinned rig is hell." **No handedness flip** (we render as-authored, D3 — same reason 3.1 used no `MakeLeftHanded`).
- **Composed local matrix:** built entirely on the glm side (`translate * mat4_cast * scale`) → already column-major. It **never** touches `ConvertAssimpMatrix`. The only assimp matrix in the whole skinning path is `inverseBindMatrix`, already converted once in 3.1.

### §E — Console audit format (Task 4)

Use the `[RAV]` info channel ([console_log.h](../../src/console_log.h), printf-style variadic). Suggested shape:

```text
[RAV] info: animation: 1 clip(s), using [0] dur 3.467s (tps 30.0), 65/99 bones animated, 8421 keys
[RAV] info:   probe t=0.000  root tx (  0.00,  0.00,  0.00)  palette finite=yes
[RAV] info:   probe t=1.733  root tx (  0.00, 12.40,  3.10)  palette finite=yes  maxDelta(t0->mid)=41.7
[RAV] info:   probe t=3.467  root tx (  0.00,  0.05, -0.20)  palette finite=yes
```

Only emit when `!asset.animations.empty()` (static/boneless/clip-less files stay silent → clean Epic 2 console and 3.1 behavior). `root tx` is `scratchGlobal[rootIdx][3].xyz` for the first bone with `parentIdx == -1` (root-motion evidence, FR7). `finite` = all 16 floats of every palette matrix pass `std::isfinite` (AC3 boundary validity). `maxDelta` = max over bones of `length(global_t_mid[i][3].xyz − global_t0[i][3].xyz)` (proves time-varying pose). These three lines are what AC1–AC3 are eyeballed against at the gate.

### §F — Explicitly deferred to 3.3 / Epic 4 (do NOT do these here, but set them up cleanly)

Real and known; flagging now prevents a 3.3 ambush. **None block 3.2's ACs**, which validate parsed channels + deterministic sampling + bind-pose-unchanged render, not on-screen deformation.

- **GPU palette upload, std140 `mat4[128]` uniform, VAO attribute locations 3/4, the skinning vertex shader, and `skinned`-flag path selection in the renderer** — all 3.3. 3.2 produces the palette in CPU memory and audits it; nothing uploads it.
- **Driving `t`.** 3.2 samples at an *arbitrary* `t` (the audit picks fixed probe times). A free-running clock for 3.3's visual validation, and the real **Reaper-transport-driven `t = playhead − itemStart`, clamped** ([prd.md](../planning-artifacts/prd.md) FR10), are **3.3 / Epic 4**. Do not wire a per-frame `ComputePose` call into the render loop here (that would risk the AC4 zero-regression and is unobservable without 3.3's shader anyway).
- **Vertex space vs. node baking.** `WalkBake` bakes each mesh's node world transform into vertex positions; for correct LBS the palette and the (un-baked) skin-space vertices must agree. Reconciling this — likely storing skinned vertices un-baked and letting the palette place them — is **3.3's** call when the shader and the no-static-regression AC are on the table. 3.2 leaves baking **unchanged** so the bind-pose render stays identical (AC4). This means the §E probe validates *sampling correctness and motion*, not yet *visual alignment* — that is 3.3's gate.
- **Per-frame zero-alloc enforcement.** 3.2 gives `ComputePose` a pre-sized-buffer API (D2) but only calls it at load. Wiring it into 3.3's frame loop with an `Animator{ std::vector<glm::mat4> palette, scratchGlobal; }` sized at `SetAsset` is 3.3's to own — the API is built for it.
- **Multi-clip selection, looping, blending, cubic/step interpolation.** MVP = clip 0, linear T/S + slerp R, clamp (no loop). [scene.h:81](../../src/scene.h#L81) ("MVP: first one used"). Clip selection UI is Epic 4+.
- **Intermediate/non-joint *animated* nodes.** §B step 2 warns+skips channels targeting non-joints. Folding their *static* transform is done (§C); folding their *animation* into the child joint needs the node tree at sample time and is a 3.3+ refinement (Mixamo/glTF char clips animate joints directly).
- **Same-name / >128-bone edge cases** — already deferred by 3.1's review (identity-model + std140 palette cap, both 3.3).

### §G — `src/animation.h` as a header-only module (no CMake change)

The sampler is **pure glm** (no assimp, no GL). Make it **header-only with `inline` free functions**, exactly like [camera.h](../../src/camera.h) (`OrbitCamera` is header-only, which is why 2.4 added no `.cpp` and no CMakeLists change). This keeps 3.2 additive and CMake-untouched (AC8) and lets both `asset_loader.cpp` (now, for the audit) and `renderer.cpp` (3.3) include it without ODR trouble.

> **Note on architecture.md:** its Phase-2 file plan lists `src/animation.h` **and `src/animation.cpp`**. The `.cpp` is the same kind of stale literal as "sokol_gfx"/"ReaImGui" — the live project has consistently chosen header-only for small pure-math modules (`camera.h`) to avoid CMake churn. **Prefer header-only.** If the dev finds the sampler genuinely too large to inline cleanly, adding `src/animation.cpp` + one `SOURCES` line in `CMakeLists.txt` is acceptable — but it is then the *only* CMake change and must be called out in the File List. Header-only is the expected outcome.

Suggested signatures (`namespace rav`, all `inline`):
```cpp
inline glm::vec3 SampleVec3(const std::vector<KeyframeT>& keys, float t);   // + KeyframeS overload
inline glm::quat SampleQuat(const std::vector<KeyframeR>& keys, float t);
inline glm::mat4 ComposeLocal(const AnimChannel& ch, float t);
inline void ComputePose(const SceneSkeleton& skel, const SceneAnimation& anim, float t,
                        std::vector<glm::mat4>& outPalette,    // caller pre-sized to bones.size()
                        std::vector<glm::mat4>& scratchGlobal); // caller pre-sized to bones.size()
```
(KeyframeT and KeyframeS are both `{float time; glm::vec3 value;}` — one templated/overloaded `SampleVec3` covers both, or template on the keyframe type.)

### Project Structure Notes

- **Files touched:** `src/asset_loader.cpp` (parse + bind-local + audit), **new `src/animation.h`** (header-only sampler), `docs/PHASE2_VALIDATOR_GATE.md` (append §4). **No `scene.h`, no other `.cpp`, no shader, no `CMakeLists.txt`** (header-only — §G).
- **Conventions** ([architecture.md](../planning-artifacts/architecture.md) Naming/Standards, as practiced in live code): namespace `rav`; types/functions PascalCase (`ParseAnimations`, `ComputePose`, `SampleQuat`); locals/members snake_case (`bone_index`, `bind_local`, `tps`); constants `k`-PascalCase. C++17, MSVC `/W3 /permissive-` warning-free, x64. glm via `<glm/glm.hpp>` + `<glm/gtc/quaternion.hpp>` (already used by `scene.h`); `glm::slerp`/`glm::mat4_cast`/`glm::quat_cast` live in `<glm/gtc/quaternion.hpp>`, `glm::translate`/`scale` in `<glm/gtc/matrix_transform.hpp>`, `std::isfinite` in `<cmath>`. MIT SPDX header on the new `animation.h`; `asset_loader.cpp` already has one — don't duplicate.
- **Comment discipline:** WHY-only, no WHAT-narration, no story/commit IDs in code. The architecture's own example WHY-comment is literally about skinning math — match that register.
- **Boundary rule (D5):** all `<assimp/...>` access stays in `asset_loader.cpp`; `animation.h` is assimp-free (glm + `scene.h` only), so it can be included anywhere.
- **Working-tree state:** 3.1's code (skeleton parse, scatter, `DumpSkeleton`, `docs/PHASE2_VALIDATOR_GATE.md`) is **done and validated but not yet committed** (`git status` shows `asset_loader.cpp` modified + the gate doc + 3.1 story file untracked). Build on the working tree as-is; do not revert 3.1.

### References

- [epics.md](../planning-artifacts/epics.md#L413-L425) — Story 3.2 BDD ACs; Epic 3 framing ([396–398](../planning-artifacts/epics.md#L396-L398)); FR7 ([178](../planning-artifacts/epics.md#L178))/FR15 ([186](../planning-artifacts/epics.md#L186)).
- [architecture.md](../planning-artifacts/architecture.md) — **D13** GPU-skinning forward contract (per-frame CPU steps: sample channels → local TRS → compose local → `globalMat[i]=globalMat[parent[i]]*localMat[i]` single linear pass → `paletteMat[i]=globalMat[i]*inverseBindMatrix[i]`; 4 weights/vertex; `mat4[128]` std140 cap; 50-bone NFR target); **D1/AR7** scene data model (`AnimChannel`/`SceneAnimation` channels-indexed-by-bone); **D3/AR9** column-major, Y-up, RH, `convertAssimpMatrix` boundary, render-as-authored (no `MakeLeftHanded`); **D2** no-alloc-in-hot-paths; **D5/D6** assimp boundary + no-throw; **AR16/AR17/AR18** console funnel / failure isolation / main-thread; Spec Change Log — raw-GL supersedes sokol, `rav`/`[RAV]` rename.
- [prd.md](../planning-artifacts/prd.md) — FR7 (sample TRS per bone incl. root motion), FR15 (per-frame bone deformation), FR10 (`animTime = playhead − itemStart`, clamped — the eventual transport `t`, Epic 4), NFR-P1 (≥60 fps @ ~20k tris/4 mats/50 bones), NFR-P5 (scrub latency ≤1 frame).
- [scene.h](../../src/scene.h#L60-L72) — `KeyframeT/R/S`, `AnimChannel`, `SceneAnimation` (`channels` indexed by bone idx), `SceneBone.inverseBindMatrix`/`parentIdx`, `Asset.animations`.
- [asset_loader.cpp](../../src/asset_loader.cpp) — `ConvertAssimpMatrix` ([60](../../src/asset_loader.cpp#L60)), `DfsIndexJoints` ([84](../../src/asset_loader.cpp#L84)), `BuildSkeleton` ([110](../../src/asset_loader.cpp#L110)), `DumpSkeleton` ([179](../../src/asset_loader.cpp#L179)), `WalkBake` node-transform via `ConvertAssimpMatrix` ([447](../../src/asset_loader.cpp#L447)), `LoadAsset` flow + import flags ([538-559](../../src/asset_loader.cpp#L538-L559)), skeleton dump + `asset.skeleton` move ([628-637](../../src/asset_loader.cpp#L628-L637)).
- [camera.h](../../src/camera.h) — the header-only pure-math module precedent for `animation.h` (§G).
- [3-1…md](3-1-parse-skeleton-and-skin-binding-into-the-asset-model.md) — §F deferrals 3.2 inherits (channel sampling, intermediate-node fold), the validated 99-bone Mixamo fixture, the single-matrix-boundary discipline.
- [docs/PHASE2_VALIDATOR_GATE.md](../../docs/PHASE2_VALIDATOR_GATE.md) — existing gate doc (3.1 §3 dump / §4 recording) — Story 3.2 audit inserts as §4, recording renumbers to §5.
- Memory: validator hooks must be **click-based** (load file → read console), never env-var/CLI.

## Dev Agent Record

### Agent Model Used

Claude Opus 4.8 (1M context) — `claude-opus-4-8[1m]`

### Debug Log References

Host syntax/behaviour check of the real `src/animation.h` + `src/scene.h` (compiled
under a `_WIN32` define with a minimal `gl_loader.h` stub, glm from the vendored tree):
a two-bone skeleton with root-motion + rotation tracks sampled at `t = 0, 1, 2, 3`
produced monotonic root translation `(0,0,0) → (0,5,0) → (0,10,0)`, a parent-composed
child pose, and **`t=3` byte-identical to `t=2`** — confirming the `[0, duration]`
clamp (AC3). An empty `AnimChannel` composed to identity (T→0, R→identity, S→1),
confirming the unanimated/omitted-component fallback. The assimp-touching parse path
(`ParseAnimations`/`DumpAnimation`/bind-local capture) is Windows-only (assimp+GL) and
is validated by the in-Reaper gate (AR19), as in 3.1.

### Completion Notes List

- **Header-only sampler `src/animation.h` (Task 3, NEW).** Pure-glm `inline` free
  functions in `namespace rav`, `camera.h` precedent → **no CMakeLists change** (AC8;
  CMake lists only `.cpp`). `SampleVec3` (templated over `KeyframeT`/`KeyframeS`, with
  an `empty_default` so a missing translation rests at 0 and a missing scale at 1) +
  `SampleQuat` (slerp + normalize) use `std::upper_bound` for the bracketing pair and
  clamp `t` at both ends (AC3). `ComposeLocal` = `translate * mat4_cast * scale` (TRS,
  column-major, D3). `ComputePose` clamps `t`, then a **single linear forward pass**
  (`parentIdx < i`, 3.1's invariant) writes into **caller-pre-sized** palette/scratch —
  it never resizes (asserts size, early-returns on mismatch) so 3.3 can call it per
  frame with **zero allocation** (D2, AC6).
- **`ParseAnimations` (Task 1, `asset_loader.cpp`).** MVP = clip `[0]` (`>1` → `LogInfo`).
  `tps = mTicksPerSecond ?: 25`, **ticks→seconds at the boundary** for every key time +
  `duration`. `channels` sized to `bones.size()`; **every** bone first defaulted from
  its bind-local (single key per T/R/S) so the sampler never branches on a missing
  channel, then the clip's `aiNodeAnim` tracks overlay the joints they name. A
  rotation-only track replaces only `rotation` → T/S keep bind-local rest (AC2 falls
  out). Null `aiAnimation*`/`aiNodeAnim*` guarded (AR18); a channel on a non-joint node
  warns once + is skipped (§F deferral).
- **Quaternion boundary (§D, the AC2 analog).** Rotation keys convert **w-first**
  `glm::quat(q.w, q.x, q.y, q.z)`; vectors are a component copy. **No `ConvertAssimpMatrix`
  on any TRS value** — verified by grep that it touches only `mTransformation`
  (bind-local matrix, §C) and `mOffsetMatrix` (inverse-bind), never a keyframe value.
- **Bind-local capture + fold (Task 2).** `DfsIndexJoints` now carries an `acc`
  accumulator that folds skipped intermediate **non-joint** node transforms into the
  next joint's bind-local (§C) and restarts at identity below each joint; for Mixamo's
  all-joint chains `acc` is always identity. `BuildSkeleton` pads `bind_local` to the
  final bone count (identity for any node-tree-absent appended bone). `DecomposeTRS`
  splits the bind-local into the default-key TRS (translation column, column magnitudes,
  de-scaled `quat_cast`), warning once on a sheared/mirrored basis rather than silently
  passing.
- **Audit probe `DumpAnimation` (Task 4, AC5).** Emitted only on an animated load
  (right after the 3.1 skeleton dump): one metadata line (clip count, `dur`/`tps`,
  animated-bone count, total keys) + three probe lines at `t = 0, duration/2, duration`
  reporting the root bone's global translation (root-motion evidence, FR7), a whole-
  palette `std::isfinite` check (AC3), and `maxDelta(t0->mid)` (proves the pose varies
  with time). Static/boneless/clip-less files stay **silent** (clean Epic 2 console).
- **Zero render regression (AC4).** Nothing reads `asset.animations` yet — the renderer
  still draws the baked bind pose; `WalkBake` is unchanged. Scope grep: only
  `src/asset_loader.cpp` modified + `src/animation.h` added; no `scene.h`, renderer,
  shader, GL-resource, or `CMakeLists.txt` change.
- **Validation status.** Sampler math verified on the host (above). `/W3 /permissive-`
  compile of the assimp-touching parse path + the in-Reaper console audit against a
  Demute/Mixamo animated rig (gate §4) are **pending Antho's Windows gate (AR19)** — the
  Linux box can only confirm the source audits + the pure-glm sampler, as for 3.1.

### File List

- `src/animation.h` — **NEW**. Header-only pure-glm pose sampler (`SampleVec3`,
  `SampleQuat`, `ComposeLocal`, `ComputePose`).
- `src/asset_loader.cpp` — **MODIFIED**. Bind-local capture + fold in
  `DfsIndexJoints`/`BuildSkeleton`; new `DecomposeTRS`, `ParseAnimations`,
  `DumpAnimation`; `LoadAsset` parses + audits the clip; `+#include "animation.h"`,
  `+#include <glm/gtc/quaternion.hpp>`.
- `docs/PHASE2_VALIDATOR_GATE.md` — **MODIFIED**. Appended §4 (Story 3.2 audit rows);
  renumbered the recording section to §5.

### Review Findings

BMAD 3-layer code-review 2026-06-26 (Blind Hunter + Edge Case Hunter + Acceptance
Auditor). Auditor verdict: **PASS** (all 8 ACs verified on the diff). Blind Hunter: no
live CRITICAL — sampler math (quat order, slerp short-arc, multiply order, binary-search
bounds, `parentIdx < i` forward pass) confirmed correct. Edge Case Hunter: 2 silent
robustness gaps in the malformed-clip path. 3 patches, 1 deferred, 6 dismissed.

- [x] [Review][Patch] Sanitize `tps`/`duration` against negative/NaN assimp metadata [asset_loader.cpp:266,270] — `tps = (mTicksPerSecond != 0.0) ? ... : 25` and `duration = mDuration/tps` only reject *exactly* `0.0`. A negative `mTicksPerSecond` (assimp has shipped this on broken FBX) sign-flips every `mTime/tps`, storing keys in *descending* time while `SampleVec3`/`SampleQuat` assume ascending → `upper_bound` brackets wrong → wrong pose, no warning. NaN `mTicksPerSecond` passes (`NaN != 0.0`) and poisons every key. A negative `mDuration` makes `glm::clamp(t, 0, duration)` a degenerate range (always clamps to the end) so `DumpAnimation`'s `maxDelta` is silently 0 → audit falsely reports a frozen pose on a real clip. Guard `tps` to finite-and-positive, clamp `duration` to ≥0, warn on rejection. Cold-path/malformed-input hardening — clean Mixamo/Demute rigs never hit it (AR19).
- [x] [Review][Patch] Mark a bone "animated" only when a track actually carried keys [asset_loader.cpp:334] — `out_animated[it->second] = true` is set unconditionally once a channel's name matches a joint, even if all three `mNum*Keys == 0`. The §E audit "X/Y bones animated" count (an AC5 validator signal) then over-reports. Set the flag only when at least one of the three key blocks was non-empty.
- [x] [Review][Patch] Story-spec gate-section numbering self-inconsistency [story 3-2 §Task 5 / §G / Change Log] — Task 5, Dev Notes §G and the Change Log say the new gate section is "## 5", but the doc was correctly delivered as "## 4 Story 3.2" with the recording section renumbered to "## 5" (3.1 already used §3 dump / §4 recording). The File List records "§4" correctly; the other three story references are stale. Doc-only, no code impact.
- [x] [Review][Defer] `ComputePose` size-mismatch no-op leaves caller buffers stale with no diagnostic [animation.h ComputePose] — on `channels.size() != bones.size()` (etc.) the function returns without writing, so a caller's pre-zeroed buffers read as a *valid-looking* all-zero pose (0 is finite, root tx (0,0,0)) with no signal. Harmless now (the only caller, `DumpAnimation`, always pre-sizes correctly); becomes relevant when 3.3 wires a live per-frame caller. Deferred to 3.3 (when a real per-frame consumer exists). Pre-existing-by-design, not introduced harm.

Dismissed (6, no action): (1) Blind Hunter `bind_local` OOB-hazard — self-verified safe in the current ordering (`resize` precedes `ParseAnimations`), latent only; (2/3) `DecomposeTRS` lossy under shear / negative-uniform-scale — by-design per §C (rigs are translate+rotate+~uniform-scale; mirror/collapse warned, "exact in practice"); (4) node-tree-absent appended bone gets identity bind-local — the only-possible fallback (no node ⇒ no `mTransformation`), already warned at skeleton build (malformed-file path); (5) `DecomposeTRS` no NaN-input guard — a NaN bind matrix means the 3.1 skeleton/bind-pose is already broken upstream, and the audit's palette finite-check surfaces it; (6) Auditor §E format-string variation — §E is explicitly "Suggested shape," all metadata present.

### Change Log

- 2026-06-26 — Implemented Story 3.2 (animation sampling + per-frame bone matrices).
  New header-only `src/animation.h` sampler; `asset_loader.cpp` parses clip[0] into
  `SceneAnimation` (ticks→seconds, w-first quat boundary, bind-local default keys with
  non-joint fold) + load-time `[RAV]` audit probe; `docs/PHASE2_VALIDATOR_GATE.md` §4.
  Purely additive, zero render regression. Sampler math host-verified; `/W3` compile +
  in-Reaper audit pending Antho's Windows gate (AR19). Status → review.
- 2026-06-26 — BMAD 3-layer code-review (Antho-requested, pre-Windows-gate). Auditor
  PASS (8/8 ACs verified on the diff). 3 patches applied to `asset_loader.cpp`/this story:
  (1) `tps` guarded finite-and-positive + `duration` clamped ≥0 (negative/NaN assimp
  metadata no longer silently flips key order / freezes the audit), (2) "bones animated"
  count only increments on a track that actually carried keys (honest AC5 signal),
  (3) story-spec gate-section numbering corrected to §4. 1 deferred (`ComputePose`
  size-mismatch diagnostic → 3.3), 6 dismissed (by-design/latent-safe). Patches are
  cold-path/malformed-input hardening — clean Mixamo/Demute rigs never exercise them, so
  Status stays **review** pending Antho's in-Reaper Windows gate (AR19, THE gate for done).
