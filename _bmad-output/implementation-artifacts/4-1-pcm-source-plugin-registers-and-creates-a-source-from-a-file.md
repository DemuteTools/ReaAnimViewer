# Story 4.1: PCM_source plugin registers and creates a source from a file

Status: ready-for-dev

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As the implementing developer,
I want a `PCM_source` factory registered with Reaper that builds a source from an animation file path,
so that animations can become first-class Reaper media (the foundation the rest of Epic 4 builds on).

## Acceptance Criteria

From [epics.md Story 4.1](../planning-artifacts/epics.md) (D8 / AR11 / AR15, NFR-R3):

1. **AC1 — Factory registered at load.** With ReaAnimViewer loaded, a `PCM_source` factory (`pcmsrc_register_t`) is registered with Reaper at extension load via `rec->Register("pcmsrc", &reg)` (the mechanism Spike 0 confirmed). A single `[RAV] info:` console line confirms the registration and lists the registered extensions.
2. **AC2 — Source instantiable from a path.** Reaper can instantiate a ReaAnimViewer `PCM_source` bound to a given `.glb`/`.gltf`/`.fbx`/`.dae` path. Concretely: dragging such a file onto a track creates a media item backed by our source (the factory's `CreateFromFile` fired and returned our object). A non-matching extension (e.g. `.txt`, `.wav`) returns `nullptr` from our factory so Reaper handles it normally — our factory never hijacks foreign files.
3. **AC3 — Symmetric deregistration, no dangling pointers.** On unload (`rec == nullptr`), the factory is deregistered via `rec->Register("-pcmsrc", &reg)` with the **same** registration pointer, leaving no live pointer into our DLL (NFR-R3, AR15). Closing the project / unloading the extension / quitting Reaper does not crash.
4. **AC4 — No regression, no scope creep.** Every Epic 1–3 behavior is unchanged: the viewer still opens via the action, renders identically, and a project with no animation items behaves exactly as before. Build stays `/W3 /permissive-` warning-free (NFR-R5). No new runtime dependency.

> **Scope boundary (read before coding):** This story is **registration + a working source object only**. The source reports a **placeholder length** — the *correctly-sized item* (item length == real animation duration, FR9, 2 s load budget) is **Story 4.2**, and **playhead→frame mapping** (FR10) is **Story 4.3**. Do **not** load the asset, touch GL, parse duration, or implement `SaveState`/`LoadState` content here. Getting an item to *appear at all* on drop is the proof that 4.1 works.

## Tasks / Subtasks

- [ ] **Task 1 — New `PCM_source` subclass + factory (AC1, AC2)** — create `src/pcm_source_anim.h` and `src/pcm_source_anim.cpp`
  - [ ] Subclass `PCM_source` (from `reaper_plugin.h`, already vendored — see Dev Notes) as `rav::AnimSource`. Implement **every** pure virtual (the SDK has ~14; see the exact list in Dev Notes → "PCM_source contract"). Most are trivial non-audio stubs.
  - [ ] Constructor takes the file path and stores it (`std::string m_path`). `GetFileName()` returns it; `SetFileName()` replaces it (return `true`). `Duplicate()` returns `new AnimSource(m_path)`.
  - [ ] `GetType()` returns the **permanent** production tag `"RAV_ANIM"` (see Dev Notes → "GetType tag is permanent").
  - [ ] Non-audio markers: `GetNumChannels()` → `0`, `GetSampleRate()` → `0.0` (`< 1.0` tells Reaper it's silent), `GetSamples()` sets `block->samples_out = 0`, `GetPeakInfo()`/`Peaks_*` are no-ops returning `0`.
  - [ ] `GetLength()` returns a **placeholder** `1.0` (real duration = Story 4.2). Add a one-line comment pointing at 4.2.
  - [ ] `SaveState()` / `LoadState()` are stubs for now (`LoadState` returns `0`) — real per-item state is Story 4.x / Epic 6 (D9). `PropertiesWindow()` returns `0`.
  - [ ] Three free factory functions matching `pcmsrc_register_t`: `CreateFromType(type, prio)` (returns `new AnimSource("")` iff `type == "RAV_ANIM"`, else `nullptr`); `CreateFromFile(filename, prio)` (returns `new AnimSource(filename)` iff the extension is one of ours, else `nullptr`); `EnumFileExtensions(i, descptr)` (enumerates `glb`/`gltf`/`fbx`/`dae`).
  - [ ] Extension matching is **case-insensitive** and on the final extension only (see spike `IExtEq`/`HasAnimExt` reference). Supported set: `.glb`, `.gltf`, `.fbx`, `.dae`.
  - [ ] Expose the registration object to `plugin_main` per the boundary rule (see Task 2 / Dev Notes → "Who calls Register"): a header function `pcmsrc_register_t* PcmSourceRegistration()` returning the address of a single static `pcmsrc_register_t`. **Do not call `rec->Register` from this file.**
  - [ ] Factory functions and overridden virtuals are **no-throw boundaries** (D5/AR18): wrap the `new AnimSource(...)` in `CreateFromFile`/`CreateFromType` in `try { … } catch (...) { return nullptr; }`. No exception escapes into Reaper.
- [ ] **Task 2 — Wire registration into the lifecycle (AC1, AC3)** — edit `src/plugin_main.cpp` only
  - [ ] At load (after the existing action/gaccel/hookcommand block, `rec != nullptr` path): `rec->Register("pcmsrc", PcmSourceRegistration());` then one `LogInfo("pcmsrc factory registered (.glb/.gltf/.fbx/.dae)");`.
  - [ ] At unload (`rec == nullptr` path): `g_register("-pcmsrc", PcmSourceRegistration());` using the **same** pointer. Place it in the reverse-order teardown block alongside the existing `-toggleaction`/`-hookcommand`/`-gaccel` (order among unregisters is not load-critical, but keep the reverse-of-load convention).
  - [ ] `#include "pcm_source_anim.h"`.
- [ ] **Task 3 — Build wiring (AC4)** — edit `CMakeLists.txt`
  - [ ] Add `src/pcm_source_anim.cpp` to the explicit `add_reaper_extension(animviewer SOURCES …)` list (no `file(GLOB)` — keep it explicit per NFR-R5). Headers are not listed (consistent with `scene.h`/`camera.h`/`console_log.h` usage). One line added.
- [ ] **Task 4 — Validator gate doc (AC1–AC3)** — create `docs/PHASE3_VALIDATOR_GATE.md`
  - [ ] Seed the Phase 3 gate following the **exact format** of `docs/PHASE2_VALIDATOR_GATE.md` (title, "You are testing the right thing if…", Prerequisites, §1 Build & install identical to prior phases, then a numbered, row-based audit section for Story 4.1). The click-based test: load → console shows `pcmsrc factory registered`; drag a `.glb`/`.fbx` onto a track → a media item appears (placeholder ~1 s length is expected and correct for 4.1); drag a `.txt` → no item / Reaper-normal behavior; unload/quit → no crash. Note that correct item length is Story 4.2.
- [ ] **Task 5 — Self-verify on Linux (what the gate can't cover)**
  - [ ] `cmake --build` configures with the new file in the list (the project's Linux configure is host-stubbed for the Reaper DLL link — same as prior stories; confirm the source list / CMake parse is clean).
  - [ ] Grep-audit scope: only `src/pcm_source_anim.{h,cpp}` (new), `src/plugin_main.cpp`, `CMakeLists.txt`, `docs/PHASE3_VALIDATOR_GATE.md` changed. **No** change to `scene.h`, `renderer.*`, `viewer_window.*`, `asset_loader.*`, `animation.h`, `reaper_api.h`.
  - [ ] Confirm no new `REAPERAPI_WANT_*` symbol was added (none is needed — see Dev Notes).

## Dev Notes

### The proven reference — start here

Spike 0 already built and validated this exact surface inside Reaper. The reference implementation is on the unmerged spike branch:

```
git show spike/0-1-feasibility:src/spike_pcmsource.cpp
git show spike/0-1-feasibility:src/spike_pcmsource.h
```

It is **THROWAWAY spike code**, not production — but the *mechanism* is the thing to copy: the `AnimSource : PCM_source` subclass, the three factory functions, the `pcmsrc_register_t g_reg`, and the case-insensitive extension check. Your job is to lift that mechanism into clean production files, drop the spike-only bits (the `CurrentAnimTime`/transport helper belongs to **Story 4.3**, not here), follow the boundary rules below, and add `.dae`.

[Source: docs/SPIKE0_FINDINGS.md#Finding-2 — "Validates D8/AR11 … It is: `pcmsrc_register_t { CreateFromType, CreateFromFile, EnumFileExtensions }`"]

### PCM_source contract — the exact pure virtuals to implement

`PCM_source` is at [extern/reaper-sdk/sdk/reaper_plugin.h:582](../../extern/reaper-sdk/sdk/reaper_plugin.h#L582). Pure virtuals (`=0`) you **must** override:

| Method | 4.1 implementation |
|---|---|
| `PCM_source *Duplicate()` | `return new AnimSource(m_path)` |
| `bool IsAvailable()` | `return true` |
| `const char *GetType()` | `return "RAV_ANIM"` |
| `bool SetFileName(const char*)` | store path, `return true` |
| `int GetNumChannels()` | `return 0` (not audio) |
| `double GetSampleRate()` | `return 0.0` (`<1.0` ⇒ silent) |
| `double GetLength()` | `return 1.0` **placeholder** (real duration = 4.2) |
| `int PropertiesWindow(HWND)` | `return 0` |
| `void GetSamples(PCM_source_transfer_t*)` | `if (block) block->samples_out = 0;` |
| `void GetPeakInfo(PCM_source_peaktransfer_t*)` | `{}` |
| `void SaveState(ProjectStateContext*)` | `{}` (stub — D9 is later) |
| `int LoadState(const char*, ProjectStateContext*)` | `return 0` (stub) |
| `void Peaks_Clear(bool)` | `{}` |
| `int PeaksBuild_Begin()` | `return 0` |
| `int PeaksBuild_Run()` | `return 0` |
| `void PeaksBuild_Finish()` | `{}` |

Also override `const char *GetFileName()` (non-pure, but we want it) → return `m_path.c_str()`. Do **not** override the optional ones you don't need (`GetSource`, `Extended`, etc.) — the SDK defaults are correct.

`pcmsrc_register_t` is at [extern/reaper-sdk/sdk/reaper_plugin.h:804](../../extern/reaper-sdk/sdk/reaper_plugin.h#L804):
```c
typedef struct _REAPER_pcmsrc_register_t {
  PCM_source *(*CreateFromType)(const char *type, int priority);
  PCM_source *(*CreateFromFile)(const char *filename, int priority);
  const char *(*EnumFileExtensions)(int i, const char **descptr);
} pcmsrc_register_t;
```
`EnumFileExtensions`: call with increasing `i` until it returns `NULL`; if `*descptr` output is `NULL`, Reaper reuses the last description. So set the description on `i==0` only (e.g. `"ReaAnimViewer animation"`), `NULL` thereafter:
```
case 0: if(descptr)*descptr="ReaAnimViewer animation"; return "glb";
case 1: if(descptr)*descptr=nullptr; return "gltf";
case 2: if(descptr)*descptr=nullptr; return "fbx";
case 3: if(descptr)*descptr=nullptr; return "dae";
default: return nullptr;
```

### No new `reaper_api.h` symbols are needed

`PCM_source` and `pcmsrc_register_t` are **SDK types**, not API functions — they come from `reaper_plugin.h`, which `reaper_api.h` already includes. They need **no** `REAPERAPI_WANT_*` define. The transport functions the spike used (`GetPlayStateEx`, `GetPlayPosition2Ex`, `GetMediaItem…`) are for **playhead→frame mapping = Story 4.3** — do **not** add them now. Adding unused `WANT_*` symbols is scope creep and risks `LoadAPI` failing if a symbol is unavailable. [Source: architecture.md#D10 — Phase 3 row; src/reaper_api.h current state]

### Who calls `Register` — the boundary rule

The architecture mandates: **`plugin_main.cpp` is the only file that calls `rec->Register(...)`**, so the symmetric register/unregister invariant (NFR-R3, AR15) is statically auditable in one place. [Source: architecture.md:875]

The spike violated this (it called `reg("pcmsrc", …)` inside `spike_pcmsource.cpp`). **Do not copy that.** Instead: `pcm_source_anim.cpp` owns a single `static pcmsrc_register_t g_reg = {…};` and exposes `pcmsrc_register_t* PcmSourceRegistration() { return &g_reg; }`. `plugin_main.cpp` does the `rec->Register("pcmsrc", PcmSourceRegistration())` at load and `g_register("-pcmsrc", PcmSourceRegistration())` at unload — the **identical pointer** on both calls (passing a different pointer to the `-` call is a classic dangling-pointer / no-op-unregister bug; AC3 guards exactly this). The existing pattern to mirror is in [plugin_main.cpp:52-56](../../src/plugin_main.cpp#L52) (`-toggleaction`/`-hookcommand`/`-gaccel`) and [plugin_main.cpp:77-79](../../src/plugin_main.cpp#L77). [Source: architecture.md:683-695 "Good — symmetric registration"]

### `GetType` tag is permanent — choose once

`GetType()` returns `"RAV_ANIM"`. This string becomes a **permanent on-disk identifier**: `CreateFromType` matches on it, and (from Story 4.x / Epic 6) it is how `SaveState`/`LoadState` and item-selection recognize *our* sources inside saved `.rpp` projects. **Changing it after ship breaks every saved project.** The spike used `"FBXAV_ANIM"`; the project was renamed to ReaAnimViewer in Story 1.1, so production uses the `RAV`-prefixed tag for consistency with the `[RAV]` console convention. Use `"RAV_ANIM"` everywhere (the subclass `GetType`, the `CreateFromType` comparison, and any future `IsOurs` check). [Source: architecture.md#D8 — `GetType()` returns "FBXAV" *(or similar tag)*; superseded by the Story 1.1 rename]

### `.dae` is in scope for registration (but not for load-validation)

Collada `.dae` was added to MVP scope by [sprint-change-proposal-2026-06-24.md](../planning-artifacts/sprint-change-proposal-2026-06-24.md) (story 6-5 renamed `fbx`→`fbx-and-collada`); the FR coverage map and the Story 4.1 AC both say `.glb/.gltf/.fbx/.dae`. So **register all four** extensions here. Whether assimp actually *loads* a given `.fbx`/`.dae` correctly is validated in **Epic 6** — not your concern in 4.1 (you don't load anything; you only register the extension and hold the path). [Source: epics.md:179, sprint-status.yaml "+Collada (.dae) scope"]

### No-exception boundary (D5 / AR18)

Every entry Reaper drives into our code must not let an exception escape. The factory functions are C-style callbacks invoked by Reaper, and the `PCM_source` virtuals are called during timeline/project operations — all are boundaries. For 4.1 the only realistic throw is `bad_alloc` from `new AnimSource` / the `std::string` copy: wrap the `new` in `CreateFromFile`/`CreateFromType` with `try { return new AnimSource(...); } catch (...) { return nullptr; }`. The virtuals are trivial and non-throwing as written. [Source: architecture.md:258, :619-626]

### Why an item *appears at all* is the whole proof

A 0-channel, `GetSampleRate()<1.0` source still produces a real **timeline item** on drop (Spike 0 confirmed: "Dropping a `.fbx/.glb/.gltf` on a track creates a timeline item"). With the placeholder `GetLength()==1.0` the item is ~1 second long — that is **expected and correct for 4.1**. Story 4.2 makes the length match the animation. Tell Antho in the gate doc not to be alarmed by the 1 s length. [Source: docs/SPIKE0_FINDINGS.md:18,68-73]

### Architecture doc uses pre-rename / pre-spike names — trust the live tree

`architecture.md` predates the Story 1.1 rename and the Spike 0 supersession. Where it says `FBXAV`, `reaper_fbxanimationviewer.dll`, `viewer_panel`/ReaImGui-FBO — the **current reality** is `RAV` / `reaper_animviewer.dll` / `viewer_window.cpp` (native docked GL window, no ReaImGui, no FBO). Follow the **actual `src/` conventions** (see `console_log.h` `[RAV]` format, the `rav` namespace, SPDX header, `plugin_main.cpp` register pattern), not the stale names. The planned filenames `pcm_source_anim.{h,cpp}` from [architecture.md:789-790](../planning-artifacts/architecture.md) are still correct. [Source: architecture.md Spec Change Log :1140; sprint-status epic-1 rename]

### Project Structure Notes

- **New:** `src/pcm_source_anim.h`, `src/pcm_source_anim.cpp` — the **only** files that subclass `PCM_source` (boundary rule, architecture.md:874).
- **Edit:** `src/plugin_main.cpp` (register/unregister `pcmsrc`), `CMakeLists.txt` (one source line), new `docs/PHASE3_VALIDATOR_GATE.md`.
- **Untouched:** `scene.h`, `renderer.*`, `viewer_window.*`, `asset_loader.*`, `animation.h`, `reaper_api.h`. If you find yourself editing any of these, you have left scope.
- Conventions to match: SPDX `// SPDX-License-Identifier: MIT` header on new files; `namespace rav { … }`; anonymous namespace for the factory functions + `g_reg`; `console_log.h` for any console output (never raw `ShowConsoleMsg`).
- DLL name is `reaper_animviewer.dll` (CMake target `animviewer`).

### Testing standards

No automated test harness exists (this is a Reaper-hosted native DLL; prior stories validate via the in-Reaper gate). The **gate is the test** (AR19): Antho runs `docs/PHASE3_VALIDATOR_GATE.md` on Windows-in-Reaper, and that in-Reaper pass **is** the completion gate — no separate unit suite is expected. On Linux you can only confirm the CMake parse and the scope/grep audit (the DLL link is host-stubbed, as in Stories 3.1/3.2). Keep the change `/W3 /permissive-` clean.

### References

- [epics.md — Epic 4 / Story 4.1](../planning-artifacts/epics.md) (AC source)
- [architecture.md#D8 — PCM_source plugin pattern](../planning-artifacts/architecture.md) (:292-315)
- [architecture.md — boundary rules](../planning-artifacts/architecture.md) (:874-875), [symmetric register example](../planning-artifacts/architecture.md) (:683-695)
- [architecture.md#D10 — API loader expansion, Phase 3 row](../planning-artifacts/architecture.md) (:329-339)
- [architecture.md — Spec Change Log, D8 confirmed](../planning-artifacts/architecture.md) (:1140)
- [docs/SPIKE0_FINDINGS.md — Finding 2 (mechanism), Disposition #3](../../docs/SPIKE0_FINDINGS.md)
- Spike reference impl: `git show spike/0-1-feasibility:src/spike_pcmsource.cpp`
- SDK: [reaper_plugin.h `PCM_source`](../../extern/reaper-sdk/sdk/reaper_plugin.h#L582), [`pcmsrc_register_t`](../../extern/reaper-sdk/sdk/reaper_plugin.h#L804)
- Current code: [plugin_main.cpp](../../src/plugin_main.cpp), [reaper_api.h](../../src/reaper_api.h), [console_log.h](../../src/console_log.h)

### Previous-story intelligence (Epic 1–3 patterns to reuse)

- **Symmetric register is a code-review focus.** Stories 1.3/1.4 reviews flagged register/unregister symmetry and cold-path init-failure handling. Get `-pcmsrc` with the identical pointer right the first time.
- **Cold-path hardening recurs in every code-review** (3.1: null `aiBone*` guards + `isfinite`; 3.2: finite/positive guards; 2.x: NaN clamps). Anticipate it: null-guard `filename`/`type` in the factory functions (the spike already does — `HasAnimExt(nullptr)` returns false), keep the no-throw wrappers.
- **AR19 — the in-Reaper Windows gate IS the gate.** When Antho validates in-Reaper and it works, that is "done"; cold-path patches in unexercised paths don't demote it. Write the gate doc so Antho can click through it.
- **Header-only is the precedent for non-`.cpp` additions** (camera.h, animation.h → no CMake change). But `pcm_source_anim` has a `.cpp`, so it **does** need the CMake source line — don't skip it.

## Dev Agent Record

### Agent Model Used

{{agent_model_name_version}}

### Debug Log References

### Completion Notes List

### File List
