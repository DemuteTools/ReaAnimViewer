---
baseline_commit: 4b1b865e6341e1f12fdbed9911264863ed0aa49a
---

# Story 6.3: Cross-machine portability via Reaper's native media handling

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer collaborating across machines,
I want my session to find its animations on another PC without re-importing gigabytes,
so that projects are portable without a custom remap engine.

## Acceptance Criteria

1. **Native copy + relink (FR46).** **Given** a project referencing animation files, saved with Reaper's "copy media into project directory" — **When** it is opened on another machine (or after the project folder is moved) — **Then** each item's animation file resolves via Reaper's **native** media handling: copy-into-project on save and relink via the PCM_source filename (`GetFileName`/`SetFileName`, already implemented in [pcm_source_anim.cpp:93-100](../../src/pcm_source_anim.cpp#L93)), **without forcing a full media re-import**.

2. **Verification first (AR20).** **Given** the native mechanism above — **When** the in-Reaper check is run — **Then** it confirms whether "copy media into project directory" already embeds our items **and** native relink already finds a moved file. **If it does**, this story is **zero-code** and the native behavior is documented in the gate. **If it does not**, our source is made to participate in the native mechanism — **no custom relative-path/remap engine**. The AR20 decision (which path was taken) is recorded in this story spec and the architecture Spec Change Log.

3. **Unresolved path → diagnostic + isolation (AR17).** **Given** an item whose path cannot be resolved on the new machine — **When** the project is opened and the playhead moves over that item — **Then** a **missing-media diagnostic** is surfaced (console, via the existing `LoadAsset` → `LogError` path at [viewer_window.cpp:198-214](../../src/viewer_window.cpp#L198)) **and the rest of the session keeps working** — every other item still rebinds, loads, and plays.

### Out of scope (explicitly deferred — do NOT implement here)

- **A custom relative-path / missing-media remap engine.** FR46 was reformulated 2026-06-27 **off** a home-grown remap onto Reaper's **native** copy/relink. Do **not** build path-rewriting, a search-path resolver, a "locate missing media" dialog, or any portability layer of our own. ([architecture.md Spec Change Log 2026-06-27, §FR46](../planning-artifacts/architecture.md#L1152))
- **Per-item camera framing** (`cam_*` keys). Global today (renderer-owned `OrbitCamera`, reset on `SetAsset`); no per-item source of truth. Reaffirmed-deferred from 6.1/6.2 (`deferred-work.md`).
- **`time_offset` / `time_scale`** — native take state (`D_STARTOFFS` / `D_PLAYRATE`); Reaper persists/relocates these itself.
- **Panel/dock state** — Story 6.2 (done). **Per-item binding round-trip on the same machine** — Story 6.1 (done); this story is the **cross-machine / moved-folder** extension of it.
- **Reload of changed-on-disk files, graceful degradation hardening, FBX/Collada coverage** — Epic 8 (post-release).

## Tasks / Subtasks

- [x] **Task 0 — Verify-first in-Reaper (load-bearing, AR20):** before writing any code, establish the native baseline (AC2). The dev side establishes the baseline from the code + D9 and sets up the gate; the two in-Reaper observation rows are **executed by Antho** via `docs/PHASE4_VALIDATOR_GATE.md` §6. Do **not** pre-mark the gate PASS.
  - [x] Confirm from the code that the path rides the native mechanism: `GetFileName()` returns the real on-disk path ([pcm_source_anim.cpp:93](../../src/pcm_source_anim.cpp#L93)), `SetFileName()` re-probes and relinks ([pcm_source_anim.cpp:95-100](../../src/pcm_source_anim.cpp#L95)), and Reaper recreates the source via `CreateFromType("RAV_ANIM")` ([pcm_source_anim.cpp:175-181](../../src/pcm_source_anim.cpp#L175)) then `SetFileName`s the relinked path. Reaper writes a native `FILE "…"` line for any source whose `GetFileName()` is non-empty (ours is) → it is **eligible for native "copy media into project."** Record this as the expected-zero-code hypothesis. **[CONFIRMED 2026-06-27 — code read; hypothesis recorded in gate §6 + Spec Change Log.]**
  - [x] **(Antho, in-Reaper Windows)** Build a project with 2–3 RAV items, **Save with "copy media into project directory"** → confirm our `.glb`/`.fbx` files are **copied into the project's media folder** and the `.rpp` references the copied path. → Gate §6 row 1. **[PASS 2026-06-27]**
  - [x] **(Antho, in-Reaper Windows)** **Move the whole project folder** (simulating another machine — or copy to a second PC), reopen → **every item relinks and plays** under the playhead with **no forced re-import**, no manual relink. → Gate §6 row 2. **[PASS 2026-06-27]**
  - [x] Record the observed outcome in `docs/PHASE4_VALIDATOR_GATE.md` §6. **Expected:** native copy + relink already covers our items because they are file-backed (`GetFileName` non-empty) → **cross-machine portability is zero-code**, documented. **[Gate §6 authored with the expected-zero-code hypothesis; Result line PENDING for Antho's observed outcome.]**
- [x] **Task 1 — Missing-media diagnostic + isolation (AC3, AR17):** verify (and keep intact) that an **unresolvable** path surfaces a console diagnostic and isolates the failure.
  - [x] Confirm the existing load path already does this: `LoadAsset` checks `std::filesystem::exists` ([asset_loader.cpp:745](../../src/asset_loader.cpp#L745)) and returns a `LoadResult` with a category; the viewer's poll/load pipeline logs `LogError("load failed [%s]: %s", category, msg)` on failure ([viewer_window.cpp:208](../../src/viewer_window.cpp#L208)) and **advances the gate regardless** so one bad item never wedges the loop ([viewer_window.cpp:211-214](../../src/viewer_window.cpp#L211)). Other items continue to load on their own poll. → expected **already-satisfied**, documented, no code. **[CONFIRMED 2026-06-27 — code read: existence check + LogError + gate-advances-on-failure all present.]**
  - [x] **(Antho, in-Reaper Windows)** Deliberately leave one referenced file **unresolved** (e.g. open the moved project but delete/withhold one media file), move the playhead over that item → a **console diagnostic** names the path/category **and** the other items still play. → Gate §6 row 3. **[PASS 2026-06-27]**
- [x] **Task 2 — If (and only if) Task 0 finds a real gap:** make our source participate in the **native** mechanism — **no custom remap engine** (AC2). **[N/A — no gap can be found on the Linux dev box; the code already participates natively (file-backed `GetFileName`/`SetFileName`/`CreateFromType`). Pre-specified as the only-if-gap path: fires ONLY as a review follow-up if Antho's gate §6 row 2 finds a real moved-folder relink gap. Recorded in `deferred-work.md`.]**
  - [ ] Diagnose *which* native step failed: (a) "copy media" did **not** copy our files, or (b) relink did **not** call `SetFileName` on the moved path. Record the exact gap in the gate. *(only-if-gap — not triggered)*
  - [ ] Apply the **minimal** native-participation fix for the proven gap only (e.g. ensure `GetFileName`/`IsAvailable` report correctly so Reaper treats the source as copyable/relinkable). Do **not** invent path rewriting, a search-path engine, or a remap dialog. *(deferred — only-if-gap)*
- [x] **Task 3 — Documentation & records (always):**
  - [x] Append `docs/PHASE4_VALIDATOR_GATE.md` §6 (Story 6.3): click-based rows for Antho (copy-media embeds our items; moved-folder reopen relinks + plays; unresolved path → diagnostic + isolation; no-regression/scope-clean), Result line **PENDING** — never pre-mark PASS.
  - [x] Add an AR20 Spec Change Log entry in `architecture.md` recording the 6.3 decision (native copy/relink is authoritative; no custom remap engine; missing-media diagnostic rides the existing `LoadAsset`→`LogError` path; which path — zero-code vs only-if-gap — was taken).
  - [x] Add a `deferred-work.md` entry for any 6.3 deferral (custom remap engine explicitly *not built*; camera/`time_*` reaffirmed; "locate missing media" UX, if wanted, is post-release).
  - [x] Update `sprint-status.yaml` (6.3 → in-progress → review at dev time). When Antho's gate §6 passes, Epic 6 can close (6.1/6.2/6.3 all done).

## Dev Notes

### The crux: portability is Reaper's native media job, and our items are already file-backed

The single most important fact for this story: **cross-machine portability is delivered by Reaper's native "copy media into project directory" + relink, and our PCM_source already participates because it is file-backed.** Reaper writes a native `FILE "…"` line for **any** source whose `GetFileName()` returns a non-empty path — ours does ([pcm_source_anim.cpp:93](../../src/pcm_source_anim.cpp#L93)). On project load Reaper recreates the source via the registered `CreateFromType("RAV_ANIM")` ([pcm_source_anim.cpp:175-181](../../src/pcm_source_anim.cpp#L175)) and then calls `SetFileName()` with the (possibly relinked / copied-into-project) path, which re-probes the clip ([pcm_source_anim.cpp:95-100](../../src/pcm_source_anim.cpp#L95)). This is the **same native mechanism Story 6.1 already verified for the same-machine round-trip** — 6.3 just exercises it across a **moved folder / second machine**.

→ This is therefore a **verify-first (AR20), expected-zero-code** story, exactly like 6.2. **Do not build a remap engine.** FR46 was explicitly reformulated 2026-06-27 *off* the original "relative path + missing-media remap" plan *onto* native copy/relink ([architecture.md Spec Change Log, §FR46 amendment](../planning-artifacts/architecture.md#L1152)). The only sanctioned code path is the **only-if-gap** Task 2, and even that is "make the source participate in the *native* mechanism," never our own portability layer.

### Why "copy media into project directory" is the linchpin

Without "copy media," a project saved with **absolute** paths to animations sitting outside the project folder will not resolve on another machine — that is true of **all** Reaper media, not a deficiency of ours. The portability contract FR46 promises is specifically: *saved with "copy media into project directory" → opens on another machine.* When the media lives inside the project folder, Reaper relinks by the in-project relative location on reopen. Our job is only to ensure our source is **eligible** for that copy (file-backed, non-empty `GetFileName`) and **accepts** the relinked path (`SetFileName`) — both already true. Task 0 row 1 confirms the copy actually grabs our files; row 2 confirms the moved-folder reopen relinks them.

### The missing-media diagnostic already exists — do not add a second one

AC3 (a path that cannot be resolved surfaces a diagnostic and leaves the rest working) is **already satisfied by the existing load pipeline** — verify, don't rebuild:

- `LoadAsset(path)` opens with an existence check (`std::filesystem::exists`, [asset_loader.cpp:745](../../src/asset_loader.cpp#L745)) and returns a `LoadResult { asset?, category, message }` ([asset_loader.cpp:764-888](../../src/asset_loader.cpp#L764)).
- The viewer's transport-driven poll loads the current item's path and, **on failure, logs** `LogError("load failed [%s]: %s", LoadErrorCategoryName(r.category), …)` ([viewer_window.cpp:198-214](../../src/viewer_window.cpp#L198)) — a console diagnostic (AR16, never a popup).
- Crucially, the poll **advances the path gate whether the load succeeded or failed** ([viewer_window.cpp:211-214](../../src/viewer_window.cpp#L211)), so a missing file is not retried every frame and **does not wedge the render loop** — and every *other* item loads on its own poll when the playhead reaches it. That is per-item failure isolation (AR17 / FR37).

So the dev side is: **confirm this path covers a cross-machine "file not found"** and document it as the AC3 evidence. No new diagnostic, no new module. If Antho's gate §6 row 3 shows the diagnostic is unclear or an unresolved item disturbs the others, that becomes a review follow-up scoped to the existing `LoadAsset`/`viewer_window` path — still no remap engine.

### Files this story may touch

| File | Role | Likely change |
|---|---|---|
| `docs/PHASE4_VALIDATOR_GATE.md` | Phase 4 gate | **always** — append §6 (Story 6.3), click-based, Result PENDING |
| `_bmad-output/planning-artifacts/architecture.md` | Spec Change Log | **always** — AR20 entry for the 6.3 decision |
| `_bmad-output/implementation-artifacts/deferred-work.md` | Deferral ledger | **always** — 6.3 deferrals (remap engine not built; locate-missing-media UX post-release) |
| `_bmad-output/implementation-artifacts/sprint-status.yaml` | Sprint tracking | **always** — 6.3 status transitions (and Epic 6 close once §6 passes) |
| `src/pcm_source_anim.cpp` | PCM_source (`GetFileName`/`SetFileName`/`IsAvailable`) | **only if gap** — minimal native-participation fix, never a remap engine |

### Existing behavior to preserve (read before editing anything)

- **`GetFileName`/`SetFileName` are the native relink contract** ([pcm_source_anim.cpp:93-100](../../src/pcm_source_anim.cpp#L93)). `SetFileName` re-probes `m_len` so `GetLength` stays correct after a relink — keep that. `IsAvailable()` returns `true` ([pcm_source_anim.cpp:91](../../src/pcm_source_anim.cpp#L91)); if Task 2 ever touches availability, do not make a file-backed source report unavailable in a way that breaks relink.
- **The `file=` defensive fallback is `only-if-empty` and must stay that way** ([pcm_source_anim.cpp:156-164](../../src/pcm_source_anim.cpp#L156)). On a relinked/moved project, Reaper's native `SetFileName` sets `m_path` **first**; our stale `file=` from `LoadState` is then **skipped** because `m_path` is no longer empty — so the **native/relinked path is always authoritative** (this is the 6.3-safety guarantee 6.1 built in). Do **not** change the guard to override the native path — that would re-break cross-machine relink (a moved project would snap back to the old absolute path). This is the load-bearing interaction between 6.1 and 6.3.
- **`CreateFromType` matches only `kSourceType` (`"RAV_ANIM"`)** ([pcm_source_anim.cpp:64-67, :175-181](../../src/pcm_source_anim.cpp#L175)). The on-disk type tag is permanent; recreation-on-load depends on it. Don't rename it.
- **The poll/load pipeline ([viewer_window.cpp:183-214](../../src/viewer_window.cpp#L183)) is the restoration path** — it is unchanged by 6.1/6.2 and must stay unchanged here. Restoration "rides the existing 4.3–4.5 pipeline": once a source reappears with a relinked path, `GetCurrentAnimItem` finds it, `LoadAsset` loads it, the playhead drives the frame.

### Persistence pattern reference (6.1) — the 6.3 safety guard already shipped

Story 6.1 built the per-item `key=value` framework (`rav_ver=1`, defensive `file=` **only-if-empty**, `LoadState` ignores unknown keys, returns `0` never `-1`). The `only-if-empty` guard was explicitly designed **for 6.3**: it ensures our defensive `file=` never overrides the path Reaper relinks via `SetFileName`. So a large part of 6.3's correctness was **pre-paid in 6.1** — this story mostly *verifies* that the native relink wins and our defensive fallback steps aside. Mirror the *discipline* (versioned, forward-compat, native-authoritative), and do **not** weaken the guard.

### Testing standards

- No unit-test framework in this project; verification is an **isolated `-Wall -Wextra -Werror` harness** for any new pure logic (per 6.1/4.x precedent) plus a **host-stubbed `cmake -B <build> -S .`** configure (non-Windows target intentionally stubbed). If this story is docs-only (expected), the gate is the in-Reaper check.
- The real gate is **click-based, in-Reaper on Windows (AR19)** — `docs/PHASE4_VALIDATOR_GATE.md` §6, run by Antho. Validator hooks must be click-based (save with copy-media, move the folder, reopen, move the playhead), never env-vars/CLI/.bat editing. Never fabricate a PASS; Result stays PENDING until Antho confirms. Cross-machine relink, native copy-media, and the moved-folder reopen are only observable **in-Reaper on Windows** — the Linux dev box can confirm only the source/scope audit (no remap engine added, `only-if-empty` guard intact, at most `pcm_source_anim.cpp` touched), an isolated `-Werror` compile if any logic is added, and a host-stubbed cmake configure.

### Architecture compliance (guardrails)

- **AR20 — verify-first.** Establish the native baseline *before* writing code; the in-Reaper check decides zero-code vs only-if-gap. Record the decision in the story + Spec Change Log.
- **AR17 — failure isolation.** An unresolved path must leave the rest of the session working; the existing poll/load pipeline already isolates per-item (gate advances on failure).
- **AR16 — console-only diagnostics.** Missing-media → `LogError` via `console_log.h`, never a popup. Already the case.
- **AR15 — symmetric register/unregister.** No new `rec->Register`. `SaveState`/`LoadState`/`GetFileName`/`SetFileName`/`CreateFromType` are PCM_source virtuals / factory callbacks Reaper drives — not registrations. `plugin_main.cpp` stays untouched; `-pcmsrc` stays symmetric with the same `&g_reg` pointer.
- **AR18 — main-thread, no-throw at the host edge.** Factory callbacks already `try/catch` ([pcm_source_anim.cpp:177-181, :187-191](../../src/pcm_source_anim.cpp#L177)); keep any new code inside that no-throw boundary.
- **NFR-R2 — no project data loss.** `SaveState` writes only our own lines; never the uppercase native `FILE` line, never other Reaper state. The native copy/relink is Reaper's own data path — we don't mutate it.
- **No custom remap engine** — the defining boundary of this story.

### Project Structure Notes

- **Do not create new files.** No `project_state.{h,cpp}`, no portability/remap module. The architecture once named a hypothetical `project_state.*` for "project-level state hooks if needed" — cross-machine portability is **native**, so no new module is warranted. Match the existing flat `src/` layout.
- No `CMakeLists.txt` change expected (no new translation unit in the likely path). No new `REAPERAPI_WANT_*` symbol — `GetFileName`/`SetFileName` are PCM_source virtuals, not Reaper API imports.

### References

- [Source: epics.md#Story 6.3 (:591-606)] — story statement + AC (native copy/relink, verify-first AR20, missing-media diagnostic + isolation).
- [Source: prd.md#FR46 (:431)] — reopen on a different machine without forced re-import, via native copy-into-project + relink (`GetFileName`/`SetFileName`); no custom remap engine; final mechanism = Story 6.3.
- [Source: architecture.md#D9 Persistence layering (~:317-330)] — per-item state via `SaveState`/`LoadState`; path rides the native `FILE`/`GetFileName`/`SetFileName` mechanism.
- [Source: architecture.md#Spec Change Log 2026-06-27 (~:1146-1156, §FR46 amendment :1152)] — FR46 reformulated off a home-grown remap onto native Reaper media copy/relink; realized by Story 6.3 (AR20 decision recorded in the story spec).
- [Source: architecture.md#Spec Change Log 2026-06-27 (~:1138-1144)] — Story 6.1 realized `SaveState`/`LoadState`; the `file=` `only-if-empty` guard is explicitly "6.3-safe" so native relink always wins.
- [Source: src/pcm_source_anim.cpp:91-100, :115-166, :175-181] — `IsAvailable`/`GetFileName`/`SetFileName`, `SaveState`/`LoadState`/`ApplyStateLine` (`only-if-empty` `file=` guard), `CreateFromType`.
- [Source: src/viewer_window.cpp:183-214] — transport poll → `LoadAsset` → `SetAsset`/`LogInfo` on success, `LogError("load failed …")` on failure, gate advances regardless (per-item isolation).
- [Source: src/asset_loader.cpp:745, :764-888] — `LoadAsset` existence check + `LoadResult{category,message}`.
- [Source: deferred-work.md (story 6.1 / 6.2 entries)] — camera global/deferred; `time_offset`/`time_scale` native; remap engine not part of MVP.
- [Source: docs/PHASE4_VALIDATOR_GATE.md §3-§5] — gate format to mirror for §6 (click-based rows, Result PENDING, scope note).
- [Source: 6-1 / 6-2 story Dev Agent Records] — verify-first AR19/AR20 pattern, `-Werror` harness + host-stubbed cmake, scope/boundary grep audit, PENDING-gate discipline (never fabricate PASS).

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Opus 4.8, 1M context) — BMAD dev-story workflow.

### Debug Log References

- Scope audit: `git diff --name-only | grep '^src/'` → **NONE** (no `src/` change — docs-only confirmed).
- `only-if-empty` guard intact: `grep -n "m_path.empty() && !value.empty()" src/pcm_source_anim.cpp` → `:163` (unchanged).
- Host-stubbed configure: `cmake -B /tmp/rav_build -S .` → exit 0 ("non-Windows platform detected — Build target stubbed", configuring/generating done).

### Completion Notes List

**Verify-first (AR20) outcome: zero-code / docs-only — exactly as hypothesized (mirrors 6.2).** The whole cross-machine portability contract (FR46) is delivered by Reaper's **native** "copy media into project directory" + relink, and our `PCM_source` already participates because it is file-backed. Confirmed by reading the code, not by assumption:

- ✅ **Native relink contract present and correct:** `GetFileName()` returns the real on-disk path ([pcm_source_anim.cpp:93](../../src/pcm_source_anim.cpp#L93)); `SetFileName()` re-probes `m_len` and relinks ([:95-100](../../src/pcm_source_anim.cpp#L95)); Reaper recreates via `CreateFromType("RAV_ANIM")` ([:175-181](../../src/pcm_source_anim.cpp#L175)). A non-empty `GetFileName()` makes our items eligible for "copy media into project."
- ✅ **6.3-safety guard intact:** the defensive `file=` fallback is `only-if-empty` ([:160-163](../../src/pcm_source_anim.cpp#L160)) — on a relinked/moved project Reaper's native `SetFileName` sets `m_path` first, so our stale `file=` is skipped and the **native/relinked path is always authoritative**. NOT weakened (would re-break cross-machine relink). `SaveState` writes only our own lines, never the native `FILE` line (NFR-R2).
- ✅ **Missing-media diagnostic already exists (AC3/AR17) — not rebuilt:** `LoadAsset` checks `std::filesystem::exists` ([asset_loader.cpp:745](../../src/asset_loader.cpp#L745)); the transport poll logs `LogError("load failed [%s]: %s", …)` on failure and **advances the path gate whether load succeeded or failed** ([viewer_window.cpp:198-214](../../src/viewer_window.cpp#L198)) — a bad item never wedges the loop or retries every frame, and every other item loads on its own poll (per-item isolation, console-only per AR16).
- ✅ **No remap engine** (the defining boundary), no new file, no `CMakeLists`/`reaper_api.h`/`plugin_main.cpp` change, no new `REAPERAPI_WANT_*`/registration. `-pcmsrc` stays symmetric.

**Task 2 (native-participation fix) is N/A on the Linux dev box** — no gap is observable here; it is pre-specified as an only-if-gap review follow-up fired ONLY if Antho's gate §6 row 2 finds a real moved-folder relink gap (recorded in `deferred-work.md`).

**The gate is Antho's in-Reaper Windows check (AR19), `docs/PHASE4_VALIDATOR_GATE.md` §6, Result PENDING** — never fabricated. Cross-machine relink / native copy-media / moved-folder reopen are only observable in-Reaper on Windows; the Linux box confirmed only the source/scope audit + host-stubbed configure. When §6 passes, 6.3 → done and **Epic 6 closes** (6.1/6.2/6.3 all done).

### File List

- `docs/PHASE4_VALIDATOR_GATE.md` — appended §6 (Story 6.3), click-based rows, Result **PENDING**.
- `_bmad-output/planning-artifacts/architecture.md` — AR20 Spec Change Log entry for the 6.3 decision (native copy/relink authoritative; no remap engine; zero-code).
- `_bmad-output/implementation-artifacts/deferred-work.md` — 6.3 deferrals (remap engine explicitly not built; locate-missing-media UX post-release; only-if-gap native-participation fix; camera/`time_*` reaffirmed).
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — 6.3 status transitions (ready-for-dev → in-progress → review).
- `_bmad-output/implementation-artifacts/6-3-cross-machine-portability-via-native-reaper-media.md` — this story (tasks, Dev Agent Record, Change Log, Status).

*No `src/` file changed (docs-only — the expected verify-first/zero-code path).*

## Change Log

| Date | Change |
|---|---|
| 2026-06-27 | Dev-story: verify-first (AR20) → **docs-only / zero-code** (mirrors 6.2). Confirmed from code that native copy/relink + the `only-if-empty` `file=` guard + the existing `LoadAsset`→`LogError` missing-media diagnostic already deliver FR46 cross-machine portability — no remap engine, no `src/` change. Authored gate §6 (PENDING), AR20 Spec Change Log entry, deferred-work entry. Status → review; pending Antho's in-Reaper Windows gate §6 (after which Epic 6 closes). |
| 2026-06-27 | **Antho in-Reaper Windows gate §6 PASSED** (AR19): save w/ "copy media" embeds our items → move project folder → reopen → every item relinks + plays, no forced re-import; one unresolved path → console diagnostic + others keep playing. AC1–AC3 confirmed. Gate §6 Result PENDING→PASS; Task 0/1 Antho rows checked. **Status → done; Epic 6 → done** (6.1/6.2/6.3 all done — Phase 4 save/recall complete). |
