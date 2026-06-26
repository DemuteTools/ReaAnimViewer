# Story 4.3: Playhead position drives the displayed animation frame

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sound designer,
I want the rig to follow the Reaper transport as I play and scrub,
so that I can align sound to motion frame-accurately.

## Acceptance Criteria

From [epics.md Story 4.3](../planning-artifacts/epics.md) (FR10, NFR-P5):

1. **AC1 — Playhead drives the displayed frame (FR10).** With an animation item on a track, when the playhead moves over the item, the viewer displays the frame at **`animTime = playheadTime − itemStart`**, clamped to `[0, itemLength]`. During **playback** the rig animates continuously as the play cursor advances (Reaper's audio clock drives `GetPlayPosition2Ex`); during **scrub / edit-cursor** moves the displayed pose tracks the cursor position. The displayed pose is **derived from the playhead every frame, never stored** (architecture.md:305) — there is no internal free-running animation clock once an item is being driven.
2. **AC2 — The displayed asset is the one bound to the item under the playhead.** The viewer shows the **asset loaded from the source file path of the RAV item spanning the playhead** — not a dialog-picked fixture. Scrubbing from one RAV item onto a different one (different source file) **switches the displayed asset** (the new file is loaded on the change). The asset is loaded through the **production loader** (`LoadAsset`, with a current GL context — the render thread) exactly as Epics 2–3 load it.
3. **AC3 — Boundary clamp, no error outside the span.** Moving the playhead **outside the item's span** clamps the displayed pose to the item's **first frame** (before the start) or **last frame** (past the end) — the rig holds, it never disappears, flickers to garbage, NaNs, or crashes. The clamp to `[0, itemLength]` is applied at the transport boundary; `ComputePose`'s own `[0, duration]` clamp is the second line of defence so an item resized **longer** than the clip still holds the last frame.
4. **AC4 — Scrub latency ≤ one rendered frame (NFR-P5).** The playhead is read and the frame is drawn **synchronously in the same render tick** (no async, no worker thread between the playhead read and the draw — architecture.md:1015). At the viewer's ~66 Hz timer this puts the displayed frame within ≈16 ms of the playhead under normal load.
5. **AC5 — No regression, scope-clean.** Every Epic 1–3 and Story 4.1/4.2 behavior is unchanged: the viewer still opens via the action and docks; **static meshes still render** (a non-animated asset shows its single pose); foreign files (`.txt`/`.wav`) are still not hijacked; the `pcmsrc` factory still registers/deregisters **symmetrically with the same pointer**; item **length and name** from 4.2 are unchanged; unload/quit does not crash. Build stays `/W3 /permissive-` warning-free (NFR-R5). **No new runtime dependency.** The only new Reaper symbols are the transport/item-query `REAPERAPI_WANT_*` defines this story needs (listed in Task 1).

> **Scope boundary (read before coding):** This story makes **one** RAV item, under the playhead, drive **which asset is shown and which frame**. It does **not**: resolve **overlap / track-priority among multiple items** (FR14 — that is **Story 4.5**; here, first RAV item spanning the playhead wins); implement native **move/resize/color/rename + audio/video/MIDI coexistence** validation (FR11/FR12 — **Story 4.4**, though most of it works for free once transport-drive lands); persist or restore per-item state (`SaveState`/`LoadState` content — **Epic 6 / D9**, still stubbed); or add a browser/preview (**Epic 5**). The proof 4.3 works: **drop an animation, press play / scrub, and the rig moves with the transport, frame-accurate, holding at the ends.**

## Tasks / Subtasks

- [x] **Task 1 — Expand the Reaper API surface with the transport/item-query symbols (AC1, AC2, AC4)** — edit `src/reaper_api.h` only
  - [x] Add these `#define REAPERAPI_WANT_*` lines (the exact set the Spike validated; architecture.md:337 lists `GetPlayPosition2Ex`/`GetPlayStateEx` as the Phase-3 additions, the rest are the item-walk needed to find the current item):
    - `REAPERAPI_WANT_GetPlayStateEx` — is the transport playing (bit 0).
    - `REAPERAPI_WANT_GetPlayPosition2Ex` — play cursor position (continuous audio clock) when playing.
    - `REAPERAPI_WANT_GetCursorPositionEx` — edit-cursor position when stopped (so a stopped scrub still drives the frame).
    - `REAPERAPI_WANT_CountMediaItems`
    - `REAPERAPI_WANT_GetMediaItem`
    - `REAPERAPI_WANT_GetMediaItemInfo_Value` — reads `"D_POSITION"` / `"D_LENGTH"`.
    - `REAPERAPI_WANT_GetActiveTake`
    - `REAPERAPI_WANT_GetMediaItemTake_Source` — gives the take's `PCM_source*` (to confirm it's ours + read its path).
  - [x] Do **not** add any other symbol (no `Main_OnCommand`, no project/state helpers — those are later phases, architecture.md:338-339). Confirm the existing Phase-0/1 defines and the `#include "reaper_plugin.h"` / `reaper_plugin_functions.h` lines are untouched. These resolve through the **existing** `REAPERAPI_LoadAPI` call (no new registration) exactly as the Story 1.3 docker symbols did.
- [x] **Task 2 — Transport→current-item query in the PCM_source boundary file (AC1, AC2, AC3)** — edit `src/pcm_source_anim.h` and `src/pcm_source_anim.cpp`
  - [x] Declare in `pcm_source_anim.h` (inside the existing `#ifdef _WIN32` guard, next to `PcmSourceRegistration()`): `bool GetCurrentAnimItem(std::string& out_path, double& out_anim_time);` — documented **no-throw**, **main-thread only** (it calls Reaper item APIs). Returns `true` and fills both outs when a RAV item spans the playhead; returns `false` (outs untouched) otherwise.
  - [x] Implement it in `pcm_source_anim.cpp` mirroring the Spike's `CurrentAnimTime` (`git show spike/0-1-feasibility:src/spike_pcmsource.cpp` L112) but **also returning the path**:
    - `ReaProject* proj = nullptr;` (current project). `const bool playing = (GetPlayStateEx(proj) & 1) != 0;` `const double pos = playing ? GetPlayPosition2Ex(proj) : GetCursorPositionEx(proj);`
    - Walk `for (int i = 0, n = CountMediaItems(proj); i < n; ++i)`: `MediaItem* it = GetMediaItem(proj, i); if (!it) continue;` read `ip = GetMediaItemInfo_Value(it, "D_POSITION")`, `il = GetMediaItemInfo_Value(it, "D_LENGTH")`; `if (pos < ip || pos >= ip + il) continue;` `MediaItem_Take* tk = GetActiveTake(it); if (!tk) continue;` `PCM_source* src = GetMediaItemTake_Source(tk); if (!IsOurs(src)) continue;`
    - On the first match: `out_path = src->GetFileName() ? src->GetFileName() : "";` `double at = pos - ip; out_anim_time = (at < 0.0) ? 0.0 : (at > il ? il : at);` (the FR10 `[0, itemLength]` clamp); `return true;`. After the loop, `return false;`.
  - [x] Add a file-local `bool IsOurs(PCM_source* s)` (anonymous namespace, mirroring the Spike L98-105) that returns true when `s->GetType()` equals the **production tag `"RAV_ANIM"`** (NOT the Spike's `"FBXAV_ANIM"` — see Dev Notes "GetType tag is RAV_ANIM"), **also checking the wrapped inner source** `s->GetSource()` so a take that wraps our source (section/reverse wrapper) is still recognised. Null-guard `s` and every `GetType()`/`GetFileName()` return (a raw deref is SEH/UB, not catchable).
  - [x] **Boundary rule holds:** this query only **reads** Reaper state — it does **not** call `rec->Register(...)` (that stays in `plugin_main.cpp`). Leave `AnimSource`, the factory functions, `g_reg`, and `PcmSourceRegistration()` exactly as 4.1/4.2 left them. `GetFileName()` already returns `m_path` ([pcm_source_anim.cpp:58](../../src/pcm_source_anim.cpp#L58)) — reuse it, don't re-probe.
- [x] **Task 3 — Drive the renderer from transport time instead of the free-running clock (AC1, AC3, AC5)** — edit `src/renderer.h` and `src/renderer.cpp`
  - [x] Change `RenderFrame`'s contract so the caller controls **loop vs. clamp**. Signature: `void RenderFrame(float anim_time_seconds, bool loop, int width, int height);` (rename the `time_seconds` param to `anim_time_seconds`; add `bool loop`). Update the header doc comment ([renderer.h:41-45](../../src/renderer.h#L41)).
  - [x] In the per-frame pose block ([renderer.cpp:277-295](../../src/renderer.cpp#L277)) replace the unconditional loop
    `const float t = (clip.duration > 0.0f) ? std::fmod(time_seconds, clip.duration) : 0.0f;`
    with a loop-or-clamp on the new flag:
    `float t = anim_time_seconds; if (clip.duration > 0.0f) { t = loop ? std::fmod(anim_time_seconds, clip.duration) : std::min(std::max(anim_time_seconds, 0.0f), clip.duration); } else { t = 0.0f; }`
    — `loop == false` (transport-driven) **clamps to `[0, duration]`** so the rig **holds the last frame** at the item's end (AC3); `loop == true` keeps the Epic-3 free-running loop for the no-item fixture fallback. **Nothing else in `RenderFrame` changes** — the `pose_valid` gate, the `glUniformMatrix4fv` palette upload, the draw loop, the static-path bit-identity (AC5) are all untouched.
  - [x] Do **not** touch `SetAsset`, `ResetCamera`, the camera, the shader, or the palette sizing. The only renderer change is the time argument's meaning + the one loop/clamp line.
- [x] **Task 4 — Poll the transport each render tick + load the current item's asset (AC1, AC2, AC3, AC4)** — edit `src/viewer_window.cpp` only
  - [x] `#include "pcm_source_anim.h"` (for `GetCurrentAnimItem`).
  - [x] Add two file-scope state vars near `g_model_path`: `std::string g_current_anim_path;` (the source path of the asset currently displayed *from an item* — empty until the first item is driven) and `double g_display_time = 0.0;` (the last transport-driven `animTime`, frozen when no item spans — AC3), plus `bool g_transport_driven = false;` (sticky-true once any RAV item has been current).
  - [x] In `RenderTick` ([viewer_window.cpp:170-177](../../src/viewer_window.cpp#L170)), **before** calling `RenderFrame`, query the transport:
    ```
    std::string item_path; double item_time = 0.0;
    if (GetCurrentAnimItem(item_path, item_time)) {
        if (item_path != g_current_anim_path) {            // item changed → (re)load its asset (cold path)
            LoadResult r = LoadAsset(item_path);            // GL context is current on this thread (AC2)
            if (r.asset) { g_renderer.SetAsset(std::move(*r.asset)); g_current_anim_path = item_path;
                           LogInfo("now showing %s", item_path.c_str()); }
            else         { LogError("load failed [%s]: %s", LoadErrorCategoryName(r.category), r.detail.c_str()); }
        }
        g_display_time = item_time;                         // playhead-driven (already clamped to [0,itemLength])
        g_transport_driven = true;
    }
    // not found → hold: g_display_time and g_current_anim_path unchanged (freeze last frame, AC3)
    g_renderer.RenderFrame(static_cast<float>(g_transport_driven ? g_display_time : ElapsedSeconds()),
                           /*loop=*/!g_transport_driven, g_client_w, g_client_h);
    ```
    - **Reload only on path change** — never every frame (a per-frame `LoadAsset` would blow NFR-P5 and thrash VRAM). The string compare is the cheap gate; `LoadAsset` runs once per scrub-onto-a-new-item.
    - **`LoadAsset` is safe here:** `RenderTick` runs with the WGL context made current in `StartRendering` (the single render thread; viewer_window.cpp:172-174), which is exactly the context `LoadAsset`/`SetAsset` require — unlike the file-**drop** path of 4.2 where there is no GL context (that is why 4.2 used the GL-free `ProbeAnimationDuration`, and why the full GPU load lives **here**).
    - **Thread safety:** the render timer is the `NULL`-hwnd `SetTimer` dispatched by Reaper's **main UI pump** (viewer_window.cpp:50-58 comment), so calling `GetMediaItem*`/`GetPlayPosition2Ex` from `RenderTick` is a **main-thread** call — correct for the Reaper API. Do not move this onto a worker thread.
  - [x] **Keep the startup dialog-fixture load** ([viewer_window.cpp:302-314](../../src/viewer_window.cpp#L302)) **as the no-item fallback** — unchanged. Before any RAV item is ever under the playhead, `g_transport_driven` is false → the fixture renders free-running (Epic 3 behaviour preserved = AC5). Once an item drives the view, transport mode is **sticky** (it does not revert to the looping fixture — that would thrash reloads and violate AC3's "hold the last frame"). Document this with a short comment.
  - [x] Leave the parked-render path, fps counter, camera input, Reset button, and teardown exactly as they are.
- [x] **Task 5 — Extend the validator gate doc (AC1–AC5)** — edit `docs/PHASE3_VALIDATOR_GATE.md`
  - [x] Append a **§6 "Story 4.3"** section in the **exact row-based format** of §3 (4.1) / §5 (4.2). Click-based tests: drop a known animation, open the viewer, **press Play** → the rig animates in time with the transport and **loops with Reaper's loop**; **scrub the edit cursor** across the item → the pose tracks the cursor frame-accurately; **scrub past the end** → the rig **holds the last frame** (no flicker/disappear); **scrub before the start** → holds the **first frame**; drop a **second** animation on another track and scrub onto it → the **displayed asset switches** to the second file; a **static `.glb`** still shows its single pose; `.txt`/`.wav` still Reaper-normal; unload/quit → no crash. Note the **single-item** scope (overlap/priority = 4.5) and that move/resize behaviour is 4.4. Add a pending-result line.
- [x] **Task 6 — Self-verify on Linux + scope audit (what the gate can't cover)**
  - [x] `cmake -B <build> -S .` configures cleanly — **no new source file** (`reaper_api.h` is a header; `pcm_source_anim.cpp`, `renderer.cpp`, `viewer_window.cpp` are already in the `add_reaper_extension` SOURCES list) → **no `CMakeLists.txt` change this story**. Linux configure is host-stubbed for the Reaper/GL link as in every prior story; a full compile is impossible by design (no WDL/swell).
  - [x] Grep-audit scope: only `src/reaper_api.h`, `src/pcm_source_anim.{h,cpp}`, `src/renderer.{h,cpp}`, `src/viewer_window.cpp`, `docs/PHASE3_VALIDATOR_GATE.md` changed (+ `sprint-status.yaml`). **No** change to `CMakeLists.txt`, `src/plugin_main.cpp`, `src/asset_loader.*`, `src/scene.h`, `src/animation.h`, `src/camera.h`, the shader.
  - [x] Confirm the new symbols are **only** the 8 transport/item `WANT_*` defines, no new dependency, and that `Register("pcmsrc"/"-pcmsrc", …)` still appears **only** in `plugin_main.cpp` (the boundary rule is intact — this story added a read-only query, not a registration).

## Dev Notes

### The proven reference — start here

The Spike already validated this exact loop inside Reaper: a playhead→item-relative-time helper feeding the renderer. Read it first:

```
git show spike/0-1-feasibility:src/spike_pcmsource.cpp    # CurrentAnimTime (L112) + IsOurs (L98)
git show spike/0-1-feasibility:src/spike_glwindow.cpp     # GlWindowRenderFrame (L142-154) — the consumer
```

The Spike's consumer is the template for Task 4:
```cpp
double animTime = 0.0;
const bool driven = spike::CurrentAnimTime(animTime);
g_renderer.DrawScene(driven ? static_cast<float>(animTime) : t, g_w, g_h);  // transport time, else free clock
```
**The one thing the Spike did NOT do** (because it loaded a single fixed fixture at window-open) is **switch which file is displayed**. Production must, so this story's helper **also returns the item's source path** and `RenderTick` reloads on a path change (AC2). That path-driven reload is the net-new logic over the Spike — everything else is the Spike's mechanism, cleaned up and boundary-corrected. [Source: docs/SPIKE0_FINDINGS.md — "the playhead drives which frame the viewer shows"; architecture.md:1140]

### THE design — the playhead IS the clock (replace the free-running clock)

Epic 3 drove the rig from a free-running frame clock (`RenderFrame(ElapsedSeconds())` → `fmod(t, duration)`) purely so the validator could *see* the rig move before transport existed (renderer.cpp:272-274 says exactly this: "transport-driven t (playhead − itemStart) is Epic 4 (§F)"). **This story makes that real.** During playback Reaper advances `GetPlayPosition2Ex` on the continuous audio clock, so `animTime = playPos − itemStart` advances smoothly → the rig plays at the full window frame rate (architecture.md:1140 confirms "the play position is continuous (audio clock), so playback renders at the full window frame rate"). During a stopped scrub, `GetCursorPositionEx` tracks the edit cursor. **No internal animation clock is needed for item-driven display** — the displayed time is "recomputed every frame, not stored" (architecture.md:305). The free-running clock survives only as the **no-item fixture fallback** (AC5).

### Why the asset is loaded HERE (and why 4.2 couldn't)

4.2 deliberately parsed only the **duration** on file-drop, CPU-only, because **there is no GL context current on a drop** (the drop is a Reaper item operation, not a viewer render). The **full GPU load** (geometry + textures + skeleton + skin → VRAM) can only happen **on the render thread with the viewer's WGL context current** — and that is precisely where `RenderTick` runs. So 4.3 is the correct home for `LoadAsset(item_path)`: it fires lazily, on the render thread, the first time an item becomes current (AC2). This is the **accepted double-parse** 4.2 flagged (probe on drop, full load on display) — D2, per-item ownership, no shared cache. [Source: 4-2 Dev Notes "Accepted tradeoff — the file is parsed twice"; asset_loader.h:8 LoadAsset requires a current GL context; architecture.md:213-220 D2]

### Boundary clamp (AC3) — hold, don't loop, don't blank

FR10 clamps `animTime` to `[0, itemLength]`. Two layers deliver "holds first/last frame":
1. **In the helper:** `out_anim_time = clamp(pos − itemStart, 0, itemLength)` — at the in-span edges this is ~0 / ~itemLength.
2. **In RenderFrame (transport path, `loop==false`):** clamp `t` to `[0, duration]` — so even when the item was **resized longer than the clip** (4.4), `t` past `duration` holds the clip's last frame instead of wrapping. `ComputePose` itself binary-searches with a `[0, duration]` clamp (3.2), a third safety net.

When the playhead leaves the span entirely, `GetCurrentAnimItem` returns `false` and `RenderTick` **freezes** `g_display_time` (and keeps the asset loaded) — under continuous scrubbing the last in-span value was already at the boundary, so the held frame *is* the first/last frame. A discontinuous cursor jump that never crossed the span holds wherever it last was (an accepted MVP edge — AC3 is phrased "moving the playhead outside," i.e. continuous motion). **Do not** revert to the looping fixture on "not found" (reload thrash + contradicts "hold").

### `GetType` tag is `RAV_ANIM` — match the production tag, not the Spike's

`IsOurs` must compare against **`"RAV_ANIM"`** — the permanent production tag Story 4.1 set ([pcm_source_anim.cpp:46](../../src/pcm_source_anim.cpp#L46)). The Spike's reference code says `"FBXAV_ANIM"` (pre-rename); **do not copy that string** or `IsOurs` silently never matches and nothing is ever driven by the transport. Check both the take source's `GetType()` and its wrapped inner `GetSource()->GetType()` (Reaper may wrap our source). [Source: 4-1 Dev Notes "GetType tag is permanent"; sprint-status epic-1 rename]

### NFR-P5 — synchronous read→draw, no async

`GetCurrentAnimItem` is called inside `RenderTick`, immediately before `RenderFrame`, on the render thread — there is **no async** between reading the playhead and drawing the frame (architecture.md:1015: "Render every frame, no async between playhead read and draw"). At ~66 Hz the displayed frame is within ≈16 ms of the playhead. The per-frame item walk is `O(items)` of cheap accessor calls — negligible for the handful of items 4.3/4.4 target; the ≥10-item case (NFR-P6) and the **priority sort on overlap** are **Story 4.5** — for 4.3, **first RAV item spanning the playhead wins** (do not build the priority/track-ranking logic now).

### Single item now, multiple/priority is 4.5 — keep the walk dumb

The epic splits this deliberately: 4.3 = "an animation item on a track" (singular), 4.5 = "multiple items … on overlap show the highest-priority track" (FR14). Your `GetCurrentAnimItem` returns on the **first** match and that is correct for 4.3. Resist generalising it — 4.5 will replace the "first match" with a priority pick. Adding track-priority now is scope creep that the Acceptance-Auditor will flag.

### No-exception boundary (D5 / AR18)

`GetCurrentAnimItem` runs Reaper-driven and touches `PCM_source` vtables — keep it **null-safe** (`it`, `tk`, `src`, every `GetType()`/`GetFileName()`), which is sufficient (the body does no allocation that can throw beyond the `std::string` assign — keep that last, after the match is confirmed). `LoadAsset` in `RenderTick` is already no-throw and returns a `LoadResult` (it never escapes — Epics 2–3). Do not wrap the whole tick in try/catch; rely on the existing no-throw contracts. [Source: architecture.md:619-626; asset_loader.cpp LoadAsset envelope]

### Architecture doc uses the pre-rename ReaImGui design — trust the live tree

`architecture.md`'s per-frame data-flow (`:885-895`, "viewer_panel::Draw() → determine current PCM_source from playhead position (FR14)") describes the superseded ReaImGui-FBO design. The **principle** — *the viewer determines the current PCM_source from the playhead and renders its asset* — is exactly this story; the **mechanism** is the native docked GL window (`viewer_window.cpp` + `renderer.cpp`), not ReaImGui. Follow the live `src/` (the `RenderTick`/`RenderFrame` path, `console_log.h` `[RAV]` format, `rav` namespace), as 4.1/4.2 did. [Source: project_reaper_docked_close memory; architecture Spec Change Log :1140; 4-1 Dev Notes "trust the live tree"]

### Project Structure Notes

- **Edit:** `src/reaper_api.h` (+8 `WANT_*`), `src/pcm_source_anim.h` (+1 decl), `src/pcm_source_anim.cpp` (`GetCurrentAnimItem` + `IsOurs`), `src/renderer.h` + `src/renderer.cpp` (`RenderFrame` loop/clamp param), `src/viewer_window.cpp` (transport poll + reload + state), `docs/PHASE3_VALIDATOR_GATE.md` (+§6).
- **Untouched:** `CMakeLists.txt` (all sources already listed; `reaper_api.h` is a header), `src/plugin_main.cpp`, `src/asset_loader.*`, `src/scene.h`, `src/animation.h`, `src/camera.h`, the shader. If you edit any of these, you have left scope.
- Conventions to match: `namespace rav`; anonymous namespace for `IsOurs`; `console_log.h` (`LogInfo`/`LogError`, never raw `ShowConsoleMsg`); `/W3 /permissive-` clean. DLL name `reaper_animviewer.dll` (CMake target `animviewer`).

### Testing standards

No automated test harness exists (Reaper-hosted native DLL; prior stories validate in-Reaper). **The gate IS the test (AR19):** Antho runs `docs/PHASE3_VALIDATOR_GATE.md` §6 on Windows-in-Reaper — drop an animation, **play and scrub**, confirm the rig follows the transport frame-accurately and holds at the ends, and that scrubbing onto a second item switches the asset. That in-Reaper pass **is** completion. On Linux you can only confirm the CMake parse and the scope/grep audit (the DLL/GL/transport link is host-stubbed, as in Stories 3.x/4.x). [Source: feedback_trust_ingame_validation; PHASE3_VALIDATOR_GATE.md §4]

### References

- [epics.md — Epic 4 / Story 4.3](../planning-artifacts/epics.md) (AC source; FR10, NFR-P5)
- [architecture.md — current time derived from playhead, recomputed every frame, not stored](../planning-artifacts/architecture.md) (:305)
- [architecture.md — displayed Asset comes from the item spanning the playhead](../planning-artifacts/architecture.md) (:466-471)
- [architecture.md — viewer determines current PCM_source from playhead (FR14)](../planning-artifacts/architecture.md) (:885-895)
- [architecture.md — NFR-P5: render every frame, no async between playhead read and draw](../planning-artifacts/architecture.md) (:1015)
- [architecture.md — Phase-3 `WANT_*` additions (`GetPlayPosition2Ex`/`GetPlayStateEx`)](../planning-artifacts/architecture.md) (:337)
- [architecture.md — Spec Change Log: playhead drives the displayed frame, continuous audio clock](../planning-artifacts/architecture.md) (:1140)
- Spike reference: `git show spike/0-1-feasibility:src/spike_pcmsource.cpp` (`CurrentAnimTime`/`IsOurs`), `:src/spike_glwindow.cpp` (`GlWindowRenderFrame` consumer)
- Current code: [viewer_window.cpp `RenderTick`/`StartRendering`](../../src/viewer_window.cpp#L170), [renderer.cpp `RenderFrame` pose block](../../src/renderer.cpp#L277), [pcm_source_anim.cpp `GetFileName`/`GetType`](../../src/pcm_source_anim.cpp#L46), [reaper_api.h](../../src/reaper_api.h)
- Previous stories: [4-1 registration](./4-1-pcm-source-plugin-registers-and-creates-a-source-from-a-file.md), [4-2 correctly-sized item](./4-2-drop-an-animation-file-on-a-track-to-create-a-correctly-sized-item.md)

### Previous-story intelligence (Epic 1–4 patterns to reuse)

- **Cold-path hardening recurs in every code-review** (4.1 no-throw factory; 4.2/3.2 finite/positive guards; 3.1 null `aiBone*`). Anticipate it: null-guard every Reaper handle in the item walk (`it`/`tk`/`src`/`GetType()`/`GetFileName()`), and reload **only** on a path change so a malformed file (whose `LoadAsset` fails) logs once and leaves the previous asset up (AR17) — never a per-frame retry storm.
- **Scope discipline is audited** — every story passes an Acceptance-Auditor scope grep. The single biggest 4.3 trap: building **track-priority / multi-item selection** (that is 4.5) or **item move/resize handling** (that is 4.4). Return on the first match; drive one item.
- **Symmetric register is a review focus** — but you add **no** registration here, only a read-only query. Confirm `plugin_main.cpp` is untouched and `Register`/`-pcmsrc` stay there with the same pointer (4.1 invariant).
- **AR19 — the in-Reaper Windows gate IS the gate.** Transport timing/scrub feel is only observable in-Reaper on Windows; Linux confirms the source audit + scope only. Write §6 so Antho can click through play/scrub/hold/switch.
- **The §C non-canonical-skin gate item (deferred-work.md) still applies:** if a validated FBX renders displaced when transport-driven, that is the same `globalInverse` knob from 3.3, **not** a 4.3 transport bug — 4.3 only changes *which time* feeds the same `ComputePose`/skinning path, it does not touch vertex space.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Claude Opus 4.8, 1M context) — BMAD dev-story workflow.

### Debug Log References

- Linux CMake configure (`cmake -B build_spike -S .`): reaches `CMakeLists.txt:4 project()`, the C++ compiler is detected and "works", and our non-Windows branch runs (`animviewer: non-Windows platform detected — Build target stubbed`). The `CMakeTestCXXCompiler … configure_file: Operation not permitted` error is a pre-existing WSL `/mnt`-mount artifact inside CMake's own compiler-probe module (a full Windows/GL/Reaper compile is impossible by design on Linux — host-stubbed exactly as every prior Phase-2/3 story). **No `CMakeLists.txt` change this story** (header-only `WANT_*`; the three edited `.cpp` are already in the SOURCES list).
- Scope grep audit: only `src/reaper_api.h`, `src/pcm_source_anim.{h,cpp}`, `src/renderer.{h,cpp}`, `src/viewer_window.cpp`, `docs/PHASE3_VALIDATOR_GATE.md` (+ `sprint-status.yaml`) changed. `CMakeLists.txt`, `src/plugin_main.cpp`, `src/asset_loader.*`, `src/scene.h`, `src/animation.h`, `src/camera.h`, the shader all verified **untouched** (`git diff --quiet`). The only new symbols are the 8 transport/item `REAPERAPI_WANT_*`; the sole live `rec->Register("pcmsrc"/"-pcmsrc", …)` stays in `plugin_main.cpp` (boundary rule intact — this story added a read-only query).

### Completion Notes List

- **Task 1** — Added the 8 transport/item `REAPERAPI_WANT_*` defines (`GetPlayStateEx`, `GetPlayPosition2Ex`, `GetCursorPositionEx`, `CountMediaItems`, `GetMediaItem`, `GetMediaItemInfo_Value`, `GetActiveTake`, `GetMediaItemTake_Source`) to `reaper_api.h`. Existing Phase-0/1 defines and the `reaper_plugin.h`/`reaper_plugin_functions.h` includes untouched; they resolve through the existing `REAPERAPI_LoadAPI` call (no new registration).
- **Task 2** — `GetCurrentAnimItem(out_path, out_anim_time)` + anon-namespace `IsOurs` in `pcm_source_anim.{h,cpp}`, mirroring the Spike's `CurrentAnimTime`/`IsOurs` but **also returning the source path** (the net-new bit over the Spike, for the AC2 asset switch). `IsOurs` matches the production tag **`"RAV_ANIM"`** (kSourceType) — NOT the Spike's `"FBXAV_ANIM"` — and also checks the wrapped inner `GetSource()`. Null-guards `s`/inner/`it`/`tk`/`src`/`GetType()`/`GetFileName()`; the `[0, itemLength]` clamp and the `std::string` assign happen only after a confirmed match (no-throw, D5/AR18). First RAV item spanning the playhead wins (single-item / first-match; overlap+priority is 4.5 — walk kept dumb).
  - **Deviation from the task wording (intentional, follows the live tree):** the task said to declare inside "the existing `#ifdef _WIN32` guard" in `pcm_source_anim.h`, but that header has **no** `_WIN32` guard (PCM_source is an SDK/swell type compiled on every platform, unlike `renderer.h`/`asset_loader.h`). Placed the declaration next to `PcmSourceRegistration()` with `#include <string>` added — matching how 4.1/4.2 left the file. Cross-platform compile preserved.
- **Task 3** — `RenderFrame(float anim_time_seconds, bool loop, int width, int height)` (renamed `time_seconds`, added `loop`). The one pose-block line became loop-or-clamp: `loop==false` (transport) clamps `t` to `[0, duration]` so the rig **holds** the last frame (and an item resized longer than the clip, 4.4, still holds — AC3); `loop==true` keeps the Epic-3 `fmod` for the fixture fallback. Nothing else in `RenderFrame` changed (pose_valid gate, palette upload, draw loop, static-path bit-identity all untouched). `std::min`/`std::max`/`std::fmod` already available (`<algorithm>`/`<cmath>` in use).
- **Task 4** — `viewer_window.cpp`: `#include "pcm_source_anim.h"`; file-scope `g_current_anim_path` / `g_display_time` / `g_transport_driven`. `RenderTick` now polls `GetCurrentAnimItem` **synchronously right before** `RenderFrame` (NFR-P5, no async, main-thread UI pump). On a path change it lazily `LoadAsset`s on the render thread (GL context current — why the full GPU load lives here, not in 4.2's GL-free drop) and `SetAsset`s. `g_display_time` is the clamped playhead time; off-span (`false`) **freezes** the last frame and keeps the asset (AC3, no revert-to-fixture). Transport mode is **sticky** once any RAV item is current; before that, the startup dialog fixture renders free-running (`loop=true`) — AC5 preserved. Startup-load block annotated as the no-item fallback.
  - **Cold-path hardening (AR17, from Dev Notes "Previous-story intelligence"):** advance `g_current_anim_path = item_path` on **both** load success **and** failure. The literal "reload only on path change" alone would re-attempt `LoadAsset` **every frame** while a malformed item stays under the playhead (its path never matches the gate) — a per-frame retry storm. Advancing the gate on failure makes a bad file log **once** and leaves the previous asset up; a later scrub onto a different path re-arms the reload.
- **Task 5** — Appended **§6 "Story 4.3"** to `docs/PHASE3_VALIDATOR_GATE.md` in the §5 row-based format: 9 click-based rows (play drives frame / scrub drives frame / displayed asset = item under playhead / 2nd-item asset switch / hold-past-end / hold-before-start / static still renders / foreign files Reaper-normal / no-regression), scope note (overlap=4.5, move/resize=4.4, SaveState=Epic 6), pending-result line.
- **Task 6** — Linux configure + scope audit above. The in-Reaper Windows gate (AR19) is the real test — transport timing, scrub feel, and hold-at-ends are only observable there; Linux confirms the source/scope audit only.

### File List

- `src/reaper_api.h` — +8 transport/item `REAPERAPI_WANT_*` defines (Task 1)
- `src/pcm_source_anim.h` — `+#include <string>`; `GetCurrentAnimItem` declaration (Task 2)
- `src/pcm_source_anim.cpp` — `IsOurs` (anon ns) + `GetCurrentAnimItem` (Task 2)
- `src/renderer.h` — `RenderFrame` gains the `bool loop` param + doc (Task 3)
- `src/renderer.cpp` — `RenderFrame` signature + the one loop-or-clamp pose line (Task 3)
- `src/viewer_window.cpp` — `#include "pcm_source_anim.h"`; transport-drive state vars; `RenderTick` transport poll + lazy `LoadAsset` on path change; startup-fixture fallback comment (Task 4)
- `docs/PHASE3_VALIDATOR_GATE.md` — appended §6 (Task 5)
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — 4-3 → in-progress → review

## Change Log

| Date | Change |
|---|---|
| 2026-06-27 | Story 4.3 implemented (→ review). 8 transport/item `WANT_*` in `reaper_api.h`; read-only `GetCurrentAnimItem` + `IsOurs` (tag `"RAV_ANIM"`, wrapped-source check, returns path for the AC2 asset switch) in `pcm_source_anim.{h,cpp}`; `RenderFrame` gains a `bool loop` (transport clamps `[0,duration]` → holds the end frame AC3, fixture keeps the Epic-3 `fmod` loop); `viewer_window` `RenderTick` polls the transport synchronously each tick (NFR-P5) and lazily `LoadAsset`s the current item's file on a path change (GL ctx current on the render thread, AC2), freezes the last frame off-span (AC3), sticky transport mode. Cold-path: the path gate advances on load **success or failure** so a malformed item logs once, no per-frame retry storm (AR17). No CMake/plugin_main change; boundary rule intact (read-only query). Gate §6 appended. Linux configure host-stubbed + scope audit clean; in-Reaper Windows gate (AR19) pending Antho. |
| 2026-06-26 | Story 4.3 created (ready-for-dev) — the Reaper playhead drives both **which asset** and **which frame** the viewer shows (FR10, NFR-P5). New read-only `GetCurrentAnimItem` (transport→item-relative time + source path, mirrors the Spike's `CurrentAnimTime` + returns the path) in `pcm_source_anim.{h,cpp}`; 8 transport/item `WANT_*` in `reaper_api.h`; `RenderFrame` gains a loop/clamp flag (transport clamps `[0,duration]` to hold the end frame, fixture fallback keeps the Epic-3 loop); `viewer_window`'s `RenderTick` polls the transport each frame, lazily `LoadAsset`s the current item's file on a path change (GL context is current on the render thread), and freezes the last frame off-span (AC3). Single-item / first-match; overlap+priority = 4.5, move/resize = 4.4, SaveState = Epic 6. No CMake/plugin_main change; boundary rule intact (read-only query). Gate §6 appended. |
