# Story 3.3: GPU vertex skinning renders the deformed rig

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want the animated rig to deform smoothly frame-by-frame,
so that I can watch the actual motion I need to score.

This is **Story 3 of Epic 3 (Phase 2 — the dominant technical risk)** and the **payoff** of the whole epic: it is the first story that puts a **moving, deforming rig on screen**. Stories 3.1 and 3.2 built the data and the math and changed **no rendered pixel**; 3.3 finally **consumes** them in the renderer.

- **3.1** parsed the static skin: a flat `SceneSkeleton` (`parentIdx` + `inverseBindMatrix`) and per-vertex `boneIds`/`boneWeights`, validated against Mixamo `Hip Hop Dancing.fbx` (99 bones / 8928 skinned verts). It left `skinned`/`skeleton`/`boneIds` driving **nothing**.
- **3.2** parsed the **motion** into `SceneAnimation` and added the pure-glm sampler [`src/animation.h`](../../src/animation.h) — `ComputePose(skel, anim, t, out_palette, scratch_global)` produces the **per-frame skinning palette** (`palette[i] = globalMat[i] * inverseBindMatrix[i]`) into **caller-pre-sized** buffers (D2 zero-alloc). A load-time console probe audited the math; **nothing uploaded the palette**.
- **3.3** wires that palette to the GPU: it **uploads the palette as a `mat4[128]` uniform**, **skins each vertex** in the vertex shader by its bone ids/weights (D13 LBS), drives `t` from the existing free-running frame clock so the rig **loops continuously**, and — critically — **reconciles vertex space** so the deformation is *correct* (the one decision 3.1/3.2 explicitly deferred to here).

Three facts decide the shape of the work:

- **The sampler is done and final — 3.3 only calls it.** [`ComputePose`](../../src/animation.h#L96) already does the full D13 per-frame pass (sample TRS → compose local → single linear forward pass `globalMat[i]=globalMat[parent]*local` → `palette[i]=globalMat[i]*inverseBind[i]`) into pre-sized buffers and never allocates. 3.3 **authors no sampling math**; it calls `ComputePose` once per frame and uploads `out_palette`.
- **The vertex data is already on the GPU — the layout has shipped since 2.1.** [`SceneVertex`](../../src/scene.h#L27-L33) interleaves `ivec4 boneIds` + `vec4 boneWeights`; the VBO already uploads them; 3.1's scatter already fills them (global-index remapped, ≤4 influences). 3.3 only has to **enable attribute locations 3/4 and bind the integer ids with `glVertexAttribIPointer`** (the one GL-loader addition — see §D).
- **The dominant risk is vertex space, not the shader.** `WalkBake` currently **bakes each mesh's node-world transform into its vertex positions** (`v.pos = world * p`) for *every* mesh. For a **skinned** mesh that double-applies the transform — the palette (`globalMat * inverseBind`) *already* places the vertex in world space. So 3.3 must store **skinned-mesh vertices un-baked** (mesh-local, the space `inverseBindMatrix` maps *from*) and let the palette place + deform them, **while leaving the static path byte-for-byte unchanged** (§C is the centerpiece of this story).

## Acceptance Criteria

From [epics.md#Story 3.3](../planning-artifacts/epics.md#L427-L439) (lines 433–439), verbatim BDD:

**Given** the per-frame bone matrices from Story 3.2
**When** they are uploaded as a matrix-palette uniform and the vertex shader skins each vertex by its bone ids/weights (D13)
1. **Then** the rendered mesh deforms correctly across the full clip, matching Blender/FBX Review for the validated fixture (FR15)
2. **And** rendering holds ≥60 fps on the reference workstation at ~20k tris / 4 materials / 50 bones (NFR-P1)
3. **And** a static (unskinned) mesh continues to render unchanged via the non-skinned path (no regression to Epic 2).

Implied, non-negotiable (the system must stay working end-to-end — requirements even though not in the AC text):

4. **The skinned-mesh bind/rest frame must look right too, not just "different".** Correct deformation (AC1) means correct *space*: the un-baked skinned vertices + the palette must reproduce the rig's geometry at the **scale, orientation, and placement** Blender/FBX Review shows. A rig that deforms but is mirrored, 100× too big/small, or collapsed to the origin is a FAIL on AC1. The static-bake render of Epic 2 / 3.1 is the no-regression reference **for static files only** — a skinned file is *expected* to change (it now deforms; AC3 protects only the static path).
5. **Determinism + zero per-frame allocation (D2/NFR-P1).** `ComputePose` is called **every frame** from `RenderFrame` (the D2 hot path). The palette + scratch buffers are sized **once** (at `SetAsset`, to `skeleton.bones.size()`) and reused — **no `new`/`malloc`/`push_back`/`resize` in `RenderFrame`** (the 1.2/2.4 zero-alloc-in-RenderFrame rule still holds). Palette upload is the only new per-frame GL traffic.
6. **>128 bones must not corrupt or crash.** The `mat4[128]` std140 uniform caps the skeleton (D13). A rig with >128 bones must clamp/skip gracefully with one `LogWarn` (this is the 3.1-deferred ">128-bone palette cap" — §F), never write past the uniform array or upload garbage.
7. **No-throw host boundary preserved (AR18/NFR-R1).** No new code path throws across the Reaper boundary. A skinned rig with a degenerate skeleton/clip (empty animation, size-mismatched channels) must degrade to the **static bind-pose render or a clear console line** — never a crash, never a NaN explosion on screen. `ComputePose` already no-ops on size mismatch (it returns without writing); 3.3 must handle that no-op (don't draw skinned with a stale/empty palette → §E).
8. **Single `reaper_animviewer.dll`, builds warning-free under `/W3 /permissive-`** for our own sources (NFR-R5), **no new dependency** (glm/assimp already vendored). The GL-loader gains exactly **one** function row (`glVertexAttribIPointer`, §D) and the renderer gains the skinning shader path; the CMake **source list does not change** (no new `.cpp` — the sampler is the existing header-only `animation.h`). If a new `.cpp` is genuinely needed, it is the *only* CMake change and must be called out in the File List.

## Tasks / Subtasks

- [x] **Task 1 — Add `glVertexAttribIPointer` to the GL loader (AC: 1, 8)**
  - [x] In [`src/gl_loader.h`](../../src/gl_loader.h#L52-L82), append one row to the `RAV_GL_FUNCS(X)` X-macro table: `X(void, glVertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void*))` and one matching `#define glVertexAttribIPointer rav_glVertexAttribIPointer` in the routing block below it. The X-macro auto-generates the typedef/extern (in the header), the definition (in [`gl_loader.cpp`](../../src/gl_loader.cpp)), and the `wglGetProcAddress` load — **no `.cpp` edit needed** beyond the header (the comment at [gl_loader.h:50](../../src/gl_loader.h#L50) literally reserves this row). This is required because `boneIds` is an **integer** `ivec4` attribute: `glVertexAttribPointer` (float) would convert/normalize the ids and corrupt them; integer attributes need `glVertexAttribIPointer` (§D).
  - [x] `GL_INT` (0x1404) is in the GL 1.1 `<gl/GL.h>` already — no new enum `#define` needed (unlike the texture row). Confirm; if absent, add it beside the other enums.

- [x] **Task 2 — Skinning vertex shader + palette uniform (AC: 1, 3, 5, 6)**
  - [x] Extend the renderer's vertex shader ([renderer.cpp:21-38](../../src/renderer.cpp#L21-L38)) — **prefer one program with a uniform flag**, mirroring the existing `u_hasTexture` pattern, over a second program (less churn, one VAO, one `glUseProgram`). Add: `layout(location=3) in ivec4 a_boneIds; layout(location=4) in vec4 a_boneWeights; uniform mat4 u_bones[128]; uniform int u_skinned;`.
  - [x] Skinning math (D13 LBS): when `u_skinned != 0`, build `mat4 skin = a_boneWeights.x*u_bones[a_boneIds.x] + .y*u_bones[a_boneIds.y] + .z*u_bones[a_boneIds.z] + .w*u_bones[a_boneIds.w];` then `vec4 skinnedPos = skin * vec4(a_pos,1.0);` and `vec3 skinnedNormal = mat3(skin) * a_normal;`. When `u_skinned == 0`, `skin` is identity (`skinnedPos = vec4(a_pos,1.0)`, `skinnedNormal = a_normal`) so the **static path is bit-identical to Epic 2**. Feed `skinnedPos` through `u_model`/`u_mvp` and `skinnedNormal` through `u_normal` exactly as the static path does today (the world/normal/MVP uniforms are unchanged — skinning happens *before* them, in mesh-local space).
  - [x] **Guard the weight sum.** Real exports are normalized to ~1.0, but a vertex 3.1 left with all-zero weights (a stray unrigged vertex on a skinned mesh) would collapse to the origin under `skin`. Either renormalize in-shader (`float w = dot(a_boneWeights, vec4(1)); if (w > 0.0) skin /= w; else skin = identity`) **or** rely on 3.1's scatter (which only writes positive weights, leaving genuinely-unrigged verts at weight 0 → collapse). **Renormalize-or-identity in the shader** is the safe MVP choice; document the call. (assimp's `aiProcess_LimitBoneWeights` does *not* renormalize after trimming, so a 5-influence vertex trimmed to 4 sums to <1 — visible as slight shrinkage without this guard.)
  - [x] Cache the new uniform locations in `Init` (`u_bones_` = location of `u_bones[0]` via `glGetUniformLocation(prog, "u_bones")` or `"u_bones[0]"`; `u_skinned_`). A `-1` from a driver that optimizes them out is a documented no-op (the existing `u_mvp_`-only-required policy at [renderer.cpp:125-132](../../src/renderer.cpp#L125-L132) stays — do **not** make `u_bones`/`u_skinned` fatal).

- [x] **Task 3 — Per-frame palette compute + upload in the renderer (AC: 1, 2, 5, 6, 7)**
  - [x] Add a renderer-owned pose buffer (D2 zero-alloc): two `std::vector<glm::mat4>` members (`palette_`, `pose_scratch_`) sized **once in `SetAsset`** to `asset_.skeleton.bones.size()` (clamped — see bone cap below). `SetAsset` runs while a load swaps the asset (cold path) → allocation is fine *there*; `RenderFrame` must never resize them.
  - [x] In `RenderFrame`, **after** `asset_.meshes.empty()` early-out and **before** the draw loop: if the asset is animated (`!asset_.skeleton.bones.empty() && !asset_.animations.empty()`), compute the pose. **Drive `t` from the frame clock** (`time_seconds`, which already flows in via [RenderTick → ElapsedSeconds](../../src/viewer_window.cpp#L176)): `float t = std::fmod(time_seconds, anim.duration)` (loop the clip for continuous visual validation; guard `duration <= 0` → `t = 0`). Call `ComputePose(asset_.skeleton, asset_.animations[0], t, palette_, pose_scratch_)`. **Transport-driven `t` is Epic 4** (§F) — a free-running loop is correct for 3.3's "watch it move" gate.
  - [x] Upload the palette **once per frame, before the mesh loop** (it is per-skeleton, shared by all skinned meshes): `glUniformMatrix4fv(u_bones_, boneCount, GL_FALSE, glm::value_ptr(palette_[0]))` where `boneCount = std::min<GLsizei>(palette_.size(), 128)`. `glUniformMatrix4fv` already supports `count > 1` (it's in the loader). Contiguous `std::vector<glm::mat4>` is a valid `mat4[]` source — column-major, `GL_FALSE` (no transpose), matching the static-uniform convention.
  - [x] **>128-bone cap (AC6, §F):** if `asset_.skeleton.bones.size() > 128`, `LogWarn` once (at `SetAsset`, not per frame) that bones beyond 128 are not uploaded, and cap `boneCount`/the buffer at 128. Bone ids ≥128 in the VBO would index past `u_bones[128]` (UB in GLSL) — for the MVP, the warning + the fact that clean Demute/Mixamo rigs are ≤128 (Mixamo standard = 65, the test rig = 99) is the accepted bound; a robust remap is post-MVP.
  - [x] If `ComputePose` no-ops (size mismatch → it leaves `palette_` whatever it was) **or** the asset is not animated, the skinned meshes must **not** draw with a stale/garbage palette: gate the per-mesh skinned path on `pose_valid` (true only when `ComputePose` ran on a well-formed skeleton+clip this frame). A skinned-but-clip-less rig falls back to `u_skinned=0` → it renders in **bind pose via the static path** (its un-baked verts → see §C: for a glTF-style skin, mesh-local == bind-pose world, so this still looks right; a rig that needs the palette to reach bind pose is the §C edge to verify).

- [x] **Task 4 — Vertex-space reconciliation: store skinned-mesh vertices un-baked (AC: 1, 3, 4) — THE story**
  - [x] This is the one decision 3.1/3.2 deferred here ([3-1 §F](3-1-parse-skeleton-and-skin-binding-into-the-asset-model.md), [3-2 §F](3-2-sample-animation-channels-and-compute-per-frame-bone-matrices.md)). Read §C in full before touching `asset_loader.cpp`.
  - [x] In [`AppendMesh`](../../src/asset_loader.cpp#L576) / [`WalkBake`](../../src/asset_loader.cpp#L662): for a **skinned** mesh (`mesh->mNumBones > 0`), transform vertex positions and normals by **identity instead of `world`** — store them in **mesh-local space** (the space `inverseBindMatrix` maps from). For a **static** mesh, keep `world` exactly as today (byte-for-byte → AC3). Thread the choice in cleanly (e.g. `AppendMesh` takes the matrix to apply, and `WalkBake` passes `glm::mat4(1.0f)` for a skinned mesh, `world` for a static one) — keep the diff to the bake transform, not a parallel code path.
  - [x] **Grow the AABB over the same space the rig renders in.** For a skinned mesh, the bind-pose world position equals the un-baked mesh-local position for a canonical glTF/FBX skin (§C proves `palette_bind · v_local = v_local`), so the AABB over un-baked positions frames the rig correctly and stays self-consistent with the palette space (camera auto-fit, D14, still works). Keep the existing NaN/Inf-skip AABB guard ([asset_loader.cpp:604](../../src/asset_loader.cpp#L604)).
  - [x] **Do NOT change `ComputePose` or the palette formula.** The palette from 3.2 is already the complete `globalMat · inverseBind` and already folds the scene-root→root-joint node chain into the root bone's `bind_local` ([asset_loader.cpp:146](../../src/asset_loader.cpp#L146), `DfsIndexJoints` starts `acc=identity` at the scene root) — so **no separate `globalInverse(rootNode)` is applied or needed** by default (architecture's KHR_skinning note; §C). That is the *first knob* if the validated fixture comes out displaced — see §C "If it's wrong".

- [x] **Task 5 — VAO/draw wiring for skinned vs static meshes (AC: 1, 3, 5)**
  - [x] In the `RenderFrame` mesh loop ([renderer.cpp:226-258](../../src/renderer.cpp#L226-L258)): for a **skinned** mesh, after binding its VBO, enable + specify attributes **3 and 4** — `glEnableVertexAttribArray(3); glVertexAttribIPointer(3, 4, GL_INT, stride, (void*)offsetof(SceneVertex, boneIds));` and `glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SceneVertex, boneWeights));` — and set `glUniform1i(u_skinned_, 1)`. For a **static** mesh, `glDisableVertexAttribArray(3/4)` and `glUniform1i(u_skinned_, 0)`. (Attributes are re-specified per mesh against the bound VBO, exactly as the existing 0/1/2 loop does — the shared VAO holds no per-mesh state. Disabling 3/4 for static meshes keeps them on the proven Epic 2 path.)
  - [x] Note `glVertexAttribIPointer` (not `…Pointer`) for location 3 — integer attribute, no normalization. `GL_INT` matches `glm::ivec4`'s 4×`int32`.
  - [x] Leave the existing pos/normal/uv (0/1/2) specification and the per-material uniform/texture binding untouched — skinning composes with the Blinn-Phong material path (a skinned, textured, multi-material rig must still texture and shade correctly, FR15 + Epic 2).

- [x] **Task 6 — Extend the Phase 2 validator gate with Story 3.3 rows (AC: 1, 2, 3, 4, 6)**
  - [x] Append a **"## 6. Story 3.3 — GPU vertex skinning (the moving rig)"** section to [docs/PHASE2_VALIDATOR_GATE.md](../../docs/PHASE2_VALIDATOR_GATE.md) (renumber the current "## 5. Recording the result" to "## 7"; mirror the existing row-table style). This is the **first story whose success signal is motion on screen.** Rows:
    - **Rig deforms** — load the validated animated rig (Mixamo `Hip Hop Dancing.fbx` and/or a Demute clip); the mesh **visibly animates/loops**, limbs follow the skeleton, no frozen/exploded/origin-collapsed geometry.
    - **Matches the reference** — the deformation **matches Blender / FBX Review** for the same fixture: correct **scale, orientation, placement**, and joint motion (AC1/AC4). Not mirrored, not 100× off, not at the origin.
    - **≥60 fps** — the docked viewport's `[RAV] … fps` line holds **≥60 fps** while animating at the ~20k-tri / 4-mat / ~50-bone target (NFR-P1). Record the actual fps + the fixture's tri/bone count.
    - **Static unchanged** — an Epic 2 static fixture (`Box`, `Duck`, `BoxTextured`) renders **exactly as before** (it takes `u_skinned=0`, attribs 3/4 disabled) — AC3 no-regression.
    - **Bone cap survivable** — a >128-bone rig (if available) logs the cap warning once and renders best-effort without crashing/garbage (AC6); note if no such fixture is on hand.
    - **Degenerate-clip survivable** — a skinned-but-clip-less rig renders in bind pose (no motion, no crash); a malformed rig degrades to a console line, Reaper survives (AC7).
    - **Single-DLL / warning-free** — only `reaper_animviewer.dll`; clean at `/W3 /permissive-`; no new dependency; CMake source list unchanged (AC8).
  - [x] Add a **scope note**: 3.3 closes Epic 3 — the rig now **moves**. `t` is a free-running loop here; **transport-driven `t`** (`playhead − itemStart`, clamped) is **Epic 4** (FR10). Multi-clip selection, blending, and >128-bone remap remain post-MVP (§F).

## Dev Notes

### Critical orientation — this is the renderer story; the math is done

Same standing caveat as every Epic 1/2/3 story: **trust the live tree over `architecture.md`'s stale names.** The doc still says "sokol_gfx" / "ReaImGui panel" / "FBO bridge", the old `fbxav` namespace, the `[FBXAV]` prefix, and lists a `src/animation.cpp` — **all superseded.** Live code is a **direct-render native GL child window**, namespace **`rav`**, console prefix **`[RAV]`**, flat `src/`, and the sampler is **header-only `animation.h`** (no `.cpp`). The architecture's **D13 skinning contract, D3/AR9 matrix/coord conventions, D2 hot-path rule, and NFR-P1** are current and authoritative for the *what*; the *how* (raw GL, single program + flag) follows the live `renderer.cpp`.

What 3.3 touches: **`src/renderer.cpp`** (skinning shader + palette upload + per-mesh skinned draw + pose buffers), **`src/renderer.h`** (the `palette_`/`pose_scratch_` members + new uniform-location fields), **`src/asset_loader.cpp`** (the un-bake-for-skinned reconciliation in `AppendMesh`/`WalkBake`, §C), **`src/gl_loader.h`** (one `glVertexAttribIPointer` row), and **`docs/PHASE2_VALIDATOR_GATE.md`** (append §6). It does **not** edit `scene.h` (types final), `animation.h` (sampler final — call it, don't change it), `viewer_window.cpp` (the frame clock + `RenderFrame(time, w, h)` call already exist — [viewer_window.cpp:176](../../src/viewer_window.cpp#L176)), `gpu_resources.h`, or `CMakeLists.txt`.

### §A — The exact pieces already in place (your inputs)

- **Sampler:** [`ComputePose(skel, anim, t, out_palette, scratch_global)`](../../src/animation.h#L96-L114) — pre-sized buffers, no alloc, single forward pass, `t` clamped to `[0, duration]`. `out_palette[i]` is the ready-to-upload `globalMat[i] * inverseBindMatrix[i]`. **Returns without writing** if `n==0 || channels.size()!=n || out_palette.size()!=n || scratch_global.size()!=n` — so size the buffers correctly and treat a no-op as "don't draw skinned this frame" (Task 3).
- **Frame clock + render entry:** [`RenderTick`](../../src/viewer_window.cpp#L170) already calls `g_renderer.RenderFrame(ElapsedSeconds(), g_client_w, g_client_h)` every ~15 ms. `RenderFrame(float time_seconds, …)` currently **ignores** `time_seconds` ([renderer.cpp:179](../../src/renderer.cpp#L179)) — 3.3 starts using it. No `viewer_window.cpp` change.
- **Vertex layout + scatter:** [`SceneVertex`](../../src/scene.h#L27-L33) has `ivec4 boneIds` / `vec4 boneWeights`; the VBO uploads the whole struct ([UploadMesh](../../src/asset_loader.cpp#L693)); 3.1's [scatter](../../src/asset_loader.cpp#L615-L638) fills global-indexed ids + positive weights (≤4, next-free-slot). Attribs 3/4 are **not yet enabled** in the VAO — that's Task 5.
- **Per-mesh skinned flag:** [`SceneMesh::skinned`](../../src/scene.h#L40) is set by 3.1 (`mesh->mNumBones > 0`) and **drives nothing yet** — Task 5's selector.
- **Renderer shape:** single `GpuProgram program_`, one shared `vao_`, holds `asset_`, derives view/proj from `cam_` per frame, draws meshes with per-material uniforms + texture. The zero-alloc-in-`RenderFrame` rule is load-bearing (D2) — keep it.

### §B — D13 GPU skinning contract (authoritative math)

Linear Blend Skinning, ≤4 weights/vertex (glTF canonical; `aiProcess_LimitBoneWeights` already trims). Per frame, on the CPU (all done by `ComputePose`): sample channels → local TRS → `globalMat[i] = globalMat[parent] * local[i]` (single linear pass, valid because `parentIdx < i`) → `palette[i] = globalMat[i] * inverseBindMatrix[i]`. Upload `palette` as `uniform mat4 u_bones[128]` (128×64 B = 8 KB, under the 16 KB std140 block limit; caps the skeleton at 128 bones — 2.5× the 50-bone NFR target). In the vertex shader:

```glsl
skinnedPos = ( w.x*u_bones[id.x] + w.y*u_bones[id.y]
             + w.z*u_bones[id.z] + w.w*u_bones[id.w] ) * vec4(a_pos, 1.0);
gl_Position = u_mvp * /* model already in u_mvp */ skinnedPos;   // see Task 2 for the model/normal flow
```

Static-mesh fallback (FR6): `skeleton.bones` empty → `u_skinned=0`, no palette upload, `a_pos` straight to MVP (the Epic 2 path, AC3).

### §C — Vertex-space reconciliation (the dominant risk — read fully)

**The problem.** [`WalkBake`](../../src/asset_loader.cpp#L662)/[`AppendMesh`](../../src/asset_loader.cpp#L576) bake the node-world transform into vertices for *every* mesh: `v.pos = world * p` where `world = root · … · meshNode`. That is correct for a **static** mesh (Epic 2). It is **wrong** for a **skinned** mesh, because the skinning palette `palette[i] = globalMat[i] · inverseBindMatrix[i]` **already** carries the vertex from mesh-local space to animated world space. Skinning a *baked* vertex applies the node hierarchy **twice** → the rig folds in on itself / explodes.

**The fix (default, decisive).** For a **skinned** mesh, store vertices **un-baked**: apply **identity**, not `world`, to positions and normals — i.e. keep raw `mesh->mVertices` / `mesh->mNormals` in **mesh-local space**, which is exactly the space `inverseBindMatrix` (assimp `mOffsetMatrix`) maps *from*. Then `palette[i] · v_local` places + deforms it correctly. For a **static** mesh, change nothing (`world` as today) → AC3 byte-for-byte.

**Why the bind pose still frames right (and why no `globalInverse`).** For a canonical glTF/FBX skin, `mOffsetMatrix_k = inverse(globalBindNode_k)` in the mesh's space, so at the bind pose `palette_bind[k] = globalBindNode_k · mOffsetMatrix_k = I`, hence `palette_bind · v_local = v_local`. The un-baked mesh-local vertex **is** its own bind-pose world position → the AABB over un-baked verts frames the rig correctly (Task 4), and a clip-less skinned rig drawn via the static path (Task 3) still looks right. `ComputePose`'s `globalMat` is accumulated from the **root bone**, whose `bind_local` already folds the scene-root→root-joint node chain ([DfsIndexJoints](../../src/asset_loader.cpp#L93), `acc` starts identity at `scene->mRootNode`). So the scene-root transform is **already inside the palette** — **do not** also pre-multiply by `globalInverse = inverse(rootNode->mTransformation)` (that is the classic assimp tutorial step this codebase folds in elsewhere; applying it here would double-cancel). This matches architecture.md's KHR_skinning note: *"glTF inverseBindMatrix is already in mesh-relative space, so we don't premultiply by the scene-root transform."*

**If it's wrong at the gate** (the rig deforms but is displaced / scaled / rotated / mirrored vs Blender — AC4), reconcile against the visual reference in this order, changing **one knob at a time** and re-checking:
1. **`globalInverse` knob** — if the whole rig is offset/scaled by a constant (a node *above* the root joint that `bind_local` did *not* fold, or an assimp-FBX unit/axis node), pre-multiply the uploaded palette by `glm::inverse(ConvertAssimpMatrix(scene->mRootNode->mTransformation))`. Try this first for FBX (assimp's FBX importer sometimes parks a cm-scale / axis-conversion node at the root). Default stays OFF (glTF needs it OFF).
2. **Mesh-node transform** — if a *single* skinned mesh is offset but the skeleton is right, the mesh node carried a non-identity transform that glTF says to ignore for skins but FBX may not; verify the un-bake (identity) is actually being applied to that mesh.
3. **Quaternion/handedness** — already locked by 3.2 (w-first, no `MakeLeftHanded`, render-as-authored D3) and audited clean; a *systematically mirrored* result would point back here, but 3.2's gate row 6 already passed, so treat this as last.

The **gate is the visual match to Blender/FBX Review** (AC1/AC4) — this is the AR19 in-Reaper Windows validation Antho runs. The Linux box can compile and source-audit but cannot see the deformation; expect 3.3 to need an iteration or two **at the gate**, which is exactly why Epic 3 is the dominant-risk phase. Keep a `[RAV]` debug line cheap to add (e.g. log `palette_[0]` translation + the AABB) so a wrong space is diagnosable from the console without a debugger.

### §D — `glVertexAttribIPointer` and integer attributes

`boneIds` is `glm::ivec4` (4×`int32`). It **must** be specified with `glVertexAttribIPointer(3, 4, GL_INT, stride, offset)` — the *integer* variant. `glVertexAttribPointer(…, GL_INT, GL_FALSE, …)` (the float variant the codebase uses for 0/1/2) would have the GPU read the ints and **convert them to float**, so the shader's `ivec4 a_boneIds` would receive garbage (or the ids silently truncated/normalized) → vertices skinned by the wrong bones. This is why the loader needs the new row. `boneWeights` is `vec4` (float) → the ordinary `glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, …)`. The X-macro at [gl_loader.h:52](../../src/gl_loader.h#L52) means adding one `X(...)` line + one `#define` is the whole loader change (the `.cpp` generates declaration/definition/`wglGetProcAddress` automatically — verified at [gl_loader.cpp:7,30](../../src/gl_loader.cpp)).

### §E — Per-frame control flow in `RenderFrame` (zero-alloc, D2)

```text
RenderFrame(time_seconds, w, h):
  ... viewport / clear / asset_.meshes.empty() early-out ... (unchanged)
  ... aspect-gated proj rebuild; view = cam_.ViewMatrix(); mvp etc. ... (unchanged)

  bool pose_valid = false;
  if (!asset_.skeleton.bones.empty() && !asset_.animations.empty()) {
      const SceneAnimation& clip = asset_.animations[0];
      float t = (clip.duration > 0.0f) ? std::fmod(time_seconds, clip.duration) : 0.0f;  // loop
      ComputePose(asset_.skeleton, clip, t, palette_, pose_scratch_);   // no alloc (pre-sized at SetAsset)
      pose_valid = (palette_.size() == asset_.skeleton.bones.size() && !palette_.empty());
      if (pose_valid) {
          const GLsizei nb = std::min<GLsizei>((GLsizei)palette_.size(), 128);
          glUniformMatrix4fv(u_bones_, nb, GL_FALSE, glm::value_ptr(palette_[0]));
      }
  }

  for (mesh : asset_.meshes):
      ... bind VBO/IBO, specify attribs 0/1/2, set material uniforms+texture ... (unchanged)
      bool draw_skinned = pose_valid && mesh.skinned;
      glUniform1i(u_skinned_, draw_skinned ? 1 : 0);
      if (draw_skinned) { enable+IPointer(3), enable+Pointer(4); }
      else              { disable(3); disable(4); }
      glDrawElements(...)
```

`palette_`/`pose_scratch_` are sized in `SetAsset` (`palette_.assign(min(bones, 128) , glm::mat4(1)); pose_scratch_.assign(min(bones,128), …)` — **but `ComputePose` requires `out_palette.size() == bones.size()`** to run; if you cap the *buffer* at 128, `ComputePose` no-ops on a >128-bone rig. Decision: size the buffers to `bones.size()` (so `ComputePose` runs) and only **cap the upload count** at 128 (Task 3). For ≤128-bone rigs — every real Demute/Mixamo fixture — this is moot.) `std::fmod`/`std::min` need `<cmath>`/`<algorithm>` (already or trivially included in `renderer.cpp`).

### §F — Explicitly deferred (do NOT do these here; set up cleanly)

None block 3.3's ACs (deform correctly + ≥60 fps + static unchanged).

- **Transport-driven `t`.** 3.3 loops `t` from the free-running frame clock for visual validation. The real `t = playhead − itemStart`, clamped ([prd.md](../planning-artifacts/prd.md) FR10) via a `PCM_source`, is **Epic 4**. Do not wire Reaper transport here.
- **Bone-id remap for >128 bones.** 3.3 caps + warns (AC6). A proper remap/partition (or a UBO/SSBO instead of a uniform array) is post-MVP. Clean rigs are ≤128 (Mixamo 65, test rig 99).
- **Multi-clip selection, looping modes, blending, cubic/step interpolation, root-motion application policy.** MVP = clip 0, linear T/S + slerp R, clamp-then-loop ([scene.h:81](../../src/scene.h#L70) "MVP: first one used"). Clip UI is Epic 4+.
- **Per-mesh / divergent-bind palettes.** 3.1 keeps the first bind matrix + warns on divergence; one shared palette per skeleton is the MVP (a per-mesh palette is a future refinement).
- **Normal-matrix exactness under skinning.** Using `mat3(skin)` on the normal is correct for rigid/uniform-scale bones (rigs in practice); the strictly-correct `inverse-transpose(mat3(skin))` is a polish item (visible only under heavy non-uniform bone scale — rare in character rigs). Document the choice; don't gold-plate.
- **Intermediate non-joint *animated* nodes.** 3.2 warns+skips them; folding their animation is post-MVP (char clips animate joints directly).

### Project Structure Notes

- **Files touched:** `src/renderer.cpp` (shader + palette upload + skinned draw + pose buffers), `src/renderer.h` (members + uniform fields), `src/asset_loader.cpp` (un-bake skinned verts, §C), `src/gl_loader.h` (one `glVertexAttribIPointer` row, §D), `docs/PHASE2_VALIDATOR_GATE.md` (append §6). **No `scene.h`, `animation.h`, `viewer_window.cpp`, `gpu_resources.h`, or `CMakeLists.txt`** (no new `.cpp`).
- **Conventions** ([architecture.md](../planning-artifacts/architecture.md) Naming/Standards, as practiced): namespace `rav`; types/functions PascalCase; locals/members snake_case (`palette_`, `pose_scratch_`, `u_bones_`, `u_skinned_`); constants `k`-PascalCase. C++17, MSVC `/W3 /permissive-` warning-free, x64. glm via `<glm/glm.hpp>` + `<glm/gtc/type_ptr.hpp>` (`glm::value_ptr`, already in `renderer.cpp`). GLSL `#version 330 core` (the proven spike profile — [renderer.cpp:21](../../src/renderer.cpp#L21)).
- **Comment discipline:** WHY-only, no WHAT-narration, no story/commit IDs in code. The architecture's own example WHY-comment is about skinning math — match that register (e.g. *why* un-bake skinned verts, *why* `IPointer` for ids).
- **Boundary rules:** all `<assimp/...>` access stays in `asset_loader.cpp` (D5); all modern-GL stays in `renderer.cpp` + `gl_loader.*` (the raw-GL D5 analog — [gl_loader.h:5-8](../../src/gl_loader.h#L5-L8)). `animation.h` stays assimp/GL-free (glm + `scene.h` only) — it is included by `renderer.cpp` for `ComputePose` without ODR/boundary trouble.
- **Working-tree state:** 3.1 and 3.2 are **committed** (`481431a feat(epic-3): story 3.2 …`, `8f4d165 … story 3.1 …`). Build on `main`-line history as-is. Two untracked helper scripts (`build_forcefail.bat`, `build_spike.bat`) are unrelated. The Linux box configure is host-stubbed (WSL2 `/mnt` quirk, pre-existing) — source audits + the gate compile run on Windows (AR19).
- **No-regression discipline (AC3):** the static-mesh path (attribs 0/1/2, `u_skinned=0`, baked verts) must stay bit-identical to Epic 2. Verify with the `Box`/`Duck` fixtures + a `git diff` that the static branch of the shader and draw loop is unchanged in behavior.

### References

- [epics.md](../planning-artifacts/epics.md#L427-L439) — Story 3.3 BDD ACs; Epic 3 framing ([396–398](../planning-artifacts/epics.md#L396-L398)); FR15 ([186](../planning-artifacts/epics.md#L186)).
- [architecture.md](../planning-artifacts/architecture.md) — **D13** GPU-skinning contract (LBS, 4 weights, `mat4[128]` std140 palette, per-frame CPU steps, vertex-shader `Σ w·palette[id]·pos`, static fallback); **AR12** GPU skinning = Phase 2 dominant risk; **D2** no-alloc-in-hot-paths (palette/scratch pre-sized, reused per frame); **AR8** RAII GPU ownership; **D3/AR9** column-major / Y-up / RH / single `convertAssimpMatrix` boundary / render-as-authored (no `MakeLeftHanded`), and the **KHR_skinning** note ("inverseBindMatrix already mesh-relative — no scene-root premultiply" → §C default-no-`globalInverse`); **NFR-P1** ≥60 fps @ 20k tris / 4 mats / 50 bones. *(Stale in the doc: sokol_gfx / ReaImGui / FBO / `fbxav` / `[FBXAV]` / `animation.cpp` — superseded; math/decisions authoritative.)*
- [prd.md](../planning-artifacts/prd.md) — FR15 (per-frame bone deformation), FR7 (TRS incl. root motion — done in 3.2), FR10 (`animTime = playhead − itemStart`, clamped — the Epic 4 transport `t`), FR6 (static-mesh path), NFR-P1 (≥60 fps), NFR-P5 (scrub latency ≤1 frame — Epic 4).
- [src/animation.h](../../src/animation.h#L96-L114) — `ComputePose` (call it, don't change it); the no-op-on-size-mismatch contract.
- [src/renderer.cpp](../../src/renderer.cpp) — shader sources ([21-66](../../src/renderer.cpp#L21-L66)), `Init` uniform caching + `u_mvp`-only-required policy ([116-132](../../src/renderer.cpp#L116-L132)), `SetAsset` ([154-163](../../src/renderer.cpp#L154-L163)), `RenderFrame` draw loop + zero-alloc rule ([179-263](../../src/renderer.cpp#L179-L263)).
- [src/renderer.h](../../src/renderer.h) — members + uniform-location fields to extend ([59-83](../../src/renderer.h#L59-L83)).
- [src/asset_loader.cpp](../../src/asset_loader.cpp) — `AppendMesh` bake + scatter ([576-649](../../src/asset_loader.cpp#L576-L649)), `WalkBake` ([662-689](../../src/asset_loader.cpp#L662-L689)), `DfsIndexJoints`/`BuildSkeleton` bind_local (the scene-root fold, §C) ([93-196](../../src/asset_loader.cpp#L93-L196)).
- [src/gl_loader.h](../../src/gl_loader.h#L52-L82) / [gl_loader.cpp](../../src/gl_loader.cpp) — the X-macro to extend with `glVertexAttribIPointer` (§D).
- [src/viewer_window.cpp](../../src/viewer_window.cpp#L170-L188) — the frame clock (`ElapsedSeconds`) + `RenderFrame(time, w, h)` call (already drives `t`; no change).
- [src/scene.h](../../src/scene.h#L27-L72) — `SceneVertex` (boneIds/boneWeights), `SceneMesh::skinned`, `SceneSkeleton`/`SceneAnimation` (final).
- [3-1…md](3-1-parse-skeleton-and-skin-binding-into-the-asset-model.md) / [3-2…md](3-2-sample-animation-channels-and-compute-per-frame-bone-matrices.md) — §F deferrals 3.3 inherits (vertex-space reconciliation, >128-bone cap, transport `t`); the validated 99-bone Mixamo fixture.
- [docs/PHASE2_VALIDATOR_GATE.md](../../docs/PHASE2_VALIDATOR_GATE.md) — existing gate (§3 skeleton, §4 sampling, §5 recording → renumber to §7); Story 3.3 rows insert as §6.
- Memory: validator hooks are **click-based** (load a file → watch it move / read the console), never env-var/CLI; **in-Reaper Windows validation IS the gate** (AR19) — for 3.3 that gate is the visual deformation match, which the Linux box cannot see.

## Dev Agent Record

### Agent Model Used

Claude Opus 4.8 (1M context) — `claude-opus-4-8[1m]`

### Debug Log References

- A `[RAV]` diagnostic line was **not** added by default (§C suggests one for gate
  iteration). The existing 3.2 `DumpAnimation` probe already logs `palette finite` +
  `root tx` at load, which covers the "is the space sane" question without per-frame
  console spam. If the gate shows a displaced rig, add a one-line `LogInfo` of
  `palette_[0]` translation + the AABB in `SetAsset` and walk the §C knob order.

### Completion Notes List

Implemented 2026-06-26 — Epic 3 payoff: the rig now moves. Purely a renderer-consumption
story; the sampler (`animation.h::ComputePose`) and the skin data (3.1/3.2) were used
**unchanged**.

- **Task 1 (GL loader):** Added exactly one function row — `glVertexAttribIPointer` — to
  the `RAV_GL_FUNCS` X-macro + its routing `#define` (`src/gl_loader.h`). The X-macro
  auto-generates the typedef/extern/definition/`wglGetProcAddress` load, so **no
  `gl_loader.cpp` edit** was needed. `GL_INT` (0x1404) is a core GL 1.1 enum already in
  `<gl/GL.h>` — confirmed, no new enum `#define`.
- **Task 2 (shader + palette uniform):** Extended the **single** vertex program with
  `layout(location=3) ivec4 a_boneIds`, `location=4 vec4 a_boneWeights`,
  `uniform mat4 u_bones[128]`, `uniform int u_skinned` (mirrors the `u_hasTexture` flag
  pattern — one program, one VAO, one `glUseProgram`). LBS blends ≤4 palette matrices,
  **renormalizes by the weight sum (else identity)** to absorb `aiProcess_LimitBoneWeights`
  trim-shrinkage and protect against all-zero-weight collapse; skins in **mesh-local**
  space before `u_model`/`u_mvp`. `u_skinned==0` → the skinning branch is dead → the
  static path is bit-identical to Epic 2 (AC3). New uniform locations cached in `Init`,
  **non-fatal on -1** (the `u_mvp`-only-required policy is unchanged).
- **Task 3 (per-frame compute + upload):** Renderer-owned `palette_`/`pose_scratch_`
  (`std::vector<glm::mat4>`, `renderer.h`) sized **once in `SetAsset`** to
  `skeleton.bones.size()` (so `ComputePose` runs) — `RenderFrame` never resizes (D2
  zero-alloc holds). Per frame, if animated, `t = fmod(time_seconds, duration)` (loop;
  guards `duration<=0`), `ComputePose(...)` into the pre-sized buffers, then **one**
  `glUniformMatrix4fv` of `min(bones,128)` matrices before the draw loop. `pose_valid`
  gates skinned drawing; a no-op pose (size-mismatched clip) leaves the identity palette
  → bind-pose fallback, never garbage (AC7). **>128-bone cap** warns **once at SetAsset**
  and caps the upload count at 128 (AC6/§F).
- **Task 4 (vertex-space reconciliation — THE story):** In `WalkBake`, a **skinned** mesh
  now bakes with **identity** instead of the node-world transform (`AppendMesh` already
  took the matrix to apply — minimal thread, no parallel path), storing vertices in
  **mesh-local** space (the space `inverseBindMatrix` maps from); the palette places +
  deforms them. A **static** mesh keeps `world` → byte-for-byte (AC3). The AABB grows over
  the same un-baked space, so camera auto-fit (D14) stays self-consistent. **No change to
  `ComputePose` or the palette formula** — no `globalInverse` premultiply (the scene-root
  fold is already inside the palette via 3.1's `bind_local`; §C). The `globalInverse` knob
  stays OFF by default and is the **first** thing to try if the FBX fixture comes out
  displaced at the gate.
- **Task 5 (VAO/draw wiring):** Bone attribs 3/4 are specified per-mesh against the bound
  VBO — `glVertexAttribIPointer(3, …, GL_INT, …)` for the **integer** ids (the float
  variant would normalize/corrupt them, §D) and `glVertexAttribPointer(4, …, GL_FLOAT, …)`
  for the weights — and `u_skinned` is set per mesh. **Design call (AC8):** every
  `SceneVertex` carries (zero-filled) bone data, so I specify 3/4 for *all* meshes and
  toggle only `u_skinned`, rather than adding a second loader row
  (`glDisableVertexAttribArray` is **not** in the loader). This keeps the loader to
  **exactly one** new function (AC8) while static output stays bit-identical (the shader
  never reads the bone attribs when `u_skinned==0`). Task 5's literal "disable 3/4 for
  static" was traded for "one new loader row" — same visual result, AC8 honored.
- **Task 6 (validator gate):** Appended a Story-3.3 section to
  `docs/PHASE2_VALIDATOR_GATE.md` (7-row table: deforms / matches reference / ≥60 fps /
  static unchanged / bone-cap survivable / degenerate-clip survivable / single-DLL) plus
  the free-running-`t`-vs-Epic-4-transport scope note. **Numbering:** the story asked for
  "## 6" + renumber Recording to "## 7", but Recording was actually **§5** in the file
  (not §6) — followed sequentially as **§5 Story 3.3 / §6 Recording** to avoid a §5 gap;
  heading text + row content are as specified.

**Validation status / the gate:** the source audits the Linux box *can* run are clean —
scope (only the 5 intended files; no `scene.h`/`animation.h`/`viewer_window.cpp`/CMake
change), single GL-loader row, no `ConvertAssimpMatrix` in `renderer.cpp`, CMake source
list unchanged, `ComputePose`/palette formula untouched. **The deformation itself (AC1/AC4)
and ≥60 fps (AC2) are the in-Reaper Windows visual gate (AR19) — the dominant risk; the
Linux box cannot see the rig move.** Expect 1–2 iterations at the gate per §C (the
`globalInverse` knob is the first lever for a displaced FBX). `/W3 /permissive-` compile
of the new shader/loader/draw path also rides the Windows build.

### File List

- `src/gl_loader.h` — one `glVertexAttribIPointer` X-macro row + routing `#define` (§D)
- `src/renderer.h` — `palette_`/`pose_scratch_` members; `u_bones_`/`u_skinned_` fields; `<vector>` include
- `src/renderer.cpp` — skinning vertex shader (LBS + weight-renorm); `u_bones_`/`u_skinned_` caching; `SetAsset` palette sizing + >128 warn; `RenderFrame` per-frame `ComputePose` + palette upload + per-mesh skinned wiring; `animation.h`/`console_log.h`/`<algorithm>` includes
- `src/asset_loader.cpp` — `WalkBake` un-bakes skinned-mesh vertices (identity vs world; §C)
- `docs/PHASE2_VALIDATOR_GATE.md` — §5 Story 3.3 gate rows + scope note; Recording renumbered §5→§6

### Change Log

| Date | Change |
|---|---|
| 2026-06-26 | Story 3.3 implemented — GPU vertex skinning: per-frame palette upload + LBS skinning shader (single program + `u_skinned` flag) + vertex-space reconciliation (skinned verts stored un-baked, §C). Consumes 3.2's `ComputePose` unchanged. Status → review (deformation match + ≥60 fps pend Antho's in-Reaper Windows gate, AR19). |

### Review Findings

BMAD 3-layer adversarial code review 2026-06-26 (Blind Hunter / Edge Case Hunter / Acceptance Auditor). Acceptance Auditor: **no AC violations, no scope violations** (5 allowed files only; exactly one GL-loader row; no `ConvertAssimpMatrix` in renderer; `ComputePose`/palette formula untouched; zero-alloc `RenderFrame`; CMake/scene.h/animation.h/viewer_window.cpp unchanged — all verified against the live tree). 3 patches, 2 deferred, ~8 dismissed. AC1/AC2/AC4 (visual deformation match + ≥60 fps) remain Antho's in-Reaper Windows gate (AR19) — the Linux box cannot see the rig move.

- [x] [Review][Patch] **(applied)** Shader indexes `u_bones[a_boneIds.*]` with no clamp → GLSL out-of-bounds (UB) on any >128-bone rig [src/renderer.cpp — vertex shader LBS block]. Both Blind+Edge rated HIGH: AC6 promises "never index past the uniform array… no garbage/crash", but a bone id ≥128 indexes past `mat4 u_bones[128]` (the upload is capped, the shader read is not) → driver-dependent UB, not the guaranteed best-effort AC6 requires. Clamp ids to `[0,127]` in-shader — zero-cost no-op for the clean ≤128 fixtures (Mixamo 65 / test rig 99), converts UB→defined best-effort for >128. (Optional-per-MVP: §F accepts the >128 *visual* bound; this only closes the UB/crash hole, not the remap.)
- [x] [Review][Patch] **(applied)** `>128`-bone `LogWarn` text claims "bones beyond may render in bind pose" — false; it is OOB indexing, not bind pose [src/renderer.cpp — `SetAsset` LogWarn]. Companion to the clamp patch: reworded to the accurate behavior (clamped → may render incorrectly).
- [x] [Review][Patch] **(applied)** `pose_valid` re-checks only `palette.size()==bones`, not `channels.size()==bones` — does NOT detect a `ComputePose` no-op, so a channel-size-mismatched clip skins with the identity palette instead of falling to bind pose [src/renderer.cpp — `RenderFrame`]. Edge Case Hunter MEDIUM/LATENT. This is the exact item the **3.2 review deferred to 3.3** (`deferred-work.md`: "add an assert/once-warn… so a mis-sized buffer is loud, not a frozen rig") — the trigger (a live per-frame caller) just fired. Tighten `pose_valid` to `ComputePose`'s full precondition (incl. `channels.size()==bones`) so a malformed clip degrades to the static bind-pose path, honoring the comment's own claim. Latent today (the loader always sizes `channels==bones`), but it closes the deferred item rather than re-deferring it.
- [x] [Review][Defer] Computed palette uploaded without an `isfinite` screen [src/renderer.cpp — `RenderFrame`] — deferred. Edge MEDIUM/LATENT: inputs are already guarded (3.1 weight `isfinite`, 3.2 tps-finite + quat-normalize), but a degenerate zero-scale/sheared bind bone could still compose a NaN into the palette → NaN-explosion on screen vs AC7. A per-frame finite scan over ≤128 mat4 has a hot-path cost; the validated fixtures decompose cleanly. Pick up as a §C gate diagnostic if a rig explodes at the AR19 gate.
- [x] [Review][Defer] Clip-less / non-canonical skinned rig drawn via the static fallback renders at **mesh-local**, which equals bind pose ONLY for canonical (identity-bind-palette) skins [src/renderer.cpp + src/asset_loader.cpp §C] — deferred. Blind+Edge HIGH-but-§C-documented: for a rig whose mesh→armature bind is non-identity, the static fallback misplaces/mis-scales it (no `inverseBind` applied). §C explicitly routes this to the AR19 visual gate; the `globalInverse` knob is the documented first lever. This IS the dominant-risk gate item, not a code defect to pre-fix on Linux.
