# Sprint Change Proposal — 2026-06-23

**Status:** Approved by Antho (2026-06-23) · **Scope:** Major (architecture amendment + Epic 1 rework), bounded · **Mode:** Batch

## 1. Issue summary

Spike 0 (throwaway feasibility prototype, branch `spike/0-1-feasibility`, verdict **GO** — see `docs/SPIKE0_FINDINGS.md`) invalidated a core architecture assumption of Epic 1 / Phase 0.5. Measured on the reference workstation (Ryzen 9 9900X / RX 9070):

- A **ReaImGui panel is hard-capped ~30 fps** for displayed content (presentation tied to Reaper's ~30 Hz UI loop; feeding it at ~66 Hz still showed ~32 fps), and ReaImGui **cannot display a foreign GPU texture** (image API copies pixels; no DrawList callback).
- A **native OpenGL window docked via `DockWindowAddEx`** (direct render + `SwapBuffers`, independent timer) reached **~62 fps**.
- **Transport integration confirmed:** a custom PCM_source turns a dropped `.fbx/.glb/.gltf` into a timeline item of the animation's length, and the playhead drives the displayed frame at full rate.

This contradicts decision **D11/AR10** ("ReaImGui panel + sokol FBO bridge"), on which Epic 1 stories 1.2/1.3/1.4 were built.

## 2. Impact analysis

- **Epic impact:** Epic 1 only. Story 1.1 (rename) unaffected. Stories 1.2/1.3/1.4 reworked. ReaImGui dependency (FR30, AR3, NFR-C2) relocates to Epic 5 (first ReaImGui panel = the browser). No new epics; no epic removed.
- **PRD:** MVP unchanged in user terms (a dockable 60 fps 3D viewer in Reaper). FR27 reinterpreted ("ReaImGui-driven panel" → "docked GL viewport"); FR30 moves to Epic 5. No scope cut.
- **Architecture:** D11/AR10 superseded; D12 amended (direct-to-window, no FBO/MSAA-resolve/readback); ReaImGui dependency deferred; sokol (AR4) to re-pin/validate or drop for raw GL; D8/AR11 (PCM_source) confirmed; new FBX loader requirement (`PreservePivots=0`).
- **UX:** none (no standalone UX doc; viewport behaves identically to the user).
- **Other artifacts:** sprint-status story keys for 1-2/1-3/1-4 renamed.

## 3. Recommended approach — Direct Adjustment (Option 1)

Modify Epic 1 stories + amend the architecture in place. **No rollback** (spike code was throwaway, never merged; `main` untouched). **No MVP reduction.** Effort: medium; risk: **low** — the dominant technical risk was just retired by the spike with large headroom (62 fps vs 60 target; transport proven).

Options not taken: *Rollback* (N/A — nothing to revert on `main`); *MVP review* (N/A — scope intact).

## 4. Detailed changes applied

**Architecture (`architecture.md`):** D11 + D12 marked superseded/amended in place; new **Spec Change Log 2026-06-23** entry (trigger / amendment / confirmed / FBX requirement / KEEP).

**Epics (`epics.md`):** AR10 marked superseded; Epic 1 intro reworked; **Story 1.2** → "Docked OpenGL viewport (direct render at 60 fps)"; **Story 1.3** → "Dockable GL window opened by a Reaper Action"; **Story 1.4** → "Graceful GL context / init failure handling" (ReaImGui-missing moved to Epic 5); FR27 reinterpreted; FR30 + AR3/NFR-C2 moved to Epic 5 (FR coverage map + Epic 1/Epic 5 FR lists updated).

**Sprint status (`sprint-status.yaml`):** epic-0 + 0-1 + retro → `done`; Epic 1 story keys renamed:
- `1-2-render-to-texture-fbo-bridge-sokol-gfx-imgui-image` → `1-2-docked-opengl-viewport-direct-render`
- `1-3-dockable-reaimgui-panel-opened-by-a-reaper-action` → `1-3-dockable-gl-window-opened-by-a-reaper-action`
- `1-4-graceful-handling-when-reaimgui-is-missing` → `1-4-graceful-gl-context-init-failure-handling`

## 5. Implementation handoff

**Scope: Major** but bounded. Artifacts updated by this proposal (dev/architect). Next step:
1. `create-story 1-1-rename-fbxanimationviewer-to-reaanimviewer` (unaffected by this change).
2. Then `create-story` for the revised 1-2 / 1-3 / 1-4 — they now read the corrected epics + architecture.

**Success criteria:** Epic 1 ships a docked OpenGL viewport at ≥60 fps, opened by `RAV: Open Viewer`, with no ReaImGui dependency; ReaImGui reappears at Epic 5.

> Note: spike code stays on the unmerged branch `spike/0-1-feasibility` as a reference; production is written fresh on `main` against these amended specs.
