---
baseline_commit: dc312831e21b28e661e2137060521a3ce942ebb9
---

# Story 6.5.2: Silence all console logging by default

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want no `[RAV]` console spam from the viewer during normal use,
so that Reaper's console stays clean — while the diagnostic capability is preserved for debug builds.

## Acceptance Criteria

1. **No console output in normal use (FR16/AR16 amendment).** **Given** a normal build and normal operation **When** I load, play (transport-driven), and save/reopen animations **Then** **no** `[RAV]` line is produced in Reaper's console — every one of the ~43 `Log{Info,Warn,Error}` call-sites is silenced **at the single `Emit()` funnel** ([src/console_log.cpp:14-22](../../src/console_log.cpp#L14-L22)), **not** by deleting call-sites. (The FPS log at [viewer_window.cpp:237](../../src/viewer_window.cpp#L237), the load-failure log at [viewer_window.cpp:208](../../src/viewer_window.cpp#L208), and all loader/PCM_source/plugin logs all go silent because they all route through `Emit()`.)

2. **Funnel retained, re-enablable in a debug build.** **Given** the funnel **When** the project is built with the diagnostic switch defined **Then** all logging returns to the prior `[RAV] <level>: <message>` console behaviour, byte-for-byte. The funnel and every call-site are **kept**; only `Emit()`'s body is compiled out by default (a compile-time switch, mirroring the existing `RAV_FORCE_INIT_FAILURE` pattern at [gl_loader.cpp:15](../../src/gl_loader.cpp#L15)). **This amends AR16** (console-only → silent-by-default + on-canvas signals) — the diagnostic ability is *gated*, never deleted.

3. **User-facing signals are not orphaned.** **Given** that this story removes the console channel **When** the FPS figure and the load-failure signal lose their console output **Then** they are explicitly **rehomed on-canvas** rather than lost: the **FPS readout** lands in Story **6.5.5** (on-canvas, top-right), and a **minimal on-canvas load-failure indication** also lands in Story **6.5.5** (it reuses the same on-canvas text-overlay mechanism the FPS readout introduces — that overlay does not exist until 6.5.3/6.5.5). This story records the deferral in `deferred-work.md` and the gate doc so neither signal is silently dropped before ship. *(Epic 6.5 is pre-ship: 6.5.5 completes before Epic 7, so the on-canvas signals exist by ship; the window between 6.5.2 and 6.5.5 is dev-time only.)*

### Out of scope (explicitly deferred — do NOT implement here)

- **Building the on-canvas FPS readout or the on-canvas load-failure indication** — both belong to Story **6.5.5** (they need the on-canvas text-overlay mechanism, which depends on the sidebar/overlay infra from 6.5.3; none of it exists yet). This story only **silences** the console and **records** the rehome. Do **not** add any GL-overlay/Win32-text drawing here.
- **Deleting or rewriting any `Log*` call-site** — the AC mandates silencing *at the funnel*. Leave all ~43 call-sites (incl. the FPS block and the load-failure block in `viewer_window.cpp`) exactly as they are. `viewer_window.cpp` must net to **zero** change.
- **Removing the FPS *measurement*** (the `QueryPerformanceCounter` / `g_frame_count` block at [viewer_window.cpp:232-240](../../src/viewer_window.cpp#L232)) — 6.5.5 reuses it to drive the on-canvas readout. Only its `LogInfo` becomes a silenced funnel call; leave the block intact.
- **Tool sidebar / light tool** (6.5.3), **floor tool** (6.5.4), **render-quality toggles** (6.5.5) — unrelated.
- **Changing the log line format** (`[RAV] <level>: <message>`, D7) or the level set (info/warn/error) — only its *default visibility* changes.
- **`plugin_main.cpp`, `reaper_api.*`, `pcm_source_anim.*`, `asset_loader.*`, `renderer.*`, `scene.h`, `gl_loader.*`, `CMakeLists.txt`** — none need to change. The whole functional change is inside `console_log.cpp` (+ its header doc-comment).

## Tasks / Subtasks

- [x] **Task 1 — Gate the `Emit()` funnel to silent-by-default (AC1, AC2).** This is the entire functional change.
  - [x] In [src/console_log.cpp](../../src/console_log.cpp), wrap the **body** of `Emit()` (the `vsnprintf` → `snprintf` → `ShowConsoleMsg` sequence, lines 16-21) in `#ifdef RAV_ENABLE_CONSOLE_LOG … #endif`. When the macro is **undefined** (the default / normal build), `Emit()` does nothing. Keep the function signature and the `Log{Info,Warn,Error}` wrappers (with their `va_start`/`va_end`) **unchanged** — they still call `Emit()`, which is now empty. This silences **all** call-sites at one point (AC1 "via the single funnel").
  - [x] In the disabled branch, suppress `/W3 /permissive-` unused-parameter warnings (`level`, `fmt`, `args`) — e.g. `(void)level; (void)fmt; (void)args;` inside the `#else`/after the `#ifdef`. (MSVC `/W3` does not emit C4100 for unused params, but be explicit so a future `/W4` bump stays clean and the intent is obvious. Do **not** remove the params — the debug branch needs them.)
  - [x] Choose the **positive opt-in** macro name `RAV_ENABLE_CONSOLE_LOG` (undefined = silent; defined = logs). This mirrors the project's existing compile-time test switch `RAV_FORCE_INIT_FAILURE` ([gl_loader.cpp:15](../../src/gl_loader.cpp#L15)), flipped via `-DCMAKE_CXX_FLAGS="/D RAV_ENABLE_CONSOLE_LOG"`. Add a short comment block above `Emit()` explaining the default-off rationale (AR16 amended) and the flip-on flag, in the same spirit as the `RAV_FORCE_INIT_FAILURE` comment.
- [x] **Task 2 — Update the `console_log.h` doc-comment (AC2).** The header's top comment ([console_log.h:3-5](../../src/console_log.h#L3)) currently calls these "the project's single user-feedback channel (AR16)" — no longer true after this story. Update it to: silent-by-default diagnostic helpers (compiled to a no-op unless `RAV_ENABLE_CONSOLE_LOG` is defined); user-facing signals are surfaced on-canvas (FR53 / 6.5.5), not the console; the `[RAV] <level>: <message>` D7 format still applies *when logging is compiled in*. Keep the function declarations unchanged.
- [x] **Task 3 — Validator re-enable build script (AC2, click-based).** Add a one-click `build_debuglog.bat` at repo root, modelled on the existing [build_forcefail.bat](../../build_forcefail.bat) (CRLF line endings; `cd /d "%~dp0"`; close-Reaper warning + `pause`; flat `errorlevel` gotos), that configures with `-DCMAKE_CXX_FLAGS="/D RAV_ENABLE_CONSOLE_LOG"`, builds Release, and installs the DLL to `%APPDATA%\REAPER\UserPlugins\`. French operator text like the other scripts. This gives Antho a **click-based** way to *prove* the funnel still works (debug build → console logs return) and to revert by re-running `build.bat`. (Per the project's validator-hook convention: hooks are click-based, never env-var/CLI editing.)
- [x] **Task 4 — Gate doc §2 (always).** Append a **Story 6.5.2** section to [docs/PHASE4.5_VALIDATOR_GATE.md](../../docs/PHASE4.5_VALIDATOR_GATE.md) with click-based rows for Antho on Windows:
  - [x] Row: run the **normal** `build.bat`, open the viewer, **load → play (move the playhead) → save & reopen** a project with a couple of animation items, and open Reaper's console (Extensions ▸ Show console / `View ▸ Show console output`) → it shows **no `[RAV]` lines** (AC1). Include an item with a **deliberately missing/broken file** to confirm even the failure path is silent (the load-failure rehome is noted as 6.5.5).
  - [x] Row (optional, proves AC2): run `build_debuglog.bat`, repeat → `[RAV]` lines reappear in the prior format; then re-run `build.bat` to return to the shipping silent build.
  - [x] **Result line PENDING** — never pre-mark PASS. *(Recorded as PENDING in gate §5.)*
- [x] **Task 5 — Spec reconciliation (always).** Record the invariant change in the docs:
  - [x] Add an **AR20 Spec Change Log** entry in [architecture.md](../planning-artifacts/architecture.md) (the "Spec Change Log" block, ~[architecture.md:1131-1171](../planning-artifacts/architecture.md#L1131)): `Emit()` funnel now compiled to a no-op by default (`RAV_ENABLE_CONSOLE_LOG` opt-in); console silenced in normal builds; FPS + load-failure rehomed on-canvas in 6.5.5. AR16 (already amended at [architecture.md:93](../planning-artifacts/architecture.md#L93)) is now realised in code.
  - [x] D7 ([architecture.md:278-290](../planning-artifacts/architecture.md#L278)) and the "log.* is the only `ShowConsoleMsg` wrapper" note ([architecture.md:879](../planning-artifacts/architecture.md#L879)) still read "ShowConsoleMsg is the only output channel". They describe the **format**, which is unchanged; add a one-line note that the channel is **silent by default per AR16** (the format applies only when logging is compiled in). Do not rip out D7 — the format invariant still holds for debug builds and the on-canvas signals are separate.
  - [x] Add a [deferred-work.md](../../_bmad-output/implementation-artifacts/deferred-work.md) entry: **on-canvas FPS readout (FR53)** and **minimal on-canvas load-failure indication** → Story 6.5.5 (both need the on-canvas text-overlay introduced there); record that 6.5.2 silenced the console and these are the rehome targets, so they are not silently dropped.
- [x] **Task 6 — Build sanity (always).** Linux `cmake --build` configures/compiles the non-`_WIN32` TUs; `console_log.cpp` has **no** `#ifdef _WIN32` guard (it includes `reaper_api.h` for `ShowConsoleMsg`), so it **does** compile on the Linux box — build it both ways to be safe: default (silent) and with `-DCMAKE_CXX_FLAGS="-D RAV_ENABLE_CONSOLE_LOG"` (note the GCC `-D` form on Linux vs MSVC `/D`) to confirm both branches compile clean. No `CMakeLists.txt` change. Update `sprint-status.yaml` (6.5.2 → review at dev time). *(Note: the real `reaper_api.h` pulls in the Reaper SDK → `WDL/swell` which is Windows-only and not vendored, so a full TU compile fails on Linux on the SDK include — a pre-existing environmental limit unrelated to this change. Validated instead via an **isolated harness** [src/console_log.{cpp,h} + a 3-line `ShowConsoleMsg` stub], compiling **both branches** under `-Wall -Wextra -Werror` clean and running each: default → no output; `-D RAV_ENABLE_CONSOLE_LOG` → byte-for-byte `[RAV] <level>: <message>`.)*

## Dev Notes

### The crux: one funnel, one `#ifdef` — the smallest possible change

Every `[RAV]` line in the codebase is produced by exactly one function — `Emit()` in [console_log.cpp:14-22](../../src/console_log.cpp#L14-L22). `Log{Info,Warn,Error}` are thin `va_list` wrappers that all call it, and `Emit()` is the **only** place in `src/` that calls `ShowConsoleMsg` (verified: a repo-wide grep finds `ShowConsoleMsg` only in `console_log.cpp` and the `REAPERAPI_WANT_ShowConsoleMsg` define in `reaper_api.h`; there is **no** stray `printf`/`fprintf`/`std::cout`/`OutputDebugString` anywhere — the only `printf` token is a *comment* in `console_log.h`). So gating `Emit()`'s body silences 100% of the ~43 call-sites in one edit. **Do not** touch the call-sites — the AC is explicit that silencing happens at the funnel, which also means the diagnostic capability comes back wholesale when the switch is flipped.

### Follow the existing compile-time-switch precedent (do not invent a new mechanism)

The project already has a compile-time diagnostic switch: `RAV_FORCE_INIT_FAILURE` ([gl_loader.cpp:15-25](../../src/gl_loader.cpp#L15)), default-undefined, flipped on with `-DCMAKE_CXX_FLAGS="/D RAV_FORCE_INIT_FAILURE"` and driven by a dedicated one-click `build_forcefail.bat`. Mirror it exactly: a `#ifdef`-gated body, a default-off macro (`RAV_ENABLE_CONSOLE_LOG`), and a sibling `build_debuglog.bat`. Same comment style ("TEST/DEBUG-only — default OFF, never `#defined` in a normal build, costs nothing"). Reusing the established pattern keeps the build story consistent and avoids a new CMake option (no `CMakeLists.txt` change needed — the flag is passed ad-hoc by the debug build script, exactly like force-fail).

### Why `viewer_window.cpp` must net to zero

The tempting-but-wrong move is to "clean up" the FPS log block ([viewer_window.cpp:232-240](../../src/viewer_window.cpp#L232)) or the load-failure log ([viewer_window.cpp:208](../../src/viewer_window.cpp#L208)). **Don't.** (1) The AC mandates silencing at the funnel, so those `LogInfo`/`LogError` calls simply go quiet on their own. (2) Story 6.5.5 **reuses** the FPS measurement (`QueryPerformanceCounter` + `g_frame_count` + the `elapsed >= 1.0` cadence) to drive the on-canvas readout — deleting it now would just have to be rebuilt. (3) The load-failure block's *gate-advance-on-failure* logic ([viewer_window.cpp:211-214](../../src/viewer_window.cpp#L211)) is load-bearing (it stops a per-frame retry storm, AR17) and is unrelated to whether the log prints. Leaving `viewer_window.cpp` untouched is both correct and the cleanest scope boundary.

### The load-failure-indication scoping decision (read this — it answers the obvious "is AC3 asking me to build something?")

AC3 / the AR16 amendment ([architecture.md:93](../planning-artifacts/architecture.md#L93)) say a "minimal load-failure indication" is surfaced **on-canvas**. That on-canvas indication is **not built in this story** — and that is deliberate, not an omission:

- There is **no on-canvas text/overlay mechanism today** — the viewport is pure 3D GL with one native "Reset View" child control. Drawing readable text over the GL canvas is new infrastructure that arrives with the **sidebar/overlay work in 6.5.3** and the **FPS readout in 6.5.5**.
- Building a one-off load-failure overlay *here*, before that mechanism exists, would mean inventing text-over-GL from scratch only to throw it away — exactly the "reinvent the wheel" trap.
- So both on-canvas signals (FPS **and** load-failure) are co-located in **6.5.5**, sharing one overlay mechanism. AC3's "not orphaned" is honoured by an **explicit, tracked deferral** (gate doc §2 + `deferred-work.md`), not by a half-built feature.
- This is ship-safe: Epic 6.5 is pre-ship and 6.5.5 lands before Epic 7, so the on-canvas signals exist by ship. The only window without a load-failure signal is **dev-time between 6.5.2 and 6.5.5**.

If Antho would rather keep the console load-failure log alive until 6.5.5 lands the on-canvas version (a narrower silencing), that is a one-line variation — see the Question at the end. The story as written silences everything now and rehomes in 6.5.5.

### `/W3 /permissive-` and the disabled branch

With the body compiled out, `Emit`'s params are unreferenced. MSVC `/W3` (the project's level) does **not** warn on unused function parameters (C4100 is `/W4`), so the build stays clean either way — but add `(void)`-casts anyway so the intent is explicit and a future `/W4` bump (NFR-R5 keeps warnings tight) doesn't regress. The `va_start`/`va_end` pairing in the wrappers stays balanced regardless (they're outside the `#ifdef`).

### Regression guardrails (the system must stay working end-to-end)

- **No behaviour change beyond output visibility.** Silencing a log must not alter control flow. The load-failure path's gate-advance, the per-item isolation (AR17), the transport poll, the save/reopen (D9) — all untouched. A silenced `LogError` is still a *return-path-neutral* call.
- **Debug build is a true superset.** With `RAV_ENABLE_CONSOLE_LOG` defined, output is byte-for-byte the prior `[RAV] <level>: <message>` (D7) — Task 1 only `#ifdef`s the body, it does not edit the format strings.
- **No new dependency / boundary change.** No `REAPERAPI_WANT_*`, no `rec->Register`, no `CMakeLists.txt` edit (AR15 unaffected). `console_log.cpp` keeps `#include "reaper_api.h"` for `ShowConsoleMsg` (used only inside the debug branch — that's fine, the include is harmless when the branch is off).
- **Single-DLL deliverable unchanged** (D15/D17) — this is a source-level `#ifdef`, no new TU, no new lib.

### Validation = Antho's in-Reaper Windows gate (AR19)

The payoff ("console is quiet") is only observable **in Reaper on Windows** — the Linux box compiles `console_log.cpp` but cannot open Reaper's console. Dev implements + self-reviews the `#ifdef`, builds both branches on Linux to confirm they compile, and authors `docs/PHASE4.5_VALIDATOR_GATE.md` §2 with **Result PENDING**. The story closes when Antho confirms (a) the normal build produces no `[RAV]` console output across load/play/save, and optionally (b) `build_debuglog.bat` brings the logs back. **Never fabricate a PASS.**

### Project Structure Notes

- Files touched: `src/console_log.cpp` (gate `Emit()` body), `src/console_log.h` (doc-comment). New: `build_debuglog.bat` (root, optional validator re-enable). Docs: `docs/PHASE4.5_VALIDATOR_GATE.md` (§2), `architecture.md` (AR20 Spec Change Log + light D7/`:879` note), `deferred-work.md` (FPS + load-failure → 6.5.5), `sprint-status.yaml`.
- **No** `src/` file other than `console_log.{cpp,h}` changes. `viewer_window.cpp` nets to zero. No `CMakeLists.txt` change.
- Naming: `RAV_ENABLE_CONSOLE_LOG` follows the `RAV_*` compile-switch convention (`RAV_FORCE_INIT_FAILURE`). `[RAV]` (not the architecture's legacy `[FBXAV]`/`log.h` Phase-0.5 naming) is the shipped prefix — match the code, not the stale doc.

### References

- [epics.md §Epic 6.5 / Story 6.5.2](../planning-artifacts/epics.md#L644) — user story + ACs (the "single `Emit()` funnel" + "no-op by default, re-enablable in debug" + "FPS/load-failure rehomed on-canvas" wording).
- [Sprint Change Proposal — Epic 6.5](../planning-artifacts/sprint-change-proposal-2026-06-27-epic-6-5.md#L91) — Story 6.5.2 block; the AR16 amendment resolution (line 46: "funnel kept but made a no-op by default (compile-time switch)"); the "43 sites, one helper `Emit()`" + "FPS today in console → motivates on-canvas FPS" evidence (lines 31-32); sequencing "6.5.2 anytime but FPS rehome lands in 6.5.5" (line 142).
- [architecture.md:93](../planning-artifacts/architecture.md#L93) — AR16 cross-cutting concern #4, **already amended** to "diagnostics silent by default; user-facing signals on-canvas (FPS + minimal load-failure)". This story realises it in code.
- [architecture.md:278-290](../planning-artifacts/architecture.md#L278) — D7 console format (unchanged; applies when compiled in). [architecture.md:879](../planning-artifacts/architecture.md#L879) — "log.* is the only `ShowConsoleMsg` wrapper" invariant (still true).
- Code: [console_log.cpp:14-22](../../src/console_log.cpp#L14) (`Emit()` funnel — the single edit point), [console_log.h:3-15](../../src/console_log.h#L3) (doc-comment + decls); [gl_loader.cpp:15-25](../../src/gl_loader.cpp#L15) (`RAV_FORCE_INIT_FAILURE` precedent to mirror); [build_forcefail.bat](../../build_forcefail.bat) (script template for `build_debuglog.bat`); [viewer_window.cpp:208](../../src/viewer_window.cpp#L208) (load-failure log — leave as-is, silenced via funnel), [viewer_window.cpp:232-240](../../src/viewer_window.cpp#L232) (FPS log+measurement — leave as-is, 6.5.5 reuses measurement).

## Review Findings (code review 2026-06-28 — BMAD 3-layer)

3-layer adversarial review (Blind Hunter / Edge Case Hunter / Acceptance Auditor). **The core change is sound** — all four central invariants independently confirmed against `src/`: (INV-1) `Emit()` is the single funnel and the only `ShowConsoleMsg` caller — repo-wide grep finds no `printf`/`cout`/`OutputDebugString`/`MessageBox` escape path, so gating the body silences 100% of the 41 `Log*` call-sites; (INV-2) `viewer_window.cpp` nets to zero (not in the diff); (INV-3) silencing is return-path-neutral — the load-failure gate-advance at `viewer_window.cpp:214` sits outside the `if/else`, unchanged; (INV-4) the compiled-in branch is byte-for-byte the prior `[RAV] %s: %s\n`. Acceptance Auditor: AC1/AC2/AC3 all PASS, every out-of-scope boundary respected, gate Result correctly PENDING (no fabricated PASS).

- [x] [Review][Patch] `build_debuglog.bat` uses the Unix `>/dev/null` redirect instead of cmd.exe `>nul` — cmd.exe redirects stdout to a literal `\dev\null` path that does not exist; the canonical `build.bat` uses the correct `>nul 2>nul` [`build_debuglog.bat` — the `where cmake >/dev/null 2>nul` line] — **FIXED** (→ `where cmake >nul 2>nul`)
- [x] [Review][Defer] `build_forcefail.bat` carries the same `>/dev/null` Unix-ism (the template `build_debuglog.bat` was copied from) [`build_forcefail.bat:21`] — deferred, pre-existing (untracked dev-only build script, belongs to the force-fail story)

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Opus 4.8, 1M context) — BMAD dev-story workflow.

### Debug Log References

Isolated both-branch compile + run harness (the real `reaper_api.h` pulls in the Reaper SDK → `WDL/swell`, Windows-only, not vendored on the Linux box, so a full TU compile fails on the SDK include — a pre-existing environmental limit, not this change):

- Copied `src/console_log.{cpp,h}` into `/tmp/cl_iso/` with a 3-line stub `reaper_api.h` (`extern "C" void ShowConsoleMsg(const char*)`) and a `stub_main.cpp` (defines `ShowConsoleMsg`→`fputs`, calls `LogInfo/Warn/Error`).
- **Branch A — default (silent):** `g++ -std=c++17 -Wall -Wextra -Werror -c console_log.cpp` → compile clean (no unused-param warning); link + run → **no output** (silent ✓ AC1).
- **Branch B — `-D RAV_ENABLE_CONSOLE_LOG`:** same flags → compile clean; link + run → `[RAV] info: hello 42` / `[RAV] warn: warn x` / `[RAV] error: err` — **byte-for-byte the prior D7 format** (✓ AC2).

Scope audit (`git diff --stat`): only `src/console_log.{cpp,h}` changed in `src/`; `viewer_window.cpp` nets to **zero**; no `CMakeLists.txt`/`cmake/` change. `grep ShowConsoleMsg src/` → the lone call is `console_log.cpp:33` inside the gated `Emit()` (single-funnel invariant intact); `build.bat` does **not** define the macro (silent is the shipped default).

### Completion Notes List

- **The whole functional change is one `#ifdef`.** `Emit()`'s body (`vsnprintf`→`snprintf`→`ShowConsoleMsg`) is wrapped in `#ifdef RAV_ENABLE_CONSOLE_LOG`; the `#else` branch `(void)`-casts `level`/`fmt`/`args` so a future `/W4` (or `-Wunused-parameter`) bump stays clean. The `Log{Info,Warn,Error}` `va_list` wrappers are **unchanged** — they still call `Emit()`, now empty by default. All ~43 call-sites go silent at the single funnel; **none deleted** (AC1).
- **Positive opt-in macro `RAV_ENABLE_CONSOLE_LOG`** (undefined = silent; defined = logs), mirroring the existing `RAV_FORCE_INIT_FAILURE` switch — default-off, costs nothing, no `CMakeLists` change (passed ad-hoc by `build_debuglog.bat`). When defined, output is byte-for-byte the prior `[RAV] <level>: <message>` (AC2).
- **`console_log.h` doc-comment** updated: no longer "the project's single user-feedback channel" — now silent-by-default helpers, on-canvas signals (FR53 / 6.5.5), D7 format applies only when compiled in. Declarations unchanged.
- **`build_debuglog.bat`** (new, repo root, CRLF, French operator text) mirrors `build_forcefail.bat`: configures with `/D RAV_ENABLE_CONSOLE_LOG`, builds Release, installs to `%APPDATA%\REAPER\UserPlugins\` — click-based proof the funnel still works; re-run `build.bat` to revert to the silent shipping build.
- **`viewer_window.cpp` nets to zero** (verified): the FPS log ([viewer_window.cpp:237](../../src/viewer_window.cpp#L237)) and load-failure log ([viewer_window.cpp:208](../../src/viewer_window.cpp#L208)) go quiet via the funnel; the FPS **measurement** block and the load-failure **gate-advance** (AR17) are left intact for 6.5.5 / per-item isolation. No behaviour change beyond output visibility.
- **AC3 (signals not orphaned)** honoured by explicit tracked deferral, not a half-built feature: on-canvas **FPS readout (FR53)** and **minimal load-failure indication** → **Story 6.5.5** (shared text-overlay mechanism; no overlay infra exists until 6.5.3/6.5.5). Recorded in `deferred-work.md` + gate §5. Ship-safe: 6.5.5 lands before Epic 7; the only signal-less window is dev-time between 6.5.2 and 6.5.5.
- **Spec reconciliation:** AR20 Spec Change Log entry added (2026-06-28); one-line silent-by-default notes added at D7 ([architecture.md:290](../planning-artifacts/architecture.md#L290)) and the `log.*`-only-`ShowConsoleMsg`-wrapper invariant ([architecture.md:879](../planning-artifacts/architecture.md#L879)); AR16 (already amended at [architecture.md:93](../planning-artifacts/architecture.md#L93)) is now realised in code.
- **Boundary rule (AR15) intact:** no `rec->Register`, no new `REAPERAPI_WANT_*`, no `CMakeLists`/`plugin_main`/`reaper_api`/`pcm_source`/`asset_loader`/`renderer`/`gl_loader` change.
- **Gate:** `docs/PHASE4.5_VALIDATOR_GATE.md` §5, Result **PENDING** — Antho's in-Reaper Windows console observation IS the gate (AR19); the Linux box compiles both branches but cannot open Reaper's console. Never fabricate a PASS.

### File List

- `src/console_log.cpp` — gated `Emit()` body behind `#ifdef RAV_ENABLE_CONSOLE_LOG` (silent default; `(void)`-cast params in `#else`); added the default-off rationale comment block.
- `src/console_log.h` — rewrote the top doc-comment to silent-by-default diagnostic helpers (declarations unchanged).
- `build_debuglog.bat` — **new**, repo root (CRLF). One-click debug build that re-enables console logs (`/D RAV_ENABLE_CONSOLE_LOG`); mirrors `build_forcefail.bat`.
- `docs/PHASE4.5_VALIDATOR_GATE.md` — appended §5 (Story 6.5.2 console-silence gate, Result PENDING).
- `_bmad-output/planning-artifacts/architecture.md` — AR20 Spec Change Log entry (2026-06-28) + silent-by-default notes at D7 and the `:879` invariant.
- `_bmad-output/implementation-artifacts/deferred-work.md` — story-6.5.2 entry (on-canvas FPS + load-failure rehome → 6.5.5).
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — 6.5.2 → in-progress → review; `last_updated`.

## Change Log

| Date | Version | Description |
|------|---------|-------------|
| 2026-06-28 | 0.1 | Story 6.5.2 created (ready-for-dev) — silence all `[RAV]` console output by default via a compile-time gate on the single `Emit()` funnel ([console_log.cpp:14-22], mirroring the `RAV_FORCE_INIT_FAILURE` switch), funnel + all ~43 call-sites retained and re-enablable with `-D RAV_ENABLE_CONSOLE_LOG` (new one-click `build_debuglog.bat`). Realises the already-amended AR16 in code. `viewer_window.cpp` nets to zero (FPS measurement + load-failure gate-advance left intact). On-canvas FPS + minimal load-failure indication explicitly deferred to Story 6.5.5 (shared text-overlay mechanism) and recorded in `deferred-work.md` + gate §2. Docs: PHASE4.5_VALIDATOR_GATE §2 (Result PENDING), AR20 Spec Change Log, D7 note. No CMake/boundary change. Gate = Antho in-Reaper Windows (AR19). |
| 2026-06-28 | 1.0 | Story 6.5.2 implemented → **review**. `Emit()` body gated behind `#ifdef RAV_ENABLE_CONSOLE_LOG` (silent default; `(void)`-cast params in `#else`); `console_log.h` doc-comment rewritten to silent-by-default. New `build_debuglog.bat` (CRLF, French, mirrors `build_forcefail.bat`). Gate §5 appended (Result PENDING). AR20 Spec Change Log entry + silent-by-default notes at D7 / `:879`. `deferred-work.md` story-6.5.2 entry (FPS + load-failure rehome → 6.5.5). Both branches compile clean under `-Wall -Wextra -Werror` via an isolated harness (real `reaper_api.h` needs Windows-only `WDL/swell`): default → no output; `-D RAV_ENABLE_CONSOLE_LOG` → byte-for-byte `[RAV] <level>: <message>`. Scope verified: only `console_log.{cpp,h}` changed in `src/`, `viewer_window.cpp` nets to zero, no CMake/boundary change, single `ShowConsoleMsg` funnel intact. Gate §5 = Antho in-Reaper Windows console check (AR19, PENDING). |
