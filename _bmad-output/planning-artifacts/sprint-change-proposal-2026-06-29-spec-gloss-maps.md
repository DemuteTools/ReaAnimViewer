# Sprint Change Proposal — Per-pixel specular + glossiness maps (artist intent)

- **Date:** 2026-06-29
- **Author:** Amelia (dev) with Antho
- **Trigger source:** in-Reaper observation on a Mixamo FBX character + offline assimp investigation
- **Change scope:** **Moderate** (new story in Epic 6.5; backlog reorg; routes to create-story → dev-story)
- **Mode:** Batch

## 1. Issue Summary

On multi-material **Mixamo FBX characters**, the head/body boundary shows a **pronounced shading seam** that is far more visible than in Mixamo's own viewer.

**Investigation (offline assimp probe on `Reaper/Media Files/Catwalk Walk Turn 180 Tight.fbx`, using the loader's exact import flags):**
- The character is **two meshes / two materials** that meet at the neck: body (`Ch30_Body1`, 15 473 v → `Ch30_1002_*` textures) and head (`Ch30_Body`, 4 433 v → `Ch30_1001_*` textures).
- **Every material carries a full set**: DIFFUSE, **SPECULAR** (`*_Specular.png`), **SHININESS = glossiness** (`*_Glossiness.png`), and NORMALS — all 8 textures embedded and resolving correctly.
- The two diffuse textures are near-identical in tone (avg (121,83,81) vs (128,88,86)); both normal maps are standard tangent-space. **So the seam is not a missing/mismatched texture.**

**Root cause:** our renderer (Story 6.5.1) applies a **uniform, derived dielectric specular** to the whole surface and **ignores the artist's per-pixel specular + glossiness maps**. Mixamo uses those maps — they say *where* skin is oily/shiny vs matte — which is exactly what makes skin read naturally and **hides the two-material seam**. Antho confirmed the seam is barely-there in Mixamo and that our aggressive uniform sheen amplifies it.

**Why we ignore them today (context, not a bug):** Story 6.5.1 deliberately stopped honoring the **flat authored `COLOR_SPECULAR`** because Mixamo's grey/white Phong specular made skin read **plastic** (`asset_loader.cpp:442`). That fix was correct. But it threw out the *per-pixel maps* along with the flat value, and the per-pixel maps were never wired in (deferred as post-MVP PBR — see `deferred-work.md`).

## 2. Impact Analysis

- **Epic impact:** Epic 6.5 (pre-ship polish). Adds **Story 6.5.7**. No other epic affected. Epic 7 (ReaPack) unchanged.
- **Story impact:** Enhances Story 6.5.1 (source-fidelity rendering) and Story 2.3 (per-material specular). **Orthogonal** to 6.5.6 (MSAA) — no conflict. The live light-tool knobs (Ambient / Specular / Relief, 6.5.x) remain and still apply on top.
- **FR impact:** **Enhances FR48** (Mixamo parity / dielectric-correct specular) and FR16 (per-material specular). **No new FR** — this is a fidelity refinement of FR48.
- **Architecture impact:** Refines the 6.5.1 decision in the Spec Change Log — *still* ignore the flat `COLOR_SPECULAR`, but now **consume the per-pixel SPECULAR + GLOSSINESS maps**. AR20 entry authored at dev time. No change to D-decisions or AR invariants.
- **Technical impact (expected scope, AR15-clean like 6.5.1):**
  - `scene.h` — `SceneMaterial` gains `specularMap` + `glossMap` (GpuImage handles; presence = the per-pixel path is active, mirroring `normalMap`).
  - `asset_loader.cpp` — resolve + upload `aiTextureType_SPECULAR` and `aiTextureType_SHININESS` (glossiness) through the existing AR14 funnel, **LINEAR** (`GL_RGBA8`, data not colour — like the normal map). Activated **only when present** (glTF metallic-roughness has no SPECULAR/SHININESS slots → unchanged).
  - `renderer.{cpp,h}` — bind the two maps to texture units 2 & 3; shader modulates the specular term **per-pixel**: the specular map scales the lobe **intensity** (keeping the dielectric base — NOT re-introducing the plastic flat value), the glossiness map drives the **shininess exponent** per-pixel. New `u_*` uniforms + `u_hasSpecularMap` / `u_hasGlossMap` gates (non-fatal at -1).
  - `gl_loader.h` — likely **no** new entry points (texture funcs already resolved); only possibly nothing.
  - **No** `CMakeLists.txt` / `plugin_main` / `reaper_api` / `pcm_source` / `asset` registration change; **no** `rec->Register`, **no** new `REAPERAPI_WANT_*`. Session-only; D9 / `pcm_source_anim.cpp` untouched.
  - D2 zero-alloc per-frame preserved (maps uploaded at load, cold path). NFR-P1 ≥60 fps.

**Risks & mitigations:**
- *Re-introducing "too plastic"* → mitigated: we modulate a dielectric base by the maps (the maps reduce sheen where skin is matte); we do **not** restore the flat `COLOR_SPECULAR`. The live Specular knob still lets Antho tame it.
- *Breaking the glTF path* → mitigated: maps activate only when the material provides SPECULAR/SHININESS textures (glTF metallic-roughness doesn't), exactly like the normal-map gate — glTF renders byte-for-byte as today.
- *Touches the 6.5.1-gate-validated shader* → in-Reaper gate **§10** required (AR19); self-reviewed on Linux (can't build `_WIN32`).

## 3. Recommended Approach

**Direct Adjustment** — add **Story 6.5.7** to Epic 6.5 as the final pre-ship fidelity story, then run **create-story** (context-rich spec) → **dev-story** → Antho in-Reaper gate §10. Same rhythm as Story 6.5.6.

Rationale: contained, additive, no rollback, no MVP-scope change; it closes a real fidelity gap (artist intent) before the ReaPack release and is the natural completion of FR48.

## 4. Detailed Change Proposals

### 4.1 `epics.md` — add Story 6.5.7 (after Story 6.5.6)

```
### Story 6.5.7: Per-pixel specular + glossiness maps (artist material intent) *(added 2026-06-29 — Correct Course)*

As a sound designer,
I want the viewer to use the specular/glossiness maps the artist authored,
So that skin and cloth read like the source DCC (Mixamo) and material seams don't pop.

**Acceptance Criteria:**

**Given** a model whose materials carry a specular map (aiTextureType_SPECULAR) and/or a glossiness map (aiTextureType_SHININESS) — e.g. a Mixamo FBX
**When** it renders
**Then** the specular reflection is modulated PER-PIXEL by those maps (intensity from the specular map, sharpness/exponent from the glossiness map) instead of a uniform sheen — so oily/shiny zones and matte zones differ as authored, and a multi-material seam (e.g. head/body) reads continuous (enhances FR48/FR16)
**And** the flat authored COLOR_SPECULAR is STILL ignored (the "too plastic" 6.5.1 fix stands) — only the per-pixel maps are used; the specular base stays dielectric
**And** a model WITHOUT these maps (e.g. glTF metallic-roughness) renders byte-for-byte as before — the maps activate only when present, like the normal map (AC4 of 6.5.1)
**And** the change is GL-boundary-only and non-fatal: a map that fails to resolve falls back to the current uniform specular (AR17); D2 zero-alloc per-frame; session-only; AR15 register-symmetry intact

*(Pulls the post-MVP "use glossiness/specular maps" item forward — see deferred-work.md. Maps uploaded LINEAR (GL_RGBA8, data not colour). The live light-tool Specular/Relief/Ambient knobs still apply on top. Validation = Antho's in-Reaper Windows gate §10, AR19.)*
```

### 4.2 `epics.md` — update the Epic 6.5 "FRs covered" note (line 267)

Append: `FR48 enhanced 2026-06-29 by Story 6.5.7 — per-pixel specular + glossiness maps (artist material intent).`

### 4.3 `sprint-status.yaml` — add the story to backlog

```
  6-5-7-per-pixel-specular-glossiness-maps: backlog  # 2026-06-29 Correct Course (sprint-change-proposal-2026-06-29-spec-gloss-maps.md) — use the artist's per-pixel SPECULAR + GLOSSINESS maps (aiTextureType_SPECULAR / SHININESS) to modulate specular per-pixel instead of a uniform derived sheen; fixes the pronounced head/body seam on multi-material Mixamo FBX (Catwalk). Keeps ignoring the flat COLOR_SPECULAR (6.5.1 "too plastic" fix). Maps activate only when present (glTF metallic-roughness unchanged), uploaded LINEAR. Enhances FR48/FR16. Scope: scene.h + asset_loader.cpp + renderer.{h,cpp} (AR15-clean, like 6.5.1). Gate §10 PENDING. Depends 6.5.1.
```

### 4.4 `deferred-work.md` — annotate the deferral as pulled forward

Mark the post-MVP "respect glossiness/specular maps" line as **pulled forward into Story 6.5.7 (2026-06-29)**.

### 4.5 `architecture.md` — AR20 Spec Change Log

Authored at **dev time** (mirrors 6.5.6): a "Per-pixel specular + glossiness maps" entry refining the 6.5.1 specular decision.

## 5. Implementation Handoff

- **Scope:** Moderate.
- **Route:** create-story → dev-story (Amelia), then Antho in-Reaper Windows gate §10 (AR19).
- **Success criteria:** Mixamo FBX skin reads like Mixamo (oily/matte zones differ, head/body seam continuous); glTF models unchanged; ≥60 fps; AR15-clean; no host instability.
