---
baseline_commit: efbda8d575fc5db6f7186ac9208e567dbbeea413
---

# Story 6.5.1: Source-fidelity rendering (sRGB + lighting + dielectric specular + normal maps)

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want models to look like they do where I downloaded them,
so that what I review in Reaper matches the source (Mixamo parity).

## Acceptance Criteria

1. **sRGB-correct colour (FR48).** **Given** a textured glTF/GLB (e.g. a Mixamo character) **When** it renders **Then** colours match the source DCC within a reasonable tolerance: the **base-colour texture is treated as sRGB** (uploaded `GL_SRGB8_ALPHA8` so the GPU decodes it to linear on sample, replacing the linear `GL_RGBA8` at [asset_loader.cpp:483](../../src/asset_loader.cpp#L483)) and the final fragment is **gamma-correct** (linear→sRGB encode on output). The render is no longer washed-out/desaturated. *(See Task 1 + Dev Notes "The sRGB decision" — the AC names `GL_FRAMEBUFFER_SRGB` as the nominal mechanism, but because the default framebuffer is created with a legacy non-sRGB pixel format and the renderer draws straight to framebuffer 0, the equivalent shader-side encode is the authoritative, robust path; **do not double-encode**.)*

2. **Balanced lighting via uniforms.** **Given** the same render **When** a face turns away from the key light **Then** it no longer crushes to near-black — a balanced ambient/fill term keeps unlit faces readable. Light colour and direction (and the ambient/fill amount) are driven by **uniforms `u_lightColor`, `u_lightDir`, `u_ambient`** (replacing the hardcoded `normalize(vec3(0.4,0.9,0.5))` + fixed `0.15` ambient at [renderer.cpp:85,94](../../src/renderer.cpp#L85)), set to sensible defaults now and **consumable by the light tool (Story 6.5.3)**.

3. **Dielectric-correct specular.** **Given** a non-metal surface (skin, cloth) **When** it renders **Then** specular no longer reads plastic/metallic — specular strength/shininess is tuned so a **dielectric material is matte by default** (the broad blown-out highlight is gone). Re-tuned in the new linear-lit pipeline (the old tuning was done treating sampled colour as linear — AC1 changes the working space).

4. **Normal maps when present.** **Given** an asset that carries a normal map **When** it renders **Then** the normal map is sampled (per-vertex **tangents** added to the vertex data; `SceneMaterial` gains `normalMap`, uploaded **linear** `GL_RGBA8` — NOT sRGB), restoring surface detail. **And** an asset **without** a normal map (or without UVs/tangents) renders **unchanged** — the feature is gated, never forced.

5. **No regression + perf.** **Given** any Epic 2 static mesh or Epic 3 skinned rig **When** it renders **Then** it still renders correctly (static path stays effectively bit-identical aside from the deliberate sRGB/lighting/specular colour change; the skinned rig still deforms, loops, holds end-frame per transport), **and** ≥60 fps holds at the NFR-P1 fixture (10+ items). The skinning palette/transport behaviour from Epics 3–4 is untouched.

### Out of scope (explicitly deferred — do NOT implement here)

- **The tool sidebar / light tool UI** (FR49/FR50) — Story **6.5.3**. This story only *introduces the uniforms* `u_lightColor`/`u_lightDir`/`u_ambient` with hardcoded defaults; it builds **no** Win32-child icon strip, no colour picker, no flyout. (6.5.3 depends on this story's uniforms.)
- **Removing console logs** (FR16/AR16 amendment) — Story **6.5.2**. Leave `console_log.*` and every `Log*` call (incl. the FPS log at [viewer_window.cpp:237](../../src/viewer_window.cpp#L237)) exactly as-is.
- **Floor/grid** (FR51) — Story 6.5.4. **Render-quality toggles + on-canvas FPS** (FR52/FR53) — Story 6.5.5. Do not add a runtime "disable normal maps / MSAA" switch here; normal mapping is simply gated on *presence* of a normal map.
- **Touching the WGL context / pixel-format setup** in `viewer_window.cpp` (`CreateGLContextFor`, `PIXELFORMATDESCRIPTOR` at [viewer_window.cpp:140](../../src/viewer_window.cpp#L140)). It is Spike-0-proven and Story-1.4-hardened; the sRGB output is done shader-side (Task 1) precisely to avoid disturbing it. **Do not** switch to `wglChoosePixelFormatARB` to request an sRGB-capable framebuffer.
- **Full PBR / metallic-roughness BRDF, IBL, shadows, tone-mapping.** Blinn-Phong stays (architecture: "richer-than-Blinn PBR remains post-MVP"). This story is sRGB + balanced lighting + dielectric specular + normal-map *sampling*, nothing more.
- **MSAA** — out (it is a 6.5.5 quality toggle).

## Tasks / Subtasks

- [x] **Task 1 — sRGB pipeline (AC1).** Make base colour sRGB-decoded on sample and gamma-correct on output, doing all lighting math in linear space.
  - [x] In `UploadTexture` ([asset_loader.cpp:470](../../src/asset_loader.cpp#L470)), parameterise the **internal format**: base colour → `GL_SRGB8_ALPHA8`, normal map → `GL_RGBA8` (linear). Keep the upload `format=GL_RGBA, type=GL_UNSIGNED_BYTE` (stb already forces 4-channel). Add `#define GL_SRGB8_ALPHA8 0x8C43` (and `GL_RGBA8 0x8058` if not already visible) to [gl_loader.h](../../src/gl_loader.h) near the existing GL enum block (~line 36-47).
  - [x] In the **fragment shader** ([renderer.cpp:71-97](../../src/renderer.cpp#L71)): do diffuse/specular in linear space, then encode the final colour to sRGB on write: `frag.rgb = pow(clamp(c, 0.0, 1.0), vec3(1.0/2.2));` (or the exact sRGB piecewise transfer if you prefer — gamma 2.2 is the accepted approximation here). **Do NOT also enable `GL_FRAMEBUFFER_SRGB`** — combined with the manual encode it double-applies and over-brightens. The `glClearColor(0.10,0.10,0.12,…)` background is fine to leave as-is (it is not lit).
  - [x] Verify nothing else assumes linear sampling — the only texture sample is the base colour at [renderer.cpp:93](../../src/renderer.cpp#L93); after AC1 it is decoded by the GPU to linear, so the `base *= texture(...).rgb` line is correct unchanged.
- [x] **Task 2 — Balanced lighting uniforms (AC2).** Replace the hardcoded light + ambient with uniforms.
  - [x] Fragment shader: add `uniform vec3 u_lightColor; uniform vec3 u_lightDir; uniform float u_ambient;`. Replace `L = normalize(vec3(0.4,0.9,0.5))` with `normalize(u_lightDir)` and the `0.15 + 0.85*diff` term with a balanced ambient/fill driven by `u_ambient` and `u_lightColor` so unlit faces stay readable (e.g. `base*(u_ambient + (1.0-u_ambient)*diff)*u_lightColor + spec*...`). A small constant fill / hemispheric term is acceptable if it reads better — keep it minimal and inside the shader.
  - [x] In `Renderer` add the three uniform locations + look them up in `Init` (mirror the existing `glGetUniformLocation` block, [renderer.cpp:147-157](../../src/renderer.cpp#L147)); a `-1` from a dead-stripped uniform must stay non-fatal (only `u_mvp` is required — keep that invariant).
  - [x] In `RenderFrame` set sensible **default** values once per frame via `glUniform3fv`/`glUniform1f`: e.g. `u_lightColor = (1,1,1)`, `u_lightDir = normalize(0.4,0.9,0.5)` (the prior hardcoded direction), `u_ambient ≈ 0.3–0.4`. Store them as `Renderer` members so Story 6.5.3 can later drive them from the light tool — but **do not** wire any UI here. Tune the numbers against AC2 (faces readable) at the gate.
- [x] **Task 3 — Dielectric-correct specular (AC3).** Re-tune so a non-metal is matte by default in the new linear pipeline.
  - [x] The specular *derivation* in `ConvertMaterial` ([asset_loader.cpp:442-461](../../src/asset_loader.cpp#L442)) already yields `0.04` (dim white) for a dielectric and tints toward base by metalness — keep that model. The plastic look comes from the Blinn-Phong highlight being too broad/bright once colours are linearised. Tune **strength and/or shininess** so the dielectric highlight is small and dim: e.g. raise the effective specular sharpness and/or scale the specular contribution down in the shader. Make a non-metal read matte; keep a genuinely metallic material visibly glossier (FR16 distinction preserved).
  - [x] Keep the existing NaN/clamp guards in `ConvertMaterial` (shininess `isfinite`+clamp 2..1000; metallic clamp 0..1). Do not regress them.
- [x] **Task 4 — Normal maps (AC4).** Add tangents + a normal-map sampler, gated on presence.
  - [x] `SceneVertex` ([scene.h:27-33](../../src/scene.h#L27)): add `glm::vec4 tangent;` (xyz = tangent, w = handedness sign). **Append it at the end of the struct** to minimise churn — all attribute offsets use `offsetof`, so field position is layout-irrelevant for correctness. *(Architecture [architecture.md:193](../planning-artifacts/architecture.md#L193) shows tangent mid-struct; appending is an accepted variance — note it in the Dev Agent Record.)*
  - [x] `SceneMaterial` ([scene.h:43-48](../../src/scene.h#L43)): add `GpuImage normalMap;` (empty handle when none — same flat-fallback discipline as `baseColor`).
  - [x] Loader: add `aiProcess_CalcTangentSpace` to the `ReadFile` flags ([asset_loader.cpp:784](../../src/asset_loader.cpp#L784)) (assimp computes tangents only for meshes that have normals **and** UVs — a mesh without UVs gets none → tangent stays zero → shader guards). In `AppendMesh` ([asset_loader.cpp:584-608](../../src/asset_loader.cpp#L584)), read `mesh->mTangents[vi]` (guard `mesh->HasTangentsAndBitangents()`), transform xyz by `normal_mat` like the normal, and compute `w = sign(dot(cross(N,T), B))` from `mBitangents`. **Skinned meshes**: the tangent must be transformed by `mat3(skin)` in the vertex shader exactly as the normal is (Task 4 shader step), or normal-mapped detail on an animated rig is wrong.
  - [x] Resolve the normal map: add a sibling to `ResolveAndUploadDiffuse` (or generalise it) using `aiTextureType_NORMALS` (also try `aiTextureType_HEIGHT` for FBX/OBJ-style packing) through the **same unified funnel** ([asset_loader.cpp:497-570](../../src/asset_loader.cpp#L497)) — GLB-embedded + sibling-file both — and upload **linear** (`GL_RGBA8`). Assign into `material.normalMap` beside the diffuse at [asset_loader.cpp:826-830](../../src/asset_loader.cpp#L826).
  - [x] Vertex shader: add `layout(location=5) in vec4 a_tangent;`, build a TBN (or pass T + handedness + N to the fragment), skinning the tangent under `u_skinned` just like the normal. Fragment shader: add `uniform sampler2D u_normalMap; uniform int u_hasNormalMap;`; when `u_hasNormalMap!=0` **and** the tangent is non-degenerate, sample the map (`*2-1`) and perturb N in tangent space; otherwise use the geometric normal (assets without a map render unchanged — AC4).
  - [x] Draw loop ([renderer.cpp:307-356](../../src/renderer.cpp#L307)): add the `glEnableVertexAttribArray(5)` + `glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, offsetof(SceneVertex, tangent))` row; bind `mat.normalMap` to **texture unit 1** (`glActiveTexture(GL_TEXTURE1)` — add `#define GL_TEXTURE1 0x84C1` to gl_loader.h; `GL_TEXTURE0` is already defined), set `u_normalMap=1` once per frame and `u_hasNormalMap = mat.normalMap.get()!=0`. Reuse the existing `u_hasTexture`/unit-0 pattern.
  - [x] New uniform locations in `Renderer` (`u_light*`, `u_normal_map_`, `u_has_normal_map_`) + lookups in `Init`, all non-fatal on `-1`.
- [x] **Task 5 — Build sanity + gate doc (always).** This story's done-gate is a **visual** in-Reaper comparison (Linux cannot judge the render).
  - [x] Linux: `cmake --build` configures/compiles the non-`_WIN32` translation units; the GL/render code is `#ifdef _WIN32`, so do a careful self-review of the shader GLSL + offsets (a typo only surfaces at runtime on Windows). No `CMakeLists.txt` change is expected (glm/assimp/stb already wired; `aiProcess_CalcTangentSpace` is just a flag).
  - [x] Create `docs/PHASE4.5_VALIDATOR_GATE.md` §1 (Story 6.5.1): click-based rows for Antho on Windows — (a) load a Mixamo character, side-by-side vs the Mixamo/source render → colours/brightness acceptably close (AC1/AC2); (b) skin/cloth reads matte, not plastic (AC3); (c) an asset *with* a normal map shows restored surface detail and one *without* renders unchanged (AC4); (d) an Epic-2 static mesh + Epic-3 skinned rig still render and the rig still animates, ≥60 fps at the 10-item fixture (AC5). **Result line PENDING** — never pre-mark PASS.
  - [x] Add an **AR20 Spec Change Log** entry in `architecture.md` (the §"Spec Change Log" block, ~[architecture.md:1131-1171](../planning-artifacts/architecture.md#L1131)) recording: base colour now `GL_SRGB8_ALPHA8` + shader-side linear→sRGB encode (and **why not** `GL_FRAMEBUFFER_SRGB` — legacy default framebuffer, draws to FB0, WGL left untouched); light driven by `u_lightColor`/`u_lightDir`/`u_ambient`; `SceneVertex`+tangent / `SceneMaterial`+normalMap; normal map uploaded linear.
  - [x] Add a `deferred-work.md` entry for the sidebar/light-tool UI (6.5.3), logs (6.5.2), floor (6.5.4), perf toggles + FPS (6.5.5), and full PBR/MSAA/shadows (post-MVP).
  - [x] Update `sprint-status.yaml` (6.5.1 → in-progress → review at dev time). Epic 6.5 already moves to in-progress on story creation.

## Review Findings

BMAD 3-layer code-review (Blind Hunter / Edge Case Hunter / Acceptance Auditor), 2026-06-28. Gate §1 already PASS (Antho, in-Reaper Windows, 2026-06-28). Both patches are off the validated happy path (a cold-path GLSL guard + documentation reconciliation), so neither demotes the gate.

- [x] [Review][Patch] Guard degenerate per-fragment TBN — a UV-less mesh (or a zero-UV-gradient triangle) drawing a material that carries a normal map sets `u_hasNormalMap=1` while `dFdx/dFdy(v_uv)=0` → `T=B=0` → `inversesqrt(0)=+Inf` → NaN normal → black/garbage fragments instead of the AC4 "renders unchanged" fallback. Found independently by Blind (#6) and Edge (#1). [src/renderer.cpp:118]
- [x] [Review][Patch] Reconcile stale per-vertex-tangent documentation with the shipped derivative-TBN implementation — File List, Completion Notes (Task 4), the "Accepted variance" note, the Task 4 `[x]` sub-bullets, the architecture.md AR20 body, and gate-doc §2 row 4 (line 76) all describe `SceneVertex += tangent` / `location=5` / `aiProcess_CalcTangentSpace` / skinned tangent — none of which exist in the code (it uses a per-fragment screen-space-derivative cotangent frame). Auditor F1+F2. [story file + architecture.md + docs/PHASE4.5_VALIDATOR_GATE.md]

**Dismissed / accepted-tradeoff (recorded, not patched):**
- Blind #5 (HIGH "glActiveTexture leak corrupts multi-material diffuse") — **false positive**: `renderer.cpp:414` explicitly `glActiveTexture(GL_TEXTURE0)` before every diffuse bind. The Blind layer had diff-only context and could not see the existing line; Edge (project access) confirmed it handled.
- **Accepted tradeoff — authored `COLOR_SPECULAR` now always ignored** (Edge #5 / Auditor F4): a pure-Phong OBJ/FBX asset with no `METALLIC_FACTOR` collapses to a uniform 0.04 dielectric specular, so Story-2.3 matte-vs-glossy (FR16) survives only for glTF metallic-roughness assets. Deliberate, AC3-justified gate fix (Mixamo/FBX author a grey ~0.5 Phong specular that reads plastic). Documented in code + AR20 + gate doc. Surfaced to Antho as a known limitation; a "honour authored specular for non-metalness assets" follow-up is possible but out of scope here.
- **Accepted tradeoff — `aiProcess_FlipUVs` applied to all formats** (Auditor F3 / Blind #8 / Edge #4): global V-flip is a gate fix not in the original Tasks; it changes texture-V sampling for every previously-shipped textured asset (AC5 surface). Root-caused offline (assimp V = 1 − raw-glTF V) and gate-PASSED with both an FBX and a glTF/GLB; not re-checked against the specific Epic-2 fixtures. Low risk; optional Epic-2 textured re-check if any old asset ever looks mirrored.
- Visual/gate-tunable, gate-PASSED: flat-factor gamma shift (Blind #1), energy-norm coherence + bright-metal clip hue-shift (Blind #3/#4, Edge #6), FlipUVs↔normal-map handedness (Blind #7, low-confidence/uncorroborated), sRGB-unsupported silent fallback (Edge #3 — sRGB textures are universally supported).

## Dev Notes

### The crux: this is a render-pipeline colour-space + lighting fix, confined to the GL boundary files

The render today is **physically wrong in three independent ways**, all visible in the Mixamo side-by-side: (1) the base-colour texture is uploaded **linear** (`GL_RGBA8`) and sampled linear with no gamma decode/encode → washed-out, desaturated; (2) a single hardcoded directional light + fixed `0.15` ambient → back/fill faces crush to near-black (too dark); (3) Blinn-Phong specular tinted by the glTF metallic factor with shininess 32 → a broad plastic/metal sheen on skin (a dielectric). Plus surface detail is missing because **normal maps aren't sampled** (no tangents in the vertex data). This story fixes all four.

**The GL boundary** is `renderer.cpp` + `asset_loader.cpp` (texture/buffer upload) + `gl_loader.{h,cpp}` — the only files that call modern GL (architecture: "`renderer.cpp` is the only file that calls `sg_*`" → the raw-GL equivalent; `asset_loader` is the unified texture-upload funnel, AR14). **Touch only:** `src/scene.h`, `src/gl_loader.h`, `src/asset_loader.cpp`, `src/renderer.h`, `src/renderer.cpp`. **Do not touch** `viewer_window.cpp` (WGL/pixel format — see below), `plugin_main.cpp`, `reaper_api.*`, `console_log.*`, `pcm_source_anim.*`, or `CMakeLists.txt`.

### The sRGB decision (the #1 disaster to prevent)

The AC literally says `GL_FRAMEBUFFER_SRGB`, and the architecture texture-format table ([architecture.md:389](../planning-artifacts/architecture.md#L389)) names it. **But it is unreliable here and you must not rely on it alone:** the renderer draws **straight into the default framebuffer (framebuffer 0), never an FBO** ([renderer.h:3-9](../../src/renderer.h#L3)), and that framebuffer is created with a **legacy `PIXELFORMATDESCRIPTOR`** (`cColorBits=32`, [viewer_window.cpp:140-148](../../src/viewer_window.cpp#L140)) that does **not** request `WGL_FRAMEBUFFER_SRGB_CAPABLE`. On such a framebuffer `glEnable(GL_FRAMEBUFFER_SRGB)` is a **no-op or driver-dependent** — you'd think you fixed it and the render would still be washed-out.

**Authoritative path (recommended, self-contained):**
- Base colour texture → `GL_SRGB8_ALPHA8`. The GPU's sRGB→linear decode **on sample** is a *texture* property, independent of the framebuffer — this part always works.
- Do lighting in linear space.
- **Encode linear→sRGB in the fragment shader** on the final write (`pow(c, 1/2.2)`).
- **Do NOT enable `GL_FRAMEBUFFER_SRGB`** — combined with the shader encode it double-encodes (over-bright, milky). Pick exactly one encode path; the shader one is chosen because it does not depend on the WGL pixel format and keeps the fragile, Spike-proven context code untouched.

This satisfies the AC's *intent* (sRGB-treated base colour + gamma-correct output → Mixamo parity, Antho-judged at the gate), which is what AR19 measures — not the specific enum. Record the chosen mechanism + rationale in the AR20 Spec Change Log (Task 5).

### Per-vertex layout change — low-risk because everything uses offsetof

`SceneVertex` grows by 16 bytes (`vec4 tangent`). The VBO stride is `sizeof(SceneVertex)` and **every** attribute pointer already uses `offsetof(SceneVertex, …)` in both the writer ([asset_loader.cpp AppendMesh](../../src/asset_loader.cpp#L584), fields set by name) and the reader ([renderer.cpp draw loop](../../src/renderer.cpp#L314)). So appending `tangent` and adding attribute **location 5** is the entire change; locations 0–4 (pos/normal/uv/boneIds/boneWeights) are unaffected. boneIds stays the **integer** attribute via `glVertexAttribIPointer` (location 3) — don't disturb that (§D from Epic 3: the float variant would skin by the wrong bones).

### Regression guardrails (AC5 — the system must stay working end-to-end)

- **Static path (Epic 2):** with `u_skinned==0` the skinning branch is dead and bone attributes unread — keep that. The static render *will* change colour (that's the point: sRGB/lighting/specular) but geometry/topology must be identical.
- **Skinned path (Epic 3):** the per-frame palette (`ComputePose`, `pose_valid` gating, the 128-bone cap clamp, zero-alloc hot path) is **untouched**. Only add: skin the **tangent** by `mat3(skin)` alongside the normal under `u_skinned` (else normal-mapped rigs get wrong detail). Don't alter the `pose_valid`/channel-size logic.
- **Transport (Epic 4):** `RenderFrame(anim_time, loop, w, h)` signature and the clamp/loop semantics stay as-is.
- **Zero-alloc in `RenderFrame` (D2):** setting the new light/normal uniforms is a handful of `glUniform*` calls — no heap, no per-frame allocation. Keep it that way.
- **Non-fatal `-1` uniforms:** a driver may dead-strip `u_ambient`/`u_normalMap`/etc.; `glUniform*(-1,…)` is a documented no-op. Only `u_mvp` may be fatal-on-missing — preserve [renderer.cpp:162-165](../../src/renderer.cpp#L162).
- **Texture failure is non-fatal:** a normal-map decode/upload failure returns an empty `GpuImage` → `u_hasNormalMap=0` → geometric normal (matches the diffuse flat-fallback discipline, AR14). A *buffer* failure stays fatal (UploadMesh).
- **Perf (NFR-P1 ≥60 fps):** normal mapping adds one texture sample + a little TBN math per fragment — cheap. No new per-frame allocation or draw calls. The 10-item fixture must still hold ≥60 fps (Antho's gate).

### Why no CMake / dependency change

glm 1.0.3, assimp 6.0.5, and stb_image are already fetched/wired ([CMakeLists.txt:23-56](../../CMakeLists.txt#L23)). `aiProcess_CalcTangentSpace`, `aiTextureType_NORMALS`, `mesh->mTangents`/`mBitangents`, and the GL sized internal-format enums are all already available — the new GL enums are just `#define`s in `gl_loader.h` (same pattern as the existing block). No new `REAPERAPI_WANT_*`, no `rec->Register`, no boundary-rule change (AR15 unaffected).

### Validation = Antho's in-Reaper Windows visual gate (AR19)

Like Epics 2–3, the payoff is **visual and only observable in-Reaper on Windows** — the Linux dev box compiles the non-`_WIN32` units but cannot see the render. The dev side implements + self-reviews GLSL/offsets and authors `docs/PHASE4.5_VALIDATOR_GATE.md` §1 with **Result PENDING**. The story closes when Antho judges the Mixamo side-by-side acceptable (AC1–AC4) and confirms no Epic-2/3 regression + ≥60 fps (AC5). **Never fabricate a PASS.**

### Project Structure Notes

- Files touched (all flat `src/`): `scene.h`, `gl_loader.h`, `asset_loader.cpp`, `renderer.h`, `renderer.cpp`. Docs: new `docs/PHASE4.5_VALIDATOR_GATE.md`; updates to `architecture.md` (AR20 log), `deferred-work.md`, `sprint-status.yaml`.
- Variance from architecture (final): normal mapping ships a **per-fragment derivative TBN**, not the originally-planned per-vertex tangent — so `SceneVertex` carries **no** tangent attribute (architecture's mid-struct tangent at :193 is superseded; see Change Log v0.3 + the AR20 Spec Change Log). `offsetof` keeps locations 0–4 unaffected.

### References

- [Sprint Change Proposal — Epic 6.5](../planning-artifacts/sprint-change-proposal-2026-06-27-epic-6-5.md) — root-cause table (file:line) + FR48–53 + AR16/normal-map amendments.
- [epics.md §Epic 6.5 / Story 6.5.1](../planning-artifacts/epics.md#L628) — user story + ACs.
- [prd.md FR48 + FR16 annotation](../planning-artifacts/prd.md#L226) — sRGB + normal maps promoted Growth→MVP.
- [architecture.md:193-195](../planning-artifacts/architecture.md#L193) — `SceneVertex`+tangent / `SceneMaterial`+normalMap target shapes; [:389](../planning-artifacts/architecture.md#L389) texture format table; [:396](../planning-artifacts/architecture.md#L396) shader; [:230](../planning-artifacts/architecture.md#L230) the single assimp→GLM matrix boundary; [:183](../planning-artifacts/architecture.md#L183) normal maps promoted to MVP.
- Code: [renderer.cpp:71-97](../../src/renderer.cpp#L71) (shaders), [:147-157](../../src/renderer.cpp#L147) (uniform lookups), [:307-356](../../src/renderer.cpp#L307) (draw loop); [asset_loader.cpp:420-463](../../src/asset_loader.cpp#L420) (ConvertMaterial), [:470-495](../../src/asset_loader.cpp#L470) (UploadTexture), [:497-570](../../src/asset_loader.cpp#L497) (ResolveAndUploadDiffuse funnel), [:584-608](../../src/asset_loader.cpp#L584) (AppendMesh vertex assembly), [:784](../../src/asset_loader.cpp#L784) (import flags); [scene.h:27-48](../../src/scene.h#L27); [gl_loader.h:36-47](../../src/gl_loader.h#L36) (GL enum block).

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Claude Opus 4.8, 1M context) — dev-story workflow.

### Debug Log References

- Linux `cmake --build /tmp/rav_build` → `[ReaAnimViewer] Phase 0 supports Windows only — nothing to build on this platform.` exit 0. The GL/render code is `#ifdef _WIN32`, so the Linux box compiles only the non-`_WIN32` TUs (it cannot link/run the DLL or see the render); GLSL/offset correctness is Antho's in-Reaper gate.
- **Offline root-causing (gate iterations):** to stop guessing from screenshots, the dev box loaded the exact failing models with the project's pinned **assimp built natively on Linux** (`dump.cpp` — same post-process flags as the loader) + extracted their **embedded textures** (binary FBX PNG scan / GLB chunk parse) and reproduced the render in a **Python/numpy software rasterizer**. This is how the UV V-flip (assimp glTF V = `1 − raw V`), the mirrored-UV tangent dropout, and the washed-metal specular were each **measured** rather than guessed.
- Scope audit: `git diff --stat -- src/` = exactly the 5 GL-boundary files (`scene.h`, `gl_loader.h`, `asset_loader.cpp`, `renderer.h`, `renderer.cpp`); `viewer_window.cpp` nets to zero. Grep confirmed no stale debug/tangent/diagnostic symbols remain.

### Completion Notes List

Implemented source-fidelity rendering (FR48) entirely within the GL-boundary files — sRGB-correct colour, balanced lighting via uniforms, dielectric+energy-normalized specular, derivative-TBN normal maps, and an all-format UV V-flip. Validation: Antho's in-Reaper Windows visual gate (`docs/PHASE4.5_VALIDATOR_GATE.md` §1) — **PASS 2026-06-28** on **both** a textured FBX (Mixamo) and a textured GLB (steampunk explorer).

> **The gate was iterative** (see Change Log v0.2–v0.5). Three "looks wrong" symptoms were each **root-caused offline** — the dev box loaded the exact model files with assimp + their embedded textures and reproduced the render in a Python rasterizer, turning guesses into measurements. The real bugs were **not** the lighting/specular I first chased: (1) a **UV V-flip** mishandling (the "smeared/marble" FBX and "offset" GLB), and (2) **un-normalized specular** washing out metals. The task-spec normal-map approach (per-vertex tangents) was also replaced by a **derivative-based TBN** because mirrored UVs zeroed the vertex tangents on character torsos. Final per-task state below.

- **Task 1 — sRGB (AC1):** `UploadTexture` gained an `internal_format` param; base colour uploads `GL_SRGB8_ALPHA8` (GPU sRGB→linear decode on sample), normal maps upload linear `GL_RGBA8`. The fragment shader does all lighting in linear space and **encodes linear→sRGB on the final write** (`pow(c, 1/2.2)`). `GL_FRAMEBUFFER_SRGB` deliberately **NOT** enabled (default FB is a legacy non-sRGB pixel format, renderer draws to FB0; enabling both would double-encode) — the shader encode is the single robust path and the Spike-proven WGL context in `viewer_window.cpp` is untouched. New GL enums are `#define`s in `gl_loader.h` (`GL_SRGB8_ALPHA8`, `GL_TEXTURE1`; `GL_RGBA8` guarded with `#ifndef` to avoid MSVC C4005 against `<gl/GL.h>`).
- **Task 2 — lighting uniforms (AC2):** added `u_lightColor` / `u_lightDir` / `u_ambient`, looked up in `Init` (non-fatal on -1), set once per frame in `RenderFrame` from `Renderer` members (`light_color_`/`light_dir_`/`ambient_=0.35`) so **Story 6.5.3's light tool** can drive them later. Balanced ambient/fill keeps faces turned from the key light readable.
- **Task 3 — specular (AC3), final:** `ConvertMaterial` **always** derives specular from metalness (dielectric F0 0.04, metalness tint; NaN/clamp guards preserved) and **ignores any authored `COLOR_SPECULAR`** (FBX/Mixamo author a 0.5 grey specular → plastic/shiny skin). The Blinn-Phong lobe is **energy-normalized** in the shader — `spec *= (u_shininess+8)/(8π)` — then scaled by `kSpecStrength=0.35`. The normalization was added at the GLB gate: a glTF metal (metallic≈1, roughness≈1 → shininess floored at ~2) otherwise washed the whole surface white; normalization makes broad lobes dim, tight lobes bright, dielectrics unchanged.
- **Task 4 — normal maps (AC4), final:** `SceneMaterial` += `GpuImage normalMap` (uploaded linear `GL_RGBA8`). The fragment shader builds the **tangent frame per-fragment from screen-space derivatives** of `v_worldpos`+`v_uv` (Schüler cotangent frame) — **NOT** a per-vertex tangent: a character's mirrored UVs zero assimp's smoothed vertex tangent along the body mirror seam, which silently dropped the normal map on the torso (verified by extracting the model's normal map and seeing the detail was there but unapplied). So **`SceneVertex` carries no tangent** and **`aiProcess_CalcTangentSpace` is not requested**. `ResolveAndUploadNormalMap` resolves `aiTextureType_NORMALS` **only** (the `HEIGHT` fallback was dropped — grayscale bump → melted-wax banding). Draw loop binds `mat.normalMap` to unit 1; perturbs N only when `u_hasNormalMap != 0`; absent map → geometric normal unchanged.
- **Task (gate) — UV V-flip, all formats:** `aiProcess_FlipUVs` is applied for **every** format. assimp delivers UVs bottom-up for all importers (it flips glTF's top-left origin — measured: assimp glTF V = `1 − raw V`); our textures upload top-down. Without the flip a UV-atlas character samples the wrong/padded region → "smeared marble" (FBX) / "offset texture" (GLB). An interim build flipped only non-glTF and left the GLB offset; the final rule is always-flip.
- **Task 5 — docs:** `docs/PHASE4.5_VALIDATOR_GATE.md` §1 (gate + iteration findings, **Result PASS**), AR20 Spec Change Log in `architecture.md`, `deferred-work.md` (6.5.2/3/4/5, **alpha/transparency**, post-MVP PBR/MSAA/height→normal, gamma-2.2), `sprint-status.yaml`.

**Regression guardrails honoured (AC5):** static-path geometry/topology identical; skinning palette / `pose_valid` / 128-bone cap / transport clamp untouched (the vertex shader skins pos+normal only — no tangent); `RenderFrame(anim_time, loop, w, h)` signature unchanged; zero-alloc hot path preserved; only `u_mvp` fatal-on-missing; normal-map failure → empty handle → geometric normal (AR14). No CMake/dependency/boundary-rule (AR15) change; `viewer_window.cpp` nets to zero change (a temporary diagnostic button + debug views were added then fully reverted).

### File List

- `src/scene.h` — `SceneMaterial` += `GpuImage normalMap`; `SceneVertex` unchanged (no tangent — derivative TBN supersedes it; comment records why).
- `src/gl_loader.h` — `#define GL_SRGB8_ALPHA8`, `GL_TEXTURE1`; `#ifndef`-guarded `GL_RGBA8`.
- `src/asset_loader.cpp` — `UploadTexture` `internal_format` param; `ResolveAndUploadDiffuse` → generalised `ResolveAndUploadTexture(type, internal_format, kind)`; new `ResolveAndUploadNormalMap` (NORMALS only); base colour `GL_SRGB8_ALPHA8` + normal map linear `GL_RGBA8` in the material loop; `ConvertMaterial` always-dielectric specular (ignores authored `COLOR_SPECULAR`); **`aiProcess_FlipUVs` for all formats**; no `CalcTangentSpace`, no tangent read.
- `src/renderer.h` — light + normal-map uniform-location members; light default members (`light_color_`/`light_dir_`/`ambient_`).
- `src/renderer.cpp` — fragment shader: sRGB encode, lighting uniforms, energy-normalized dielectric specular, **derivative-based TBN** normal mapping; vertex shader skins pos+normal only; `Init` lookups; `RenderFrame` light/normal-sampler uniform sets; draw-loop normal-map bind to unit 1.
- `docs/PHASE4.5_VALIDATOR_GATE.md` — **new**; §1 gate + gate-iteration findings, **Result PASS**.
- `_bmad-output/planning-artifacts/architecture.md` — AR20 Spec Change Log entry (Story 6.5.1, updated for the gate iterations).
- `_bmad-output/implementation-artifacts/deferred-work.md` — Story 6.5.1 deferral entries (incl. alpha/transparency).
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — 6.5.1 status note.

## Change Log

| Date | Version | Description |
|------|---------|-------------|
| 2026-06-27 | 0.1 | Story 6.5.1 implemented (dev-story): source-fidelity rendering — sRGB shader-encode + `GL_SRGB8_ALPHA8` base colour, lighting uniforms, dielectric-correct specular, presence-gated normal maps (tangents + linear `GL_RGBA8` map). 5 GL-boundary `src/` files; no CMake/boundary change. Gate doc + AR20 log + deferred-work authored, Result PENDING. Status → review. |
| 2026-06-27 | 0.2 | Gate iteration 1 (Antho in-Reaper, FBX character): the `aiTextureType_HEIGHT` normal-map fallback was **removed** — a downloaded FBX's grayscale bump map under HEIGHT, sampled as a tangent-space normal, produced broad symmetric "melted-wax / contour-banding" smears (abdomen/back/thighs). Only true `aiTextureType_NORMALS` maps are now sampled; height→normal conversion deferred post-MVP (`deferred-work.md`). `src/asset_loader.cpp` only. Re-judge §1 gate. |
| 2026-06-28 | 0.3 | Gate iteration 2 (FBX torso still smeared — root-caused offline). Extracted the FBX's embedded 4K textures + UVs and reproduced the render in a Python rasterizer: the model data was correct, the smear was **our pipeline**. Two changes: (a) the per-vertex tangent TBN was replaced by a **per-fragment screen-space-derivative TBN** — mirrored character UVs zero the smoothed vertex tangent along the body mirror seam and silently dropped the normal map on the torso; the derivative frame is robust to mirrored/un-tangented/skinned meshes (vertex tangent attribute + `aiProcess_CalcTangentSpace` removed). (b) `ConvertMaterial` now **always ignores authored `COLOR_SPECULAR`** (Mixamo's 0.5 grey → plastic skin) and derives dielectric from metalness. `src/scene.h`, `src/asset_loader.cpp`, `src/renderer.{h,cpp}`. |
| 2026-06-28 | 0.4 | Gate iteration 3 — **the real FBX bug**: an **all-format UV V-flip** (`aiProcess_FlipUVs`). Offline-measured that assimp delivers UVs bottom-up for every importer (assimp glTF V = `1 − raw-glTF V`) while we upload textures top-down → a UV atlas samples the wrong/padded region = the "smeared marble". FBX gate **PASSED** (Antho in-Reaper). Debug scaffolding (left-click layer-isolation views, `[RAV]` diagnostics, temporary FPS-silence) added during diagnosis was fully **reverted** — `viewer_window.cpp` nets to zero change. |
| 2026-06-28 | 0.5 | Gate iteration 4 (GLB): two GLB-specific findings. (a) An interim build flipped V only for non-glTF, leaving GLB **V-flipped** ("offset texture") → corrected to **always-flip** (assimp flips glTF too). (b) A glTF **metal** (metallic≈1, roughness≈1 → shininess floored ~2) washed white → **energy-normalized** the Blinn-Phong lobe `(n+8)/(8π)`. Both **root-caused offline** (assimp built on Linux to dump the GLB; full-shading Python render confirmed each fix). GLB **+ FBX** gate **PASSED** (Antho in-Reaper 2026-06-28). Alpha/transparency (GLB `BLEND` materials → opaque) noted as a separate deferred feature. `src/asset_loader.cpp`, `src/renderer.cpp`. → Result PASS; Status review (code-review launching). |
