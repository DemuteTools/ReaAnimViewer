---
baseline_commit: 73bd498b4f4f91f4789226a5c933a95ce0997545
---

# Story 6.2: Persist and restore the panel/viewport state through project save/load

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want my viewer panel layout to come back where I left it,
so that reopening a session doesn't make me re-dock and re-frame the viewport.

## Acceptance Criteria

**Given** the viewer panel docked at a chosen position
**When** I save and reopen the project (and across a Reaper restart)
**Then** the panel's dock position/state is restored via Reaper's **native** docker/screenset state — the viewport is a native GL window docked via `DockWindowAddEx` with a stable `identstr` (`kDockIdent`), so Reaper remembers its dock target/position in `reaper.ini`; ReaImGui is **not** used (FR32, reformulé 2026-06-27, D9)

**And** restoring the panel never blocks the host or leaks GL/window resources — the existing `DockWindowAddEx` ↔ `DockWindowRemove` symmetric lifecycle and `UnregisterViewerClass` on close/unload are preserved unchanged (NFR-R3, AR15)

**And** **verification first (AR20):** an in-Reaper check confirms whether the native docker already round-trips the dock position across save → close → reopen (and across a Reaper restart). If it does, this story is **zero-code on the dock position** and the native behavior is documented in the gate; if a real gap is found (panel does not return to its remembered dock), the minimal fix is **our own Reaper ext-state** (`SetExtState`/`GetExtState`) — **never** ReaImGui and **never** a custom screenset engine. The decision is recorded in this story spec and the architecture Spec Change Log.

### Out of scope (explicitly deferred — do NOT implement here)

- **Per-item camera framing persistence** (`cam_*` keys). The camera is **global today**: the renderer owns one `OrbitCamera` (`cam_`), reset on every `SetAsset` (Story 2.4) — there is **no per-item camera source of truth** to persist. Deferred per `deferred-work.md` (story 6.1 entry); the epic AC names "an adjusted camera" in its *Given*, but the testable *Then* is dock position only. Do not add `cam_*` to `SaveState`/`LoadState`.
- **`time_offset` / `time_scale`** — native take state (`D_STARTOFFS` / `D_PLAYRATE`), already deferred (6.1 / 4.4).
- **Cross-machine relink** — Story 6.3.
- **ReaImGui panel** — post-MVP (Epic 5, postponed).

## Tasks / Subtasks

- [x] **Task 0 — Verify-first in-Reaper (load-bearing, AR20):** before writing any code, determine the native baseline (AC3). *(Dev side complete: the native baseline is established from the code + D9, the expected outcome is recorded, and gate §5 is set up. The two in-Reaper observation subtasks below were **executed by Antho** via `PHASE4_VALIDATOR_GATE.md` §5 — gate Result is now **PASS** (Antho, in-Reaper Windows, 2026-06-27); it was never pre-marked PASS before his run.)*
  - [x] In Reaper (Windows): open the viewer (`RAV: Open Viewer`), dock it at a deliberate, non-default position (e.g. drag to the right docker / a specific tab). Save the project. Close and reopen the project → confirm whether the panel returns to the same dock position when re-opened. → **Gate §5 row 1 (Antho, PASS 2026-06-27).**
  - [x] Quit and relaunch Reaper, reopen the project, open the viewer → confirm the dock position is still remembered (this is the `reaper.ini` / `kDockIdent` path — the most likely place native persistence actually lives, since dock position is **global**, not per-project). → **Gate §5 row 2 (Antho, PASS 2026-06-27).**
  - [x] Record the observed outcome in `docs/PHASE4_VALIDATOR_GATE.md` §2. **Expected:** native `DockWindowAddEx(..., kDockIdent, true)` already remembers the dock target → **dock position is zero-code**, documented. → **Done:** expected outcome + verify-first hypothesis recorded in gate §5.
- [x] **Task 1 — If (and only if) Task 0 finds a real gap:** add minimal ext-state persistence (AC1, AC3). → **NOT triggered (conditional).** No code added — the verify-first hypothesis is expected-zero-code (stable `kDockIdent`, dock state is Reaper-global per D9). If Antho's gate §5 row 1/2 finds a real gap, this task fires as a review follow-up (the steps below are pre-specified for that case).
  - [ ] Add `REAPERAPI_WANT_SetExtState` / `REAPERAPI_WANT_GetExtState` to `src/reaper_api.h` (global ext-state → `reaper.ini`, `persist=true`). These resolve through the existing `REAPERAPI_LoadAPI` call — **no new `rec->Register`** (AR15 unaffected; ext-state getters/setters are not registrations). *(deferred — only-if-gap)*
  - [ ] In `src/viewer_window.cpp`, persist only the minimal missing datum (e.g. "panel was open" and/or last dock state) under our namespace key, and restore it on `OpenViewerWindow`. Keep it behind the existing AR18 no-throw boundary. *(deferred — only-if-gap)*
  - [ ] Do **not** reimplement what `DockWindowAddEx` already does — only fill the proven gap. *(deferred — only-if-gap)*
- [x] **Task 2 — Preserve the lifecycle invariants (AC2, NFR-R3, AR15):** confirm (and keep intact) that `OpenViewerWindow` docks via `DockWindowAddEx(g_hwnd, kDockName, kDockIdent, true)` and `CloseViewerWindow` calls `DockWindowRemove` → `DestroyWindow` → `UnregisterViewerClass`; the unload path (`plugin_main.cpp` `rec==nullptr`) calls `CloseViewerWindow()` first. No leaked GL context / window class / dock registration. → **Verified intact, no change** ([viewer_window.cpp:554-604, :638-653](../../src/viewer_window.cpp#L554)); `git diff` shows zero `src/` change.
- [x] **Task 3 — Documentation & records (always):**
  - [x] Append `docs/PHASE4_VALIDATOR_GATE.md` §2 (click-based rows for Antho; Result line **PENDING** — never pre-mark PASS). → Added as gate **§5** (4 click-based rows + Recording section, Result **PENDING**, expected-outcome note).
  - [x] Add an AR20 Spec Change Log entry in `architecture.md` recording the 6.2 decision (native docker is authoritative for dock position / ext-state only-if-gap / camera deferred).
  - [x] Add a `deferred-work.md` entry for any 6.2 deferral (camera reaffirmed; auto-reopen-on-project-load deferred; ext-state-only-if-gap).
  - [x] Update `sprint-status.yaml` (6.2 → in-progress → review at dev time).

## Review Findings

*(bmad-code-review, 3-layer adversarial — Blind Hunter / Edge Case Hunter / Acceptance Auditor — 2026-06-27)*

- [x] **[Review][Decision] RESOLVED — Antho confirmed he ran gate §5 in real Reaper on Windows and it PASSED (2026-06-27); the PASS is genuine. Body reconciled to a consistent `done` state (Task 0 rows, Task 3, Completion Notes, File List). Gate §5 PASS + `done` status asserted, but the story body had still said PENDING throughout — needed Antho's explicit gate confirmation** [`docs/PHASE4_VALIDATOR_GATE.md:192`; story `:7`,`:157`; `sprint-status.yaml:105`] — A done/PASS layer was added (Status `done`; Change Log :157 *"Antho ran gate §5… PENDING → PASS"*; gate Result :192 `PASS`; sprint `done`) while the dev-time layer was left intact and contradicts it (Task 0 rows :37-38 `(Antho, PENDING)`; Task 3 :46 `Result PENDING — never pre-mark PASS`; Completion Notes :142-143 *"sprint status moves to **review** (not done)… gate Result stays **PENDING**… Nothing is fabricated as PASS"*; File List :149/:152 `Result PENDING`/`→ review`). All changes are uncommitted with **no** closure commit (contrast 6.1's `73bd498`). Per project rule (never fabricate a gate PASS; AR19/AR20), the PASS cannot be trusted at face value — **resolution depends on Antho's answer:** if he genuinely ran §5 in Reaper → reconcile the whole body to `done` consistently; if it was flipped prematurely → revert the done/PASS layer back to `review`/PENDING.
- [x] **[Review][Dismiss] gate §5 `AC1/AC2/AC3` labels not anchored to numbered ACs** [`docs/PHASE4_VALIDATOR_GATE.md` §5] — the story writes ACs as unlabeled Given/When/Then prose; the gate invents numeric tags. Cosmetic traceability only; each row's substance maps to a real story criterion (verified). Dismissed as noise.

**Verified clean (no findings):** every load-bearing code claim in the docs checks out against the real source — `DockWindowAddEx(g_hwnd, kDockName, kDockIdent, true)` ([viewer_window.cpp:602](../../src/viewer_window.cpp#L602)), `kDockIdent = "ReaAnimViewer.Viewer"` constant + "persists the dock position across sessions" comment, the `DockWindowAddEx`↔`DockWindowRemove`→`UnregisterViewerClass` lifecycle + never-destroy-on-hide invariant, **zero `src/`/CMake change** since baseline (`git diff 73bd498 -- src/ CMakeLists.txt` empty), `pcm_source_anim.cpp` untouched, and both deferrals (camera global / auto-reopen-not-implemented) confirmed against the code. AC plan-coverage is complete. The only issue is the PASS/status integrity one above.

## Dev Notes

### The crux: dock state is GLOBAL/native, not per-project

The single most important fact for this story: **a docked native GL window's dock position is a Reaper-global UI concern (stored in `reaper.ini`, keyed by the window's `identstr`), not per-project state.** The viewport is already created and handed to the docker as:

```cpp
DockWindowAddEx(g_hwnd, kDockName, kDockIdent, true);   // viewer_window.cpp:602
DockWindowActivate(g_hwnd);
```

with `kDockIdent = "ReaAnimViewer.Viewer"` — the existing code comment already states *"identstr persists the dock position across sessions"* ([viewer_window.cpp:599-601](../../src/viewer_window.cpp#L599)). This means Reaper, **by design**, remembers where this ident was last docked. The architecture's D9 table assigns "Panel dock position, panel size, dock target" to **"Reaper native docker/screenset"** with persistence "restored on project load (`DockWindowAddEx`), or our ext-state" and format "**Opaque to us**" [Source: architecture.md#D9 (~:323-327)].

→ This is therefore a **verify-first, likely-zero-code** story exactly like 6.3. Do not invent serialization. The PCM_source `SaveState`/`LoadState` (D9 per-item channel built in 6.1) is the **wrong** layer for dock state — it persists *per-item* data inside our source's project chunk, not panel chrome. **Do not touch `pcm_source_anim.cpp` for this story.**

### Why "through project save/load" still makes sense

Because the panel is a global UI element, closing/reopening a *project* (within one Reaper session) does not disturb the docked viewer at all — it stays put. The persistence that actually matters is **across a Reaper restart** (the `reaper.ini` screenset path), which `DockWindowAddEx` + a stable `kDockIdent` already covers. Task 0's two checks (project reopen, then Reaper restart) exist to confirm both and pin down exactly where (if anywhere) a gap lives. State the honest finding in the gate — if native already does it, say so plainly; that is a PASS, not a shortfall.

### Files this story may touch

| File | Role | Likely change |
|---|---|---|
| `docs/PHASE4_VALIDATOR_GATE.md` | Phase 4 gate | **always** — append §2 (Story 6.2), click-based, Result PENDING |
| `_bmad-output/planning-artifacts/architecture.md` | Spec Change Log | **always** — AR20 entry for the 6.2 decision |
| `_bmad-output/implementation-artifacts/deferred-work.md` | Deferral ledger | **always** — 6.2 deferrals (camera reaffirmed; auto-reopen if deferred) |
| `_bmad-output/implementation-artifacts/sprint-status.yaml` | Sprint tracking | **always** — 6.2 status transitions |
| `src/reaper_api.h` | API WANT_ surface | **only if gap** — `WANT_SetExtState` / `WANT_GetExtState` |
| `src/viewer_window.cpp` | Window/dock lifecycle | **only if gap** — minimal ext-state save/restore behind the AR18 try/catch |

### Existing lifecycle to preserve (read before editing `viewer_window.cpp`)

The window/dock lifecycle is already correct and AR15/NFR-R3-clean — **do not regress it**:

- **Open** ([viewer_window.cpp:554-604](../../src/viewer_window.cpp#L554)): if already created, just `DockWindowActivate`; else `EnsureClassRegistered` → `CreateWindowExW` (WS_CHILD, not visible) → `DockWindowAddEx(..., kDockIdent, true)` → `DockWindowActivate`. On create-failure it unwinds the class (`UnregisterViewerClass`).
- **Close/unload** ([viewer_window.cpp:638-653](../../src/viewer_window.cpp#L638)): `DockWindowRemove(g_hwnd)` → `DestroyWindow` (WM_DESTROY runs `StopRendering`) → null `g_hwnd` → `UnregisterViewerClass`. Idempotent.
- **Hide handling** ([viewer_window.cpp:9-18](../../src/viewer_window.cpp#L9)): a docker-tab-X close sends our window NOTHING and is indistinguishable from a transient hide/minimize, so the window is **never destroyed on hide** — only on the toggle action or unload. The render loop parks while `!IsWindowVisible`. **Any ext-state work must not break this** — do not destroy/recreate the window to "restore" a dock position; let Reaper's docker own layout.
- **Toggle state**: `OnToggleAction` ([plugin_main.cpp:35](../../src/plugin_main.cpp#L35)) reports `ViewerWindowIsVisible()`; `RefreshToolbar` reflects it. If Task 1 adds "reopen on load," keep the toggle state coherent.

### Persistence pattern reference (6.1) — for shape only, NOT for reuse here

Story 6.1 built the per-item `key=value` framework in `pcm_source_anim.cpp` (`rav_ver=1`, defensive `file=` only-if-empty, `LoadState` ignores unknown keys, returns 0 never -1). That is the **per-item** surface; this story is the **panel** surface, a different layer (D9 row 2 vs row 1). Mirror only the *discipline* (versioned, forward-compat, namespaced, no host-state mutation), not the code path.

### Testing standards

- No unit-test framework in this project; verification is an **isolated `-Wall -Wextra -Werror` harness** for any new pure logic (per 6.1/4.x precedent) plus a **host-stubbed `cmake -B <build> -S .`** configure (non-Windows target intentionally stubbed). If this story is docs-only (likely), the gate is the in-Reaper check.
- The real gate is **click-based, in-Reaper on Windows (AR19)** — `docs/PHASE4_VALIDATOR_GATE.md` §2, run by Antho. Validator hooks must be click-based (open the action, dock, save, reopen), never env-vars/CLI. Never fabricate a PASS; Result stays PENDING until Antho confirms.

### Architecture compliance (guardrails)

- **AR15 — symmetric register/unregister.** Ext-state getters/setters are **not** `rec->Register` surfaces, so they add no new symmetric-register obligation. The existing `DockWindowAddEx` ↔ `DockWindowRemove` pairing must stay intact. Do **not** add any `rec->Register(...)` outside `plugin_main.cpp` (boundary rule).
- **AR16 — console-only diagnostics.** Any error (e.g. ext-state read fails) → `LogError`/`LogInfo` via `console_log.h`, never a popup.
- **AR18 — main-thread, no-throw at the host edge.** All window/dock work stays on the main thread inside the existing `try/catch` in `ToggleViewerWindow`.
- **AR17 — failure isolation.** A dock/ext-state restore failure must leave the rest of the session working (the viewer just opens at the default dock).
- **No ReaImGui** — the viewport is a native GL window; FR32 was reformulated away from ReaImGui (architecture Spec Change Log 2026-06-27).
- **Boundary rule:** `pcm_source_anim.cpp` is the PCM_source boundary (per-item state) — **off-limits** for panel state.

### Project Structure Notes

- The architecture's original layout named a hypothetical `project_state.{h,cpp}` for "project-level state hooks if needed beyond PCM_source ext-state" [Source: architecture.md (~:772-794, :826)]. **Do not create new files speculatively.** Given dock state is native/global, the verify-first outcome almost certainly needs **no** new module; if Task 1 fires, a few lines of global ext-state in `viewer_window.cpp` is the right-sized fix, not a new file. Match the existing flat `src/` layout.
- No `CMakeLists.txt` change expected (no new translation unit in the likely path).

### References

- [Source: epics.md#Story 6.2 (:578-589)] — the story statement + AC (native docker/screenset or ext-state, not ReaImGui; NFR-R3).
- [Source: epics.md#FR32 (:202), #FR33 (:203)] — serialize panel dock position via native docker/screenset (reformulé 2026-06-27); restore viewport state on reopen.
- [Source: architecture.md#D9 Persistence layering (~:319-327)] — three persistence surfaces; panel dock = Reaper native docker, opaque to us, restored via `DockWindowAddEx` or ext-state.
- [Source: architecture.md#Spec Change Log 2026-06-27 (~:1138-1149)] — FR32 reformulated off ReaImGui to native docker; realized by Story 6.2.
- [Source: architecture.md#NFR-R3 (:53)] — clean unload: no leaked GL contexts, window classes, or dangling Reaper API pointers.
- [Source: src/viewer_window.cpp:40-45, :554-653] — `kDockName`/`kDockIdent`, `OpenViewerWindow`/`CloseViewerWindow`/`ToggleViewerWindow` lifecycle.
- [Source: src/reaper_api.h:10-17] — `DockWindowAddEx`/`DockWindowActivate`/`DockWindowRemove`/`RefreshToolbar` already WANT-declared; ext-state not yet declared.
- [Source: deferred-work.md (story 6.1 entry, :11-12) + (:109-115)] — camera is global/deferred; "project state hooks … add `REAPERAPI_WANT_*` as needed."
- [Source: 6-1 story Dev Agent Record] — verify-first AR19 pattern, `-Werror` harness + host-stubbed cmake, scope/boundary grep audit, PENDING gate discipline.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Opus 4.8, 1M context) — bmad-dev-story workflow

### Debug Log References

- Scope audit — `git diff --name-only -- src/` → **empty** (no `src/` change); `git diff --name-only -- src/reaper_api.h src/plugin_main.cpp CMakeLists.txt` → **empty**. Full diff touches only `docs/PHASE4_VALIDATOR_GATE.md`, `_bmad-output/planning-artifacts/architecture.md`, `_bmad-output/implementation-artifacts/deferred-work.md`, `sprint-status.yaml` (+ this story file).
- Host-stubbed configure — `cmake -S . -B /tmp/rav_build` → exit 0 ("non-Windows platform detected … Build target stubbed", configuring/generating done). Build byte-identical to the 6.1 baseline (`73bd498`) since no source changed.

### Completion Notes List

**Verify-first (AR20), expected-zero-code — realized as docs-only.** Story 6.2's central finding: the viewer panel's **dock position is owned by Reaper's native docker and is Reaper-global** (`reaper.ini`, keyed by `identstr`), **not** per-project state. The viewport is already docked via `DockWindowAddEx(g_hwnd, kDockName, kDockIdent, true)` with a **stable** `kDockIdent = "ReaAnimViewer.Viewer"` ([viewer_window.cpp:599-602](../../src/viewer_window.cpp#L599)), and the existing code comment already states the ident *"persists the dock position across sessions."* The D9 layering (architecture.md) assigns panel dock to "Reaper native docker, opaque to us, restored via `DockWindowAddEx` or our ext-state." → No serialization invented; **no `src/` change**.

- **Task 2 (lifecycle invariants):** verified intact and unchanged. `OpenViewerWindow` docks via `DockWindowAddEx(..., kDockIdent, true)` → `DockWindowActivate`; `CloseViewerWindow` does `DockWindowRemove` → `DestroyWindow` (WM_DESTROY runs `StopRendering`) → null `g_hwnd` → `UnregisterViewerClass`; the unload path calls `CloseViewerWindow()` first. The hide-handling invariant (never destroy on a docker-tab-X hide — only on toggle/unload) is preserved. NFR-R3 (clean unload, no leaked GL/window/dock) and AR15 (symmetric `DockWindowAddEx`↔`DockWindowRemove`) hold; `git diff` confirms zero `src/` change.
- **Task 3 (docs & records):** `docs/PHASE4_VALIDATOR_GATE.md` **§5** added — 4 click-based, in-Reaper-on-Windows rows for Antho (dock survives project reopen; dock survives Reaper restart = the `reaper.ini` path; no host block / no GL-window leak; no-regression/scope-clean) + a Recording section, the only-if-gap → ext-state trigger, and Result **PENDING** (expected-outcome hypothesis stated, **not** pre-marked PASS). `architecture.md` Spec Change Log got the dated AR20 entry (native docker authoritative; ext-state only-if-gap; never ReaImGui / never custom screenset engine; camera/`time_*`/cross-machine/ReaImGui deferred). `deferred-work.md` got the 6.2 entry (camera reaffirmed-deferred, auto-reopen-on-project-load deferred, ext-state-only-if-gap trigger).
- **Task 0 (verify-first) / Task 1 (only-if-gap):** the dev-side baseline analysis is complete and the gate was set up; the **actual in-Reaper confirmation was Antho's** via gate §5 (rows 1–2, **PASS 2026-06-27**) — the native docker round-trips the dock position with zero code, confirming the verify-first hypothesis. Task 1 was **NOT triggered** — no code added; the native path held so the only-if-gap global `SetExtState`/`GetExtState` fallback was not needed.
- **Honesty / gate discipline:** at dev time sprint status moved to **review** (not done) with the gate Result held at **PENDING**; it was promoted to **done / PASS** only after Antho ran §5 in real Reaper on Windows (AR19) and confirmed it during code-review. Nothing was fabricated — the PASS was recorded against Antho's actual run.

### File List

*(No `src/` source changed — verify-first docs-only path.)*

- `docs/PHASE4_VALIDATOR_GATE.md` — **modified** — added §5 (Story 6.2 panel/dock gate: 4 click-based rows + Recording + Result PENDING→**PASS** after Antho's in-Reaper run).
- `_bmad-output/planning-artifacts/architecture.md` — **modified** — added the 2026-06-27 AR20 Spec Change Log entry for the 6.2 dock-persistence decision.
- `_bmad-output/implementation-artifacts/deferred-work.md` — **modified** — added the story-6.2 deferral entry (camera reaffirmed; auto-reopen; ext-state-only-if-gap).
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — **modified** — 6-2 status transitions (ready-for-dev → in-progress → review → **done** after gate §5 PASS).
- `_bmad-output/implementation-artifacts/6-2-persist-and-restore-panel-viewport-state-through-project-save-load.md` — **modified** — frontmatter `baseline_commit`, task checkboxes, Dev Agent Record, File List, Change Log, Status.

### Change Log

- 2026-06-27 — **Story 6.2 → done.** Antho ran gate §5 in real Reaper on Windows (AR19): the docked viewer returns to its remembered dock across project save/close/reopen **and** across a full Reaper quit/relaunch, no host block, no GL/window/dock leak. Verify-first hypothesis confirmed — the native docker round-trips the dock position **zero-code**; the ext-state fallback (Task 1) was not needed. Gate §5 Result PENDING → **PASS**. Status review → done.
- 2026-06-27 — Story 6.2 implemented (dev-story) as **docs-only** (verify-first AR20, expected-zero-code). Confirmed the panel **dock position is owned by Reaper's native docker** (`DockWindowAddEx` + stable `kDockIdent`, Reaper-global `reaper.ini`, not per-project — D9) → no serialization invented, **no `src/` change**. Task 2 lifecycle invariants verified intact (`DockWindowAddEx`↔`DockWindowRemove` + `UnregisterViewerClass`; never-destroy-on-hide; NFR-R3/AR15). Task 3 docs done: gate `PHASE4_VALIDATOR_GATE.md` §5 (Result **PENDING**), `architecture.md` AR20 Spec Change Log entry, `deferred-work.md` 6.2 deferrals. Task 1 (ext-state fallback) **NOT triggered** — only-if-gap, fires as a review follow-up if Antho's gate §5 finds a real dock gap. Scope-clean (`git diff` = docs + sprint-status only); host-stubbed cmake configures. Status → review (gate §5 = Antho in-Reaper Windows, AR19, PENDING).
- 2026-06-27 — Story 6.2 drafted (create-story). Persist/restore the panel/viewport dock state through project save/load. **Verify-first (AR20):** the native docker (`DockWindowAddEx` + stable `kDockIdent`) is expected to already round-trip dock position (dock state is Reaper-global/`reaper.ini`, not per-project, per D9) → likely **docs-only** (gate §2 + Spec Change Log + deferred-work + sprint-status); a minimal global `SetExtState`/`GetExtState` fallback in `viewer_window.cpp` (+ `reaper_api.h` WANT_) is added **only if** an in-Reaper gap is found. Camera framing (global today), `time_offset`/`time_scale` (native), cross-machine relink (6.3), and ReaImGui (post-MVP) all explicitly out of scope. `pcm_source_anim.cpp` is off-limits (wrong layer — per-item, not panel). Existing `DockWindowAddEx`↔`DockWindowRemove` lifecycle + `UnregisterViewerClass` preserved (NFR-R3/AR15). Status → ready-for-dev.
