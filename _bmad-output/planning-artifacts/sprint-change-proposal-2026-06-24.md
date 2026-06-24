# Sprint Change Proposal — Add Collada (.dae) as a supported animation format

**Date:** 2026-06-24
**Author:** Antho (validator) + Dev (Claude)
**Trigger context:** Start of Epic 2 (Phase 1). No story in flight; Epic 2 is `backlog`.
**Scope classification:** **Moderate** — additive spec change across PRD + Architecture + Epics + several story ACs + sprint-status. No rollback, no new epic, no resequencing, no invalidation of completed work (Epic 1 / Phase 0.5 is done and untouched).

---

## Section 1 — Issue Summary

While preparing to start Epic 2, Antho observed that the supported-format set is narrow (glTF/GLB primary, FBX secondary) and asked for compatibility with **a maximum of standard formats**.

After scoping, the realistic answer for an **animation** viewer is bounded: only three interchange formats carry skeletal animation that this tool renders — **glTF/GLB**, **FBX**, and **Collada (`.dae`)**. Static-only formats (OBJ, STL, 3DS, PLY) were explicitly **rejected** by Antho: he does not want the tool to present a frozen mesh with no animation, which would mislead users of an animation tool.

**Decision (Antho, 2026-06-24):** Add **Collada (`.dae`)** as a first-class animation format. Do not enable static-only importers.

**Why this is cheap and low-risk:**
- The loader already funnels every file through **assimp** (AR5/D15). Enabling Collada is primarily a **CMake flag** (`ASSIMP_BUILD_COLLADA_IMPORTER=ON`), not a new code path.
- Coordinate-convention tolerance is **already designed in**: AR9/AR13 render as-authored and tolerate up-axis/unit differences **by camera, not by remapping**. Collada's `<up_axis>` (often Z-up) is handled by the same mechanism as a Z-up FBX — **no new code**.
- The failure-isolation net (FR34 / Story 6.4) already guarantees a malformed `.dae` degrades gracefully rather than crashing the host.

**Real costs (honest):**
- Slightly larger binary + wider assimp attack surface (the original reason AR5 narrowed to 2 importers). One extra parser; acceptable.
- Validation: Demute clients export **FBX/glTF, not Collada**, so there is **no Demute `.dae` corpus** to set a percentage target against. Collada is therefore validated **best-effort** against ≥1 public Collada sample with animation (Khronos/assimp test asset), with graceful degradation — **not** gated on a Demute-percentage target like FBX's 60%.

---

## Section 2 — Impact Analysis

### Epic Impact
- **Epic 2** (start point): enable the Collada importer in the assimp build; a static `.dae` loads and renders via the same path as glTF. **Story 2.1 AC** changes (importer set).
- **Epic 3** (animation): Collada animation flows through the existing assimp→Asset→TRS-sampling path. No structural change; `.dae` joins the formats exercised.
- **Epic 4** (transport): drag-drop and PCM_source accept `.dae`. **Stories 4.1, 4.2** extension lists change.
- **Epic 5** (browser): browser format filter adds `.dae`. **Story 5.1 AC (FR43)** changes.
- **Epic 6** (reliability): **Story 6.5** extends from "FBX support" to "FBX + Collada support" — FBX keeps its 60% Demute-corpus target; Collada is best-effort against a public sample.
- **Epics 1 & 7:** no change.

### Artifact Conflicts
- **PRD:** add a Collada load FR; extend the drag-drop format list (FR8) and browser filter (FR43); add a Collada compatibility NFR; update the "key requirements" and compatibility narrative lines that enumerate formats. (Product name in the PRD is still the historical "FBXAnimationViewer" — out of scope here; the rename to ReaAnimViewer is already shipped in code via Story 1.1 and the planning artifacts may retain historical names.)
- **Architecture:** add Collada to the assimp narrowing block (AR5/D15) and CMake snippet; add Collada to the format-support summary and dependency table; note that AR9/AR13 already cover Collada's up-axis with no new mechanism.
- **Epics doc:** Requirements Inventory (FR list + new FR), FR Coverage Map, AR5 text, Epic 2 description + Story 2.1, Stories 4.1/4.2, Story 5.1, Story 6.5.
- **sprint-status.yaml:** rename the Story 6.5 key to reflect FBX **+ Collada**. No new stories, no status changes (everything from Epic 2 onward is still `backlog`).

### Technical Impact
- Single new CMake importer flag; one extra parser statically linked.
- No new rendering, skinning, camera, persistence, or transport code is required by this change — Collada reuses every existing path.

---

## Section 3 — Recommended Approach

**Option 1 — Direct Adjustment (SELECTED).** Modify existing stories/specs in place; add one FR and one NFR. **Effort: Low. Risk: Low.**

Rejected alternatives:
- **Rollback:** N/A — no completed work conflicts.
- **MVP Review / broaden to all assimp formats:** rejected by Antho — dishonest guarantees and pointless static-only formats for an animation tool.

**Tiered guarantee model (the honest contract):**
| Tier | Formats | Promise |
|------|---------|---------|
| 1 — validated | glTF / GLB | Primary. ≥80% on Demute fixtures. |
| 2 — target % | FBX | ≥60% on the Demute FBX corpus (Epic 6). |
| 2 — best-effort | Collada `.dae` | Loads + renders; validated against a public sample; degrades gracefully. No Demute-% target (no `.dae` corpus). |

---

## Section 4 — Detailed Change Proposals

### PRD (`prd.md`)

**Add FR47 (after FR3):**
> **FR47:** The sound designer can load animation files in Collada (`.dae`) format (best-effort via assimp; renders as-authored, degrades gracefully). Collada textures (external sibling files or embedded) resolve through the same unified texture-resolution path as glTF.

**FR8 — OLD:** `drag an animation file (.glb, .gltf, .fbx)` → **NEW:** `(.glb, .gltf, .fbx, .dae)`

**FR43 — OLD:** `filter the browser to supported animation formats (.glb, .gltf, .fbx)` → **NEW:** `(.glb, .gltf, .fbx, .dae)`

**Add NFR-C6 (after NFR-C5):**
> **NFR-C6:** Collada (`.dae`) support follows assimp's parser capabilities for the pinned assimp version. Validated best-effort against at least one public Collada animation sample; no Demute-percentage target applies (Demute does not export Collada). Malformed `.dae` degrades gracefully per FR34.

**Compatibility line (~L103):** append `; Collada .dae best-effort (no Demute corpus).`
**Key requirements (~L138):** `glTF/GLB primary, FBX secondary, Collada .dae best-effort (all via assimp).`
**Format enumerations (~L141, L231, L280):** add `.dae` to the `.glb/.gltf/.fbx` lists.

### Architecture (`architecture.md`)

**assimp narrowing (AR5 / D15, ~L139, L437-444):**
- CMake: add `set(ASSIMP_BUILD_COLLADA_IMPORTER ON CACHE BOOL "" FORCE)`
- Text: "only glTF + FBX importers" → "only glTF + FBX + Collada importers"
- Deps table (~L134): "importers narrowed to glTF + FBX" → "...glTF + FBX + Collada"

**Format-support summary (~L41):** add Collada to the FR1–FR7 description.

**Coordinate note (AR9/AR13):** add: *Collada's `<up_axis>` (commonly Z-up) is handled by the existing camera-tolerance mechanism (AR13) with no new code — identical to a Z-up FBX.*

### Epics (`epics.md`)

- **Requirements Inventory:** add FR47 under "Animation Loading & Format Support"; update FR8 and FR43 enumerations.
- **FR Coverage Map:** add `FR47: Epic 2 — load Collada .dae (best-effort)`; FBX corpus validation of `.dae` noted under Epic 6.
- **AR5 text:** add COLLADA importer.
- **Epic 2 description + Story 2.1 AC:** "assimp built with only glTF and FBX importers" → "...glTF, FBX, and Collada importers"; add that a static `.dae` loads and renders as-authored.
- **Stories 4.1, 4.2:** add `.dae` to the bound/droppable path list.
- **Story 5.1 AC:** add `.dae` to the browser filter.
- **Story 6.5:** retitle to "FBX & Collada support validated"; FBX keeps 60% Demute target, Collada validated best-effort against a public sample + graceful degradation.

### sprint-status.yaml
- Rename key `6-5-fbx-support-validated-to-the-60-demute-target` → `6-5-fbx-and-collada-support-validated`. Status stays `backlog`.

---

## Section 5 — Implementation Handoff

**Scope:** Moderate → spec/backlog edits, executed directly by the Dev agent in this session (no PM/Architect escalation needed — additive, no architectural conflict).

**Success criteria:**
- All four artifacts consistently list `.dae` wherever the format set is enumerated.
- AR5/D15 + CMake enable the Collada importer.
- The FBX/Collada validation distinction (60% corpus vs best-effort sample) is recorded so Epic 6 implements the right gate.
- sprint-status.yaml key renamed; no status regressions.

**Next action after approval:** apply the edits above, then Epic 2 / Story 2.1 can be created with the corrected importer scope.
