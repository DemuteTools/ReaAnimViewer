---
baseline_commit: 4f6ad8062f4f1cd2acea8810b5a61c4f66299221
---

# Story 4.4: Animation items behave like native media and coexist with other tracks

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want to move, resize, color, and rename animation items and run them alongside audio/video/MIDI,
so that they integrate into my normal session without special handling.

## Acceptance Criteria

From [epics.md Story 4.4](../planning-artifacts/epics.md) (FR11, FR12):

1. **AC1 — Native item controls behave identically (FR11).** With an animation item on a track, using Reaper's **native** item controls the sound designer can **move** it (drag along/across tracks), **resize** it (both edges), **recolor** it, and **rename** the item/take — and each responds **exactly like any other Reaper media item**. **Move** re-anchors playback to the item's new start and **right-resize** changes how much timeline the clip spans — both ride for free on Story 4.3's live per-frame read of `D_POSITION`/`D_LENGTH`. **Color** and **rename** are pure Reaper item operations that never touch our `PCM_source` (the viewer keys its asset off the source **file path**, not the item name/color), so they have **zero** effect on rendering and never crash.
2. **AC2 — Left-edge trim offsets into the clip, like native media (FR11 — the one piece that needs code).** Trimming an item's **left** edge in Reaper sets the take's **`D_STARTOFFS`** (start-in-source), exactly as it does for an audio item. The viewer **honors it**: the displayed time becomes **`animTime = (playheadTime − itemStart) + D_STARTOFFS`** — so left-trimming **reveals later frames of the same clip** (the animation stays anchored in source time) rather than restarting from frame 0. This is the net-new behavior over Story 4.3, and it **amends the pinned mapping** `animTime = playheadTime − itemStart` → it **requires an AR20 Spec Change Log entry** (Task 4) since it changes a D-decision formula.
3. **AC3 — Resize/trim edge cases hold, never error.** Resizing an item **longer than its clip** (or left-trimming so far the source window runs past the clip end) keeps the rig **holding its last frame** (the `loop==false` `[0, duration]` clamp in `RenderFrame`, Story 4.3); resizing **shorter** simply ends the item earlier; moving the playhead **outside** the span holds the **first/last visible** frame — none of these flicker to garbage, NaN, disappear, or crash. The low end is guarded `animTime ≥ 0`; the high end is clamped to the clip duration by the existing `RenderFrame` clamp (so a deep startoffs past the clip end holds the last frame, not garbage).
4. **AC4 — Coexistence: other media plays back unaffected (FR12).** With an animation item in the **same session** as **audio**, **video**, and **MIDI** items on other tracks, pressing Play plays **all** of them: the rig follows the transport **and** the audio/video/MIDI play back **exactly as they would without the extension loaded** — no audio dropouts/glitches, no stuck video, no missed MIDI, no host crash. This holds because our source reports itself **non-audio** (`GetNumChannels()==0`, `GetSampleRate()==0` → silent, `GetSamples` emits nothing — Story 4.1), so Reaper's audio engine never pulls samples from it, and the per-frame item walk (Story 4.3) **skips** every non-RAV item (`IsOurs` is false for audio/video/MIDI sources). No code change is needed for coexistence — the story **validates** it in-Reaper with real audio/video/MIDI present.
5. **AC5 — No regression, scope-tight, deferral documented.** Every Epic 1–3 and Story 4.1/4.2/4.3 behavior is unchanged: the viewer opens via `RAV: Open Viewer` and docks; play/scrub drive the rig and hold at the ends; static meshes still render; foreign files (`.txt`/`.wav`) are not hijacked; the `pcmsrc` factory registers/deregisters **symmetrically with the same pointer** (`plugin_main.cpp` untouched); item **length + name** from 4.2 are unchanged; unload/quit does not crash. The **only** new Reaper symbol is `REAPERAPI_WANT_GetMediaItemTakeInfo_Value` (for `D_STARTOFFS`); **no new dependency, no `CMakeLists.txt` / `plugin_main.cpp` change**. **Take playrate (`D_PLAYRATE`) is the one item-edit deliberately NOT honored** — recorded in `deferred-work.md` (Task 5): a playrate change does not time-stretch the animation in this MVP (it also touches 4.2's item-length sizing, a larger change). Build stays `/W3 /permissive-` warning-free (NFR-R5).

> **Scope boundary (read before coding):** This story makes an animation item a **first-class Reaper media item** (move / resize **both edges** / color / rename) and **validates coexistence** with audio/video/MIDI (FR11/FR12). The **only production code** is: (a) one new `WANT_` symbol, and (b) adding the `D_STARTOFFS` term to `GetCurrentAnimItem`. It does **NOT**: honor take **`D_PLAYRATE`** (deferred — AC5); resolve **overlap / track-priority among multiple items** (FR14 — **Story 4.5**); guarantee **≥10 simultaneous items** without degradation (NFR-P6 — also **Story 4.5**); or persist/restore per-item state (`SaveState`/`LoadState` content — **Epic 6 / D9**, still stubbed). **The proof 4.4 works:** drop an animation next to an audio + a video + a MIDI item, **move / resize (both edges) / recolor / rename** the animation item with Reaper's normal controls, press Play, and watch **everything play together** — the rig follows the transport (a left-trimmed item revealing later frames of the clip) and the other media is untouched.

## Tasks / Subtasks

- [x] **Task 1 — Add the take-info symbol for `D_STARTOFFS` (AC2, AC5)** — edit `src/reaper_api.h` only
  - [x] Add **one** line to the `REAPERAPI_WANT_*` block, next to the Story 4.3 transport/item set ([reaper_api.h:32](../../src/reaper_api.h#L32)): `#define REAPERAPI_WANT_GetMediaItemTakeInfo_Value  // "D_STARTOFFS" (take start-in-source — left-trim offset)`.
  - [x] Do **not** add any other symbol. `D_STARTOFFS` is a **take-level** parameter, read via `GetMediaItemTakeInfo_Value(MediaItem_Take*, "D_STARTOFFS")` — **not** `GetMediaItemInfo_Value` (which is item-level: `D_POSITION`/`D_LENGTH`, already present). Resolves through the existing `REAPERAPI_LoadAPI` call (no new registration). Leave the includes and all other defines untouched.
- [x] **Task 2 — Honor `D_STARTOFFS` in the transport→current-item query (AC2, AC3)** — edit `src/pcm_source_anim.cpp` only
  - [x] In `GetCurrentAnimItem` ([pcm_source_anim.cpp:159-191](../../src/pcm_source_anim.cpp#L159)), after the active take is confirmed and `IsOurs(src)` passes (we already hold `tk`), read the take start offset and add it to the item-relative time. Replace the current clamp block:
    ```
    double at = pos - ip;
    out_anim_time = (at < 0.0) ? 0.0 : (at > il ? il : at);
    ```
    with:
    ```
    // Native left-trim: D_STARTOFFS is the take's start-in-source. animTime advances
    // from there, so trimming the left edge reveals LATER frames of the same clip
    // (FR11 native-media behavior) rather than restarting at frame 0. Take-level value.
    const double off = GetMediaItemTakeInfo_Value(tk, "D_STARTOFFS");
    double at = (pos - ip) + off;
    if (at < 0.0) at = 0.0;   // low guard; RenderFrame (loop==false) clamps the HIGH end
                              // to the clip duration, so a deep startoffs past the clip
                              // end holds the last frame (AC3) — do NOT re-clamp to il here.
    out_anim_time = at;
    ```
  - [x] **Why the upper `il` clamp is dropped (not a regression):** with no trim (`off == 0`) and an in-span `pos` (`pos < ip + il`), `at = pos − ip ∈ [0, il)` — identical to 4.3. The old `> il ? il` branch never fired for in-span values, so removing it changes nothing for the no-trim case. With `off > 0` the value legitimately exceeds `il`; the existing `RenderFrame` `[0, duration]` clamp (Story 4.3, renderer.cpp) is the correct upper bound (clip duration, not item length), so a startoffs that runs past the clip end **holds the last frame**. Keep the **low** guard (`≥ 0`) for a pathological negative `off`.
  - [x] **Off-span hold still works:** when the playhead leaves the span, `GetCurrentAnimItem` returns `false` and the viewer freezes the last in-span value (Story 4.3) — which is now `~off` at the left edge / `~off+il` at the right edge, i.e. the first/last **visible** frame. No viewer_window change needed.
  - [x] Keep everything else byte-for-byte: the walk, `IsOurs`, the null guards, the first-match-wins single-item rule (overlap/priority is 4.5), the no-throw/boundary contract. The **only** change in this file is the three-line startoffs block above. Do **not** touch `renderer.*`, `viewer_window.cpp`, `pcm_source_anim.h`, or anything else.
- [x] **Task 3 — Write the validator gate §7 (AC1–AC5)** — edit `docs/PHASE3_VALIDATOR_GATE.md` only
  - [x] Append a **§7 "Story 4.4"** section in the **exact row-based format** of §5 (4.2) / §6 (4.3): a short intro paragraph, a **Suggested fixtures** list (a known-duration animation clip; an **audio**, a **video**, and a **MIDI** item to sit alongside), the row table, a **Scope note**, and a **pending Result** line.
  - [x] Rows (click-based, in-Reaper on Windows): **(1) move** the item along/across tracks → the rig stays in transport-sync at the new position; **(2) resize right edge LONGER** than the clip → past the clip end the rig **holds its last frame**, still plays from frame 0 at its start; **(3) resize right edge SHORTER** → ends earlier, plays only the trimmed span, no error; **(4) trim LEFT edge** → the rig shows **later frames of the same clip** at the new start (it does **NOT** restart from frame 0) — the native-media offset behavior (AC2); **(5) recolor** → item changes color, rendering **unaffected**, no crash; **(6) rename** the item/take → renamed, rendering **unaffected** (asset still keyed to the file); **(7) coexist — audio**: an **audio** item on another track plays back **normally** alongside; **(8) coexist — video**: a **video** item plays normally; **(9) coexist — MIDI**: a **MIDI** item plays normally; **(10) all together**: audio + video + MIDI + the animation item in one session, press Play → **everything plays at once**, the rig follows the transport and the other media has **no dropout/glitch/stutter**, no host crash; **(11) no regression / scope-clean**: 4.1–4.3 behavior intact (open/dock, play/scrub/hold, item length+name, symmetric `-pcmsrc` same pointer, `plugin_main.cpp` untouched, unload/quit no crash), the **only** new symbol is `GetMediaItemTakeInfo_Value`, no new dependency, build clean at `/W3 /permissive-`.
  - [x] In the **Scope note** state: overlap/track-priority + ≥10-item capacity = **Story 4.5**; `SaveState`/`LoadState` content = **Epic 6**; **take playrate (`D_PLAYRATE`) is deferred (AC5)** — changing an item's playrate does not retime the animation in this MVP. Add the pending-result line in the §5/§6 style.
- [x] **Task 4 — Record the formula amendment in the Spec Change Log (AC2, AR20)** — edit `_bmad-output/planning-artifacts/architecture.md` only
  - [x] Append a dated entry to the **`## Spec Change Log`** section ([architecture.md:1126](../planning-artifacts/architecture.md)) in the same **Trigger / Amendment / KEEP** format as the existing entries: **"2026-06-27 — Item-relative mapping gains the take start-offset term (Story 4.4, FR11 native left-trim)."** State the **amendment**: the pinned mapping `animTime = playheadTime − itemStart` (architecture.md:42, and the "Confirmed" line of the 2026-06-23 Spike entry) becomes **`animTime = (playheadTime − itemStart) + take.D_STARTOFFS`**, so a left-trimmed item reveals later source frames like native media. **KEEP:** the clamp discipline (`[0, duration]` in `RenderFrame`), single-item/first-match (4.5 owns priority), and **take `D_PLAYRATE` explicitly NOT yet honored** (deferred). Note this is the trigger for the new `GetMediaItemTakeInfo_Value` `WANT_` symbol.
- [x] **Task 5 — Record the playrate deferral (AC5)** — edit `_bmad-output/implementation-artifacts/deferred-work.md` only
  - [x] Append a dated entry **"Deferred from: story 4.4 (native-media coexistence)"** recording that take **`D_PLAYRATE`** is **not** honored — changing an item's playrate does not time-stretch the animation in this MVP. Give the **trigger** ("if sound designers need to retime an animation item via playrate, multiply the item-relative time by `D_PLAYRATE` in `GetCurrentAnimItem` **and** reconcile with 4.2's `GetLength`/item-sizing, which currently assumes playrate 1.0 — a cross-story change"). Match the existing deferred-work entry format. (Note: `D_STARTOFFS` is **no longer** deferred — it ships in this story.)
- [x] **Task 6 — Self-verify on Linux + scope audit (AC5)**
  - [x] `cmake -B <build> -S .` configures (host-stubbed for the Reaper/GL link, as every Phase-2/3 story) — **no new source file** (`reaper_api.h` is a header; `pcm_source_anim.cpp` is already in SOURCES) → **no `CMakeLists.txt` change**.
  - [x] Grep-audit scope: the **only** files changed are `src/reaper_api.h` (+1 `WANT_`), `src/pcm_source_anim.cpp` (the 3-line startoffs block), `docs/PHASE3_VALIDATOR_GATE.md` (+§7), `_bmad-output/planning-artifacts/architecture.md` (+ Spec Change Log entry), `_bmad-output/implementation-artifacts/deferred-work.md` (+ playrate entry), and `sprint-status.yaml`. **Confirm `git diff` touches nothing else in `src/`** — no `renderer.*`, `viewer_window.cpp`, `pcm_source_anim.h`, `plugin_main.cpp`, `asset_loader.*`, `scene.h`, `animation.h`, `camera.h`, shader, or `CMakeLists.txt` edit.
  - [x] Confirm the only new symbol is `GetMediaItemTakeInfo_Value`, no new dependency, and `rec->Register("pcmsrc"/"-pcmsrc", …)` still appears **only** in `plugin_main.cpp` (boundary rule intact — this story added a take-info read, not a registration).

### Review Findings

_Code review 2026-06-27 (BMAD 3-layer: Blind Hunter / Edge Case Hunter / Acceptance Auditor). 2 patches, 5 dismissed (4 Blind false-positives verified by the Edge Case Hunter against the live `RenderFrame` clamp; 1 commit-hygiene note)._

- [x] [Review][Patch] §7 gate Result line claimed an in-Reaper PASS that had not happened — changed to a PENDING line until Antho runs the gate (story is in `review`; every other artifact says pending; Task 3 asked for a pending result) [docs/PHASE3_VALIDATOR_GATE.md:269]
- [x] [Review][Patch] NaN-unsafe low guard — `if (at < 0.0)` did not catch a non-finite `D_STARTOFFS` (NaN compares false); rewrote as `if (!(at >= 0.0))` so a pathological value resets to 0 instead of propagating through `RenderFrame` into `ComputePose`. Defensive/unreachable in practice, matches the project's cold-path hardening pattern [src/pcm_source_anim.cpp:189]

## Dev Notes

### What this story is — mostly free, one real line of code

Story 4.3's scope note pre-declared 4.4 as *"native move/resize/color/rename + audio/video/MIDI coexistence validation (FR11/FR12 … most of it works for free once transport-drive lands)."* That holds — **with one deliberate exception Antho asked for:** **left-edge trim must behave like native media** (offset into the clip, not restart at 0). So 4.4 is a **validation gate plus one surgical code addition** (`D_STARTOFFS`):

- **Move + right-resize** ride for free on Story 4.3's live read: `GetCurrentAnimItem` reads `D_POSITION`/`D_LENGTH` **fresh every render tick**, so Reaper mutating the item geometry is reflected on the next frame. [Source: [pcm_source_anim.cpp:170-188](../../src/pcm_source_anim.cpp#L170)]
- **Color + rename** never reach our code — Reaper owns `I_CUSTOMCOLOR`/`P_NAME`; the viewer keys its asset off the source **file path** (`GetFileName()` → `g_current_anim_path`), so recolor/rename are no-ops for the renderer. [Source: [pcm_source_anim.cpp:67](../../src/pcm_source_anim.cpp#L67); 4-3 Task 4]
- **Coexistence** is structural: our source is silent / 0-channel (4.1 stub), so Reaper's mixer never pulls audio from it, and the item walk `continue`s past every non-`RAV_ANIM` source. Audio/video/MIDI are never touched — the gate just **observes** this with real media present. [Source: [pcm_source_anim.cpp:75-88,139](../../src/pcm_source_anim.cpp#L75)]
- **Left-trim** is the **one** behavior that needs code: honor `D_STARTOFFS` so trimming the left edge reveals later clip frames (native), not a restart. That is Task 2.

### The `D_STARTOFFS` math — precise (AC2, AC3)

Native media: the visible source window is `[D_STARTOFFS, D_STARTOFFS + D_LENGTH]` (at playrate 1.0), displayed across the timeline span `[D_POSITION, D_POSITION + D_LENGTH]`. So for a playhead `pos` inside the span:

```
animTime = (pos − D_POSITION) + D_STARTOFFS
```

- `D_STARTOFFS` is **take-level** → `GetMediaItemTakeInfo_Value(tk, "D_STARTOFFS")` (we already hold `tk`). Do **not** use `GetMediaItemInfo_Value` (item-level).
- **Clamp:** keep only the **low** guard (`animTime ≥ 0`). The existing `RenderFrame(loop==false)` clamp to `[0, duration]` is the correct **upper** bound — the **clip duration**, not the item length — so a startoffs that pushes past the clip end **holds the last frame** (AC3). The old 4.3 upper clamp to `il` would be *wrong* now (it would cap below the legitimately-larger offset value), and it was a no-op for the no-trim in-span case anyway (`pos − ip < il`), so dropping it does not regress 4.3.
- **No-trim case is byte-identical to 4.3:** `off == 0` ⇒ `animTime = pos − ip` exactly as before — move and right-resize keep working unchanged.

[Source: [pcm_source_anim.cpp:184-188](../../src/pcm_source_anim.cpp#L184); renderer.cpp `RenderFrame` `[0,duration]` clamp (Story 4.3 Task 3); architecture.md:42]

### Why the formula change needs an AR20 Spec Change Log entry (Task 4)

The mapping `animTime = playheadTime − itemStart` is **pinned**: architecture.md:42 states it, and the 2026-06-23 Spike Change Log "Confirmed" line repeats it verbatim (`animTime = playPos − itemStart`). Adding the `+ D_STARTOFFS` term is a **deviation from a D-decision formula**, and AR20 / architecture.md:1115 require *"a Spec Change Log entry on the active phase spec, with the trigger / amendment / KEEP discipline."* This is not optional bookkeeping — it is how the project keeps D1–D17 honest. Without it, a future reader of architecture.md:42 would believe the code ignores startoffs. Use the existing entries (lines 1128, 1146) as the format template. [Source: architecture.md:1115, AR20; Spec Change Log format :1128]

### Coexistence — why "no interference" is structural, not luck (AC4)

FR12 ("coexist … without interfering") is satisfied by construction:
- **We are not in the audio graph.** `GetSampleRate()` returns `0.0` (`<1.0 ⇒ silent`), `GetNumChannels()==0`, `GetSamples` emits nothing — Reaper's mixer never schedules or pulls audio from our item, so it cannot starve/glitch/preempt an audio/video/MIDI item. [Source: [pcm_source_anim.cpp:50-53,75-81](../../src/pcm_source_anim.cpp#L50)]
- **Our read-only walk is inert.** `GetCurrentAnimItem` only *reads* project state (and now one take-info value) — it never mutates, registers, or issues a transport command, so it cannot perturb other tracks. [Source: [pcm_source_anim.cpp:159-191](../../src/pcm_source_anim.cpp#L159); boundary rule]
- **Rendering is on an independent ~66 Hz timer**, decoupled from Reaper's audio thread (architecture.md:1132 Spike finding) — audio continuity does not depend on our frame rate. (Heavy-load fps with ≥10 items = NFR-P6 / Story 4.5.)

The §7 gate's job is the **in-Reaper confirmation** of this structural property with real audio + video + MIDI present. [Source: 4.1 gate PASS 2026-06-26; architecture.md:1132]

### Boundary rule + no-throw still apply

This story registers nothing — the boundary rule (`rec->Register` only in `plugin_main.cpp`) is preserved; **confirm it, don't touch it**. `GetCurrentAnimItem` stays null-safe on every Reaper handle and no-throw (D5/AR18); the added `GetMediaItemTakeInfo_Value(tk, …)` call uses the already-null-checked `tk` (the walk `continue`s on `!tk`), so it adds no new unguarded deref. [Source: [pcm_source_anim.cpp:176-188](../../src/pcm_source_anim.cpp#L176); AR15/AR18]

### Take playrate (`D_PLAYRATE`) stays deferred (AC5) — and why

Honoring playrate (`animTime = startoffs + (pos − ip) * playrate`) is *also* one `GetMediaItemTakeInfo_Value` call, but it is **deliberately out of scope** because it does **not** stand alone: changing a take's playrate makes Reaper auto-resize the item, and 4.2's `GetLength()`/item-sizing assumes playrate 1.0. Honoring playrate correctly means reconciling the displayed-time mapping **and** the item-length contract across 4.2/4.4 — a cross-story change. Left-trim (`D_STARTOFFS`) has no such coupling (item length is unchanged by a left trim past the displayed window), so it ships here while playrate is logged in `deferred-work.md` with its trigger. [Source: 4-2 `GetLength`/`ProbeAnimationDuration`; deferred-work.md format]

### Architecture doc uses the pre-rename ReaImGui design — trust the live tree

As in 4.1–4.3, `architecture.md`'s data-flow prose describes the superseded ReaImGui-FBO viewer; the live mechanism is the native docked GL window (`viewer_window.cpp` + `renderer.cpp`) reading the transport via `GetCurrentAnimItem`. Follow the **live `src/`** (the `rav` namespace, `console_log.h` `[RAV]` format). The pinned *principle* — item is first-class Reaper media, time derived from the playhead — is unchanged; only the formula gains the startoffs term (Task 4). [Source: project_reaper_docked_close memory; architecture Spec Change Log :1140; 4-3 Dev Notes "trust the live tree"]

### Project Structure Notes

- **Edit:** `src/reaper_api.h` (+1 `WANT_GetMediaItemTakeInfo_Value`), `src/pcm_source_anim.cpp` (3-line `D_STARTOFFS` block in `GetCurrentAnimItem`), `docs/PHASE3_VALIDATOR_GATE.md` (+§7), `_bmad-output/planning-artifacts/architecture.md` (+ Spec Change Log entry), `_bmad-output/implementation-artifacts/deferred-work.md` (+ playrate entry), `sprint-status.yaml`.
- **Untouched:** `src/pcm_source_anim.h` (no API change — the helper signature is unchanged), `src/renderer.{h,cpp}` (the `[0,duration]` clamp already does the upper bound), `src/viewer_window.cpp` (off-span freeze already correct), `src/plugin_main.cpp`, `src/asset_loader.*`, `src/scene.h`, `src/animation.h`, `src/camera.h`, the shader, `CMakeLists.txt`. If you edit any of these you have left scope.
- Conventions to match: `namespace rav`; `console_log.h` (`LogInfo`/`LogError`); `/W3 /permissive-` clean; the §5/§6 gate-doc row format; the Spec Change Log Trigger/Amendment/KEEP format. DLL `reaper_animviewer.dll`.

### Testing standards

No automated test harness exists (Reaper-hosted native DLL). **The gate IS the test (AR19):** Antho runs `docs/PHASE3_VALIDATOR_GATE.md` §7 on Windows-in-Reaper — move / resize **both edges** / recolor / rename the animation item, **left-trim it and confirm it reveals later frames of the clip (not a restart)**, drop it next to **audio + video + MIDI** items, press Play, and confirm everything plays together with the rig in transport-sync and the other media untouched. That in-Reaper pass **is** completion (FR11/FR12 are only observable in-Reaper on Windows). On Linux you can only confirm the CMake configure + the scope/grep audit (the DLL/GL/transport link is host-stubbed, as in every prior Phase-2/3 story). [Source: feedback_trust_ingame_validation; AR19; PHASE3_VALIDATOR_GATE.md §6]

### References

- [epics.md — Epic 4 / Story 4.4](../planning-artifacts/epics.md) (AC source; FR11, FR12)
- [prd.md — FR11 native item controls / FR12 coexistence](../planning-artifacts/prd.md) (:373-374)
- [architecture.md — `animTime = playheadTime − itemStart`, clamped (the formula 4.4 amends)](../planning-artifacts/architecture.md) (:42)
- [architecture.md — Spec Change Log (format + the "Confirmed" mapping line 4.4 amends)](../planning-artifacts/architecture.md) (:1126)
- [architecture.md — D1–D17 deviation requires a Spec Change Log entry (AR20)](../planning-artifacts/architecture.md) (:1115)
- [architecture.md — native docked GL window on an independent ~66 Hz timer](../planning-artifacts/architecture.md) (:1132)
- Spike reference: `git show spike/0-1-feasibility:src/spike_pcmsource.cpp` (`CurrentAnimTime` = `pos − D_POSITION`, **no** startoffs — 4.4 adds the term)
- Current code: [pcm_source_anim.cpp `GetCurrentAnimItem` (clamp block to edit)](../../src/pcm_source_anim.cpp#L181), [`AnimSource` silent source](../../src/pcm_source_anim.cpp#L54), [reaper_api.h `WANT_*` block](../../src/reaper_api.h#L25), [renderer.cpp `RenderFrame` `[0,duration]` clamp](../../src/renderer.cpp#L277), [viewer_window.cpp `RenderTick` off-span freeze](../../src/viewer_window.cpp#L170)
- Previous stories: [4-1 registration](./4-1-pcm-source-plugin-registers-and-creates-a-source-from-a-file.md), [4-2 correctly-sized item](./4-2-drop-an-animation-file-on-a-track-to-create-a-correctly-sized-item.md), [4-3 playhead drives the frame](./4-3-playhead-position-drives-the-displayed-animation-frame.md)
- Gate doc: [docs/PHASE3_VALIDATOR_GATE.md](../../docs/PHASE3_VALIDATOR_GATE.md) (§5/§6 format to mirror for §7)
- Deferred log: [deferred-work.md](./deferred-work.md) (format to mirror for the playrate entry)

### Previous-story intelligence (Epic 1–4 patterns to reuse)

- **Scope discipline is audited every story.** 4.4 has a narrow code surface (one `WANT_`, three lines). The traps: (a) **also** honoring `D_PLAYRATE` (deferred — AC5, it's coupled to 4.2's sizing); (b) building **multi-item/priority** logic (that is 4.5 — keep the walk first-match); (c) re-adding an upper `il` clamp "to be safe" (it would cap the legitimate startoffs value — the `[0,duration]` clamp lives in `RenderFrame`, not here).
- **Formula changes are governed (AR20).** The single most-likely review flag is implementing the startoffs term **without** the Spec Change Log entry (Task 4). The entry is a required deliverable, not a nicety — architecture.md:42 must not silently disagree with the code.
- **Cold-path hardening recurs in code-review.** The new `GetMediaItemTakeInfo_Value(tk, …)` reuses the already-null-checked `tk`; no new unguarded deref. A pathological negative `D_STARTOFFS` is caught by the `≥ 0` low guard. Keep the `std::string` assign last, after the match (4.3 contract).
- **Symmetric register is a review focus** — you add **no** registration, only a take-info read. Confirm `plugin_main.cpp` is untouched and `-pcmsrc` stays there with the same pointer (4.1 invariant).
- **AR19 — the in-Reaper Windows gate IS the gate.** FR11 item-control feel, the left-trim offset, and FR12 simultaneous audio/video/MIDI playback are only observable in-Reaper on Windows; Linux confirms the source/scope audit only. Write §7 so Antho can click move/resize-both-edges/left-trim/color/rename/coexist and tick each row.
- **The §C non-canonical-skin gate item (deferred-work.md) still applies** transitively: 4.4 changes *which time* feeds `ComputePose` (it adds the startoffs offset) but **not** vertex space or the skinning path, so any displaced-rig observation at this gate is the same `globalInverse` knob from 3.3, not a 4.4 bug.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8 (1M context) — BMAD dev-story workflow.

### Debug Log References

- CMake host-stubbed configure (Linux): `cmake -B /tmp/build_story44 -S .` → exit 0, "non-Windows platform detected — Phase 0 only supports Windows. Build target stubbed." (the Reaper/GL link is Windows-only, as in every prior Phase-2/3 story). The first `/mnt` build-dir attempt failed only on a sandbox `configure_file` "Operation not permitted" filesystem restriction — unrelated to the change (no `CMakeLists.txt` edit, no new source file).
- Scope grep-audit: `git diff --name-only -- src/` = exactly `src/reaper_api.h` + `src/pcm_source_anim.cpp`; `renderer.{h,cpp}`, `viewer_window.cpp`, `pcm_source_anim.h`, `plugin_main.cpp`, `asset_loader.{cpp,h}`, `scene.h`, `animation.h`, `camera.h`, `CMakeLists.txt` all confirmed clean.
- Boundary rule intact: `rec->Register("pcmsrc", …)` (plugin_main.cpp:87) + `g_register("-pcmsrc", …)` (plugin_main.cpp:56) with the **same** `PcmSourceRegistration()` pointer — unchanged, no `Register` added by this story.
- Upper-clamp confirmation: `RenderFrame` (renderer.cpp:286) already does `std::min(std::max(anim_time_seconds, 0.0f), clip.duration)` for `loop==false` — the correct `[0, duration]` upper bound, so dropping the old `il` cap in `GetCurrentAnimItem` is not a regression.

### Completion Notes List

- **Task 1** — added the single new symbol `REAPERAPI_WANT_GetMediaItemTakeInfo_Value` to `src/reaper_api.h` (take-level `D_STARTOFFS`), next to the 4.3 transport/item set; resolves through the existing `REAPERAPI_LoadAPI` call, no new registration. Confirmed it is the **only** new `WANT_` symbol via `git diff`.
- **Task 2** — honored `D_STARTOFFS` in `GetCurrentAnimItem` (`src/pcm_source_anim.cpp`): `animTime = (pos − itemStart) + GetMediaItemTakeInfo_Value(tk, "D_STARTOFFS")`, kept the low guard (`≥ 0`), and **dropped** the old upper `il` clamp (the `RenderFrame` `[0, duration]` clamp is the correct clip-duration upper bound; the old `> il ? il` branch never fired for in-span no-trim values, so no 4.3 regression). Reuses the already-null-checked `tk` — no new unguarded deref. The 3-line block is the only change in the file.
- **Task 3** — appended `§7 Story 4.4` to `docs/PHASE3_VALIDATOR_GATE.md` in the §5/§6 row format: 11 click-based rows (move / resize-right-longer-holds / resize-right-shorter / left-trim-reveals-later-frames / recolor / rename / coexist audio / video / MIDI / all-together / no-regression), suggested fixtures (known-duration clip + audio + video + MIDI), a Scope note (overlap+priority & ≥10 items = 4.5; SaveState/LoadState = Epic 6; playrate deferred), and a PENDING result line.
- **Task 4** — added the dated AR20 Spec Change Log entry to `architecture.md` (Trigger / Amendment / KEEP): the pinned `animTime = playheadTime − itemStart` (architecture.md:42 + the 2026-06-23 Spike "Confirmed" line) becomes `animTime = (playheadTime − itemStart) + take.D_STARTOFFS`; KEEP the `[0, duration]` `RenderFrame` clamp, single-item/first-match (priority = 4.5), and `D_PLAYRATE` not-yet-honored; boundary rule intact (read-only take-info query, not a `Register`).
- **Task 5** — recorded the `D_PLAYRATE` deferral in `deferred-work.md` with its trigger (multiply the item-relative term by `D_PLAYRATE` **and** reconcile with 4.2's `GetLength`/`ProbeAnimationDuration` item-sizing — a cross-story 4.2↔4.4 change; `D_STARTOFFS` has no such coupling, so it ships here).
- **Task 6** — self-verified on Linux: CMake configures host-stubbed; scope grep-audit clean (only the 2 `src/` files + 4 docs); only new symbol is `GetMediaItemTakeInfo_Value`; no new dependency; `plugin_main.cpp`/`CMakeLists.txt` untouched.
- **Testing:** no automated harness exists (Reaper-hosted native DLL) — the §7 gate IS the test (AR19). FR11 native-control feel, the left-trim offset, and FR12 simultaneous audio/video/MIDI playback are observable only in-Reaper on Windows → **awaiting Antho's in-Reaper Windows gate (§7)**.

### File List

- `src/reaper_api.h` — +1 `REAPERAPI_WANT_GetMediaItemTakeInfo_Value` (D_STARTOFFS).
- `src/pcm_source_anim.cpp` — 3-line `D_STARTOFFS` term in `GetCurrentAnimItem` (drop old upper `il` clamp, keep `≥ 0` low guard).
- `docs/PHASE3_VALIDATOR_GATE.md` — +§7 Story 4.4 (11-row gate + scope note + pending result).
- `_bmad-output/planning-artifacts/architecture.md` — +AR20 Spec Change Log entry (2026-06-27, startoffs amendment).
- `_bmad-output/implementation-artifacts/deferred-work.md` — +Story 4.4 `D_PLAYRATE` deferral entry.
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — 4-4 → in-progress → review.
- `_bmad-output/implementation-artifacts/4-4-…-coexist-with-other-tracks.md` — this story (baseline_commit, tasks, Dev Agent Record, Status).

## Change Log

| Date | Change |
|---|---|
| 2026-06-27 | Story 4.4 implemented (→ review). One code addition: `GetCurrentAnimItem` now honors take `D_STARTOFFS` — `animTime = (pos − itemStart) + D_STARTOFFS` (low guard `≥ 0`; old upper `il` clamp dropped, `RenderFrame`'s `[0, duration]` clamp is the correct upper bound), +1 `REAPERAPI_WANT_GetMediaItemTakeInfo_Value`. Move/right-resize/color/rename/coexistence ride free on 4.1–4.3 (validated by §7 gate, not code). Added `PHASE3_VALIDATOR_GATE.md` §7 (11-row gate), AR20 Spec Change Log entry (architecture.md), and the `D_PLAYRATE` deferral (deferred-work.md). Scope audit clean (only `reaper_api.h` + `pcm_source_anim.cpp` in `src/`; no CMake/plugin_main change; boundary rule intact). CMake host-stubbed configure OK. Gate = Antho in-Reaper Windows (AR19) — pending. |
| 2026-06-27 | Story 4.4 created (ready-for-dev) — FR11 (native move/resize/color/rename) + FR12 (audio/video/MIDI coexistence). Move/right-resize ride free on 4.3's live `D_POSITION`/`D_LENGTH` read; color/rename never touch our source; coexistence is structural (silent 0-channel source + `IsOurs`-skip). **The one code addition (per Antho 2026-06-27): honor `D_STARTOFFS` so left-edge trim reveals later clip frames like native media** — `animTime = (pos − itemStart) + take.D_STARTOFFS`, +1 `WANT_GetMediaItemTakeInfo_Value`, 3-line change in `GetCurrentAnimItem` (drop the old upper `il` clamp; `RenderFrame`'s `[0,duration]` clamp is the correct upper bound). This **amends the pinned mapping** (architecture.md:42) → **AR20 Spec Change Log entry required** (Task 4). Deliverables: `reaper_api.h` (+1 symbol), `pcm_source_anim.cpp` (startoffs term), `PHASE3_VALIDATOR_GATE.md` §7 (move/resize-both-edges/left-trim/color/rename/coexist-audio-video-MIDI/all-together/no-regression), architecture.md Spec Change Log entry, `deferred-work.md` (take `D_PLAYRATE` deferred — coupled to 4.2 sizing). Overlap/priority + ≥10 items = 4.5; SaveState/LoadState = Epic 6. Gate = Antho in-Reaper Windows (AR19). |
