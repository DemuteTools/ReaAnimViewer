# Story 2.3: Multi-material rendering with per-material specular

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want each material on a mesh rendered with its own parameters,
so that I can visually distinguish material types like matte leather vs polished steel.

This is the **third (and last shading) story of Epic 2 (Phase 1)**. Stories 2.1 and 2.2 already built **almost all of the multi-material machinery**: 2.1 stood up the per-mesh draw loop that binds a per-material `baseColorFactor` / `specularColor` / `shininess` and a Blinn-Phong shader that already computes a specular term; 2.2 added the per-material diffuse texture. **The render side of FR20 (multi-material) is therefore already working** — `WalkBake` emits one `SceneMesh` per (node, mesh) with its own `materialIdx`, the loader converts **all** `scene->mNumMaterials`, and the draw loop draws each mesh with its referenced material.

What is **not** yet correct is the *specular response itself* (FR16). assimp's glTF2 importer maps glTF `roughnessFactor` → `AI_MATKEY_SHININESS` but **leaves `AI_MATKEY_COLOR_SPECULAR` unset and ignores `metallicFactor`** for the common metallic-roughness material — so today every glTF material renders with the same flat `0.04` grey specular color, and a matte dielectric and a polished metal look the *same hue of highlight*. This story closes exactly that gap in **`ConvertMaterial`**: derive a meaningful, visually-distinguishable per-material specular (color from metalness, exponent from roughness) while honoring an explicitly-authored specular (FBX/Collada Phong, glTF `KHR_materials_specular`, `pbrSpecularGlossiness`). **It is overwhelmingly a loader-side change — the renderer, shader, scene model, GL loader, and CMake do not change.**

## Acceptance Criteria

From [epics.md#Story 2.3](../planning-artifacts/epics.md) (lines 369–380), verbatim BDD:

**Given** a mesh composed of multiple materials with differing specular properties
**When** the renderer draws each `SceneMesh` with its referenced `SceneMaterial` (baseColor, specularColor, shininess)
1. **Then** each sub-mesh shows its own diffuse texture and specular response (FR16, FR20)
2. **And** a visibly matte surface and a visibly glossy surface are distinguishable in the same frame.

Implied, non-negotiable (system must stay working end-to-end — requirements even though not in the AC text):

3. **No regression to Stories 2.1 / 2.2.** A single-material textured file, a textureless file, a non-canonical file, and a `.dae` all still render exactly as before. The change is purely *which values* `ConvertMaterial` writes into each `SceneMaterial`; the draw loop and shader are byte-for-byte unchanged, so 2.1's rows 1–9 and 2.2's rows 10–14 must still pass.
4. The deliverable remains a **single `reaper_animviewer.dll`**, builds **warning-free under `/W3 /permissive-`** for our own sources (NFR-R5), holds **≥60 fps** (NFR-P1), and the loader stays **no-throw** across its boundary (AR18 — the new code is pure arithmetic on values already read, allocates nothing, and cannot throw).
5. The material work happens **only in `asset_loader.cpp::ConvertMaterial`** (the existing material-conversion site). No new translation unit, no new uniform, no new GL entry point, no `scene.h` field — the `SceneMaterial { specularColor, shininess }` fields already exist and are already bound by the renderer.

## Tasks / Subtasks

- [x] **Task 1 — Derive a per-material Blinn-Phong specular from the PBR/Phong inputs in `ConvertMaterial` (AC: 1, 2, 5)**
  - [x] This is the whole story's core and lives entirely in `ConvertMaterial` ([asset_loader.cpp:67-84](../../src/asset_loader.cpp#L67-L84)). Keep the existing diffuse read (`AI_MATKEY_COLOR_DIFFUSE`) and the `if (!mat) return out;` synthetic-default guard unchanged. Exact recipe in **Dev Notes §A**.
  - [x] **Shininess (specular exponent → highlight tightness).** Read `AI_MATKEY_SHININESS`. assimp **synthesizes this for all three importers**: glTF metallic-roughness writes `(1 − roughnessFactor)² × 1000` (`glTF2Importer.cpp:270-272`), glTF `pbrSpecularGlossiness` writes `glossiness × 1000`, and FBX/Collada Phong write the authored exponent. A fully-rough glTF material yields **0**, so replace 2.1's `&& s >= 1.0f` guard (which silently snaps a rough material back to the default 32) with a **clamp to a small floor**: `out.shininess = clamp(s, 2.0f, 1000.0f)` only when the `Get` succeeds. Floor 2 reads as a broad highlight (matte), not "absent" and not "default".
  - [x] **Specular color (metal vs dielectric).** **Prefer an explicitly-authored specular**: if `mat->Get(AI_MATKEY_COLOR_SPECULAR, c) == AI_SUCCESS`, use it as-authored — this single branch covers FBX/Collada Phong, glTF `KHR_materials_specular`, and `pbrSpecularGlossiness` (all of which set the key; see Dev Notes §B). **Otherwise** (plain glTF metallic-roughness — the common case, where assimp leaves `COLOR_SPECULAR` unset), **derive** it from metalness: read `AI_MATKEY_METALLIC_FACTOR` (default to `0.0` = dielectric if absent) and tint `specularColor = mix(vec3(0.04), baseColorFactor, metallic)` — a dielectric reflects a dim ~4 % white highlight (F0 ≈ 0.04), a metal reflects its own base color. This is the line that makes leather and steel distinguishable (FR16).
  - [x] Use **plain arithmetic** for the mix (`vec3(0.04f) + (baseColorFactor - vec3(0.04f)) * metallic`) and a manual or `std::clamp` (add `<algorithm>` if you use `std::clamp`) for shininess — no new include risk, no glm function dependency beyond what `scene.h` already pulls. Allocation-free and exception-free (AC4/AR18).
  - [x] **Comment the WHY**, not the what (architecture line 517-524 convention): the 0.04 dielectric F0, the metal-tint-toward-baseColor, and "assimp leaves COLOR_SPECULAR unset for metallic-roughness so we must derive it" are exactly the non-obvious reasons worth one line each. No story/commit IDs in comments.

- [x] **Task 2 — Confirm the multi-material render path is intact and needs NO change (AC: 1, 3, 5)**
  - [x] **Do not rewrite the draw loop or the loader's material/mesh loops — they already implement FR20.** Verify (read, don't edit) that: `WalkBake` still emits one `PendingMesh` per (node, mesh) carrying `mesh->mMaterialIndex` ([asset_loader.cpp:250-274](../../src/asset_loader.cpp#L250-L274)); the material loop converts every `scene->mNumMaterials` ([asset_loader.cpp:396-407](../../src/asset_loader.cpp#L396-L407)); the out-of-range `materialIdx` clamp ([asset_loader.cpp:413-415](../../src/asset_loader.cpp#L413-L415)) stays in place; and the per-mesh draw binds `mat.baseColorFactor` / `mat.specularColor` / `mat.shininess` + texture ([renderer.cpp:241-255](../../src/renderer.cpp#L241-L255)). The only behavior change in this story is the *values* Task 1 writes into each `SceneMaterial`.
  - [x] **Do not touch the shader.** The fragment shader's `c = base*(0.15+0.85*diff) + u_specularColor*spec` term ([renderer.cpp:52-64](../../src/renderer.cpp#L52-L64)) already renders a per-material specular; feeding it better per-material values is the entire fix. Adding a metalness uniform or a PBR BRDF is **out of scope** (Scope Fences).
  - [x] Sanity grep after Task 1: confirm `renderer.cpp`, `renderer.h`, `scene.h`, `gl_loader.h`, and `CMakeLists.txt` are unmodified by this story (their diff is empty).

- [x] **Task 3 — Extend the Phase 1 validator gate with Story 2.3 rows (AC: 1, 2, 3)**
  - [x] Append a **"## 8. Story 2.3 — acceptance checks (multi-material + per-material specular)"** section to `docs/PHASE1_VALIDATOR_GATE.md` (continue the numbering — 2.2 ended at row 14). Rows:
    - **Multi-material renders distinctly (FR20)** — a model with several materials (e.g. Khronos `FlightHelmet` or `BoomBox`, or `DamagedHelmet`) shows each sub-mesh with its **own** diffuse/texture and highlight — not one uniform surface.
    - **Matte vs glossy distinguishable (FR16, AC2)** — Khronos **`MetalRoughSpheres`** (a grid varying metallic × roughness) is the ideal fixture: the low-roughness/metallic spheres show a **tight, bright, base-color-tinted** highlight and the high-roughness ones a **broad, dim** one, side by side in one frame. (A two-material custom file — one matte, one glossy — also works.)
    - **No-regression** — re-run the 2.1 canonical/non-canonical/`.dae`/corrupt rows and the 2.2 texture rows; all still pass unchanged (the draw loop and shader did not change).
    - **Single-DLL / warning-free still hold** — no new DLL beside `reaper_animviewer.dll`; build clean at `/W3 /permissive-`.
  - [x] Add a **specular-tuning note** (mirroring 2.2's UV-flip note — a "looks-right" judgment confirmable only on the Windows gate, not the Linux dev box): if metals read too flat/too bright or the matte/glossy contrast is too subtle, the knobs are the **dielectric F0 constant** (`0.04`), the **shininess floor/ceiling clamp** (`2 … 1000`), and optionally a mild **metal diffuse suppression** (`baseColorFactor *= (1 − k·metallic)`) — flag it for a one-line follow-up rather than guessing blind. The starting position (F0 0.04, clamp 2…1000, **no** diffuse suppression) is the principled default; see Dev Notes §C for why diffuse suppression is *not* applied by default.

### Review Findings

Code review 2026-06-24 (BMAD 3-layer: Blind Hunter + Edge Case Hunter + Acceptance Auditor). Acceptance Auditor verdict: **PASS** — code matches Dev Notes §A line-for-line, all scope fences respected, renderer/shader/scene.h/gl_loader/CMake byte-for-byte unchanged, `materialIdx` clamp intact. Triage: 2 patch, 1 defer, 4 dismissed.

- [x] [Review][Patch] NaN-safe shininess clamp — the refactor dropped the old `&& s >= 1.0f` guard, which implicitly rejected NaN. `std::clamp(NaN, 2, 1000)` returns NaN (documented footgun), and that NaN flows to `u_shininess` → `pow(...)` in the fragment shader, poisoning every pixel of the material (the fixed-function write does not bring NaN into [0,1]). The project already NaN-guards analogous loader→GPU values (AppendMesh vertex positions, SetAsset radius). Restore robustness: `out.shininess = std::isfinite(s) ? std::clamp(s, 2.0f, 1000.0f) : 32.0f;` [src/asset_loader.cpp:86-87]
- [x] [Review][Patch] Clamp `metallic` to its glTF-defined [0,1] domain before the mix — `metallic` is read and fed straight into `0.04 + (base-0.04)*metallic` with no validation. The lerp is only a convex blend for `metallic` in [0,1]; a malformed file with metallic >1 overshoots specular past baseColor, <0 drives it negative (dark-ring artifact), NaN poisons the fragment. Make the comment's intent ("tint toward baseColor by metalness") robust: `metallic = std::isfinite(metallic) ? std::clamp(metallic, 0.0f, 1.0f) : 0.0f;` [src/asset_loader.cpp:101-103]
- [x] [Review][Defer] Textured colored metal derives a white specular highlight [src/asset_loader.cpp:100-103] — deferred, out of scope. A gold/copper metal commonly authors baseColorFactor=(1,1,1) with the color in the diffuse *texture*; `ConvertMaterial` sees only the factor, so the derived specular is white instead of base-color-tinted. Correct handling needs the albedo texel (texture-aware / PBR shading), explicitly out of this story's scope (§C) — revisit as optional Epic-6 polish.

**Dismissed (noise / by-design):** (1) baseColorFactor >1 amplifies derived specular — cosmetic only, the fixed-point framebuffer saturates positive overshoot to white, and the pre-2.3 code wrote specular unclamped too. (2) shininess floor of 2.0 silently raises authored values in (0,2) — deliberate per Dev Notes §A. (3) `Get(AI_MATKEY_METALLIC_FACTOR)` return ignored — the 0 default *is* the correct dielectric fallback and assimp leaves the out-param untouched on failure; subsumed by the metallic-clamp patch. (4) Blind Hunter's claim that `std::clamp(NaN)` returns the low bound — incorrect; superseded by the Edge Case Hunter's correct NaN analysis (patch 1).

## Dev Notes

### Critical orientation — the render machinery already exists; this is a loader-values story

The expensive mistake here would be to "implement multi-material rendering" from scratch — **it is already implemented and working** (2.1 built the per-mesh/per-material draw loop, 2.2 added textures). Confirmed against the live tree:
- **Multiple materials already load:** [asset_loader.cpp:396-407](../../src/asset_loader.cpp#L396-L407) loops `for (mi = 0 … scene->mNumMaterials)` and pushes one `SceneMaterial` each.
- **Each mesh already maps to its material:** `PendingMesh::materialIdx = mesh->mMaterialIndex` ([asset_loader.cpp:265](../../src/asset_loader.cpp#L265)), clamped in-bounds at [asset_loader.cpp:413-415](../../src/asset_loader.cpp#L413-L415) (a 2.1 CRITICAL review patch — **do not remove or weaken it**).
- **The draw loop already binds per-material specular:** [renderer.cpp:241-244](../../src/renderer.cpp#L241-L244) sets `u_baseColor`/`u_specularColor`/`u_shininess` per mesh; the shader's `u_specularColor * spec` with `pow(dot(N,H), u_shininess)` ([renderer.cpp:58-63](../../src/renderer.cpp#L58-L63)) already produces a per-material Blinn-Phong highlight.

So the **only** behavioral defect is upstream of all that: `ConvertMaterial` writes a *flat* specular (`0.04` / clamped-to-32) for every glTF material, because assimp doesn't hand it a usable specular for metallic-roughness. Fix the values; everything downstream already works. As in 2.1/2.2, trust the live code over `architecture.md`'s stale names (`fbxav`→`rav`, sokol→raw GL, FBO/ReaImGui→direct render into the docked GL window); `src/` is flat.

### §A — The `ConvertMaterial` recipe (Task 1)

Replace the specular/shininess block in [asset_loader.cpp:71-83](../../src/asset_loader.cpp#L71-L83). Diffuse read and the `!mat` guard are unchanged:

```cpp
SceneMaterial ConvertMaterial(const aiMaterial* mat)
{
    SceneMaterial out;
    out.baseColorFactor = glm::vec3(0.8f);   // mid-grey if the file carries no diffuse
    out.specularColor   = glm::vec3(0.04f);  // dielectric F0 fallback
    out.shininess       = 32.0f;
    if (!mat) return out;                     // synthetic fallback material — all defaults

    aiColor3D c;
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, c) == AI_SUCCESS)
        out.baseColorFactor = glm::vec3(c.r, c.g, c.b);

    // assimp synthesizes SHININESS for every importer we ship: glTF metallic-roughness
    // from (1-roughness)^2*1000, glTF specular-glossiness from glossiness*1000, FBX/
    // Collada Phong from the authored exponent. A fully-rough glTF material yields 0,
    // so clamp to a small floor (a broad, present highlight) instead of snapping to 32.
    float s = 0.0f;
    if (mat->Get(AI_MATKEY_SHININESS, s) == AI_SUCCESS)
        out.shininess = std::clamp(s, 2.0f, 1000.0f);

    // Specular color. Honor an explicitly-authored specular first — that single key
    // covers FBX/Collada Phong, glTF KHR_materials_specular, and pbrSpecularGlossiness.
    // Otherwise (plain glTF metallic-roughness, where assimp leaves COLOR_SPECULAR
    // UNSET) derive it: a dielectric reflects a dim ~4% white highlight (F0=0.04), a
    // metal reflects its own base color — tint toward baseColor by metalness. Without
    // this, every metallic-roughness material shares one flat specular and matte vs
    // glossy is indistinguishable (FR16).
    aiColor3D spec;
    if (mat->Get(AI_MATKEY_COLOR_SPECULAR, spec) == AI_SUCCESS) {
        out.specularColor = glm::vec3(spec.r, spec.g, spec.b);
    } else {
        float metallic = 0.0f;
        mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic);   // leaves 0 (dielectric) if absent
        out.specularColor = glm::vec3(0.04f) + (out.baseColorFactor - glm::vec3(0.04f)) * metallic;
    }
    return out;
}
```

Add `#include <algorithm>` for `std::clamp` (or inline the clamp). `glm::vec3` arithmetic is already available via `scene.h`'s `<glm/glm.hpp>`. No other include changes.

### §B — What assimp actually hands `ConvertMaterial` (verified against the vendored v6.0.5 source)

From `build/_deps/assimp-src/code/AssetLib/glTF2/glTF2Importer.cpp`, the glTF2 importer's material conversion (per material):

| glTF input | assimp key it writes | What we do |
|---|---|---|
| `baseColorFactor` | `AI_MATKEY_COLOR_DIFFUSE` **and** `AI_MATKEY_BASE_COLOR` (lines 256-257) | read as `baseColorFactor` (already in 2.1) |
| `baseColorTexture` | `aiTextureType_DIFFUSE` (line 259) | resolved in 2.2 (`ResolveAndUploadDiffuse`) |
| `metallicFactor` | `AI_MATKEY_METALLIC_FACTOR` (line 267) | **new — tints specular** |
| `roughnessFactor` | `AI_MATKEY_ROUGHNESS_FACTOR` (268) **and** baked into `AI_MATKEY_SHININESS` = `(1-rough)²·1000` (270-272) | **new — read SHININESS** |
| `KHR_materials_specular` (if present) | `AI_MATKEY_COLOR_SPECULAR` (291) | honored by the explicit branch |
| `KHR_materials_pbrSpecularGlossiness` (if present) | `AI_MATKEY_COLOR_SPECULAR` (302) + `SHININESS`=`gloss·1000` (304-305) | honored by the explicit branch |

The decisive fact: for a **plain metallic-roughness** material (no specular extension — the overwhelmingly common glTF export), assimp writes **no** `AI_MATKEY_COLOR_SPECULAR`. So `mat->Get(AI_MATKEY_COLOR_SPECULAR, …)` returns non-`AI_SUCCESS` and we take the derive-from-metalness branch. FBX (`FBXConverter` sets Phong `COLOR_SPECULAR`/`SHININESS`) and Collada Phong both *do* set the key, so they take the honor-as-authored branch. This is why "prefer explicit, else derive" cleanly covers all three importers (AR5) with one conditional.

### §C — Why specular tinting, and why NOT auto-suppress diffuse (Task 3 note)

- The AC asks only that matte and glossy be **distinguishable**, not that the shading be physically energy-conserving. Tinting specular toward `baseColor` by metalness + the roughness-driven exponent already produces a visibly different highlight (tight+bright+colored for polished metal, broad+dim for matte leather) under the existing Blinn-Phong shader — sufficient for FR16/FR20.
- A "true metal" also kills diffuse (energy goes to specular). The tempting `baseColorFactor *= (1 - metallic)` is **deliberately omitted**: it would zero out a *textured* metal (where the factor is white `(1,1,1)` and the texture carries the color — the 2.2 shader does `base = u_baseColor; if (hasTexture) base *= texture`), turning textured metals black. Suppressing diffuse correctly needs a metalness uniform threaded into the shader so the suppression applies post-texture — that is a PBR-shader change, out of this story's scope (Blinn-Phong approximation only, architecture line 392). Leave diffuse as-authored; revisit as optional Epic-6 polish if the gate finds metals too bright.
- The exact constants (F0 `0.04`, clamp `2…1000`) are a visual judgment confirmable only on Antho's Windows gate. Ship the principled default and flag the knobs (gate §8 tuning note), exactly as 2.2 flagged the UV-flip it couldn't confirm on Linux.

### §D — Boundary discipline — unchanged from 2.2

`asset_loader.cpp` remains the only `<assimp/...>` TU; this story adds **no** new heavy include (only possibly `<algorithm>` for `std::clamp`). No new modern-GL call (no upload here — values only). No `console_log.h` need (deriving specular never fails — it is arithmetic on values already read; a material with neither specular nor metalness simply defaults to a dielectric, which is correct, not an error → no diagnostic). `reaper_api.h` unchanged. The renderer stays a sanctioned modern-GL TU with no new calls.

### Scope fences — what this story does NOT do

- **No real PBR BRDF / metallic-roughness lighting model** — stays the existing Blinn-Phong approximation (architecture line 392: "Blinn-Phong + diffuse texture + per-material specular"). We *approximate* metalness via specular tint + exponent, we do not implement Cook-Torrance/GGX.
- **No metalness/roughness/normal/emissive/occlusion texture maps** — the metallic-roughness *texture* (`aiTextureType_METALNESS`/`DIFFUSE_ROUGHNESS`) and other PBR maps are out of MVP. Only the scalar `metallicFactor`/`roughnessFactor` (→ shininess) and the diffuse texture (2.2) are used.
- **No diffuse suppression for metals / no energy conservation** — see §C.
- **No transparency / alpha / double-sided handling** — `AI_MATKEY_OPACITY`, `AI_MATKEY_GLTF_ALPHAMODE`, `AI_MATKEY_TWOSIDED` are read by assimp but ignored; opaque-only forward render (architecture line 386). Out of MVP.
- **No camera controls** — Story 2.4. Auto-fit framing from 2.1 stands.
- **No renderer / shader / scene.h / gl_loader.h / CMake change** — if you find yourself editing any of these, stop: the fix is values in `ConvertMaterial` (the one exception would be the optional metal-diffuse-suppression knob, which is explicitly deferred per §C).
- **No FBX/Collada specular *gating*** — FBX is Epic 6; the explicit-`COLOR_SPECULAR` branch is built to handle Phong specular when those formats arrive, but no FBX/Collada specular fixture is gated here (the gate uses glTF metallic-roughness fixtures).

### Testing standards

No automated test harness exists (architecture lines 933-935); validation is the **per-phase validator gate run by Antho in real Reaper** (AR19). Dev-side verification on this Linux/WSL box is limited to `cmake -S . -B /tmp/rav_build` configuring clean (the `else()` branch stubs the Windows target — the C++ compile, `/W3` cleanliness, single-DLL output, ≥60 fps, and **all** visual ACs are confirmed only on Antho's Windows gate) plus source greps (confirm only `asset_loader.cpp` changed; confirm the `materialIdx` clamp survives). Per project memory `feedback_trust_ingame_validation`: when Antho validates in-Reaper and it works, that IS the gate. The matte-vs-glossy and multi-material checks are inherently **visual** (Task 3 rows). Khronos **glTF-Sample-Assets** `MetalRoughSpheres` (matte↔glossy gradient in one frame) and a multi-material model (`FlightHelmet` / `BoomBox` / `DamagedHelmet`) are the recommended fixtures.

### Project Structure Notes

- **New files:** none. **Modified:**
  - `src/asset_loader.cpp` — `ConvertMaterial` specular/shininess derivation (Task 1); possibly `#include <algorithm>` for `std::clamp`.
  - `docs/PHASE1_VALIDATOR_GATE.md` — §8 Story 2.3 acceptance rows + specular-tuning note (Task 3).
  - `src/scene.h`, `src/renderer.cpp`, `src/renderer.h`, `src/gl_loader.h`, `CMakeLists.txt` — **no change** (the `specularColor`/`shininess` fields and their binding already exist; FR20's render path is already built).
- **Naming conventions** (architecture line 517-524): Types/functions **PascalCase**, locals **snake_case**, file-internal helpers in the anonymous namespace. SPDX header already on the file. **Comment WHY only** (the F0 0.04, the metal tint, the "assimp leaves COLOR_SPECULAR unset" rationale). No story/commit IDs in comments.

### Previous Story Intelligence (Stories 2.1 + 2.2 and their reviews)

- **The seam was left open for this exact story.** 2.1's `ConvertMaterial` already reads `AI_MATKEY_COLOR_SPECULAR`/`AI_MATKEY_SHININESS` and the draw loop already binds them — but with the `s >= 1.0f` guard that drops a rough glTF material to default 32, and with no metalness awareness. 2.2 left this untouched (it only added textures). This story completes the material-value derivation.
- **`materialIdx` clamp (2.1 CRITICAL patch):** `materials[mesh.materialIdx]` is guaranteed in-bounds by the load-time clamp + synthetic default. Multi-material rendering reads through that **same** index — no new bounds risk; **do not remove or weaken the clamp** ([asset_loader.cpp:406-415](../../src/asset_loader.cpp#L406-L415)).
- **Relaxed uniform-location rule (2.1 review):** only `u_mvp` is fatal on `-1`; `u_specularColor`/`u_shininess` may legitimately come back `-1` if a driver optimizes them out, and `glUniform*(-1,…)` is a documented no-op ([renderer.cpp:125-132](../../src/renderer.cpp#L125-L132)). This story changes no uniforms, so the rule is untouched — just don't reintroduce a fatal check.
- **Hot-path discipline (1.2 review, 10 findings):** zero heap alloc in `RenderFrame`. This story adds nothing to the render path. The new `ConvertMaterial` arithmetic is load-time only and allocates nothing.
- **No-throw boundary (AR18, 1.4 + 2.1 + 2.2):** the whole `LoadAsset` body is wrapped. The new code is `aiMaterial::Get` calls (return codes, no throw) + arithmetic — naturally no-throw; nothing new escapes.
- **Windows `.bat` (project memory):** CRLF, flat `errorlevel` gotos, `cd /d "%~dp0"`, close Reaper before copying the DLL. The untracked `build_spike.bat` / `build_forcefail.bat` must stay **out** of this story's commits. No new dependency → no build-time change to flag.

### Latest tech notes

- **assimp v6.0.5** (pinned, `extern/VENDORED.md`). Material keys used: `AI_MATKEY_COLOR_DIFFUSE`, `AI_MATKEY_COLOR_SPECULAR`, `AI_MATKEY_SHININESS`, `AI_MATKEY_METALLIC_FACTOR` — all stable, all in `<assimp/material.h>` (already included transitively via `<assimp/scene.h>`). The glTF2 importer's metallic-roughness → shininess/specular behavior was verified directly against the vendored source (Dev Notes §B), not assumed.
- **glTF 2.0 metallic-roughness vs Blinn-Phong:** glTF has no "specular color/shininess" — it has `metallicFactor`/`roughnessFactor`. The honest MVP move (per architecture line 392) is to *approximate* a Blinn-Phong specular from them, which is what §A does. A future PBR shader (post-MVP) would consume the factors directly; this story deliberately stays in the existing Blinn-Phong shader.
- **`metallicFactor` default** is `1.0` in the glTF spec, but assimp always writes the file's actual value (line 267), and our fallback for a *missing* key is `0.0` (dielectric) — the safe neutral when there is no PBR metalness at all (e.g. an FBX that also lacks `COLOR_SPECULAR`).

### References

- Story + ACs: [epics.md#Story 2.3](../planning-artifacts/epics.md) (lines 369–380); Epic 2 intro (336–338); FR16/FR20 map (lines 187, 191).
- AR5 importer narrowing (glTF/FBX/Collada only): [epics.md] line 138. AR7 POD scene model: 143. AR9 single convert boundary: 145. AR16 console-only / AR18 no-throw: 155, 157. AR19 gate: 161.
- D1 `SceneMaterial { … vec3 specularColor; float shininess; }`: [architecture.md] line 195. Render pipeline / "Shader (Phase 1) = Blinn-Phong + diffuse texture + per-material specular (Per PRD FR16)": [architecture.md] lines 384-392. D14 camera (Story 2.4): 409.
- Live code (the values to change / the path that already works): [asset_loader.cpp:67-84](../../src/asset_loader.cpp#L67-L84) (`ConvertMaterial` — the only edit), [asset_loader.cpp:396-415](../../src/asset_loader.cpp#L396-L415) (material loop + clamp — unchanged), [renderer.cpp:40-66](../../src/renderer.cpp#L40-L66) (shader — unchanged), [renderer.cpp:241-255](../../src/renderer.cpp#L241-L255) (per-mesh material bind — unchanged), [scene.h:43-48](../../src/scene.h#L43-L48) (`SceneMaterial` fields — unchanged).
- assimp behavior verified: `build/_deps/assimp-src/code/AssetLib/glTF2/glTF2Importer.cpp` lines 256-311 (metallic-roughness → COLOR_DIFFUSE/SHININESS, COLOR_SPECULAR only on KHR_materials_specular / pbrSpecularGlossiness).
- Prior stories (seam, review patches, gate): [2-1-load-and-display-a-static-gltf-glb-mesh.md](2-1-load-and-display-a-static-gltf-glb-mesh.md), [2-2-diffuse-texture-resolution-across-glb-embedded-and-gltf-sibling-sources.md](2-2-diffuse-texture-resolution-across-glb-embedded-and-gltf-sibling-sources.md). Gate to extend: [docs/PHASE1_VALIDATOR_GATE.md](../../docs/PHASE1_VALIDATOR_GATE.md).

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Opus 4.8, 1M context)

### Debug Log References

- `git diff --name-only -- src/ CMakeLists.txt` → only `src/asset_loader.cpp` (renderer/scene/gl_loader/CMake diff empty, confirming Task 2's no-change scope and AC5).
- `cmake -S . -B /tmp/rav_build` → "non-Windows platform detected … Build target stubbed", configures done with 0 errors. As designed, the C++ compile / `/W3 /permissive-` cleanliness / single-DLL / ≥60 fps / all visual ACs are confirmed only on Antho's Windows gate (AR19); the Linux box only verifies the configure + source greps.

### Completion Notes List

- **Task 1 (the whole story):** rewrote the specular/shininess block in `ConvertMaterial` ([asset_loader.cpp:67-106](../../src/asset_loader.cpp#L67-L106)). Diffuse read and the `!mat` synthetic-default guard are unchanged. Shininess now clamps `AI_MATKEY_SHININESS` to `2…1000` (removing 2.1's `s >= 1.0f` guard that snapped a fully-rough glTF material — which yields 0 — back to 32). Specular color now prefers an explicitly-authored `AI_MATKEY_COLOR_SPECULAR` (covers FBX/Collada Phong, glTF `KHR_materials_specular`, `pbrSpecularGlossiness`) and otherwise derives it from `AI_MATKEY_METALLIC_FACTOR` as `mix(vec3(0.04), baseColorFactor, metallic)` — dielectric F0 0.04 → base-color metal. Pure arithmetic on values already read: allocation-free and no-throw (AC4/AR18). Added `#include <algorithm>` for `std::clamp`. WHY-comments added (F0 0.04, metal tint, "assimp leaves COLOR_SPECULAR unset for metallic-roughness").
- **Task 2 (verify-only, no edits):** confirmed by reading that the FR20 render path is intact — `WalkBake` emits one `PendingMesh` per (node,mesh) carrying `mMaterialIndex` ([asset_loader.cpp:256-270](../../src/asset_loader.cpp#L256-L270)), the material loop converts every `scene->mNumMaterials` and the out-of-range `materialIdx` clamp survives ([asset_loader.cpp:397-415](../../src/asset_loader.cpp#L397-L415)), the per-material shader binds `u_specularColor`/`u_shininess` ([renderer.cpp:45-63](../../src/renderer.cpp#L45-L63)), and `SceneMaterial { specularColor, shininess }` is unchanged ([scene.h:43-48](../../src/scene.h#L43-L48)). `git diff` confirms renderer/scene/gl_loader/CMake are byte-for-byte unchanged.
- **Task 3:** appended "## 8. Story 2.3 — acceptance checks" (rows 15–18) + a specular-tuning note (F0 0.04, clamp 2…1000, why no diffuse suppression by default) to [docs/PHASE1_VALIDATOR_GATE.md](../../docs/PHASE1_VALIDATOR_GATE.md), continuing the numbering after 2.2's row 14.
- **Untracked `build_spike.bat` / `build_forcefail.bat` are intentionally kept out of this story's scope** (per Story 2.1/2.2 intelligence — not part of this change).

### File List

- `src/asset_loader.cpp` — `ConvertMaterial` specular/shininess derivation (Task 1) + `#include <algorithm>` for `std::clamp`.
- `docs/PHASE1_VALIDATOR_GATE.md` — §8 Story 2.3 acceptance rows (15–18) + specular-tuning note (Task 3).
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — story 2-3 status tracking (ready-for-dev → in-progress → review).

## Change Log

| Date | Change |
|---|---|
| 2026-06-24 | Story 2.3 implemented — per-material specular derivation in `ConvertMaterial` (metalness-tinted color + roughness-clamped shininess, honoring explicit Phong/KHR specular). Verified FR20 render path unchanged. Gate §8 rows 15–18 added. Status → review. |
| 2026-06-24 | Code-review (BMAD 3-layer) — Acceptance Auditor PASS. 2 patches applied to `ConvertMaterial`: NaN-safe shininess clamp (`std::isfinite` gate, restoring the implicit NaN rejection the §A refactor dropped) + `metallic` clamped to [0,1] before the specular mix. 1 deferred (white specular on textured colored metal → Epic-6 PBR polish), 4 dismissed. Hardening on malformed-input/cold paths; Antho's gate unaffected. Status stays → done. |
