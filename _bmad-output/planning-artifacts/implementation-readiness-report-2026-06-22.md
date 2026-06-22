---
stepsCompleted: ['step-01-document-discovery', 'step-02-prd-analysis', 'step-03-epic-coverage-validation', 'step-04-ux-alignment', 'step-05-epic-quality-review', 'step-06-final-assessment']
documentsIncluded:
  - prd.md
  - architecture.md
  - epics.md
  - prfaq-FBXAnimationViewer.md
---

# Implementation Readiness Assessment Report

**Date:** 2026-06-22
**Project:** FBXAnimationViewer

## Document Inventory

| Type | File | Size | Modified | Format |
|------|------|------|----------|--------|
| PRD | `prd.md` | 42.8 KB | 2026-05-11 | Whole |
| Architecture | `architecture.md` | 71.5 KB | 2026-05-12 | Whole |
| Epics & Stories | `epics.md` | 44.0 KB | 2026-06-22 | Whole |
| PRFAQ (context) | `prfaq-FBXAnimationViewer.md` | 17.6 KB | 2026-05-09 | Whole |

**Duplicates:** None.
**Missing:** No dedicated UX document found (UX likely embedded in PRD/Architecture — to confirm during analysis).

## PRD Analysis

### Functional Requirements (41 total; FR21 recategorized to NFR-P1)

**Animation Loading & Format Support**
- FR1: Load `.glb` (glTF 2.0 binary).
- FR2: Load multi-file `.gltf` + `.bin` + textures.
- FR3: Load FBX, ≥60% of common Demute exports rendering correctly.
- FR4: Load files with non-canonical coordinate conventions (Y/Z-up, m/cm) without per-file pre-config.
- FR5: Load files using any bone-naming convention (incl. non-English).
- FR6: Render static mesh when file has no animation channels.
- FR7: Sample TRS animation channels per bone, incl. root motion.

**Timeline Integration**
- FR8: Drag animation file onto a Reaper track → media item bound to that animation.
- FR9: Created item length = animation duration.
- FR10: Map playhead to anim time via item-relative offset (clamped).
- FR11: Position/move/resize/color/rename items via native Reaper controls.
- FR12: Coexist with audio/video/MIDI without interfering.
- FR13: Support multiple animation items across tracks in one project.
- FR14: Determine displayed animation from item spanning playhead; highest-priority track wins on overlap.

**3D Rendering & Visual Fidelity**
- FR15: Render skinned mesh with per-frame bone deformation.
- FR16: Diffuse-texture shading + per-material specular (distinguish material types).
- FR17: Resolve textures embedded in GLB.
- FR18: Resolve external sibling textures in multi-file glTF.
- FR19: Resolve textures embedded in FBX.
- FR20: Render multi-material meshes, each with own parameters.
- FR21: *(recategorized → NFR-P1)*

**Camera & Viewport Control**
- FR22: Orbit (right-click drag).
- FR23: Zoom (scroll).
- FR24: Pan (middle-click drag).
- FR25: Reset camera to bounding-box framing via toolbar button.
- FR26: Continue updating animation frame during camera manipulation.

**Panel & Docking**
- FR27: Preview inside a ReaImGui-driven panel.
- FR28: Dock panel into any Reaper docker (incl. floating).
- FR29: Register Reaper Action `FBXAV: Open Viewer Window`.
- FR30: Detect missing ReaImGui at load; emit diagnostic; fail gracefully without registering Action.

**Project State Persistence**
- FR31: Serialize per-item state (file path, camera angle, time offset/scale) into project file.
- FR32: Serialize panel dock position (delegated to ReaImGui).
- FR33: Restore item binding + viewport state on project reopen without manual reconfig.

**Error Tolerance & Graceful Degradation**
- FR34: Don't crash host on malformed/unsupported/partial file; render best-effort.
- FR35: Emit console diagnostic on load failure (path + error category).
- FR36: Manual "Reload" button picks up on-disk changes.
- FR37: Continue operating after a single item fails to load.

**Distribution & Operation**
- FR38: One-click ReaPack install from Demute repo, auto-installs `cfillion/reaimgui`.
- FR39: Manual install by copying DLL to UserPlugins (ReaImGui pre-installed).
- FR40: Fully offline (no network/telemetry/license/remote assets).
- FR41: Updates via ReaPack; no in-extension update mechanism.

### Non-Functional Requirements (16 total)

**Performance**
- NFR-P1: ≥60 fps on reference workstation for ~20k tris / 4 materials / 50 bones.
- NFR-P2: Initial load of typical fixture < 2 s (drop → first frame).
- NFR-P3: Manual reload < 1 s (click → first updated frame).
- NFR-P4: DLL loads at Reaper startup < 2 s.
- NFR-P5: Scrubbing latency ≤ 1 frame (~16 ms) under normal load.
- NFR-P6 (capacity): ≥10 simultaneous animation items without measurable degradation; up to ~50k tris render but may drop fps.

**Reliability**
- NFR-R1: Zero host crashes attributable to the extension (Phase 4 onward).
- NFR-R2: No project data loss / corruption / dropped tracks / invalid item refs.
- NFR-R3: Clean unload (`rec == nullptr`): no leaked GL contexts/window classes/API pointers/timers; symmetric (de)registration.
- NFR-R4: Load failure non-lethal: host up, other items keep working, diagnostic emitted.
- NFR-R5: Warning-free at MSVC `/W3 /permissive-`; new warnings block the phase gate.

**Compatibility & Integration**
- NFR-C1: Targets Reaper 7.x, SDK `caller_version` 0x20E; older builds fail cleanly with `return 0`.
- NFR-C2: Declares `cfillion/reaimgui` as auto-install ReaPack dependency.
- NFR-C3: Windows MSVC x64 ABI exclusively (no mingw/clang-cl/32-bit).
- NFR-C4: glTF conformance follows pinned assimp commit (re-vendoring → re-validation).
- NFR-C5: FBX support follows pinned assimp; 60% target applies to representative Demute sample.

**Deliberately out of scope:** Security, Scalability (beyond NFR-P6 item count), Accessibility (deferred post-MVP, inherits ReaImGui defaults).

### Additional Requirements / Constraints
- C++17, MSVC ABI on Windows; no exceptions in plugin entry path.
- Single-threaded UI + single-threaded GL (ReaImGui callbacks on Reaper main thread; sokol_gfx GL context same thread). Worker threads out of MVP scope.
- Phased delivery: Phase 0 ✓ (scaffolding, shipped 2026-05-09) → 0.5 (ReaImGui refactor) → 1 (static textured rendering) → 2 (skinned animation, dominant risk) → 3 (transport sync via PCM_source) → 4 (reload + camera persistence) → 5 (polish + ReaPack release).
- Documented scope-cut hierarchy if schedule slips (FBX Phase 4 → file-watcher → camera persistence → PCM_source SaveState).

### PRD Completeness Assessment
Strong PRD: requirements are numbered, atomic, and largely testable. Each FR is traceable to a journey/capability table. NFRs have explicit measurable targets and named out-of-scope categories. **No dedicated UX document** — UX is embedded as user journeys + capability tables + camera/panel FRs (FR22–FR33), adequate for a single-panel developer tool. Minor traceability watch-items to verify against epics: graceful-degradation NFRs (NFR-R1/R3/R4) span "all phases" rather than a single epic; FBX (FR3/FR19) is a documented scope-cut candidate.

## Epic Coverage Validation

The epics document maintains its own **FR Coverage Map** (epics.md §"FR Coverage Map") and a full Requirements Inventory. Validation below cross-checks both the PRD→epics direction (gaps) and the epics→PRD direction (orphans).

### Coverage Matrix (PRD FRs → Epic/Story)

| FR | Epic | Story | Status |
|----|------|-------|--------|
| FR1 | Epic 2 | 2.1 | ✓ Covered |
| FR2 | Epic 2 | 2.1 | ✓ Covered |
| FR3 | Epic 6 | 6.5 | ✓ Covered |
| FR4 | Epic 2 | 2.1 | ✓ Covered |
| FR5 | Epic 2 | 2.1 | ✓ Covered |
| FR6 | Epic 2 | 2.1 | ✓ Covered |
| FR7 | Epic 3 | 3.2 | ✓ Covered |
| FR8 | Epic 4 | 4.2 | ✓ Covered |
| FR9 | Epic 4 | 4.2 | ✓ Covered |
| FR10 | Epic 4 | 4.3 | ✓ Covered |
| FR11 | Epic 4 | 4.4 | ✓ Covered |
| FR12 | Epic 4 | 4.4 | ✓ Covered |
| FR13 | Epic 4 | 4.5 | ✓ Covered |
| FR14 | Epic 4 | 4.5 | ✓ Covered |
| FR15 | Epic 3 | 3.3 | ✓ Covered |
| FR16 | Epic 2 | 2.3 | ✓ Covered |
| FR17 | Epic 2 | 2.2 | ✓ Covered |
| FR18 | Epic 2 | 2.2 | ✓ Covered |
| FR19 | Epic 6 | 6.5 | ✓ Covered |
| FR20 | Epic 2 | 2.3 | ✓ Covered |
| FR21 | — | — | ↪ Recategorized → NFR-P1 |
| FR22 | Epic 2 | 2.4 | ✓ Covered |
| FR23 | Epic 2 | 2.4 | ✓ Covered |
| FR24 | Epic 2 | 2.4 | ✓ Covered |
| FR25 | Epic 2 | 2.4 | ✓ Covered |
| FR26 | Epic 2 | 2.4 | ✓ Covered |
| FR27 | Epic 1 | 1.3 | ✓ Covered |
| FR28 | Epic 1 | 1.3 | ✓ Covered |
| FR29 | Epic 1 | 1.3 | ✓ Covered |
| FR30 | Epic 1 | 1.4 | ✓ Covered |
| FR31 | Epic 6 | 6.1 | ✓ Covered |
| FR32 | Epic 6 | 6.1 | ✓ Covered |
| FR33 | Epic 6 | 6.1 | ✓ Covered |
| FR34 | Epic 6 | 6.4 | ✓ Covered |
| FR35 | Epic 6 | 6.4 | ✓ Covered |
| FR36 | Epic 6 | 6.3 | ✓ Covered |
| FR37 | Epic 6 | 6.4 | ✓ Covered |
| FR38 | Epic 7 | 7.1 | ✓ Covered |
| FR39 | Epic 7 | 7.2 | ✓ Covered |
| FR40 | Epic 7 | 7.3 | ✓ Covered |
| FR41 | Epic 7 | 7.3 | ✓ Covered |

### Orphan FRs (in Epics but NOT in PRD)

| FR | Epic | Story | Issue |
|----|------|-------|-------|
| FR42 | Epic 5 | 5.1 | ⚠️ Not in PRD — Animation Browser (open in-Reaper browser) |
| FR43 | Epic 5 | 5.1 | ⚠️ Not in PRD — filter browser to `.glb/.gltf/.fbx` |
| FR44 | Epic 5 | 5.2 | ⚠️ Not in PRD — preview from browser before placing |
| FR45 | Epic 5 | 5.3 | ⚠️ Not in PRD — place browsed animation onto track |
| FR46 | Epic 6 | 6.2 | ⚠️ Not in PRD — cross-machine path portability |

### Missing Requirements (in PRD but NOT in Epics)

**None.** Every PRD functional requirement (FR1–FR41, FR21 recategorized) maps to an epic AND to a concrete story with acceptance criteria. NFRs P1–P6, R1–R5, C1–C5 are referenced as acceptance criteria within the relevant stories rather than owned as standalone items (appropriate for cross-cutting quality attributes).

### Coverage Statistics

- Total PRD FRs (excl. recategorized FR21): **40**
- PRD FRs covered in epics: **40**
- **Coverage: 100%**
- Orphan FRs added in epics, absent from PRD: **5** (FR42–FR46)

### Finding — PRD ↔ Epics Drift (NON-BLOCKING, should resolve)

On 2026-06-22, a new **Epic 5 (Animation Browser)** with FR42–FR45, a portability requirement FR46, a throwaway **Spike 0** (feasibility walking skeleton), and a product **rename to ReaAnimViewer (AR21)** were added to the epics — but the PRD (last touched 2026-05-11) was not updated to match. Consequences to decide on:
1. **FR42–FR46 have no PRD provenance.** The epics are now the de-facto source of truth for the browser scope. Recommend back-porting FR42–FR46 (and the rename) into the PRD so the PRD remains the single source of truth, OR explicitly accepting the epics as the authoritative scope doc going forward.
2. **MVP scope grew** (extra epic + spike) vs. the PRD's "Phases 0–5, 6 weeks." The schedule/effort framing in the PRD no longer reflects reality. Worth a one-line note in the PRD scoping section.
3. **Rename (FBXAnimationViewer → ReaAnimViewer)** is in the epics but the PRD title/strings still say FBXAnimationViewer. Low risk (Epic 1 Story 1.1 handles code), but the PRD is now internally inconsistent with the plan.

## UX Alignment Assessment

### UX Document Status
**Not Found** — and this is **intentional and documented**. Both the PRD and the epics explicitly state no standalone UX document was produced; UI/interaction requirements are captured inline. This is appropriate for the product: a single dockable developer tool with one viewport panel (plus a browser panel), no multi-screen flows, no visual-design surface, console-only feedback (AR16). A dedicated UX doc would add ceremony without value here.

**Where UX actually lives:**
- PRD: User Journeys 1–3, Camera & Viewport FRs (FR22–FR26), Panel & Docking FRs (FR27–FR30), Journey Requirements Summary table.
- Architecture: D11 (FBO bridge / panel render loop), D14 (camera controller), AR16 (console-only feedback).
- Epics: per-story acceptance criteria describe the interaction (right-click orbit, scroll zoom, Reset Camera button, dock gestures, Reload button).

### UX ↔ PRD ↔ Architecture Alignment — Covered
The original interaction surface (viewport panel + camera + docking + reload) is **fully aligned across all three documents**. Architecture D11/D14 directly implement the PRD's camera/panel FRs, and each epic story cites the matching FR + architecture decision. No misalignment on the core viewer.

### Alignment Gaps (NON-BLOCKING but should be closed before Epic 5)

1. **⚠️ Animation Browser (FR42–FR45 / Epic 5) has NO architecture.** The architecture predates the browser (2026-05-12 vs browser added 2026-06-22). It contains no decision covering: filesystem navigation/tree UI, format filtering, selection state, or the **"preview without creating a track item"** mechanism (Story 5.2) — which is a genuinely new render/lifecycle path (the viewer must show an asset not bound to any PCM_source item). Epic 5 says it "reuses the PCM_source surface and the viewer," but the *preview-without-item* path and the browser panel itself are undesigned. **Recommendation:** add a short architecture decision (call it D18: Browser panel + transient preview asset) before starting Epic 5, or accept the story spec as the design-of-record and flag the preview-path risk explicitly.

2. **⚠️ Cross-machine path portability (FR46 / Story 6.2) is an open decision.** The architecture's persistence design (D9) stores per-item state via PCM_source `SaveState`/`LoadState` but does not address path portability across machines. Story 6.2 itself acknowledges this — it requires the mechanism (relative path vs missing-media remap vs optional re-import) to be "recorded as a decision in this epic's story spec before implementation (AR20)." So it is a *known, tracked* gap, not an oversight — but the decision is still pending.

### Warnings
- No accessibility UX (deferred post-MVP, inherits ReaImGui defaults) — consistent across PRD/architecture/epics, acceptable for MVP.
- Architecture still uses pre-rename strings (`ImGui::Begin("FBX Animation Viewer")`, Action `FBXAV: Open Viewer Window`). Cosmetic; Epic 1 Story 1.1 rewrites these. No action needed beyond awareness.

## Epic Quality Review

Validated all 8 epics (Spike 0 + Epics 1–7) and 24 production stories against create-epics-and-stories standards: user value, epic independence, forward dependencies, story sizing, AC quality, and brownfield handling.

### User Value Focus — PASS
Every production epic is framed as a user outcome, not a technical milestone:
- Epic 1 "Viewer lives inside Reaper", Epic 2 "See a textured 3D model", Epic 3 "Watch the rig animate", Epic 4 "Drive animation from the Reaper transport", Epic 5 "Browse and place animations without leaving Reaper", Epic 6 "Reliable across sessions/machines/files", Epic 7 "Install and ship via ReaPack". All express what the user can do/get.

### Epic Independence & Forward Dependencies — PASS
Strict backward-only dependency chain, and the reuse is explicitly stated in each epic preamble:
- Epic 1 stands alone → 2 uses 1 (panel) → 3 uses 2 (renderer) → 4 uses 3 (animation) → 5 reuses Epic 4 PCM_source + Epics 2–3 viewer → 6 uses Epic 4 PCM_source → 7 ships all.
- **No epic requires a later epic.** No circular dependencies. Cross-story references inside epics (e.g. Story 6.5 → "degrade per Story 6.4", Story 5.3 → "as in Stories 4.2–4.3") all point backward.

### Story Sizing & Acceptance Criteria — PASS (high quality)
- Every story is single-capability and properly scoped.
- All ACs use proper **Given/When/Then** BDD structure, are testable, and cite the exact FR/NFR/AR they satisfy — strong traceability.
- Error/edge paths are present (missing ReaImGui, malformed file, missing texture fallback, t=0/t=duration clamp, unresolvable path, FBX over-capacity degrade). This is above-average AC completeness.

### Brownfield & Starter-Template Handling — PASS
- Correctly brownfield: AR1 declares the Phase 0 commit IS the foundation; no spurious "set up project" story. The brownfield migration story is Epic 1 Story 1.1 (rename existing scaffold).
- Architecture states no starter template exists for the Reaper SDK ecosystem; epics correctly omit a starter-template story. Compliant.
- No database in this project; the analog (GPU resources, `Asset` model) is created per-item when needed (D1/D2 RAII), satisfying the "create when first needed" principle.

### Findings by Severity

🔴 **Critical Violations:** None.

🟠 **Major Issues:**
- **M1 — Epic 5 has an undesigned story (Story 5.2, "preview without placing").** Beyond the architecture gap noted in UX Alignment, this is also an implementation-readiness gap: the story requires loading an asset into the viewer **not bound to any PCM_source item**, but the entire render/lifecycle design (D8/D11) assumes the displayed asset comes from the playhead-spanning item. The "transient preview asset" lifecycle (ownership, teardown, interaction with the playhead-driven display) is unspecified. Epic 5 should not start until this path has a decision (new D18 or a story-spec design note). *(Cross-ref: UX Alignment gap #1.)*

🟡 **Minor Concerns:**
- **m1 — Spike 0 owns no user value (by design).** A throwaway feasibility spike is a technical epic, which the standard normally rejects. Here it is *legitimate and correctly bounded*: timeboxed ~2 days, separate branch, never merged, explicit go/no-go gate, owns no FRs, feeds findings into Phase 0.5/1/2. Accepted as the sanctioned spike exception — flagged only so it's a conscious choice, not an oversight.
- **m2 — Epic 1 front-loads two enabling stories before user-visible value.** Story 1.1 (rename) and 1.2 (FBO bridge) are technical; user-visible value lands at 1.3 (dockable panel). Acceptable because the epic *as a whole* delivers a dockable panel and the FBO bridge is the architecturally-mandated Phase 0.5 de-risk (D11/AR10). No reordering needed.
- **m3 — FR42–FR46 traceability.** The five browser/portability FRs exist in epics with full stories but have no PRD provenance. *(Cross-ref: Epic Coverage Drift finding — back-port to PRD or declare epics authoritative.)*
- **m4 — FR46 mechanism undecided.** Story 6.2 correctly defers the path-portability mechanism to a story-spec decision (AR20), but that decision is still open going into Phase 4.

### Best-Practices Compliance Checklist
- [x] Epics deliver user value (Spike 0 = sanctioned exception)
- [x] Epics function independently (backward-only chain)
- [x] Stories appropriately sized
- [x] No forward dependencies
- [x] Resources/entities created when needed (no upfront-all)
- [x] Clear, testable Given/When/Then acceptance criteria
- [x] FR traceability maintained (100% PRD coverage; 5 epic-only FRs flagged)

## Summary and Recommendations

### Overall Readiness Status

**✅ READY (with two pre-Epic-5 housekeeping items)**

The planning suite is strong and internally coherent for the original MVP scope. 100% of PRD functional requirements trace to a concrete epic AND a story with testable Given/When/Then acceptance criteria. Epic structure is clean: user-value framing throughout, strictly backward-only dependencies, no critical violations. The architecture (D1–D17) fully supports the original viewer scope. Phase 0 is shipped and validated, and a feasibility spike de-risks the dominant technical unknown before production resumes.

The only real gaps come from a **2026-06-22 scope addition** (Animation Browser FR42–FR45, cross-machine portability FR46, plus the rename and a feasibility spike) that was added to the epics but not propagated back to the PRD and architecture. None of these block the start of implementation — Epics 1–4 are unaffected and can begin immediately — but two items should be resolved before Epic 5 (browser) and Epic 6 (persistence/portability) begin.

### Critical Issues Requiring Immediate Action
**None.** No blocker prevents starting implementation at the Spike / Epic 1.

### Issues to Resolve (by when needed)

| # | Severity | Issue | Needed before |
|---|----------|-------|---------------|
| M1 | 🟠 Major | Epic 5 Story 5.2 "preview without placing" has no design — transient-preview asset lifecycle is unspecified in the architecture (D8/D11 assume the asset comes from the playhead item) | **Epic 5 start** |
| D1 | 🟠 Major | PRD ↔ Epics drift: FR42–FR46, the rename (AR21), and the added spike/epic are not in the PRD; PRD scope/schedule framing is now stale | Before treating PRD as source of truth; ideally **soon** |
| A1 | 🟡 Minor | Architecture predates the browser — no decision covers filesystem navigation/filtering/browser panel | **Epic 5 start** |
| F1 | 🟡 Minor | FR46 path-portability mechanism still undecided (Story 6.2 defers it per AR20) | **Epic 6 / Story 6.2 start** |

### Recommended Next Steps
1. **Proceed now** with the feasibility Spike 0 → Epic 1 → Epic 4. They are fully specified, fully covered, and unaffected by the gaps above. This is the critical path and nothing blocks it.
2. **Back-port the 2026-06-22 additions into the PRD** (FR42–FR46, the ReaAnimViewer rename, the extra epic + spike, and a one-line scope/schedule note) so the PRD remains the single source of truth — or make an explicit decision that the epics are now the authoritative scope document. *(Resolves D1.)*
3. **Add a short architecture decision (D18) for Epic 5** covering the browser panel (filesystem navigation, format filtering) and especially the **transient preview-without-item asset lifecycle**, before Epic 5 work begins. *(Resolves M1 + A1.)*
4. **Lock the FR46 portability mechanism** (relative path vs missing-media remap vs optional re-import) as a Story 6.2 spec decision before Phase 4. *(Resolves F1.)*

### Final Note
This assessment reviewed 4 artifacts (PRD, Architecture, Epics, PRFAQ context) and identified **6 issues across 3 categories** (1 coverage drift, 2 architecture/UX gaps, 4 epic-quality observations of which 2 are accepted-by-design). **Zero are critical/blocking.** The dominant theme is a single recent scope addition that hasn't yet been propagated backward through the PRD and architecture — normal drift, cheaply fixed. Implementation can begin on the critical path immediately; resolve the four tracked items before their respective epics. These findings can be used to tighten the artifacts, or you may proceed as-is with the gaps logged.

---

**Assessor:** Claude (Implementation Readiness workflow, BMAD)
**Date:** 2026-06-22
**Artifacts reviewed:** prd.md, architecture.md, epics.md, prfaq-FBXAnimationViewer.md

---

## Remediation Log (2026-06-22, same session)

Three of the four tracked items were resolved immediately after the assessment:

- **D1 — RESOLVED.** `prd.md` updated: added FR42–FR46 ("Animation Browser / Explorer" section) and a "Scope update (2026-06-22)" note in Product Scope covering the rename to ReaAnimViewer, the new browser epic, the feasibility spike, and the now-optimistic schedule. PRD is realigned as source of truth. *(Rename strings in the PRD are intentionally left historical, per Epic 1 Story 1.1 AC which exempts `_bmad-output/` planning artifacts.)*
- **M1 + A1 — RESOLVED.** `architecture.md` updated: added decision **D18 — Browser panel + transient preview asset lifecycle**, specifying the browser panel (filesystem nav, format filter, main-thread/console-diagnostic discipline) and, critically, the transient-preview asset ownership + explicit display precedence (preview slot overrides playhead-driven display; place-on-track reuses the Epic 4 PCM_source path). Added the Epic 5 row to the Decision Impact Analysis table. Epic 5 now has a design-of-record.
- **F1 — STILL OPEN (by design).** The FR46 path-portability mechanism remains a Story 6.2 spec decision (AR20), to be locked before Phase 4. No action needed now; tracked.

Net: implementation can proceed on the critical path; the only outstanding item (F1) is correctly deferred to its own epic.
