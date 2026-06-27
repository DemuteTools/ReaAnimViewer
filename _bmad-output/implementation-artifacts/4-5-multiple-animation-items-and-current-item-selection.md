---
baseline_commit: 4077f59b577da78d087c04567858b97262da71a2
---

# Story 4.5: Multiple animation items and current-item selection

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want several animations across tracks with the viewer showing the one under the playhead,
so that I can score a sequence of animations in one session.

## Acceptance Criteria

From [epics.md Story 4.5](../planning-artifacts/epics.md) (FR13, FR14, NFR-P6):

1. **AC1 — Multiple animation items coexist and the right one shows (FR13, FR14).** With **multiple** animation items placed across **one or more tracks** in the same Reaper project, when the playhead spans an item the panel displays **that** item's animation (its asset + its playhead-driven frame). Scrubbing/playing from one item's span into another's **switches** the displayed asset to the new item (this already works per Story 4.3's path-change reload — 4.5 keeps it working with many items present). Between items (playhead over **no** RAV item) the viewer **holds** the last shown frame (Story 4.3, unchanged).
2. **AC2 — Overlap resolves to the highest-priority track (FR14 — the one piece that needs code).** When **two or more** RAV items span the playhead **at the same time** (overlapping in timeline, on **different** tracks), the panel displays the one on the **highest-priority track**. "Highest priority" is defined as the **topmost track in the track list** — `IP_TRACKNUMBER == 1` is highest, larger numbers are lower priority — mirroring Reaper's native video-compositing precedence (the topmost track wins). This is the **net-new behavior** over Story 4.3/4.4, which deliberately took the **first** spanning RAV item (the interim "single-item / first-match" rule). It **amends the pinned "single-item / first-match" interim decision** → it **requires an AR20 Spec Change Log entry** (Task 4); note architecture.md:42 already pins the *intent* as "current-item selection by playhead **+ priority**", so this story **realizes** that pin rather than deviating from it.
3. **AC3 — Deterministic, never-error selection (FR14).** The selection is **deterministic** every frame: the same playhead position with the same item layout always picks the same item (no flicker between candidates). Ties (two RAV items on the **same** track, or a track whose number can't be read) resolve to a **stable** choice (first encountered in the item walk) — no oscillation, no garbage, no crash. Every Reaper handle in the new track lookup is **null-guarded** (`GetMediaItem_Track` may return null; a 0 / -1 / unreadable `IP_TRACKNUMBER` is treated as **lowest** priority, never as "track 0 wins"). No-throw, main-thread-only, read-only (D5/AR18) — exactly as Story 4.3/4.4.
4. **AC4 — ≥10 simultaneous items, no measurable degradation (NFR-P6).** With **at least 10** animation items in the project (across one or more tracks), the extension runs **without measurable performance degradation** on the reference workstation: the docked viewer holds **≥60 fps** (NFR-P1/P5 unchanged), play/scrub stays frame-synced, and the host does not stutter or crash. This holds **structurally** because (a) only the **single** item under the playhead is ever loaded/rendered — the viewer lazily loads exactly one asset on path-change (Story 4.3), so VRAM stays at ~one asset regardless of item count, far under the architecture.md:1016 ~130 MB budget; and (b) the per-frame current-item walk is **O(itemCount)** of cheap read-only Reaper calls (~10 iterations at the ~66 Hz tick = negligible). 4.5 **validates** this in-Reaper with ≥10 items present; the only code is the priority-selection change.
5. **AC5 — No regression, scope-tight, deferrals documented.** Every Epic 1–3 and Story 4.1–4.4 behavior is unchanged: the viewer opens via `RAV: Open Viewer` and docks; a single item still drives the rig exactly as in 4.3/4.4 (play/scrub/hold, left-trim `D_STARTOFFS` offset, move/resize/color/rename); static meshes still render; foreign files (`.txt`/`.wav`) are not hijacked; the `pcmsrc` factory registers/deregisters **symmetrically with the same pointer** (`plugin_main.cpp` untouched); item length + name from 4.2 unchanged; unload/quit does not crash. The **only** new Reaper symbols are `REAPERAPI_WANT_GetMediaItem_Track` and `REAPERAPI_WANT_GetMediaTrackInfo_Value` (to read the spanning item's track number); **no new dependency, no `CMakeLists.txt` / `plugin_main.cpp` change**. **Same-track overlap front-most precedence** and **take playrate (`D_PLAYRATE`, still deferred from 4.4)** and **an asset cache to share VRAM across duplicate files** remain out of scope — recorded in `deferred-work.md` (Task 5). Build stays `/W3 /permissive-` warning-free (NFR-R5).

> **Scope boundary (read before coding):** This story makes the current-item selection pick the **highest-priority track** on overlap (FR14) and **validates ≥10-item capacity** (NFR-P6). The **only production code** is: (a) two new `WANT_` symbols, and (b) generalizing the `GetCurrentAnimItem` walk from "**first** spanning RAV item wins" to "**lowest `IP_TRACKNUMBER`** among all spanning RAV items wins" (full scan, no early-return on first match; compute the matched path + `animTime` for the **winner**). It does **NOT**: resolve **same-track** overlap by item front-most/Z-order (deferred — ties keep first-encountered); honor take **`D_PLAYRATE`** (still deferred from 4.4); add an **asset cache** to dedup VRAM for the same file on two tracks (architecture.md:1101 post-MVP); change `renderer.*`, `viewer_window.cpp`, or the per-item geometry/clamp (`RenderFrame`'s `[0,duration]` clamp and the path-change reload are unchanged); or persist/restore per-item state (`SaveState`/`LoadState` — **Epic 6 / D9**, still stubbed). **The proof 4.5 works:** put ≥10 animation items across several tracks, **overlap two on different tracks**, scrub/play across them — the viewer always shows the item on the **topmost** of the overlapping tracks and switches asset as the playhead moves between items, all at ≥60 fps with no stutter.

## Tasks / Subtasks

- [x] **Task 1 — Add the two track-lookup symbols (AC2, AC5)** — edit `src/reaper_api.h` only
  - [x] Add **two** lines to the `REAPERAPI_WANT_*` block, next to the Story 4.3/4.4 transport/item set ([reaper_api.h:25-38](../../src/reaper_api.h#L25)):
    - `#define REAPERAPI_WANT_GetMediaItem_Track       // item -> MediaTrack* (for the spanning item's track)`
    - `#define REAPERAPI_WANT_GetMediaTrackInfo_Value   // "IP_TRACKNUMBER" (1-based, top=1 = highest priority; 0=not found, -1=master)`
  - [x] Do **not** add any other symbol. `IP_TRACKNUMBER` is **track-level**, read via `GetMediaTrackInfo_Value(MediaTrack*, "IP_TRACKNUMBER")` — it **returns the int directly** (a 1-based track number; **not** a pointer-out param like the bool/`int*` attributes). Use `GetMediaItem_Track(item)` (the canonical item→track accessor) to get the `MediaTrack*` first. Both resolve through the existing `REAPERAPI_LoadAPI` call (no new registration). Leave the includes and all other defines untouched. (`GetMediaItemTrack` — no underscore — also exists in the SDK; use `GetMediaItem_Track` with the underscore, the one this story declares.)
- [x] **Task 2 — Select the highest-priority spanning item in the walk (AC2, AC3)** — edit `src/pcm_source_anim.cpp` only
  - [x] Generalize `GetCurrentAnimItem` ([pcm_source_anim.cpp:159-198](../../src/pcm_source_anim.cpp#L159)) from "return on the **first** spanning RAV item" to "scan **all** items, keep the spanning RAV item on the **lowest `IP_TRACKNUMBER`**". Concretely: remove the `return true` inside the loop; instead track the best candidate and emit the outputs **after** the loop. Sketch (match the existing null-guard discipline and the "`std::string` assign LAST" rule):
    ```cpp
    MediaItem*      best     = nullptr;
    MediaItem_Take* best_tk  = nullptr;
    double          best_ip  = 0.0;
    int             best_tn  = INT_MAX;   // smaller = higher priority; sentinel = none yet

    for (int i = 0, n = CountMediaItems(proj); i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (!it) continue;
        const double ip = GetMediaItemInfo_Value(it, "D_POSITION");
        const double il = GetMediaItemInfo_Value(it, "D_LENGTH");
        if (pos < ip || pos >= ip + il) continue;          // not under the playhead
        MediaItem_Take* tk = GetActiveTake(it);
        if (!tk) continue;
        if (!IsOurs(GetMediaItemTake_Source(tk))) continue; // skip non-RAV (coexistence)

        // Highest-priority track = topmost = smallest 1-based IP_TRACKNUMBER.
        // 0 (not found) / -1 (master) / null track => lowest priority, never "wins as 0".
        MediaTrack* tr = GetMediaItem_Track(it);
        int tn = tr ? (int)GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER") : 0;
        if (tn <= 0) tn = INT_MAX;

        if (tn < best_tn) {            // strictly-less keeps the FIRST item on a tie (AC3)
            best_tn = tn; best = it; best_tk = tk; best_ip = ip;
        }
    }
    if (!best) return false;          // no RAV item under the playhead -> viewer holds (AC1)
    ```
  - [x] After the loop, compute the outputs **for the winner** exactly as 4.4 does today (path from the winner's source, `D_STARTOFFS` term, low guard — **unchanged math**, just sourced from `best`/`best_tk` instead of the first match):
    ```cpp
    PCM_source* src = GetMediaItemTake_Source(best_tk);
    const char* fn  = src ? src->GetFileName() : nullptr;   // re-read; null-guard
    out_path = fn ? fn : "";
    const double off = GetMediaItemTakeInfo_Value(best_tk, "D_STARTOFFS");
    double at = (pos - best_ip) + off;
    if (!(at >= 0.0)) at = 0.0;       // low guard (also catches non-finite); RenderFrame clamps the HIGH end
    out_anim_time = at;
    return true;
    ```
  - [x] Add `#include <climits>` for `INT_MAX` (currently `pcm_source_anim.cpp` includes `<cctype>`/`<cstring>`/`<string>` — add the one header). Do **not** otherwise change the includes.
  - [x] **Keep byte-for-byte:** `IsOurs` (incl. the wrapped `GetSource()` check), the silent-source stubs, the no-throw/boundary contract, and the `D_STARTOFFS` math (the clamp is the same — only the **selection** changed, not the per-item time mapping). Do **not** touch `renderer.*`, `viewer_window.cpp`, `plugin_main.cpp`, `asset_loader.*`. The walk **no longer early-returns** — it must scan every item to find the highest-priority one; that O(itemCount) cost is negligible at ~10 items / ~66 Hz (AC4).
  - [x] **Why first-encountered on a tie (AC3):** two RAV items on the **same** track (same `IP_TRACKNUMBER`) is the only tie; `tn < best_tn` (strict) leaves the first walk-order item selected, which is **deterministic** (no flicker). Resolving same-track overlap by item front-most/Z-order is explicitly deferred (Task 5) — the FR14 wording is "highest-priority **track**", which this satisfies.
- [x] **Task 3 — Update the `GetCurrentAnimItem` doc comment (AC2, AC5)** — edit `src/pcm_source_anim.h` + the in-`.cpp` comment
  - [x] In `src/pcm_source_anim.h` ([pcm_source_anim.h:28-40](../../src/pcm_source_anim.h#L28)), update the doc block: the walk now selects the spanning RAV item on the **highest-priority (topmost) track**, not "the FIRST RAV item spanning it (single-item / first-match — overlap+priority is Story 4.5)". Keep the rest (no-throw, main-thread, read-only, fills `out_path`/`out_anim_time`, returns false on no match) verbatim. This is a **comment-only** change to the header — **no signature/API change**.
  - [x] In `src/pcm_source_anim.cpp`, replace the now-stale inline comment ([pcm_source_anim.cpp:167-169](../../src/pcm_source_anim.cpp#L167) — "First RAV item spanning the playhead wins (single-item / first-match; overlap + track-priority is Story 4.5 — do NOT generalise this walk)") with the new rule (highest-priority track wins; ties keep first-encountered; full scan).
- [x] **Task 4 — Record the selection amendment in the Spec Change Log (AC2, AR20)** — edit `_bmad-output/planning-artifacts/architecture.md` only
  - [x] Append a dated entry to the **`## Spec Change Log`** section ([architecture.md:1126](../planning-artifacts/architecture.md#L1126)) in the same **Trigger / Amendment / KEEP** format as the existing entries (use the 2026-06-27 Story 4.4 entry at :1128 as the template): **"2026-06-27 — Current-item selection resolves overlap by highest-priority track (Story 4.5, FR14)."** State the **amendment**: the interim **"single-item / first-match"** rule (pinned as KEEP in the 4.4 entry, architecture.md:1154) becomes **"among all RAV items spanning the playhead, the one on the lowest `IP_TRACKNUMBER` (topmost track) wins"** — realizing the intent already pinned at architecture.md:42 ("current-item selection by playhead **+ priority**"). Note this is the trigger for the two new `WANT_` symbols (`GetMediaItem_Track`, `GetMediaTrackInfo_Value`). **KEEP:** the clamp discipline (`[0, duration]` in `RenderFrame`); the `D_STARTOFFS` left-trim term (4.4); the path-change lazy single-asset load (4.3 — only the playhead item is in VRAM, NFR-P6); **same-track Z-order precedence and take `D_PLAYRATE` still NOT honored** (deferred). No registration is added (boundary rule intact — read-only track query).
- [x] **Task 5 — Record the remaining deferrals (AC5)** — edit `_bmad-output/implementation-artifacts/deferred-work.md` only
  - [x] Append a dated entry **"Deferred from: story 4.5 (multi-item / current-item selection)"** recording **two** items: (1) **same-track overlap front-most precedence** — two RAV items overlapping on the **same** track resolve to the first walk-order item, not Reaper's visual front-most/Z-order; **trigger:** if a sound designer stacks RAV items on one track and expects the front-most to show, read item Z-order / lane info and tie-break on it in `GetCurrentAnimItem`. (2) **asset cache for duplicate files** — the same `.glb` on two tracks would load twice if both were ever current (architecture.md:220/1101); **trigger:** if VRAM or load-time becomes an issue with many distinct-then-repeated items, add the shared `Asset` cache (D2 factory pattern) keyed by file path. Match the existing deferred-work entry format. (Note: take `D_PLAYRATE` is **already** logged from 4.4 — do not duplicate; you may cross-reference it.)
- [x] **Task 6 — Write the validator gate §8 (AC1–AC5)** — edit `docs/PHASE3_VALIDATOR_GATE.md` only
  - [x] Append a **§8 "Story 4.5"** section in the **exact row-based format** of §6 (4.3) / §7 (4.4) ([PHASE3_VALIDATOR_GATE.md:215-277](../../docs/PHASE3_VALIDATOR_GATE.md#L215)): a short intro paragraph, a **Suggested fixtures** list (≥10 animation clips — they can be copies of the same file on different tracks; two clips placed to **overlap** on different tracks), the row table, a **Scope note**, and a **pending Result** line.
  - [x] Rows (click-based, in-Reaper on Windows): **(1) several items, one shows** — place 3–4 RAV items at different timeline positions across one or more tracks; scrub/play across them → the viewer shows whichever item is **under the playhead** and switches asset as you cross into the next item; **(2) between items holds** — park the playhead in a gap with no RAV item → the rig **holds** the last frame (no revert to the startup fixture, no crash); **(3) overlap → topmost track wins** — place two RAV items (different clips) overlapping in time on **two different tracks**; move the playhead into the overlap → the viewer shows the item on the **topmost** (higher-in-the-list) track; **(4) swap track order** — move the lower track **above** the other (or vice-versa) so the priority flips → the viewer now shows the **other** clip in the overlap (confirms it's track-order, not item-order, driving it); **(5) ≥10 items / fps** — put **at least 10** RAV items in the project (across several tracks); the docked viewer holds **≥60 fps** (watch the console fps line), play across them with **no stutter** and no host hitch; **(6) coexist at scale** — with the 10+ animation items plus an audio + a video + a MIDI item, press Play → everything plays together, no dropout/glitch (4.4 coexistence holds at scale); **(7) no regression / scope-clean** — 4.1–4.4 behavior intact (open/dock, play/scrub/hold, left-trim offset, move/resize/color/rename, item length+name, symmetric `-pcmsrc` same pointer, `plugin_main.cpp` untouched, unload/quit no crash); the **only** new symbols are `GetMediaItem_Track` + `GetMediaTrackInfo_Value`; no new dependency; build clean at `/W3 /permissive-`.
  - [x] In the **Scope note** state: **same-track** overlap resolves to first-encountered (front-most Z-order = deferred); take **`D_PLAYRATE`** still deferred (4.4); **asset cache** for duplicate files = post-MVP (architecture.md:1101); `SaveState`/`LoadState` content = **Epic 6**. Add the pending-result line in the §6/§7 style.
- [x] **Task 7 — Self-verify on Linux + scope audit (AC5)**
  - [x] `cmake -B <build> -S .` configures (host-stubbed for the Reaper/GL link, as every Phase-2/3 story) — **no new source file** (`reaper_api.h` is a header; `pcm_source_anim.{h,cpp}` are already in SOURCES) → **no `CMakeLists.txt` change**.
  - [x] Grep-audit scope: the **only** files changed are `src/reaper_api.h` (+2 `WANT_`), `src/pcm_source_anim.cpp` (the priority-selection walk + `<climits>`), `src/pcm_source_anim.h` (comment-only), `docs/PHASE3_VALIDATOR_GATE.md` (+§8), `_bmad-output/planning-artifacts/architecture.md` (+ Spec Change Log entry), `_bmad-output/implementation-artifacts/deferred-work.md` (+ two deferrals), and `sprint-status.yaml`. **Confirm `git diff` touches nothing else in `src/`** — no `renderer.*`, `viewer_window.cpp`, `plugin_main.cpp`, `asset_loader.*`, `scene.h`, `animation.h`, `camera.h`, shader, or `CMakeLists.txt` edit.
  - [x] Confirm the only new symbols are `GetMediaItem_Track` + `GetMediaTrackInfo_Value`, no new dependency, and `rec->Register("pcmsrc"/"-pcmsrc", …)` still appears **only** in `plugin_main.cpp` (boundary rule intact — this story added a read-only track query, not a registration).

## Dev Notes

### What this story is — validation-heavy, one real change (the priority walk)

Like 4.4, most of 4.5 already works for free; the single net-new behavior is **overlap → highest-priority track**. Story 4.3 already (a) finds the RAV item under the playhead each tick, (b) lazily reloads the asset on a path change, and (c) holds the last frame off-span — so **multiple items**, **switching between them**, and **holding in the gaps** (AC1) all work today with the existing first-match walk. The only thing first-match gets *wrong* is **overlap**: when two RAV items span the playhead at once, it returns whichever appears first in `CountMediaItems` order (arbitrary), not the one on the highest-priority track. Task 2 fixes exactly that — scan all spanning RAV items, keep the lowest `IP_TRACKNUMBER`. [Source: [pcm_source_anim.cpp:170-196](../../src/pcm_source_anim.cpp#L170); 4-3 RenderTick path-change reload [viewer_window.cpp:197-225](../../src/viewer_window.cpp#L197)]

### "Highest-priority track" = topmost = smallest `IP_TRACKNUMBER` (AC2)

The PRD (FR14) says "the one on the highest-priority track" but doesn't define the direction. **Define it as the topmost track in the track list** (`IP_TRACKNUMBER == 1`), which is how Reaper natively composites overlapping **video** items (the item on the higher track wins) — so a sound designer's intuition from video reference carries over. Mechanics:
- `GetMediaItem_Track(item)` → `MediaTrack*` (null-guard it).
- `GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER")` **returns the 1-based track number directly** (it is *not* a pointer-out attribute like `B_MUTE`/`I_SOLO`): `1` = top, larger = lower, `0` = not found, `-1` = master. Treat `≤ 0` (and a null track) as **lowest** priority (`INT_MAX`) so a not-found/master case never spuriously "wins as track 0". [Source: [reaper_plugin_functions.h:2185](../../extern/reaper-sdk/sdk/reaper_plugin_functions.h#L2185) IP_TRACKNUMBER semantics; [:1966](../../extern/reaper-sdk/sdk/reaper_plugin_functions.h#L1966) GetMediaItem_Track]

### The walk changes from early-return to full-scan-with-best (AC2, AC3)

The current loop `return true`s on the first match (correct for single-item). For priority you **cannot** early-return — a higher-priority item may appear later in `CountMediaItems` order — so scan **all** items, track `best` + `best_tn`, and emit after the loop. Two correctness points:
- **Compute the time for the WINNER, not the first match.** `out_path`, `D_STARTOFFS`, and `(pos − ip)` must all come from `best`/`best_tk`/`best_ip`, not the first spanning item. (The per-item *math* is unchanged from 4.4 — `animTime = (pos − itemStart) + D_STARTOFFS`, low-guarded; only *which* item feeds it changed.)
- **Strict `<` keeps the first item on a tie** (same-track overlap) → deterministic, no flicker (AC3). [Source: [pcm_source_anim.cpp:181-194](../../src/pcm_source_anim.cpp#L181) — the winner-emit block to lift out of the loop]

### NFR-P6 (≥10 items) is satisfied by construction — confirm, don't re-architect (AC4)

architecture.md:1016 budgeted "~13 MB × 10 = 130 MB" assuming all 10 assets resident. **Our implementation is lighter than that budget:** the viewer loads exactly **one** asset — the item under the playhead — and only reloads on a path change (Story 4.3), so VRAM holds **one** asset regardless of item count, and only one rig is skinned/drawn per frame. The per-frame walk is `O(itemCount)` of cheap read-only calls (`CountMediaItems`/`GetMediaItem`/two `…Info_Value`/`GetActiveTake`/`GetMediaItemTake_Source`/`GetMediaItem_Track`/`GetMediaTrackInfo_Value`) — ~10 iterations at ~66 Hz is hundreds of calls/sec, negligible. So **no asset cache, no batching, no code beyond the priority walk** is needed for NFR-P6; the §8 gate just **observes** ≥60 fps with ≥10 items present. The one honest caveat: rapid scrubbing **across** many distinct items triggers the existing per-path cold reload at each boundary (Story 4.3 behavior, already bounded by the path-change gate) — that's a per-asset load cost, not an item-count scaling cost, and is unchanged by 4.5. [Source: architecture.md:1016, :220 (VRAM budget), :1101 (asset cache = post-MVP); [viewer_window.cpp:197-225](../../src/viewer_window.cpp#L197) lazy single-asset reload]

### Why the selection change needs an AR20 Spec Change Log entry (Task 4)

The 4.4 Spec Change Log entry's **KEEP** line explicitly pinned "single-item / first-match (overlap + track-priority is **Story 4.5**, not this change)" (architecture.md:1154). 4.5 changes that interim rule, so AR20 / architecture.md:1115 require a Spec Change Log entry with the trigger / amendment / KEEP discipline — otherwise a future reader of the 4.4 entry would believe first-match is still the rule. Note this is **convergence**, not divergence: architecture.md:42 has always pinned "current-item selection by playhead **+ priority**", so 4.5 *realizes* the long-standing pin (the first-match walk was the documented interim). Use the 2026-06-27 (4.4) and 2026-06-23 (Spike) entries as the format template. [Source: architecture.md:1115 (AR20), :1154 (the 4.4 KEEP line 4.5 amends), :42 (the pinned "playhead + priority" intent)]

### Boundary rule + no-throw still apply

This story registers nothing — the boundary rule (`rec->Register` only in `plugin_main.cpp`) is preserved; **confirm it, don't touch it**. `GetCurrentAnimItem` stays null-safe on every Reaper handle and no-throw (D5/AR18); the two added calls reuse the already-validated `it`/use a fresh null-guarded `MediaTrack*`, so they add no new unguarded deref. The `std::string` assign stays **after** the loop (only for the winner), preserving the 4.3 "assign last" contract. [Source: [pcm_source_anim.cpp:139-148](../../src/pcm_source_anim.cpp#L139) IsOurs null-safety; AR15/AR18]

### Architecture doc uses the pre-rename ReaImGui design — trust the live tree

As in 4.1–4.4, `architecture.md`'s data-flow prose (e.g. :894 "determine current PCM_source from playhead position (FR14)") describes the superseded ReaImGui-FBO viewer; the live mechanism is the native docked GL window (`viewer_window.cpp` + `renderer.cpp`) polling the transport via `GetCurrentAnimItem`. Follow the **live `src/`** (the `rav` namespace, `console_log.h` `[RAV]` format). The pinned *principle* — current item chosen by playhead **+ track priority** — is exactly what 4.5 implements. [Source: project_reaper_docked_close memory; architecture Spec Change Log :1160; 4-4 Dev Notes "trust the live tree"]

### Project Structure Notes

- **Edit:** `src/reaper_api.h` (+2 `WANT_`: `GetMediaItem_Track`, `GetMediaTrackInfo_Value`), `src/pcm_source_anim.cpp` (priority-selection walk + `<climits>`), `src/pcm_source_anim.h` (comment-only doc update), `docs/PHASE3_VALIDATOR_GATE.md` (+§8), `_bmad-output/planning-artifacts/architecture.md` (+ Spec Change Log entry), `_bmad-output/implementation-artifacts/deferred-work.md` (+ two deferrals), `sprint-status.yaml`.
- **Untouched:** `src/renderer.{h,cpp}` (the `[0,duration]` clamp + skinning are unchanged — selection changed, not per-item time/geometry), `src/viewer_window.cpp` (the path-change reload + off-span hold already handle multi-item switching), `src/plugin_main.cpp`, `src/asset_loader.*`, `src/scene.h`, `src/animation.h`, `src/camera.h`, the shader, `CMakeLists.txt`. If you edit any of these you have left scope.
- Conventions to match: `namespace rav`; `console_log.h` (`LogInfo`/`LogError`); `/W3 /permissive-` clean; the §6/§7 gate-doc row format; the Spec Change Log Trigger/Amendment/KEEP format; the deferred-work entry format. DLL `reaper_animviewer.dll`.

### Testing standards

No automated test harness exists (Reaper-hosted native DLL). **The gate IS the test (AR19):** Antho runs `docs/PHASE3_VALIDATOR_GATE.md` §8 on Windows-in-Reaper — place several items across tracks and confirm the one under the playhead shows; **overlap two on different tracks and confirm the topmost-track one wins** (and flips when you reorder the tracks); put **≥10 items** in the project and confirm **≥60 fps** with no stutter, audio/video/MIDI still coexisting. That in-Reaper pass **is** completion (FR13/FR14 selection feel + NFR-P6 fps are only observable in-Reaper on Windows). On Linux you can only confirm the CMake configure + the scope/grep audit (the DLL/GL/transport link is host-stubbed, as in every prior Phase-2/3 story). [Source: feedback_trust_ingame_validation; AR19; PHASE3_VALIDATOR_GATE.md §7]

### References

- [epics.md — Epic 4 / Story 4.5](../planning-artifacts/epics.md) (AC source; FR13, FR14, NFR-P6)
- [prd.md — FR13 multi-item / FR14 current-item-by-priority](../planning-artifacts/prd.md) (:375-376); [prd.md — NFR-P6 ≥10 items](../planning-artifacts/prd.md) (:442)
- [architecture.md — "current-item selection by playhead + priority"](../planning-artifacts/architecture.md) (:42) — the pinned intent 4.5 realizes
- [architecture.md — Spec Change Log + the 4.4 "single-item / first-match" KEEP line 4.5 amends](../planning-artifacts/architecture.md) (:1126, :1154)
- [architecture.md — D1–D17 deviation requires a Spec Change Log entry (AR20)](../planning-artifacts/architecture.md) (:1115)
- [architecture.md — NFR-P6 mapping + VRAM budget + post-MVP asset cache](../planning-artifacts/architecture.md) (:1016, :220, :1101)
- Current code: [pcm_source_anim.cpp `GetCurrentAnimItem` (the walk to generalize)](../../src/pcm_source_anim.cpp#L159), [reaper_api.h `WANT_*` block](../../src/reaper_api.h#L25), [viewer_window.cpp `RenderTick` path-change reload + off-span hold](../../src/viewer_window.cpp#L197)
- SDK: [GetMediaItem_Track](../../extern/reaper-sdk/sdk/reaper_plugin_functions.h#L1966), [GetMediaTrackInfo_Value / IP_TRACKNUMBER semantics](../../extern/reaper-sdk/sdk/reaper_plugin_functions.h#L2185)
- Previous stories: [4-1 registration](./4-1-pcm-source-plugin-registers-and-creates-a-source-from-a-file.md), [4-2 correctly-sized item](./4-2-drop-an-animation-file-on-a-track-to-create-a-correctly-sized-item.md), [4-3 playhead drives the frame](./4-3-playhead-position-drives-the-displayed-animation-frame.md), [4-4 native-media + coexistence](./4-4-animation-items-behave-like-native-media-and-coexist-with-other-tracks.md)
- Gate doc: [docs/PHASE3_VALIDATOR_GATE.md](../../docs/PHASE3_VALIDATOR_GATE.md) (§6/§7 format to mirror for §8)
- Deferred log: [deferred-work.md](./deferred-work.md) (format to mirror; take `D_PLAYRATE` already logged from 4.4)

### Previous-story intelligence (Epic 1–4 patterns to reuse)

- **Scope discipline is audited every story.** 4.5 has a narrow code surface (two `WANT_`, one walk generalization). The traps: (a) **also** resolving same-track Z-order (deferred — FR14 is "highest-priority **track**"); (b) building an **asset cache** "for NFR-P6" (not needed — only one asset is ever resident; it's post-MVP per architecture.md:1101); (c) touching `renderer.*`/`viewer_window.cpp` (the multi-item switch + clamp already work — only the *selection* in `GetCurrentAnimItem` changes); (d) honoring `D_PLAYRATE` (still deferred from 4.4).
- **Formula/decision changes are governed (AR20).** The most-likely review flag is implementing the priority walk **without** the Spec Change Log entry (Task 4) — the 4.4 KEEP line ("first-match … is Story 4.5") must not silently disagree with the code.
- **Full-scan correctness.** The single most-likely *bug* is emitting the **first** match's path/time instead of the **winner's** after switching to a best-candidate scan — verify `out_path`/`D_STARTOFFS`/`(pos − ip)` all read from `best`/`best_tk`/`best_ip`.
- **Cold-path / null-guard hardening recurs in code-review.** `GetMediaItem_Track` can return null and `IP_TRACKNUMBER` can be `0`/`-1` — map all of those to lowest priority (`INT_MAX`), never let a not-found track win as "0". Keep `<climits>` for `INT_MAX`. No new unguarded deref.
- **Symmetric register is a review focus** — you add **no** registration, only two read-only track queries. Confirm `plugin_main.cpp` is untouched and `-pcmsrc` stays there with the same pointer (4.1 invariant).
- **AR19 — the in-Reaper Windows gate IS the gate.** FR14 overlap-by-priority and NFR-P6 ≥10-item fps are only observable in-Reaper on Windows; Linux confirms the source/scope audit only. Write §8 so Antho can click "place items / overlap two on different tracks / reorder tracks / 10+ items at ≥60 fps" and tick each row.
- **Epic 4 closes here.** 4.5 is the **final Epic 4 story** — after its gate passes, the epic-4 retrospective is optional and Phase 3 (transport-driven animation) is complete. Keep the §C non-canonical-skin gate item (deferred-work.md) in mind transitively: 4.5 changes *which item* feeds the renderer, not vertex space or skinning, so any displaced-rig observation at this gate is still the 3.3 `globalInverse` knob, not a 4.5 bug.

## Dev Agent Record

### Agent Model Used

Claude Opus 4.8 (1M context) — `claude-opus-4-8[1m]`

### Debug Log References

- CMake host-stubbed configure clean (`cmake -B /tmp/rav-build-45 -S .`): "non-Windows platform detected — Phase 0 only supports Windows. Build target stubbed." — exit 0. No new source file → no `CMakeLists.txt` change (confirmed: not in diff).
- Full `/W3` SDK compile is not checkable on Linux (the Reaper SDK's `reaper_plugin.h` includes `../WDL/swell/swell.h`, not vendored → host-stub, as in every prior Phase-2/3 story). Instead: isolated `-Wall -Wextra -Werror` syntax+logic compile of the priority walk with SDK types stubbed — **clean (exit 0)**. A second isolated test proved the walk selects the **lowest-`IP_TRACKNUMBER`** item (track 2) even though it sits at walk index 1, not index 0 — confirming the winner's path/`animTime` are emitted, not the first match's (the #1 likely bug per Dev Notes).
- Scope grep audit: `git diff --name-only -- src/` = `pcm_source_anim.cpp`, `pcm_source_anim.h`, `reaper_api.h` **and `viewer_window.cpp`** (4 files). **CORRECTION (code review 2026-06-27):** the original draft of this line claimed `viewer_window.cpp` was untouched and the src diff was *exactly* 3 files — that was **inaccurate**. `viewer_window.cpp` is in fact modified (launch file-picker + startup-fixture removal); see the File List entry and the AR20 Spec Change Log entry. `plugin_main.cpp`, `renderer.*`, `asset_loader.*`, `scene.h`, `animation.h`, `camera.h`, shader, `CMakeLists.txt` remain untouched. `Register("pcmsrc", …)` / `("-pcmsrc", …)` still appear only in `plugin_main.cpp` with the same pointer (boundary rule + symmetry intact). Only new symbols: `GetMediaItem_Track`, `GetMediaTrackInfo_Value`.

### Completion Notes List

- **One net-new behavior, as scoped.** Generalized `GetCurrentAnimItem`'s walk from early-return-on-first-match to a full scan keeping the spanning RAV item on the **lowest `IP_TRACKNUMBER`** (topmost track = highest priority, mirroring Reaper's native video compositing). Removed the in-loop `return true`; track `best`/`best_tk`/`best_ip`/`best_tn`; emit `out_path` / `D_STARTOFFS` / `(pos − best_ip)` for the **winner** after the loop. Strict `<` keeps the first item on a same-track tie → deterministic, no flicker (AC3).
- **Null / not-found / master guards (AC3).** `GetMediaItem_Track` null-guarded; `IP_TRACKNUMBER` of `0` (not found) / `-1` (master) / null track all map to `INT_MAX` (lowest priority) — a not-found track never "wins as track 0". Added `#include <climits>` for `INT_MAX`.
- **Per-item time math byte-for-byte unchanged (AC1, AC5).** Only *which* item feeds the mapping changed; `animTime = (pos − itemStart) + D_STARTOFFS`, low-guarded `≥0`, `RenderFrame`'s `[0,duration]` high clamp — all identical to 4.4. `IsOurs` (incl. wrapped `GetSource()`), the silent stubs, the no-throw boundary, and the "`std::string` assign LAST" rule preserved.
- **NFR-P6 by construction, not by new code (AC4).** Only the single playhead item is ever loaded (Story 4.3 lazy single-asset reload), so VRAM ≈ one asset regardless of item count; the per-frame walk is O(itemCount) of cheap read-only calls. No asset cache, no batching added — the §8 gate just observes ≥60 fps with ≥10 items.
- **Governance (AC2/AR20).** Added the dated Spec Change Log entry amending the 4.4 "single-item / first-match" KEEP → "lowest-`IP_TRACKNUMBER` wins", realizing the long-pinned architecture.md:42 "playhead + priority" intent. Recorded the two remaining deferrals (same-track Z-order tie-break; asset cache for duplicate files) in `deferred-work.md`; `D_PLAYRATE` cross-referenced, not duplicated (already logged from 4.4).
- **Gate is the test (AR19).** Wrote PHASE3_VALIDATOR_GATE.md §8 (7 click-based rows: several-items/one-shows, gap-holds, overlap→topmost wins, swap-track-order flips, ≥10 items @ ≥60 fps, coexist-at-scale, no-regression/scope-clean). Linux confirms only the configure + scope/grep audit; FR14 overlap-by-track-priority and NFR-P6 fps are observable only in-Reaper on Windows.

### File List

- `src/reaper_api.h` — +2 `REAPERAPI_WANT_` symbols (`GetMediaItem_Track`, `GetMediaTrackInfo_Value`) for the spanning item's track number (Task 1).
- `src/pcm_source_anim.cpp` — generalized `GetCurrentAnimItem` to the highest-priority-track selection walk; `+#include <climits>`; updated inline comment (Tasks 2, 3). **(Review patch P1)** the candidate guard is `if (!best || tn < best_tn)` so a lone spanning item on an unreadable track (`tn` remapped to `INT_MAX`) is still selected (AC3) rather than dropped by `INT_MAX < INT_MAX`.
- `src/pcm_source_anim.h` — comment-only doc-block update for the new selection rule (Task 3).
- `src/viewer_window.cpp` — **(added in review, see Spec Change Log 2026-06-27 "Launch file-picker + Epic-3 startup fixture removed")** removed the launch-time "choose a 3D model" dialog (`PromptForModelFile`, `g_model_path`, `#include <commdlg.h>`) and the Epic-3 startup-fixture fallback; the viewport now opens idle and is driven solely by the timeline (Story 4.3+). This was **out of the story's original declared scope** (which named `viewer_window.cpp` untouched) — Antho confirmed it as an intentional timeline-only-load-path decision during the code review; regularized via an AR20 Spec Change Log entry + this File List correction.
- `_bmad-output/planning-artifacts/architecture.md` — Spec Change Log entry (AR20) for the overlap-by-track-priority amendment (Task 4).
- `_bmad-output/implementation-artifacts/deferred-work.md` — two deferrals: same-track Z-order tie-break + asset cache for duplicate files (Task 5).
- `docs/PHASE3_VALIDATOR_GATE.md` — §8 validator gate for Story 4.5 (Task 6).
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — story 4-5 status `ready-for-dev` → `in-progress` → `review`; `last_updated`.
- `_bmad-output/implementation-artifacts/4-5-multiple-animation-items-and-current-item-selection.md` — task checkboxes, Dev Agent Record, File List, Change Log, Status.

### Change Log

- 2026-06-27 — Story 4.5 implemented: current-item selection resolves overlap by highest-priority track (lowest `IP_TRACKNUMBER`); +2 `WANT_` symbols; AR20 Spec Change Log entry; two deferrals recorded; §8 validator gate written. Scope: 3 `src/` files (`reaper_api.h`, `pcm_source_anim.{cpp,h}`) + 4 docs/tracking files. Status → review (awaiting Antho's in-Reaper Windows gate §8, AR19).

- 2026-06-27 — Code review (BMAD 3-layer). 1 decision-needed → Antho confirmed the `viewer_window.cpp` launch-picker + Epic-3 startup-fixture removal as intentional (timeline-only load path); regularized via an AR20 Spec Change Log entry, File List + Dev Agent Record corrections, and a §8 gate note. 1 patch applied (P1 — `if (!best || tn < best_tn)` so a lone item on an unreadable track isn't dropped, AC3). 4 deferred (same-track Z-order [already logged], half-open end-edge, stale-frame-after-delete, empty-path-topmost — last three added to deferred-work.md). 2 dismissed as noise. Status stays `review` pending Antho's in-Reaper Windows gate §8 (AR19).

### Review Findings (code-review BMAD 3-layer — 2026-06-27)

- [x] **[Review][Decision → RESOLVED: KEEP] `src/viewer_window.cpp` is modified despite the story declaring it untouched/out-of-scope** — The working tree removes the startup "choose a 3D model" file-picker (`PromptForModelFile`, `g_model_path`, `#include <commdlg.h>`) **and** the Epic-3 startup-fixture free-running fallback from `viewer_window.cpp` (−78 lines). This contradicts (a) the **Scope boundary** + **Project Structure Notes** + Task 2/Task 7 which name `viewer_window.cpp` as untouched ("If you edit any of these you have left scope"); (b) the **Dev Agent Record** "Scope grep audit" which asserts the `src/` diff is *exactly* `pcm_source_anim.{cpp,h}` + `reaper_api.h` (3 files — actual is **4**) and the File List which omits `viewer_window.cpp`; (c) **AC5** ("Every Epic 1–3 / Story 4.1–4.4 behavior is unchanged") — the removed startup fixture was pinned by Story 4.3 as "free-running Epic-3 loop, AC5 preserved". No Task and no AR20 Spec Change Log entry authorize this removal, and it leaves the `loop=/*!g_transport_driven*/` branch in `RenderFrame` effectively dead (no startup asset ever loads). **Needs Antho's intent:** is the launch-picker/fixture removal a deliberate "timeline-is-the-only-load-path" decision to keep (→ amend story scope + File List + Dev Record + add an AR20 Spec Change Log entry for the AC5 behavior change + a gate note), or accidental working-tree spill to revert out of this commit (→ restore the picker, handle its removal in its own story)?
- [x] **[Review][Patch → APPLIED] Lone/all spanning RAV item(s) with an unreadable track (`tn<=0`) are never selected** [src/pcm_source_anim.cpp:198] — `best_tn` seeds at `INT_MAX`; a track that is null / `0` (not found) / `-1` (master) remaps `tn` to `INT_MAX`, and the strict guard `if (tn < best_tn)` makes `INT_MAX < INT_MAX` false → `best` stays null → returns false even when a valid RAV item is under the playhead. This contradicts the code's own comment (:192-193 "lowest priority (INT_MAX), never 'wins as track 0'") and **AC3** ("a track whose number can't be read resolves to a stable choice — first encountered"). Cold path (a real media item almost always has `IP_TRACKNUMBER ≥ 1`), trivial fix: `if (!best || tn < best_tn)` (preserves the first-on-tie semantics).
- [x] **[Review][Defer] Same-track overlap resolves to walk-order, not visual Z-order/front-most** [src/pcm_source_anim.cpp:198] — deferred, already recorded in deferred-work.md (story 4.5 deferrals); FR14 is "highest-priority **track**", which is satisfied.
- [x] **[Review][Defer] Half-open span `[ip, ip+il)`: a lone item with the playhead parked exactly on its end shows nothing** [src/pcm_source_anim.cpp:185] — deferred, pre-existing (4.3/4.4 boundary, unchanged by 4.5's selection change); single-sample edge.
- [x] **[Review][Defer] After the current item is deleted/moved off-playhead the viewer holds a stale frame indefinitely** [src/viewer_window.cpp RenderTick hold] — deferred, pre-existing (`g_transport_driven` latch + AC3 "freeze on leave"); tangled with the decision item above.
- [x] **[Review][Defer] An "ours"-typed topmost item with an empty source path can mask a valid lower-track item** [src/pcm_source_anim.cpp:206-208] — deferred, pre-existing exposure (our source carries `m_path`, so realistically unreachable); `LoadAsset("")` fails+logs once, no per-frame storm.
