# Story 4.2: Drop an animation file on a track to create a correctly-sized item

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want to drag an animation file onto a Reaper track and get an item the length of the animation,
so that it sits on my timeline exactly like a video reference does today.

## Acceptance Criteria

From [epics.md Story 4.2](../planning-artifacts/epics.md) (FR8/FR9, NFR-P2):

1. **AC1 — Item length == real animation duration.** Dragging a `.glb`/`.gltf`/`.fbx`/`.dae` with an animation clip onto a track creates a media item whose **timeline length equals the clip's duration in seconds** (FR9), not the 4.1 placeholder. A 4.0 s clip yields a ~4.0 s item (within rounding); a 1.5 s clip yields a ~1.5 s item. The duration comes from a **CPU-only parse** (no GL), so it is correct whether or not the viewer window is open.
2. **AC2 — Item named after the file.** The created item/take is **named after the dropped file** (Reaper derives this from the source's `GetFileName()`), so a sound designer can tell which animation an item holds at a glance.
3. **AC3 — Graceful fallback for clip-less / unparseable files.** A supported-extension file that has **no animation clip** (a static `.glb`) or that **fails to parse** still produces a usable item: `GetLength()` falls back to the **placeholder `1.0` s** rather than a 0-length (degenerate) item. The drop never crashes Reaper and never blocks past the load budget (no-throw boundary, AR18).
4. **AC4 — Load budget.** Parsing a typical Demute fixture for its duration completes **well within the 2 s load budget** (NFR-P2). The probe does only the cheap CPU work needed for duration (no mesh post-processing, no GPU upload, no texture decode).
5. **AC5 — No regression, no scope creep.** Every Epic 1–3 behavior and Story 4.1 behavior is unchanged: the viewer still opens via the action and renders identically, foreign files (`.txt`/`.wav`) are still not hijacked, register/unregister is still symmetric (`-pcmsrc`, same pointer), and unload/quit does not crash. Build stays `/W3 /permissive-` warning-free (NFR-R5). **No new runtime dependency, no new `REAPERAPI_WANT_*` symbol.**

> **Scope boundary (read before coding):** This story makes the item **correctly sized and named** — nothing more. It does **not** display the asset in the viewer, **not** load geometry/textures/GPU, **not** read the Reaper playhead, and **not** map `playhead → frame`. **Playhead → displayed frame (FR10) is Story 4.3**; per-item `SaveState`/`LoadState` content (D9) is Epic 6. The proof 4.2 works is that **the item's length matches the animation and the item is named after the file** — visible on the timeline with the viewer closed.

## Tasks / Subtasks

- [x] **Task 1 — Add a CPU-only duration probe to the loader (AC1, AC3, AC4)** — edit `src/asset_loader.h` and `src/asset_loader.cpp`
  - [x] In `asset_loader.h` (inside the existing `#ifdef _WIN32` guard), declare: `double ProbeAnimationDuration(const std::string& path);` — returns the first clip's duration in **seconds**, or **`0.0`** for no-clip / parse-failure / no-file. Document it as **no-throw** and **GL-free** (it does *not* require a current GL context, unlike `LoadAsset`).
  - [x] In `asset_loader.cpp`, implement it as a **minimal, CPU-only** assimp parse — **NOT** a call to `LoadAsset` (which uploads to the GPU and would fail/crash with no GL context on drop). Pattern:
    - Wrap the whole body in the **same try/catch envelope** as `LoadAsset` ([asset_loader.cpp:769-889](../../src/asset_loader.cpp#L769)) so nothing escapes (AR18). Any throw → `return 0.0;`.
    - `if (!FileExists(path)) return 0.0;` (reuse the existing `FileExists`, [asset_loader.cpp:738](../../src/asset_loader.cpp#L738)).
    - `Assimp::Importer importer; const aiScene* scene = importer.ReadFile(path, 0);` — **pass `0` post-process flags** (no `aiProcess_Triangulate`/`GenSmoothNormals`/etc.). `mDuration`/`mTicksPerSecond` live on `aiAnimation` and need **zero** mesh processing; this is the NFR-P2 win (AC4). Do **not** set up textures, meshes, skeleton, or GL.
    - `if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || scene->mNumAnimations == 0) return 0.0;` then `const aiAnimation* a = scene->mAnimations[0]; if (!a) return 0.0;` (null-slot guard — a raw deref is SEH/UB, not catchable).
    - **Ticks → seconds with the identical guard already vetted in 3.2** ([asset_loader.cpp:270-283](../../src/asset_loader.cpp#L270)): `double tps = a->mTicksPerSecond; if (!(tps > 0.0) || !std::isfinite(tps)) tps = 25.0;` then `return std::max(0.0, a->mDuration / tps);`. (Finite-AND-positive, fallback 25 — a negative/NaN rate must not sign-flip or poison the length. You may keep the `LogWarn` on a genuine bad value as `ParseAnimations` does, but it is optional for the probe.)
  - [x] Do **not** require or touch a skeleton: unlike `ParseAnimations` (which returns `{}` when `skeleton.bones.empty()`), the probe reads duration **directly** off `mAnimations[0]` and is independent of skinning. A file with an animation but no skin still yields a correct length.
- [x] **Task 2 — Use the real duration in the PCM_source (AC1, AC2, AC3)** — edit `src/pcm_source_anim.cpp` only
  - [x] `#include "asset_loader.h"` for `ProbeAnimationDuration`.
  - [x] Add a cached `double m_len = 0.0;` member to `AnimSource`.
  - [x] Compute it **once at construction**: `explicit AnimSource(const char* path) : m_path(path ? path : "") { m_len = ProbeAnimationDuration(m_path); }`. (Reaper calls `GetLength()` immediately after creating the item, so eager-probe is correct; caching avoids re-parsing on every `GetLength` call. This mirrors the Spike's `AnimSource` exactly — see Dev Notes → "The proven reference".)
  - [x] `GetLength()` returns the cached real duration with the **placeholder fallback**: `return m_len > 0.0 ? m_len : 1.0;` (AC3 — never return 0, which would be a degenerate item). Update the `// PLACEHOLDER` comment to reflect that 1.0 is now only the *fallback*.
  - [x] `SetFileName()` **re-probes**: `m_path = fn ? fn : ""; m_len = ProbeAnimationDuration(m_path); return true;` (so a path change re-sizes the source — the Spike does this).
  - [x] `Duplicate()` is unchanged (`new AnimSource(m_path.c_str())` re-probes in the new instance — acceptable; Reaper duplicates rarely). *Optional micro-optimization:* a private ctor that copies `m_len` to skip the re-parse on `Duplicate` — only if it stays a clean one-liner, not required.
  - [x] `CreateFromType("RAV_ANIM")` still builds `new AnimSource("")` → empty path → probe returns `0.0` → `GetLength()` = `1.0` placeholder. That is correct: project-restore sets the real path later via `LoadState` (stubbed → Epic 6/D9), out of scope here. Leave the factory functions, `g_reg`, and `PcmSourceRegistration()` **as-is**.
- [x] **Task 3 — Confirm Reaper auto-names the item (AC2)** — no code expected
  - [x] `GetFileName()` already returns `m_path` ([pcm_source_anim.cpp:58](../../src/pcm_source_anim.cpp#L58)); Reaper derives the take/item name from it (the Spike got correctly-named items for free). **No code change** is anticipated for AC2 — it is a **gate-verification** item (Task 5 row). Only if the gate shows an unnamed item would a fallback be needed; do not pre-emptively add naming code.
- [x] **Task 4 — Extend the validator gate doc (AC1–AC5)** — edit `docs/PHASE3_VALIDATOR_GATE.md`
  - [x] Append a **§5 "Story 4.2"** section in the **exact row-based format** of the existing §3 (Story 4.1). Update the file's intro line (it already says "Stories 4.2 … append their own"). The click-based test: drop a **known-duration** animation (e.g. a Mixamo clip whose length you know from Blender/FBX Review, or a Demute fixture) → the item's **length on the timeline matches that duration** (no longer ~1 s); the item is **named after the file**; drop a **static/clip-less** `.glb` → item appears at the **1 s fallback** (not 0-length, no crash); drop a `.txt`/`.wav` → still Reaper-normal; load is **near-instant / well under 2 s**. Note explicitly that the viewer does **not** yet show the asset or follow the playhead — that is **Story 4.3** (do not fail 4.2 for it).
- [x] **Task 5 — Self-verify on Linux + scope audit (what the gate can't cover)**
  - [x] `cmake -B <build> -S .` configures cleanly with **no new source file** (both `asset_loader.cpp` and `pcm_source_anim.cpp` are **already** in the `add_reaper_extension` SOURCES list — **no `CMakeLists.txt` change this story**). The Linux configure is host-stubbed for the Reaper/GL link as in prior stories; a full compile is impossible by design (no WDL/swell).
  - [x] Grep-audit scope: only `src/asset_loader.{h,cpp}`, `src/pcm_source_anim.cpp`, `docs/PHASE3_VALIDATOR_GATE.md` changed (+ `sprint-status.yaml`). **No** change to `src/plugin_main.cpp`, `CMakeLists.txt`, `src/pcm_source_anim.h`, `renderer.*`, `viewer_window.*`, `scene.h`, `animation.h`, `reaper_api.h`.
  - [x] Confirm **no new `REAPERAPI_WANT_*` symbol** (`reaper_api.h` unchanged) and **no new dependency**. The transport/item APIs (`GetPlayPosition2Ex`, `GetMediaItem`, `GetMediaItemInfo_Value`, …) belong to **Story 4.3** — do not add them now.

## Dev Notes

### The proven reference — start here

The Spike already solved "correctly-sized item" exactly the way this story should: a **CPU-only `ParseDuration`** (no GL) cached on the source, with a `1.0` fallback. Read it:

```
git show spike/0-1-feasibility:src/spike_pcmsource.cpp   # ParseDuration + m_len + GetLength
git show spike/0-1-feasibility:src/spike_loader.h        # the CPU-only duration loader
```

The Spike's `AnimSource` does precisely Task 2:
```cpp
explicit AnimSource(const char* fn) : m_fn(fn ? fn : "") { m_len = ParseDuration(m_fn.c_str()); }
bool SetFileName(const char* newfn) override { m_fn = newfn ? newfn : ""; m_len = ParseDuration(m_fn.c_str()); return true; }
double GetLength() override { return m_len > 0.0 ? m_len : 1.0; }
```
and `ParseDuration` is a CPU-only model parse that returns `m.clip.duration` (no GPU). Your job: keep this mechanism, but route the CPU-only parse through the **production** loader (`asset_loader.cpp`, the single assimp boundary) via the new `ProbeAnimationDuration`, instead of a separate spike loader. The Spike's transport helper (`CurrentAnimTime`, the `GetPlayPosition2Ex`/item-walk) is **Story 4.3 — do not lift it here.**

[Source: docs/SPIKE0_FINDINGS.md — "a dropped `.fbx/.glb/.gltf` [becomes] a timeline item of the animation's length"; architecture.md:1140]

### THE critical constraint — no GL context on drop, so DO NOT call `LoadAsset`

`LoadAsset` ([asset_loader.cpp:764](../../src/asset_loader.cpp#L764)) **requires a current GL context** — its header says so ([asset_loader.h:8](../../src/asset_loader.h#L8)) and it calls `UploadMesh` (`glGenBuffers`/`glBufferData`). When Reaper calls our `CreateFromFile`/`SetFileName` on a **file drop**, there is **no GL context current** (the viewer window may not even be open — the WGL context lives in `viewer_window.cpp` and is only current during its own render timer on the main thread). Calling `LoadAsset` from the source would hit `GpuUploadFailed` at best and is undefined at worst. **That is the entire reason for a separate CPU-only `ProbeAnimationDuration`** that does no GL, no mesh upload, no texture decode — just enough assimp to read `mDuration`/`mTicksPerSecond`. [Source: asset_loader.h:8; Explore of viewer_window.cpp GL-context ownership]

### Why duration-only, and why the viewer is NOT wired here

Getting the asset to **display** in the viewer requires loading geometry to the GPU, which (per above) can only happen on the main thread with the viewer's GL context current — and the *choice of which item to display* is driven by the **playhead / current-item selection**, which is **Story 4.3** (`animTime = playheadTime − itemStart`, FR10) and **4.5** (current-item by playhead+priority). So 4.2 deliberately stops at "correct length + name", which is fully testable on the timeline **with the viewer closed**. This matches the architecture's phasing: D8 (PCM_source) `GetLength()` = FR9 is this story; the playhead drive is the next. [Source: architecture.md:310 "GetLength() returns animation duration in seconds (FR9)"; epics.md Story 4.3]

**On the AC's "from drop to first rendered frame ≤ 2 s" (NFR-P2):** the *actionable* part in 4.2 is that the **duration parse stays fast** — it is the dominant cost of the eventual load, and with `0` post-process flags it is far under budget (AC4). The full "to first **rendered** frame" path is only completed in **4.3**, when the viewer actually displays the item; it cannot be measured in 4.2 because 4.2 renders nothing. Do not try to force a render here to "satisfy" the clause — that is 4.3's scope, and doing it now reintroduces the GL-context problem above.

### Accepted tradeoff — the file is parsed twice (probe now, full load in 4.3)

4.2 parses the file once (CPU, duration only). When 4.3 displays it, the viewer's `LoadAsset` parses it again (CPU + GPU). This **double parse is intentional and accepted** — it is exactly what the Spike did (separate `spike_loader` for duration vs the viewer's load), and it matches **D2** (per-item asset ownership; no shared cache for MVP). **Do not** try to "optimize" by caching the full `Asset` on the `PCM_source` at drop time — that needs the GL-context-on-drop you don't have, and the asset must be (re)loaded when display is needed anyway. The probe with `0` flags is cheap (mostly file I/O + minimal parse). [Source: architecture.md:213-220 D2; architecture.md:179 "Asset cache / sharing between PCM_source instances — accept VRAM duplication for MVP"]

### Units & the ticks→seconds guard (reuse 3.2's vetted logic verbatim)

Duration is in **seconds**; assimp reports `mDuration` in **ticks** and `mTicksPerSecond` is importer-dependent (FBX vs glTF differ — never hard-code a rate). The exact guard is already proven in `ParseAnimations` ([asset_loader.cpp:270-283](../../src/asset_loader.cpp#L270)) and you must replicate its behavior in the probe:
- `tps = a->mTicksPerSecond;` then **if not (finite AND > 0)** → `tps = 25.0` (assimp's own fallback). A bare `!= 0` check is wrong: a **negative** rate sign-flips the value and **NaN** slips through (`NaN != 0` is true).
- `duration = std::max(0.0, mDuration / tps)` — clamp ≥ 0 so a broken negative `mDuration` can't produce a negative item length.

This is the same conversion the renderer's loop and `ComputePose` rely on, so the item length will match the animation the viewer eventually plays.

### `GetType` / registration / boundary rules are already correct — leave them

Story 4.1 already established: the permanent tag `"RAV_ANIM"` ([pcm_source_anim.cpp:46](../../src/pcm_source_anim.cpp#L46)), the three factory functions, the single `g_reg`, `PcmSourceRegistration()`, and the **`plugin_main.cpp`-only** `rec->Register("pcmsrc"/"-pcmsrc", …)` with the **identical pointer** ([plugin_main.cpp:56,87](../../src/plugin_main.cpp#L56)). **None of that changes in 4.2.** You are only making `GetLength()` real and re-probing on `SetFileName`. If you find yourself editing `plugin_main.cpp`, `pcm_source_anim.h`, `CMakeLists.txt`, or `reaper_api.h`, you have left scope. [Source: architecture.md:874-875 boundary rules; Story 4.1 File List]

### `.dae`/`.fbx` register but assimp-load-correctness is Epic 6

All four extensions are registered (4.1). Whether assimp parses a given `.fbx`/`.dae` *duration* correctly is the same assimp path Epic 3 already exercises for `.fbx` (Mixamo rigs validated in 3.1–3.3). Collada `.dae` full-load validation is Epic 6; for 4.2, if a `.dae` returns `0.0` from the probe it simply falls back to the 1 s placeholder (AC3) — no crash, no special-casing. [Source: sprint-change-proposal-2026-06-24.md; Story 4.1 Dev Notes]

### No-exception boundary (D5 / AR18)

`ProbeAnimationDuration` is called from `CreateFromFile`/`SetFileName`/the ctor — all Reaper-driven boundaries. assimp throws `std::exception`-derived on some malformed inputs; wrap the probe body in the same `try { … } catch (...) { return 0.0; }` envelope `LoadAsset` uses. The existing `CreateFromFile`/`CreateFromType` `try/catch` around `new AnimSource(...)` ([pcm_source_anim.cpp:82-95](../../src/pcm_source_anim.cpp#L82)) stays — and now also catches a (theoretical) throw escaping the ctor's probe, but the probe must be self-contained no-throw regardless. [Source: architecture.md:619-626; asset_loader.cpp:766-889]

### Project Structure Notes

- **Edit:** `src/asset_loader.h` (+1 declaration, inside `#ifdef _WIN32`), `src/asset_loader.cpp` (+1 small function `ProbeAnimationDuration`), `src/pcm_source_anim.cpp` (include + `m_len` member + ctor/`SetFileName`/`GetLength` wiring), `docs/PHASE3_VALIDATOR_GATE.md` (+§5).
- **Untouched:** `src/pcm_source_anim.h`, `src/plugin_main.cpp`, `CMakeLists.txt` (both `.cpp`s already listed), `renderer.*`, `viewer_window.*`, `scene.h`, `animation.h`, `reaper_api.h`. If you edit any of these, you have left scope.
- Conventions to match: `namespace rav`; the loader's existing `FileExists`/importer/try-catch patterns; `console_log.h` (`LogWarn`) for any optional console output (never raw `ShowConsoleMsg`); keep `/W3 /permissive-` clean.
- DLL name is `reaper_animviewer.dll` (CMake target `animviewer`).

### Testing standards

No automated test harness exists (Reaper-hosted native DLL; prior stories validate via the in-Reaper gate). **The gate IS the test (AR19):** Antho runs `docs/PHASE3_VALIDATOR_GATE.md` §5 on Windows-in-Reaper — drop a known-duration clip, confirm the **item length matches** and the **name is the filename**; that in-Reaper pass **is** completion. On Linux you can only confirm the CMake parse and the scope/grep audit (the DLL/GL link is host-stubbed, as in Stories 3.x). [Source: feedback_trust_ingame_validation; PHASE3_VALIDATOR_GATE.md §4]

### References

- [epics.md — Epic 4 / Story 4.2](../planning-artifacts/epics.md) (AC source; FR8/FR9, NFR-P2)
- [architecture.md#D8 — `GetLength()` returns animation duration (FR9)](../planning-artifacts/architecture.md) (:310)
- [architecture.md#D2 — per-item Asset ownership, no shared cache](../planning-artifacts/architecture.md) (:213-220, :179)
- [architecture.md — NFR-P2 synchronous parse on main thread](../planning-artifacts/architecture.md) (:1012)
- [architecture.md — boundary rules (`Register` in plugin_main only)](../planning-artifacts/architecture.md) (:874-875)
- [architecture.md — Spec Change Log, D8 confirmed (item == animation length)](../planning-artifacts/architecture.md) (:1140)
- Spike reference: `git show spike/0-1-feasibility:src/spike_pcmsource.cpp` (`ParseDuration`/`m_len`/`GetLength`), `:src/spike_loader.h` (CPU-only duration loader)
- Current code: [pcm_source_anim.cpp](../../src/pcm_source_anim.cpp), [asset_loader.cpp `ParseAnimations` tps guard](../../src/asset_loader.cpp#L270), [`LoadAsset` try/catch + `FileExists`](../../src/asset_loader.cpp#L764), [asset_loader.h](../../src/asset_loader.h)
- Previous story: [4-1 PCM_source registration](./4-1-pcm-source-plugin-registers-and-creates-a-source-from-a-file.md)

### Previous-story intelligence (Epic 1–4 patterns to reuse)

- **Cold-path hardening recurs in every code-review** (4.1: no-throw factory; 3.2: finite/positive `tps` guard; 3.1: null `aiBone*`/`isfinite`). Anticipate it now: the probe must be no-throw, null-safe (`FileExists`, null `scene`/`aiAnimation` guards), and reuse the **finite-AND-positive `tps`** guard — don't ship a bare `!= 0` check (a 3.2 code-review patch fixed exactly that bug).
- **AR19 — the in-Reaper Windows gate IS the gate.** When Antho drops a clip and the item length matches, that is "done"; Linux-side hardening patches in cold paths don't demote it.
- **Scope discipline is audited** (every story so far passes an Acceptance-Auditor scope grep). Touch only the four files in Project Structure Notes. The single biggest 4.2 mistake to avoid: pulling the viewer/renderer/transport into this story — that is 4.3.
- **No new dependency / no new `WANT_*`** — every Epic 4 story so far adds zero. Keep it that way (transport symbols are 4.3).

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Claude Opus 4.8, 1M context) — bmad-dev-story workflow

### Debug Log References

- Linux `cmake -B /tmp/rav_build_42 -S .` → configures cleanly (exit 0), host-stubbed for non-Windows ("Build target stubbed") as in prior stories — no new source file added (both `asset_loader.cpp` and `pcm_source_anim.cpp` were already in the `add_reaper_extension` SOURCES list). A full compile is impossible by design on Linux (no WDL/swell); the in-Reaper Windows gate is the real test (AR19).
- Scope grep audit: my 4.2 edits touch only `src/asset_loader.{h,cpp}`, `src/pcm_source_anim.cpp`, `docs/PHASE3_VALIDATOR_GATE.md` (+ `sprint-status.yaml`). `CMakeLists.txt` and `src/plugin_main.cpp` show as modified in the tree but contain **zero** 4.2 markers — that diff is Story 4.1's uncommitted pcmsrc registration, not this story. `reaper_api.h` untouched → **no new `REAPERAPI_WANT_*`**, no new dependency, no transport symbols.

### Completion Notes List

- **Task 1 — CPU-only duration probe.** Added `double ProbeAnimationDuration(const std::string&)` to `asset_loader.{h,cpp}` (inside the `#ifdef _WIN32` guard). It is a minimal assimp parse with **`0` post-process flags** (no Triangulate/GenSmoothNormals/etc. — the NFR-P2 win, AC4), wrapped in the same `try { … } catch (...) { return 0.0; }` envelope as `LoadAsset` (AR18). Guards: `FileExists` (reused), null `scene`/`AI_SCENE_FLAGS_INCOMPLETE`/`mNumAnimations==0`, null `mAnimations[0]` slot. Ticks→seconds uses the **identical finite-AND-positive `tps` guard** vetted in 3.2 (`if (!(tps>0.0) || !std::isfinite(tps)) tps = 25.0;`) and clamps `std::max(0.0, mDuration/tps)`. **No GL** — this is the whole point (no GL context on drop). Reads duration directly off `mAnimations[0]`, independent of any skeleton.
- **Task 2 — real duration in the PCM_source.** `pcm_source_anim.cpp` now `#include "asset_loader.h"`, caches `double m_len = 0.0`, probes **once at construction** and **re-probes on `SetFileName`**. `GetLength()` returns `m_len > 0.0 ? m_len : 1.0` (AC3 — 1.0 is now only the fallback, never a 0-length degenerate item). `Duplicate()`/`CreateFromType("")`/the factories left as-is (empty path → probe 0.0 → 1.0 placeholder, correct for project-restore stub → Epic 6).
- **Task 3 — auto-naming (AC2).** No code needed: `GetFileName()` already returns `m_path`; Reaper derives the take/item name from it (gate-verification row only).
- **Task 4 — gate doc.** Appended **§5 "Story 4.2"** to `docs/PHASE3_VALIDATOR_GATE.md` in the row-based format of §3 (7 rows over AC1–AC5 incl. "correct length with viewer closed" and "clip-less → 1 s fallback"), with fixtures, scope note (display/playhead = 4.3), and a pending result line.
- **Task 5 — self-verify + scope audit.** CMake configure clean; scope grep clean (see Debug Log). No new dependency / WANT_* / transport symbol.
- **Accepted tradeoff:** the file is parsed twice (cheap duration probe now, full GPU load in 4.3) — intentional, matches D2 and the Spike.

### File List

- `src/asset_loader.h` — declared `ProbeAnimationDuration` (no-throw, GL-free) next to `LoadAsset`.
- `src/asset_loader.cpp` — implemented `ProbeAnimationDuration` (CPU-only assimp parse, `0` flags, no-throw, finite/positive tps guard + clamp).
- `src/pcm_source_anim.cpp` — `#include "asset_loader.h"`; cached `m_len`; probe at ctor + re-probe on `SetFileName`; `GetLength()` returns real duration with 1 s fallback.
- `docs/PHASE3_VALIDATOR_GATE.md` — new §5 (Story 4.2 validator rows).
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — 4-2 → in-progress → review.

### Review Findings

BMAD 3-layer code-review (Blind Hunter / Edge Case Hunter / Acceptance Auditor), 2026-06-26 — Antho-requested. Scope-clean (only the 4 allowed files carry 4.2 markers; `CMakeLists.txt`/`plugin_main.cpp` now have an empty diff vs HEAD = 4.1 committed). AC1–AC5 all PASS. Both patches are cold-path/doc, so they do **not** demote the AR19 in-Reaper gate pass — status stays `done`.

- [x] [Review][Patch] Non-finite (+Inf) `mDuration` yields an infinite-length item — `std::max(0.0, +Inf)` returns `+Inf` and `GetLength()`'s `m_len > 0.0` test passes for `+Inf`, handing Reaper an infinite-length item; guard the divided result with `std::isfinite` (→ 0.0 → 1 s fallback). Cross-confirmed blind+edge; same latent hole as `ParseAnimations` but here it reaches Reaper directly. **APPLIED** — `std::isfinite(seconds)` gate added before the clamp. [src/asset_loader.cpp:921]
- [x] [Review][Patch] Gate §5 Result line still reads "pending" though the story is `done` / gate passed — sync the §5 Result line to PASS (2026-06-26). **APPLIED.** [docs/PHASE3_VALIDATOR_GATE.md:157]

**Dismissed (5):** `CreateFromFile(nullptr)`→`HasAnimExt` (false positive — `HasAnimExt` null-guards `if (!fn) return false;`); `Duplicate()` re-parses on copy (spec explicitly sanctions this as an optional, not-required micro-opt — accepted D2 tradeoff); only `mAnimations[0]` read (intended MVP per spec); negative `mDuration`→0→1 s fallback (works as documented); probe omits the multi-clip `LogInfo` (cosmetic).

## Change Log

| Date | Change |
|---|---|
| 2026-06-26 | Story 4.2 created (ready-for-dev) — correctly-sized + named item via a CPU-only `ProbeAnimationDuration` (no GL) cached on the `PCM_source`; `GetLength()` returns real duration with a 1 s fallback. Edits scoped to `asset_loader.{h,cpp}` + `pcm_source_anim.cpp` + gate §5; viewer/transport/display deferred to 4.3. |
| 2026-06-26 | Story 4.2 code-review (BMAD 3-layer — Blind Hunter / Edge Case Hunter / Acceptance Auditor, Antho-requested). Acceptance Auditor PASS (AC1–AC5 all met; scope clean — only the 4 allowed files carry 4.2 markers, `CMakeLists.txt`/`plugin_main.cpp` now empty-diff = 4.1 committed; no new `WANT_*`/dependency). **2 patches applied, 5 dismissed** — both patches are cold-path/doc so the AR19 in-Reaper gate pass stands and status STAYS `done`: (1) `ProbeAnimationDuration` now `std::isfinite`-guards the ticks→seconds **result** (not just `tps`) — a corrupt clip with `mDuration == +Inf` previously survived `std::max(0.0,+Inf)=+Inf` AND `GetLength()`'s `m_len > 0.0` test, handing Reaper an infinite-length item; non-finite → 0.0 → 1 s fallback (AC3). Same latent hole exists in `ParseAnimations` but here it reaches Reaper directly. (2) Gate §5 Result line synced `pending` → **PASS** (2026-06-26). Dismissed: `CreateFromFile(nullptr)`→`HasAnimExt` (false positive, `HasAnimExt` null-guards); `Duplicate()` re-parse (spec-accepted optional micro-opt, D2); only `mAnimations[0]` read (intended MVP); negative `mDuration`→0→fallback (works as documented); probe omits multi-clip `LogInfo` (cosmetic). |
| 2026-06-26 | Story 4.2 → **done** — Antho in-Reaper Windows gate PASSED (PHASE3_VALIDATOR_GATE §5): dropped animation creates a correctly-sized item (length == real clip duration, viewer-closed), named after the file; clip-less → 1 s fallback; foreign files Reaper-normal. AR19 in-Reaper pass IS the gate. (4.1 code still uncommitted by design — Antho handling its push separately.) |
| 2026-06-26 | Story 4.2 implemented → **review**. Added GL-free no-throw `ProbeAnimationDuration` (assimp, `0` post-process flags, finite/positive tps guard + clamp) to `asset_loader.{h,cpp}`; `pcm_source_anim.cpp` caches `m_len` (probe at ctor + re-probe on `SetFileName`), `GetLength()` = real duration with 1 s fallback (AC3). Auto-naming free via existing `GetFileName()` (AC2). Gate §5 appended. Linux CMake configure clean + scope/boundary grep-audit pass (no new file, no `WANT_*`, no new dependency; `CMakeLists.txt`/`plugin_main.cpp` carry only 4.1's uncommitted diff). Item length/name + 1 s fallback pend Antho's in-Reaper Windows gate (AR19). |
