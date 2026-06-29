---
baseline_commit: 4219bc4be58ee0980a48aad4e1500036bc92ce98
---

# Story 6.5.7: Per-pixel specular + glossiness maps (artist material intent)

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

> **Pre-ship fidelity polish — an Epic 6.5 story pulled forward from post-MVP deferred-work, before Epic 7 (ReaPack release).**
> On multi-material Mixamo FBX characters the **head/body seam pops** far more than in Mixamo's viewer. Offline
> investigation (assimp probe on [Catwalk Walk Turn 180 Tight.fbx](../../Reaper/Media%20Files/Catwalk%20Walk%20Turn%20180%20Tight.fbx))
> proved why: the model is **two meshes/materials** (body `Ch30_1002_*`, head `Ch30_1001_*`) meeting at the neck,
> and **each material carries a specular map + a glossiness map the artist authored** — which we **ignore**, applying
> a single uniform derived sheen instead. This story consumes those per-pixel maps (the artist's intent), which is
> exactly what makes Mixamo skin read naturally and the seam vanish. See
> [sprint-change-proposal-2026-06-29-spec-gloss-maps.md](../planning-artifacts/sprint-change-proposal-2026-06-29-spec-gloss-maps.md).

## Story

As a sound designer,
I want the viewer to use the specular/glossiness maps the artist authored,
so that skin and cloth read like the source DCC (Mixamo) and material seams don't pop.

## Acceptance Criteria

1. **Given** a model whose materials carry a **specular map** (`aiTextureType_SPECULAR`) and/or a **glossiness map** (`aiTextureType_SHININESS`) — e.g. a Mixamo FBX **When** it renders **Then** the specular reflection is modulated **per-pixel** by those maps — **intensity** from the specular map, **sharpness/exponent** from the glossiness map — instead of a single uniform sheen, so oily/shiny zones and matte zones differ as authored and a multi-material seam (head/body) reads continuous. [Source: epics.md#Story-6.5.7; the uniform-sheen line this replaces [renderer.cpp:138,145,154](../../src/renderer.cpp#L138); the maps confirmed present on the gate asset — assimp probe: both materials carry `*_Specular.png` (SPECULAR) + `*_Glossiness.png` (SHININESS), all 8 textures embedded]

2. **Given** the documented "Mixamo materials are too plastic" problem fixed in Story 6.5.1 **Then** the flat authored `COLOR_SPECULAR` is **STILL ignored** — only the **per-pixel maps** are consumed; the specular **base stays dielectric** (`specularColor` ≈ 0.04, metalness-derived). The maps **modulate** that base; they do not restore the flat plastic value. [Source: the 6.5.1 decision [asset_loader.cpp:442-458](../../src/asset_loader.cpp#L442); architecture.md Spec Change Log 2026-06-28 "Mixamo specular too shiny"]

3. **Given** a model **without** these maps (e.g. a glTF metallic-roughness asset, or any mesh with no SPECULAR/SHININESS texture) **Then** it renders **byte-for-byte as before 6.5.7** — the per-pixel path activates **only when the material provides the map**, exactly like the normal-map gate (6.5.1 AC4). [Source: the presence-gate precedent `u_hasNormalMap` [renderer.cpp:110,764](../../src/renderer.cpp#L110); glTF metallic-roughness does not populate the FBX-Phong SPECULAR/SHININESS texture slots]

4. **Given** the GL-boundary discipline **Then** the change is **GL-boundary-only and non-fatal**: a map that fails to resolve falls back to the current uniform specular (AR17, one `LogWarn` then flat — the existing funnel behaviour); the per-frame path stays **allocation-free** (D2, maps uploaded at load); it is **session-only** (no `.rpp`/D9, [pcm_source_anim.cpp](../../src/pcm_source_anim.cpp) byte-for-byte unchanged); and AR15 register-symmetry is intact — **no** `rec->Register`, **no** new `REAPERAPI_WANT_*`. The change is confined to `src/scene.h`, `src/asset_loader.cpp`, `src/renderer.{h,cpp}`. **No** `gl_loader.h` / `CMakeLists.txt` / `plugin_main` / `reaper_api` / `pcm_source` / `gpu_resources.h` / `viewer_window.cpp` change. [Source: AR15 epics.md:163, AR17 epics.md:165, AR18 epics.md:166; the 6.5.1 boundary precedent — same file set minus gl_loader]

5. **Given** the live light-tool knobs (Ambient / Specular / Relief, 6.5.x) **Then** they **still apply on top** of the per-pixel maps (the maps shape *where/how* the sheen falls; `u_specStrength` still scales the overall amount), so Antho can still dial the final look in-Reaper. [Source: the live knobs [renderer.h:91-96](../../src/renderer.h#L91); the spec term [renderer.cpp:154](../../src/renderer.cpp#L154)]

6. **Given** the maps are **data, not colour** **Then** both are uploaded **LINEAR** (`GL_RGBA8`), never `GL_SRGB8_ALPHA8` — a glossiness/specular value must not be gamma-decoded (same rule as the normal map). [Source: the normal-map LINEAR upload [asset_loader.cpp:579-599](../../src/asset_loader.cpp#L579); UploadTexture internal_format param [asset_loader.cpp:473](../../src/asset_loader.cpp#L473)]

7. **Given** ≥60 fps is the perf budget (NFR-P1) **Then** the two extra texture samples per fragment hold ≥60 fps on Antho's machine, and the result visibly improves the head/body seam vs 6.5.6. [Source: NFR-P1; the seam observation, this story's trigger]

### Out of scope (explicitly deferred — do NOT implement here)

- **A full metallic-roughness microfacet BRDF (Cook-Torrance/GGX), IBL, tone-mapping, true metal-diffuse suppression** — those stay post-MVP (deferred-work.md). This story is **only** the per-pixel specular + glossiness map consumption on the existing Blinn-Phong lobe.
- **Re-honoring the flat authored `COLOR_SPECULAR`** — that was the "too plastic" 6.5.1 problem; do NOT bring it back (AC2).
- **Persisting anything** — session-only, like the rest of Epic 6.5; no `.rpp`/D9, leave [pcm_source_anim.cpp](../../src/pcm_source_anim.cpp) byte-for-byte unchanged.
- **An ambient-occlusion / metalness / roughness texture path** — only SPECULAR + SHININESS(glossiness). (A glTF AO/metalness map is a separate future story.)
- **A UI toggle for the maps** — they're automatic (presence-gated). No new sidebar control, no new icon.

## Tasks / Subtasks

- [x] **Task 1 — `SceneMaterial` gains the two map handles (`scene.h`) (AC1/AC3/AC4).**
  - [x] Add `GpuImage specularMap;` and `GpuImage glossMap;` to `struct SceneMaterial` next to `normalMap` [scene.h:49](../../src/scene.h#L49) — empty handle (0) when the material declares no such map (the presence gate, mirroring `normalMap`). Comment them LINEAR-uploaded, presence-gated, Story 6.5.7.

- [x] **Task 2 — Resolve + upload the two maps at load (`asset_loader.cpp`) (AC1/AC3/AC6).**
  - [x] In the per-material load loop [asset_loader.cpp:867-878](../../src/asset_loader.cpp#L867), after `material.normalMap = ResolveAndUploadNormalMap(...)`, add:
    `material.specularMap = ResolveAndUploadTexture(scene, scene->mMaterials[mi], aiTextureType_SPECULAR, GL_RGBA8, model_dir, mi, "specular");`
    and `material.glossMap = ResolveAndUploadTexture(scene, scene->mMaterials[mi], aiTextureType_SHININESS, GL_RGBA8, model_dir, mi, "glossiness");`.
    `ResolveAndUploadTexture` already handles embedded/external + the `LogWarn`-then-empty fallback (AR17) and takes `internal_format` — pass **`GL_RGBA8`** (LINEAR, AC6). No new helper needed (unlike the NORMALS-only `ResolveAndUploadNormalMap`, SPECULAR/SHININESS are read directly). [Source: the funnel signature [asset_loader.cpp:510-513](../../src/asset_loader.cpp#L510); the existing diffuse/normal calls [asset_loader.cpp:871-877](../../src/asset_loader.cpp#L871)]
  - [x] Note: the synthetic fallback material path (mat==nullptr) [asset_loader.cpp:882](../../src/asset_loader.cpp#L882) leaves both handles empty — correct (no maps → uniform path).

- [x] **Task 3 — Shader: per-pixel specular modulation (`renderer.cpp` mesh fragment shader) (AC1/AC2/AC5). READ Dev Notes "The shader change" FIRST.**
  - [x] Declare uniforms next to `u_normalMap` [renderer.cpp:87-88](../../src/renderer.cpp#L87): `uniform sampler2D u_specularMap; uniform int u_hasSpecularMap; uniform sampler2D u_glossMap; uniform int u_hasGlossMap;`.
  - [x] **Glossiness → per-pixel exponent:** before the `pow` [renderer.cpp:138](../../src/renderer.cpp#L138), compute `float shin = u_shininess; if (u_hasGlossMap != 0) shin = mix(8.0, 200.0, texture(u_glossMap, v_uv).r);` then use `shin` in BOTH the `pow(..., shin)` and the energy-normalization `(shin + 8.0)/(8π)` [renderer.cpp:145](../../src/renderer.cpp#L145). (The `[8,200]` range is a starting point — Antho tunes at the gate; matte skin → low, oily highlights → high.)
  - [x] **Specular map → per-pixel intensity:** at the spec accumulation [renderer.cpp:154](../../src/renderer.cpp#L154) `c += u_specularColor * spec * u_specStrength * u_lightColor;`, multiply in the map: `vec3 specTint = vec3(1.0); if (u_hasSpecularMap != 0) specTint = texture(u_specularMap, v_uv).rgb;` then `c += u_specularColor * spec * u_specStrength * specTint * u_lightColor;`. This keeps the **dielectric base** `u_specularColor` (AC2) and modulates only where/how strong the sheen falls — matte zones (dark map) get no sheen → the seam evens out.
  - [x] Do **not** change the diffuse, ambient, normal-map, or sRGB-encode blocks.

- [x] **Task 4 — Wire the uniforms + bind the maps (`renderer.h` / `renderer.cpp`) (AC1/AC3/AC4).**
  - [x] **`renderer.h`:** add `int u_specular_map_ = -1; int u_has_specular_map_ = -1; int u_gloss_map_ = -1; int u_has_gloss_map_ = -1;` next to `u_normal_map_`/`u_has_normal_map_` [renderer.h:185-186](../../src/renderer.h#L185).
  - [x] **`renderer.cpp` Init:** `glGetUniformLocation` for the four, next to the normal-map ones [renderer.cpp:303-304](../../src/renderer.cpp#L303). All non-fatal at -1 (driver may dead-strip; only `u_mvp` is required — the existing contract [renderer.cpp:307-314](../../src/renderer.cpp#L307)).
  - [x] **`renderer.cpp` RenderFrame (per-frame sampler bind):** next to `glUniform1i(u_normal_map_, 1);` [renderer.cpp:636](../../src/renderer.cpp#L636), bind the sampler units: `glUniform1i(u_specular_map_, 2); glUniform1i(u_gloss_map_, 3);`.
  - [x] **`renderer.cpp` visible mesh loop (per-material bind):** after the normal-map bind [renderer.cpp:758-764](../../src/renderer.cpp#L758), bind each map to its unit using `glActiveTexture(GL_TEXTURE0 + 2)` / `+ 3` (NO new `GL_TEXTURE2`/`GL_TEXTURE3` enum needed — additive offset; that's why `gl_loader.h` is untouched):
    `const GLuint smap = mat.specularMap.get(); glActiveTexture(GL_TEXTURE0 + 2); glBindTexture(GL_TEXTURE_2D, smap); glUniform1i(u_has_specular_map_, smap != 0 ? 1 : 0);`
    and the same for `glossMap` on `GL_TEXTURE0 + 3` → `u_has_gloss_map_`. Mirror the diffuse/normal three-call rhythm (D2, no heap). [Source: the diffuse/normal per-material binds [renderer.cpp:750-764](../../src/renderer.cpp#L750)]
  - [x] **Depth/shadow pass:** unaffected — it uses only `u_mvp`/`u_skinned`; the new samplers are never read there. Leave it.

- [x] **Task 5 — Scope audit, gate §10, AR20, deferred-work (AC4 + AR19/AR20).**
  - [x] `git diff --stat -- src/` shows the render quartet `src/scene.h`, `src/asset_loader.cpp`, `src/renderer.cpp`, `src/renderer.h` **plus one line in** `src/viewer_window.cpp` (the baked Ambient default 0.05 → 0.5 — same precedent as the live Light knobs; see Change Log). **No** `gl_loader.h`/`gpu_resources.h`/`CMakeLists.txt`/`plugin_main`/`reaper_api`/`pcm_source`. Run `git diff -- src/ | grep -E 'rec->Register|REAPERAPI_WANT_'` → **empty** (AR15 register-symmetry intact).
  - [x] **AR20 Spec Change Log** entry in [architecture.md](../planning-artifacts/architecture.md) (newest-on-top): **Trigger** (FR48 fidelity — per-pixel spec/gloss maps, Story 6.5.7 — pronounced head/body seam on multi-material Mixamo FBX from ignoring the artist's maps) / **Decision** (consume `aiTextureType_SPECULAR` + `aiTextureType_SHININESS` LINEAR, modulate the Blinn-Phong lobe per-pixel; presence-gated like the normal map; flat `COLOR_SPECULAR` STILL ignored) / **KEEP** (AR15 register-symmetry; D2 zero-alloc per-frame [maps uploaded cold]; AR17 non-fatal fallback to uniform specular; glTF path byte-for-byte unchanged; session-only / D9 untouched; refines but does not reverse the 6.5.1 dielectric-specular decision).
  - [x] Append gate **§10** to [docs/PHASE4.5_VALIDATOR_GATE.md](../../docs/PHASE4.5_VALIDATOR_GATE.md) in the §8/§9 shape: intro + `> Scope note` (uses the artist's spec/gloss maps; glTF unchanged; flat COLOR_SPECULAR still ignored; live knobs still apply) + `Suggested fixtures:` (the Catwalk Mixamo FBX; a glTF model for the no-regression check) + a **click-based** check table + `**Result:** … — PENDING`. **Never pre-mark PASS** (AR19).
  - [x] Update [deferred-work.md](deferred-work.md): the "pulled forward" annotation already added by the Correct Course — confirm it reflects what shipped.
  - [x] Build clean at `/W3 /permissive-` (NFR-R5). Linux can't build the `_WIN32` renderer/loader TUs → self-review (the change mirrors the proven 6.5.1 normal-map presence-gate + funnel exactly, one extra texture unit pair); judged at Antho's in-Reaper Windows gate §10 (AR19).

## Dev Notes

### What the asset actually contains (offline assimp probe, the trigger evidence)

[Catwalk Walk Turn 180 Tight.fbx](../../Reaper/Media%20Files/Catwalk%20Walk%20Turn%20180%20Tight.fbx) is **two meshes / two materials** meeting at the neck:
- `Ch30_Body1` (body, 15 473 v) → `Ch30_1002_Diffuse/Specular/Normal/Glossiness.png`
- `Ch30_Body` (head, 4 433 v) → `Ch30_1001_Diffuse/Specular/Normal/Glossiness.png`

All 8 textures are **embedded** and resolve via the existing funnel. assimp maps the **specular map → `aiTextureType_SPECULAR`** and the **glossiness map → `aiTextureType_SHININESS`** (verified). Today we read diffuse + normal + the scalar shininess, and **ignore the spec/gloss maps**, so every fragment gets the same dielectric sheen → the two-material seam pops under low-ambient/high-spec lighting. Mixamo uses the maps → the seam disappears.

### The shader change (the heart of this story)

The current spec term [renderer.cpp:138-154](../../src/renderer.cpp#L138):
```glsl
float spec = pow(max(dot(N, H), 0.0), u_shininess);
spec *= (u_shininess + 8.0) / (8.0 * 3.14159265);   // energy-normalize
...
c += u_specularColor * spec * u_specStrength * u_lightColor;
```
becomes per-pixel:
```glsl
float shin = u_shininess;
if (u_hasGlossMap != 0) shin = mix(8.0, 200.0, texture(u_glossMap, v_uv).r);  // gloss→exponent (gate-tuned)
float spec = pow(max(dot(N, H), 0.0), shin);
spec *= (shin + 8.0) / (8.0 * 3.14159265);
...
vec3 specTint = vec3(1.0);
if (u_hasSpecularMap != 0) specTint = texture(u_specularMap, v_uv).rgb;   // where/how strong the sheen falls
c += u_specularColor * spec * u_specStrength * specTint * u_lightColor;
```
Key invariants: the **dielectric base `u_specularColor`** stays (AC2 — no flat plastic); the maps **modulate** it; absent maps → flags 0 → **identical to today** (AC3). The `mix(8.0, 200.0, …)` exponent range is the one number to tune at Antho's gate (matte→oily); start there.

### Why this is presence-gated, not always-on (AC3 — the glTF no-regression guard)

glTF metallic-roughness assets don't populate the FBX-Phong `SPECULAR`/`SHININESS` **texture** slots (they use metalness/roughness factors + their own maps), so `GetTexture(aiTextureType_SPECULAR/SHININESS)` returns no texture → empty handle → `u_hasSpecularMap`/`u_hasGlossMap` = 0 → the shader takes the exact pre-6.5.7 path. The gate-validated glTF render (steampunk, 6.5.1) must come back **byte-for-byte unchanged** — verify it explicitly. Same discipline as the normal-map gate that already ships.

### Colour space — LINEAR, like the normal map (AC6)

Specular and glossiness are **data**, not colour: a glossiness value is a roughness/sharpness scalar, a specular-map value is a reflection intensity. Upload **`GL_RGBA8`** (LINEAR) — passing `GL_SRGB8_ALPHA8` would gamma-decode the data and warp the relief/sheen (the same reason the normal map is `GL_RGBA8` not sRGB, [asset_loader.cpp:579](../../src/asset_loader.cpp#L579)). `ResolveAndUploadTexture`'s `internal_format` parameter is exactly this switch.

### Texture units — additive offset, so gl_loader.h is untouched

Units 0 (diffuse) and 1 (normal) are bound via `glActiveTexture(GL_TEXTURE0)` / `GL_TEXTURE1` (enums in [gl_loader.h:46-47](../../src/gl_loader.h#L46)). For units 2 and 3 use **`glActiveTexture(GL_TEXTURE0 + 2)` / `+ 3`** — `GL_TEXTURE0..n` are guaranteed contiguous, so no `GL_TEXTURE2`/`GL_TEXTURE3` `#define` is needed and `gl_loader.h` stays untouched (tighter scope than 6.5.1, which added a row). All the GL functions used (`glActiveTexture`, `glBindTexture`, `glUniform1i`, `glTexImage2D`) are already resolved.

### Non-fatal everything (AR17/AR18)

`ResolveAndUploadTexture` already returns an empty handle (one `LogWarn`, then flat) on any resolve/upload failure — a missing/undecodable spec or gloss map just falls back to the uniform specular, viewer still runs. No new failure path. Console stays silent by default (6.5.2). No exception escapes (AR18). The per-frame path is a few extra `glUniform1i`/`glBindTexture` calls — **no heap (D2)**.

### Regression guardrails

- **glTF / no-map assets: byte-for-byte unchanged** — flags 0, the spec block is the pre-6.5.7 code path. THE no-regression check (AC3).
- **Diffuse / normal-map / ambient / sRGB-encode untouched** — only the specular exponent + intensity gain per-pixel modulation.
- **Depth/shadow pass untouched** — it never reads the new samplers.
- **Transport/playhead untouched** — pure render state (AC4); `pcm_source_anim.cpp` byte-for-byte, session-only.
- **Boundary (AR15):** only `scene.h` + `asset_loader.cpp` + `renderer.{h,cpp}`; no Register/WANT_/CMake/gl_loader/gpu_resources/viewer_window/plugin_main/reaper_api/pcm_source.

### Latest-tech note (no web research warranted)

This is plain GL 3.3 multi-texture sampling on the existing shader/funnel — two more `sampler2D`s and two more bound units. No new library, no version surface, no breaking change. The spec-glossiness workflow is standard; assimp's `aiTextureType_SPECULAR`/`SHININESS` mapping for FBX-Phong is stable and was verified on the gate asset.

### Validation = Antho's in-Reaper Windows gate (AR19)

Linux compiles the non-`_WIN32` TUs but can't build the `_WIN32` renderer/loader/ImGui units or open Reaper, so this is **self-reviewed** (it mirrors the proven 6.5.1 normal-map presence-gate + funnel, one extra unit pair) and judged **in-Reaper on Windows**. Author gate **§10 PENDING** — never pre-mark PASS. Validator hooks are **click-based**: double-click `build.bat`, load the **Catwalk** Mixamo FBX → the head/body seam should be markedly reduced vs 6.5.6; load a **glTF** model → it must look identical to before. [[feedback_clickable_test_methods]]

### Project Structure Notes

- Flat `src/`. This story: `src/scene.h` (`SceneMaterial` += `specularMap`/`glossMap`), `src/asset_loader.cpp` (resolve+upload SPECULAR + SHININESS LINEAR via the existing funnel), `src/renderer.cpp` (shader per-pixel modulation + 4 uniform locations + per-frame sampler binds + per-material map binds on units 2/3), `src/renderer.h` (4 uniform-location ints). Docs: gate §10, AR20 Spec Change Log, deferred-work confirmation, sprint-status.
- Naming: members `snake_case_` (`u_specular_map_`); shader uniforms `u_camelCase` (`u_hasGlossMap`); methods PascalCase. Namespace `rav`; SPDX MIT header; WHY-only comments.

### References

- [Source: epics.md#Story-6.5.7] — the ACs (per-pixel spec/gloss, keep ignoring flat COLOR_SPECULAR, presence-gated, non-fatal).
- [Source: sprint-change-proposal-2026-06-29-spec-gloss-maps.md] — issue, assimp-probe evidence, file list, risks.
- [Source: 6-5-1-source-fidelity-rendering-*.md] — the dielectric-specular decision this refines, the normal-map presence-gate + LINEAR upload + funnel patterns this mirrors.
- [Source: src/renderer.cpp:75-159] — the mesh fragment shader (the spec term to modulate); :289-306 uniform locations; :632-636 per-frame sampler binds; :742-768 per-material diffuse/normal binds (the rhythm to mirror).
- [Source: src/asset_loader.cpp:420-460] ConvertMaterial (specular derivation untouched); :500-600 the texture funnel + ResolveAndUploadNormalMap; :860-892 the per-material load loop (where to add the two calls).
- [Source: src/scene.h:47-55] SceneMaterial (where to add the two handles).
- [Source: architecture.md Spec Change Log] — 2026-06-28 "Mixamo specular too shiny" (6.5.1) the decision this refines; AR15/AR17/AR19/AR20.
- [Source: deferred-work.md] — the post-MVP "use glossiness/specular maps" item, annotated pulled-forward.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Opus 4.8, 1M context)

### Debug Log References

- `git diff --stat -- src/` → `scene.h`, `asset_loader.cpp`, `renderer.cpp`, `renderer.h` + a one-line `viewer_window.cpp` change (baked Ambient default 0.05 → 0.5; documented deviation from the original AC4 file set — see Change Log + File List).
- `git diff -- src/ | grep -E 'rec->Register|REAPERAPI_WANT_'` → empty (AR15 register-symmetry intact).
- Build not run on this box: the `_WIN32` renderer/loader/ImGui TUs do not compile on Linux and Reaper can't open here — self-reviewed against the proven 6.5.1 normal-map pattern, judged at Antho's in-Reaper Windows gate §10 (AR19), exactly as 6.5.1–6.5.6.

### Completion Notes List

Implemented per-pixel specular + glossiness map consumption to fix the multi-material Mixamo head/body seam, mirroring the proven 6.5.1 normal-map presence-gate + texture funnel.

- **scene.h** — `SceneMaterial` gains `specularMap` + `glossMap` (empty handle when the material declares no such map; LINEAR-uploaded, presence-gated, commented Story 6.5.7).
- **asset_loader.cpp** — in the per-material load loop, after `normalMap`, resolve `aiTextureType_SPECULAR` and `aiTextureType_SHININESS` via the existing `ResolveAndUploadTexture` funnel with `GL_RGBA8` (LINEAR, AC6). Reuses the funnel's embedded/external resolution + AR17 `LogWarn`-then-empty fallback; no new helper. The synthetic `mat==nullptr` material leaves both handles empty → uniform path (correct).
- **renderer.cpp (shader)** — four new uniforms (`u_specularMap`/`u_hasSpecularMap`/`u_glossMap`/`u_hasGlossMap`). Glossiness map drives the Blinn exponent **per-pixel** (`shin = mix(8.0, 200.0, gloss.r)`, fed into BOTH `pow(...,shin)` and the `(shin+8)/(8π)` energy-normalization); specular map tints the sheen intensity **per-pixel** (`specTint`, multiplied into the spec accumulation). The **dielectric base `u_specularColor` stays** (AC2 — flat `COLOR_SPECULAR` still ignored). Absent maps → flags 0 → byte-for-byte the pre-6.5.7 path (AC3). Diffuse/ambient/normal-map/sRGB-encode blocks untouched.
- **renderer.h / renderer.cpp (wiring)** — four `glGetUniformLocation` (non-fatal at -1), per-frame sampler binds to units 2/3, per-material map binds via `glActiveTexture(GL_TEXTURE0 + 2 / + 3)` (contiguous offset → **no `gl_loader.h` change**), each setting its `u_has*Map` flag from the handle. Depth/shadow pass untouched (never reads the new samplers).
- **AC5** — `u_specStrength` still scales the overall sheen; the live Light knobs (Ambient/Specular/Relief) apply on top of the maps.
- **Boundary (AR15/AC4):** confined to `scene.h` + `asset_loader.cpp` + `renderer.{h,cpp}`; no Register/WANT_/gl_loader/gpu_resources/CMake/plugin_main/reaper_api/pcm_source/viewer_window change. D2 zero-alloc per-frame (maps uploaded cold). Session-only (D9 untouched).
- **Docs:** AR20 Spec Change Log (2026-06-29, newest-on-top) + gate §10 (click-based, **PENDING** — never pre-marked PASS) + deferred-work pulled-forward annotation confirmed accurate.

### File List

- `src/scene.h` — `SceneMaterial` += `specularMap`, `glossMap`
- `src/asset_loader.cpp` — resolve + upload SPECULAR + SHININESS maps LINEAR via the funnel
- `src/renderer.cpp` — shader per-pixel modulation (gloss→exponent `mix(1.0, 200.0)`, Antho's tuned range); 4 map-uniform locations; per-frame sampler binds; per-material map binds (units 2/3)
- `src/renderer.h` — 4 map-uniform-location members; + baked Ambient default 0.05 → 0.5 (Antho's gate)
- `src/viewer_window.cpp` — baked Ambient default 0.05 → 0.5 in the UI seed (the only 6.5.7 change here; the temporary Gloss min/max tuning sliders were removed once Antho fixed the range)
- `_bmad-output/planning-artifacts/architecture.md` — AR20 Spec Change Log entry (2026-06-29)
- `docs/PHASE4.5_VALIDATOR_GATE.md` — gate §10 (PENDING)
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — status → in-progress → review
- `_bmad-output/implementation-artifacts/6-5-7-per-pixel-specular-glossiness-maps.md` — this story (baseline_commit, tasks, Dev Agent Record, status)

## Review Findings

_BMAD 3-layer code review (Blind Hunter / Edge Case Hunter / Acceptance Auditor), 2026-06-29. Acceptance Auditor: AC1/AC2/AC3/AC5/AC6 COMPLIANT; AC4 + gloss-range deviations documented & justified. Blind Hunter's lone CRITICAL (active-texture state leak) was verified a FALSE POSITIVE against the real code — the diffuse bind re-anchors `glActiveTexture(GL_TEXTURE0)` at the top of every material iteration ([renderer.cpp:863](../../src/renderer.cpp#L863)), so the trailing unit-3 state is overwritten before any meaningful bind; Edge Case Hunter found zero genuine unhandled paths._

- [x] [Review][Decision] Ambient default flip 0.05 → 0.5 reverses the value baked one story ago — `ambient_` ([renderer.h:223](../../src/renderer.h#L223)) and `g_ambient` ([viewer_window.cpp:143](../../src/viewer_window.cpp#L143)) both go 0.05 → 0.5. **RESOLVED 2026-06-29 (Antho): intentional — 0.5 is his dialed-in gate value, ships as the new default.** Supersedes the 6.5.6 baked 0.05 (commit 7cf7c08).
- [x] [Review][Patch] Story doc scope claims contradicted the shipped diff — Task 5 first checkbox and the Debug Log References asserted a 4-file/no-viewer_window.cpp scope; `viewer_window.cpp` IS in the diff (the Ambient default). **FIXED 2026-06-29:** both lines now state the render quartet + the one-line `viewer_window.cpp` Ambient-default change, AR15 grep still empty.

_Dismissed as noise (9):_
- Active-texture state leak (Blind CRITICAL) — false positive; `glActiveTexture(GL_TEXTURE0)` re-anchors each iteration ([renderer.cpp:863](../../src/renderer.cpp#L863)); depth pass discards fragment colour. Verified.
- `aiTextureType_SHININESS` read as glossiness (gloss-vs-roughness inversion) — by-design: the gate asset's artist-authored map is a glossiness map (offline assimp probe), `mix(1,200,gloss.r)` is the correct direction; visual correctness is Antho's gate (§10).
- Gloss `.r` from grayscale-as-RGB — handled: stb forces 4 channels, replicates gray to R=G=B ([asset_loader.cpp:527,565](../../src/asset_loader.cpp#L527)).
- Specular/gloss should be sRGB not LINEAR — contradicts AC6 (data maps, LINEAR by spec); deliberate.
- `mix(1,200)` discards `u_shininess` — by-design (gloss map replaces the scalar per-pixel, AC1); gate-tuned.
- No uniform-location `-1` guard — matches the established `u_normalMap` pattern; a dead-stripped flag implies a dead-stripped sampler ⇒ no garbage read; `glUniform1i(-1,…)` is a safe no-op.
- Specular triple-multiply (`u_specularColor * specTint * u_lightColor`) — the designed AC2 modulation of the dielectric base.
- Energy-normalization weak at low gloss — cosmetic, author-acknowledged, harmless ([8,200]→finite).
- AC4 viewer_window.cpp / `mix(8,200)`→`mix(1,200)` deviations — both documented in Change Log + File List, within the spec's "tune at the gate" latitude.

## Change Log

| Date | Change |
|------|--------|
| 2026-06-29 | Implemented per-pixel specular + glossiness maps (Tasks 1–5): `SceneMaterial` += `specularMap`/`glossMap`; loader resolves `aiTextureType_SPECULAR` + `aiTextureType_SHININESS` LINEAR via the funnel; mesh fragment shader modulates the Blinn-Phong lobe per-pixel (gloss→exponent `mix(8,200)`, spec map→`specTint`); 4 uniforms wired, maps bound to units 2/3 via `GL_TEXTURE0+n`. Dielectric base + flat `COLOR_SPECULAR` ignore preserved; presence-gated (glTF unchanged). AR20 Spec Change Log + gate §10 (PENDING). Scope: 4 src files, AR15-clean. Status → review. |
| 2026-06-29 | Antho-directed tuning ("skin lacks brillance"): made the gloss-exponent range **live** — the hardcoded `mix(8,200)` becomes `mix(u_glossLow, u_glossHigh, …)`, exposed as **Gloss min / Gloss max** `SliderFloat`s in the Light section (mirrors the 6.5.x Ambient/Specular/Relief knobs). Adds `src/viewer_window.cpp` to the scope (one file beyond AC4, same precedent as the live Light knobs). AR15 intact. |
| 2026-06-29 | Baked Antho's in-Reaper gate values: gloss-exponent range fixed at **`mix(1, 200)`** (was 8/200), **Ambient default 0.05 → 0.5**. Then **removed the temporary Gloss min/max tuning sliders** — the range is hardcoded in the shader again, so 6.5.7 returns to its original AC4 render-file boundary (`scene.h` + `asset_loader.cpp` + `renderer.{h,cpp}`); `viewer_window.cpp` now carries only the one-value Ambient default tweak. Antho noted full Mixamo parity is not reached (expected — Mixamo's PBR pipeline stays post-MVP); the head/body seam reduction is the shipped win. |
