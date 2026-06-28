---
baseline_commit: 7cf7c0877311909e31cf5a45bf77f7c6c89de904
---

# Story 6.5.5: Render-quality toggles + on-canvas FPS readout

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

> **This is the LAST story of Epic 6.5 — the final pre-ship polish before Epic 7 (ReaPack release).**
> It closes the two remaining FR52 levers (normal-map / MSAA toggles) and FR53 (on-canvas FPS),
> and it is the designated home for the on-canvas signals that Story 6.5.2 silenced in the console
> (FPS — and a minimal load-failure indication). After this story Epic 6.5 can close and Epic 7 begins.

## Story

As a sound designer on a weaker PC,
I want to drop expensive render elements and see my FPS on-canvas,
so that I can keep playback smooth and know when it is.

## Acceptance Criteria

1. **Given** the viewport tool menu (the frameless Dear ImGui hamburger from Story 6.5.3)
   **When** I open it
   **Then** there is a **Performance** section (a new `CollapsingHeader` with its own icon, in the same rhythm as Light / Ground / Shadow) that hosts the render-quality toggles and the FPS toggle. [Source: epics.md#Story-6.5.5 / FR52, FR53; existing `Section` lambda [viewer_window.cpp:441-447](../../src/viewer_window.cpp#L441-L447)]

2. **Given** the Performance section
   **When** I toggle **Normal maps** off
   **Then** normal-map sampling is disabled **immediately** (next ~66 Hz frame) for every material — the shader falls back to the geometric normal exactly as it already does for assets that carry no normal map — and toggling it back on restores the relief; the toggle has **no other effect** on colour/lighting/transport. [Source: existing `u_hasNormalMap` gate [renderer.cpp:88,110](../../src/renderer.cpp#L88), per-material set site [renderer.cpp:750-753](../../src/renderer.cpp#L750-L753); inline-setter precedent [renderer.h:81,113](../../src/renderer.h#L113)]

3. **Given** the Performance section
   **When** I toggle **MSAA** (anti-aliasing) off/on
   **Then** multisample anti-aliasing turns off/on **live** (jagged ↔ smooth silhouettes) with no window/context recreation, no flicker, and no crash; MSAA is **on by default** (quality), and a weak PC turns it off to recover frame time. [Source: FR52 "MSAA" example; GL context creation [viewer_window.cpp:209-237](../../src/viewer_window.cpp#L209-L237); architecture render pipeline "MSAA 4×" intent architecture.md:391,396. **See Dev Notes "MSAA — the one real design decision" — this is the riskiest task; a documented descope path exists.**]

4. **Given** the Performance section
   **When** I toggle **FPS** on
   **Then** an on-canvas FPS readout appears **top-right** of the viewport (a second frameless ImGui overlay, distinct from the top-left menu), updating live from the existing frame measurement, and toggling it off hides it — this **replaces the console FPS log removed in Story 6.5.2** (do **not** re-add any console FPS output). [Source: FR53; live FPS measurement still running [viewer_window.cpp:309-317](../../src/viewer_window.cpp#L309-L317); deferred-work "On-canvas FPS readout (FR53)"; amended AR16 architecture.md:93]

5. **Given** the existing graceful-degradation surface
   **Then** the **floor** lever (FR52 "floor" example) is **already** delivered by the Ground checkbox ([viewer_window.cpp:483-488](../../src/viewer_window.cpp#L483-L488), `SetFloorVisible`) and the **shadow** lever by the Shadow Off/Low/Mid/High selector ([viewer_window.cpp:491-501](../../src/viewer_window.cpp#L491-L501)) — this story **reuses** them and does **not** add a duplicate floor toggle; together with the new Normal-maps + MSAA toggles they form the complete FR52 set. [Source: Story 6.5.4 floor + shadow; epics.md#Story-6.5.5 FR52 "e.g."]

6. **Given** weak-hardware operation and the boundary discipline
   **Then** every toggle is **purely visual/perf** — it never touches transport, the playhead/scrub/clamp path, persistence (`.rpp`/D9/`pcm_source` untouched — session-only state, a fresh viewer opens at the defaults), and it never crashes the host (NFR-R1, AR17 non-fatal); the per-frame render path stays **allocation-free** (D2 — any (re)allocation, e.g. an MSAA buffer resize, happens only on a toggle/resize cold path); the change is confined to `viewer_window.cpp`, `renderer.{h,cpp}`, the **regenerated** `overlay_icons.h` (+ `Icons/icon_perf.svg`, `tools/gen_icons.py`), and **at most** an append-only enum addition to `gl_loader.h` if MSAA needs it. **No** `scene.h` / `CMakeLists.txt` / `plugin_main` / `reaper_api` / `pcm_source` / `asset_loader` change; **no** `rec->Register`, **no** new `REAPERAPI_WANT_*` (AR15 register-symmetry intact). [Source: AR15 epics.md:163, AR17 epics.md:165, AR18 epics.md:166, AR20 epics.md:171; D2 zero-alloc architecture.md:211-235]

7. **Given** the on-canvas signal mechanism this story introduces
   **Then** a **minimal on-canvas load-failure indication** is surfaced too (a short transient text near the top — e.g. "Failed to load: <category>"), reusing the same overlay path, **rehoming the load-failure signal that Story 6.5.2 silenced** — its load-bearing gate-advance-on-failure logic stays unchanged. [Source: deferred-work "Minimal on-canvas load-failure indication" — explicitly rehomed to 6.5.5; LogError site [viewer_window.cpp:282](../../src/viewer_window.cpp#L282); amended AR16 requires user-facing signals on-canvas. **Secondary to AC1-AC4 — see Out of scope for the descope condition.**]

### Out of scope (explicitly deferred — do NOT implement here)

- **Persisting any toggle** across save/reopen. All toggles are **session-only** like the camera / light / shadow. **No** `.rpp`/SaveState wiring — leave [pcm_source_anim.cpp](../../src/pcm_source_anim.cpp) byte-for-byte unchanged.
- **A duplicate floor toggle.** The Ground checkbox already toggles the floor (AC5) — reference/reuse it; do not add a second one in the Performance section (you *may* mention floor/shadow there in a one-line note, but the controls stay in their existing sections).
- **Configurable MSAA sample count UI** (2×/4×/8× selector) or per-element quality sliders. MSAA is a single on/off at a fixed sample count (4× if available, else the best the driver offers); shadow quality already has its own selector.
- **A full PBR / tone-mapping / IBL pipeline.** Post-MVP (deferred-work). This story only gates existing effects.
- **MSAA descope path (conditional):** if the `wglChoosePixelFormatARB` multisample bootstrap (Dev Notes) proves too risky to ship un-seen on Linux, MSAA may be **deferred** to a tracked item — but then say so explicitly in the gate + deferred-work, and **the rest of the story (normal-maps toggle, FPS readout, load-failure indication) still ships**. The MSAA toggle is the only descopable AC; AC1/AC2/AC4/AC7 are not.
- **A bespoke `icon_perf.svg`** is Antho's to provide (like `icon_shadow`/`icon_ground`). If it is not available, ship the Performance section with a text-only header (skip the `Section` icon for it) rather than blocking — note it in the gate. Do **not** invent/commit a placeholder icon.

## Tasks / Subtasks

- [x] **Task 1 — Performance section in the Dear ImGui menu (AC1).**
  - [x] In [DrawToolUi()](../../src/viewer_window.cpp#L401) `if (g_menu_open)` block, after the **Shadow** section ([viewer_window.cpp:491-501](../../src/viewer_window.cpp#L491-L501)), add a **Performance** section using the existing `Section(icon, "Performance")` lambda. If `Icons/icon_perf.svg` exists (Antho), use `g_icon_perf`; otherwise use a plain `ImGui::CollapsingHeader("Performance", ...)` (text-only — see Out of scope).
  - [x] Inside it (mirroring the Ground checkbox rhythm [viewer_window.cpp:483-488](../../src/viewer_window.cpp#L483-L488)): a **Normal maps** checkbox (Task 2), an **MSAA** checkbox (Task 3), and an **FPS** checkbox (Task 4). Indent 8.0f like the other sections.
  - [x] Add the UI-state globals next to the others ([viewer_window.cpp:130-155](../../src/viewer_window.cpp#L130-L155)): `bool g_normal_maps_on = true;`, `bool g_msaa_on = true;`, `bool g_fps_overlay_on = false;` (FPS default **off** — opt-in; Antho can flip the default at the gate). Seed `g_normal_maps_on`/`g_msaa_on` from their renderer/default constants in `StartRendering` like `g_floor_visible` ([viewer_window.cpp:601-614](../../src/viewer_window.cpp#L601-L614)).

- [x] **Task 2 — Normal-map on/off toggle (AC2).**
  - [x] Add to `Renderer` ([renderer.h:113](../../src/renderer.h#L113) area, mirroring `SetFloorVisible`): member `bool normal_maps_on_ = true;` + inline setter `void SetNormalMapsEnabled(bool v) { normal_maps_on_ = v; }`.
  - [x] In the visible mesh loop, **AND-gate** the existing per-material flag — change [renderer.cpp:753](../../src/renderer.cpp#L753) `glUniform1i(u_has_normal_map_, nmap != 0 ? 1 : 0);` to `glUniform1i(u_has_normal_map_, (normal_maps_on_ && nmap != 0) ? 1 : 0);`. **No GLSL change** — the shader already keeps the geometric normal when the flag is 0 ([renderer.cpp:110-124](../../src/renderer.cpp#L110-L124), AC4 of 6.5.1). The depth/shadow pass is unaffected (it discards colour).
  - [x] UI: `if (ImGui::Checkbox("Normal maps", &g_normal_maps_on)) g_renderer.SetNormalMapsEnabled(g_normal_maps_on);`.

- [x] **Task 3 — MSAA on/off toggle (AC3). READ Dev Notes "MSAA — the one real design decision" FIRST.**
  - [x] **Context creation (one-time):** upgrade [CreateGLContextFor](../../src/viewer_window.cpp#L209) to obtain a **multisample-capable pixel format** via `wglChoosePixelFormatARB` (the standard dummy-context bootstrap — see Dev Notes for the exact recipe), requesting `WGL_SAMPLE_BUFFERS_ARB=1`, `WGL_SAMPLES_ARB=4` (fall back to fewer samples, then to the legacy `ChoosePixelFormat` path if the ARB extension is absent — **never fail context creation over MSAA**; a no-MSAA context is a valid degraded state, AR17). Keep the existing depth/stencil/double-buffer requirements.
  - [x] **Live toggle:** `glEnable(GL_MULTISAMPLE)` / `glDisable(GL_MULTISAMPLE)` driven by the checkbox. `glEnable` is GL 1.1 (already available); `GL_MULTISAMPLE` (0x809D) and the `WGL_*_ARB` constants are not in `<gl/GL.h>` — declare them locally (or, for the GL enum only, append to the `gl_loader.h` enum block, `#ifndef`-guarded). The `wglChoosePixelFormatARB`/`wglGetExtensionsStringARB` function pointers are WGL extensions resolved via `wglGetProcAddress` — declare/typedef them **locally in `viewer_window.cpp`**, not in `gl_loader.h` (they are WGL, not GL).
  - [x] **Where the toggle lives:** purely a renderer/GL-state flag. Either a `Renderer::SetMsaaEnabled(bool)` that flips the GL state in `RenderFrame` (preferred — keeps GL in the renderer), or set it directly in `viewer_window.cpp` around the draw. If in the renderer, add `bool msaa_on_ = true;` + the inline setter (mirror `SetFloorVisible`) and `glEnable/glDisable(GL_MULTISAMPLE)` once per frame is cheap and allocation-free (D2).
  - [x] UI: `if (ImGui::Checkbox("MSAA", &g_msaa_on)) g_renderer.SetMsaaEnabled(g_msaa_on);` (or the local equivalent).
  - [x] **If you descope MSAA** (Out of scope condition): remove the checkbox + globals for it, leave `CreateGLContextFor` as-is, and record the deferral in the gate + deferred-work. AC2/AC4/AC7 still ship.

- [x] **Task 4 — On-canvas FPS readout, top-right (AC4).**
  - [x] Store the already-computed FPS: add `float g_fps = 0.0f;` near [viewer_window.cpp:112](../../src/viewer_window.cpp#L112) and assign it in the 1-Hz roll-up at [viewer_window.cpp:313-316](../../src/viewer_window.cpp#L313) — `g_fps = (float)(g_frame_count / elapsed);` (keep the `LogInfo` call; it is already silent by default — 6.5.2 — and re-enabled only in a debug build; do **not** delete it, do **not** add new console output). For a steadier number you may smooth it (EMA) but the existing 1-Hz cadence is acceptable.
  - [x] Draw a **second** frameless ImGui window for the readout, **inside the same `NewFrame`/`Render` pair** — add it after `ImGui::End()` for `##tools` ([viewer_window.cpp:504](../../src/viewer_window.cpp#L504)) and **before** `ImGui::Render()` ([viewer_window.cpp:507](../../src/viewer_window.cpp#L507)). Anchor top-right with a pivot: `ImGui::SetNextWindowPos(ImVec2((float)g_client_w - 10.0f, 10.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));` then `Begin("##fps", nullptr, kFlags | ImGuiWindowFlags_NoInputs)` (NoInputs so it never steals mouse from the camera — see `ImGuiWantsMouse` [viewer_window.cpp:664-666](../../src/viewer_window.cpp#L664)), `ImGui::Text("%.0f FPS", g_fps);`, `End()`. Gate the whole block on `g_fps_overlay_on`.
  - [x] UI: `ImGui::Checkbox("FPS", &g_fps_overlay_on);` in the Performance section (no renderer call — pure UI state).

- [x] **Task 5 — Minimal on-canvas load-failure indication (AC7) — rehome of the 6.5.2-silenced signal.**
  - [x] Capture the failure: where the load-failure `LogError` fires ([viewer_window.cpp:282](../../src/viewer_window.cpp#L282)) — which already runs and **advances the gate on failure** (AR17, keep that logic **unchanged**) — also store a short message + a timestamp into globals (e.g. `char g_load_error[96]; double g_load_error_until;`). Use `LoadErrorCategoryName(r.category)` (already in scope) for the category.
  - [x] Render it via the **same overlay path** as the FPS readout (a third tiny frameless window, top-centre or just under the top-right, `NoInputs`), shown while `ElapsedSeconds()`/QPC is before `g_load_error_until` (e.g. ~4 s), then it fades out (or just disappears). Do not block, do not popup (AR16 — no message boxes).
  - [x] This honours Story 6.5.2's AC3 deferral ("the load-failure signal is **not** orphaned — rehomed on-canvas") and amended AR16. If genuinely time-boxed at the gate, this is the only AC besides MSAA that may slip to deferred-work — but it is cheap (no GL, reuses the overlay) so prefer to land it.

- [x] **Task 6 — Performance icon (AC1, optional asset).**
  - [x] **DONE — Antho provided `Icons/icon_performance.svg`** (post-impl, 2026-06-28). Added `'performance'` to `names` in [tools/gen_icons.py:84](../../tools/gen_icons.py#L84), regenerated `overlay_icons.h` (`python3 tools/gen_icons.py` → `kIcon_performance`), added `GLuint g_icon_performance`, upload in `StartRendering` + free in `StopRendering`, and the header now uses `Section(g_icon_performance, "Performance")`.
  - [x] Text-only fallback path was the initial ship (no icon present at first); superseded by the icon above. (Either way the section ships.)

- [x] **Task 7 — Scope audit, gate §8, docs (AC6 + AR19/AR20).**
  - [x] `git diff --stat` shows only: `src/viewer_window.cpp`, `src/renderer.cpp`, `src/renderer.h`, optionally `src/gl_loader.h` (a single guarded `GL_MULTISAMPLE` enum), and (if the icon ships) `src/overlay_icons.h` (regenerated) + `Icons/icon_perf.svg` + `tools/gen_icons.py`. **No** `scene.h`/`CMakeLists.txt`/`plugin_main`/`reaper_api`/`pcm_source`/`asset_loader`; **no** `rec->Register`/`WANT_*`. Run `git diff -- src/ | grep -E 'rec->Register|REAPERAPI_WANT_'` → must be empty.
  - [x] If `CreateGLContextFor` moves to the ARB multisample path, log it as an **AR20 Spec Change Log** entry in [architecture.md](../planning-artifacts/architecture.md) (newest-on-top): Trigger (FR52 MSAA toggle, Story 6.5.5) / Decision (one-time `wglChoosePixelFormatARB` multisample pixel format with legacy `ChoosePixelFormat` fallback; live `glEnable/glDisable(GL_MULTISAMPLE)`; realises the architecture's long-intended 4× MSAA — architecture.md:391 — which the direct-render docked-GL window had never actually enabled) / KEEP (single `SetPixelFormat` per HDC, no window/context recreation, AR15 register-symmetry, D2 zero-alloc per-frame, AR17 non-fatal). Note that AR16's on-canvas FPS + load-failure signals (this story) complete the 2026-06-27 AR16 amendment.
  - [x] Append gate **§8** to [docs/PHASE4.5_VALIDATOR_GATE.md](../../docs/PHASE4.5_VALIDATOR_GATE.md) in the §7 shape: intro + `> Scope note` (floor/shadow toggles already shipped in 6.5.4 and aren't re-tested here; MSAA may be absent if descoped; FPS default-off) + `Suggested fixtures:` + a click-based check table + `**Result:** … — PENDING`. **Never pre-mark PASS** (AR19).
  - [x] Update [deferred-work.md](deferred-work.md): mark "Remaining FR52 levers (normal-map / MSAA) + FR53 on-canvas FPS — Story 6.5.5" **DONE** (or note MSAA deferred if descoped); mark the "On-canvas FPS readout (FR53)" and "Minimal on-canvas load-failure indication" rehome targets **DONE** (delivered here).
  - [x] Build clean at `/W3 /permissive-` (NFR-R5). Linux can't build the `_WIN32` units / ImGui / open Reaper → self-review (ImGui checkboxes mirror the proven 6.5.4 widgets; the FPS overlay mirrors the existing `##tools` window; MSAA bootstrap is the textbook Win32 path); judged at Antho's in-Reaper Windows gate (AR19).

## Dev Notes

### ⚠️ The UI is Dear ImGui (the epic's "Win32-child / GL-overlay" language is stale)

The epic FR49/FR52/FR53 text says "native Win32-child / GL-overlay widgets — no ReaImGui". Since Story 6.5.3 the tool menu is **vendored Dear ImGui** (frameless hamburger; Antho: "C'est parfait"). All new controls are **ImGui widgets** in [DrawToolUi](../../src/viewer_window.cpp#L401) (`Checkbox`, `CollapsingHeader`, `Text`), not Win32 children, not ReaImGui (which is a separate extension, post-MVP). This matches Stories 6.5.3 and 6.5.4. [[project_imgui_ui]]

### Most of FR52 already shipped — be honest about what's left

FR52 ("toggle costly render elements e.g. normal maps, MSAA, floor") is **mostly already delivered**: the **floor** on/off (Ground checkbox, `SetFloorVisible`, 6.5.4 post-gate) and the **shadow** Off/Low/Mid/High selector (6.5.4) are the two biggest perf levers and already exist. The genuinely **new** levers in this story are **normal maps** (real per-fragment ALU) and **MSAA** (fill-rate / VRAM). Don't rebuild floor/shadow — reference them (AC5). The story is small *except* for MSAA.

### MSAA — the one real design decision (read before Task 3)

**MSAA is NOT enabled today.** Context creation uses the **legacy GDI `ChoosePixelFormat`** ([viewer_window.cpp:209-237](../../src/viewer_window.cpp#L209)), which has no samples field → a single-sample format. The architecture always *intended* 4× MSAA (architecture.md:391) but it was specified "in offscreen pass" via the D11 FBO bridge — and D11 was **superseded** (Spike 0): the viewport is a direct-render docked GL window with **no offscreen pass**, so MSAA never landed.

Two hard Win32 facts shape the fix: (1) `SetPixelFormat` can be called **only once per HDC**; (2) the window/context is deliberately **never recreated** (the long header comment [viewer_window.cpp:9-20] and the WM_SIZE "no recreation on resize" note [viewer_window.cpp:715-724] explain why — recreation corrupted the docked window). So you **cannot** toggle MSAA by reselecting a pixel format on the live window.

**Chosen approach (do this): one-time multisample pixel format + live `glEnable/glDisable(GL_MULTISAMPLE)`.**
- Request a multisample-capable format **once** at creation via `wglChoosePixelFormatARB`. Because that function itself needs a current GL context to be resolved, use the **standard dummy-context bootstrap**: create a throwaway hidden window, give it a legacy pixel format, make a temp context current, `wglGetProcAddress("wglChoosePixelFormatARB")` (+ `wglGetExtensionsStringARB` to confirm `WGL_ARB_multisample`), then destroy the temp window/context. Then on the **real** `g_hdc`, `wglChoosePixelFormatARB` with `WGL_DRAW_TO_WINDOW_ARB`, `WGL_SUPPORT_OPENGL_ARB`, `WGL_DOUBLE_BUFFER_ARB`, `WGL_PIXEL_TYPE_ARB=WGL_TYPE_RGBA_ARB`, `WGL_COLOR_BITS_ARB=32`, `WGL_DEPTH_BITS_ARB=24`, `WGL_STENCIL_BITS_ARB=8`, `WGL_SAMPLE_BUFFERS_ARB=1`, `WGL_SAMPLES_ARB=4` → `SetPixelFormat` once → `wglCreateContext`.
- **Fallbacks (non-fatal, AR17):** if `WGL_ARB_multisample` is absent or `wglChoosePixelFormatARB` returns nothing, fall back to fewer samples (2×) and finally to the **existing legacy `ChoosePixelFormat` path verbatim** — context creation must still succeed; MSAA just won't be available (hide/disable the checkbox or leave it inert). Never `return false` over MSAA.
- **Live toggle is then trivial:** `glEnable(GL_MULTISAMPLE)` / `glDisable(GL_MULTISAMPLE)` per the checkbox. The multisample buffer exists for the session; the GL enable just turns resolve on/off. Zero per-frame allocation (D2).
- **WGL constants/typedefs** (`WGL_SAMPLES_ARB 0x2042`, `WGL_SAMPLE_BUFFERS_ARB 0x2041`, `WGL_DRAW_TO_WINDOW_ARB 0x2001`, etc.) and `typedef BOOL (WINAPI* PFNWGLCHOOSEPIXELFORMATARBPROC)(...)` go **locally in `viewer_window.cpp`** (WGL is not GL — keep it out of `gl_loader.h`). Only `GL_MULTISAMPLE 0x809D` is a core GL enum; declare it locally or append (guarded) to the `gl_loader.h` enum block.

**Honest risk note:** this is the riskiest task — it rewrites the *working* context-creation path, can't be seen on Linux, and lands right before ship. It is textbook code (the dummy-context MSAA bootstrap is one of the most-documented Win32 GL recipes), but if you judge the un-seeable risk too high for a pre-ship story, **descope MSAA** (Out of scope) and ship the rest. Document the choice in the gate either way.

### FPS — the value is already measured, just not stored

Story 6.5.2 silenced the FPS **log**, not the **measurement**: `QueryPerformanceCounter` + `g_frame_count` + the `elapsed >= 1.0` roll-up still run every frame ([viewer_window.cpp:309-317](../../src/viewer_window.cpp#L309)). The computed `g_frame_count / elapsed` is consumed inline by `LogInfo` (silent) and **not stored**. One line — `g_fps = (float)(g_frame_count / elapsed);` at [viewer_window.cpp:314](../../src/viewer_window.cpp#L314) — gives the overlay its value. The `console_log.cpp:18-19` comment already names this story as the rehome. **Do not re-add a console FPS log.**

### The second/third ImGui windows share one frame

There is exactly **one** `NewFrame`/`Render` pair per frame, both inside `DrawToolUi` ([viewer_window.cpp:405-407, 507-508](../../src/viewer_window.cpp#L405)). Add the FPS overlay (and the load-failure overlay) as additional `Begin`/`End` blocks **between** `##tools`'s `End()` and `ImGui::Render()` — never call `NewFrame`/`Render` again. Use the same `kFlags` ([viewer_window.cpp:410-414](../../src/viewer_window.cpp#L410)) plus `ImGuiWindowFlags_NoInputs` so the readouts never capture the mouse (the camera-vs-ImGui arbitration is `ImGuiWantsMouse()` [viewer_window.cpp:664-666](../../src/viewer_window.cpp#L664) — a readout that grabs focus would block orbit/zoom). `g_client_w`/`g_client_h` ([viewer_window.cpp:104-105](../../src/viewer_window.cpp#L104), updated in WM_SIZE [viewer_window.cpp:720-721](../../src/viewer_window.cpp#L720)) give the live size for the top-right pivot.

### Normal-map toggle is a one-character AND-gate

The shader already branches on `u_hasNormalMap` ([renderer.cpp:88,110](../../src/renderer.cpp#L88)) and keeps the geometric normal when it's 0 (6.5.1 AC4 — the cold-path NaN guard). Today the flag is set per material from `nmap != 0` ([renderer.cpp:753](../../src/renderer.cpp#L753)). AND-ing a global `normal_maps_on_` is the entire renderer change — **no GLSL edit, no new uniform**. Mirror `SetFloorVisible` ([renderer.h:113](../../src/renderer.h#L113)) for the setter.

### Non-fatal everything (AR17/AR18)

MSAA-bootstrap failure → fall back (no MSAA), context still created. ImGui checkboxes mutate state only; no synchronous render in a handler; no exception escapes `WindowProc` (AR18). Console is silent by default (6.5.2) — any `LogWarn` is wired-but-silent (`build_debuglog.bat` to see it, `build.bat` to revert). **No `printf`/`cout`/`OutputDebugString`** — the single-funnel `Emit()` invariant ([console_log.cpp](../../src/console_log.cpp)) holds; the on-canvas overlays are the user-facing channel now (amended AR16).

### Regression guardrails

- **Mesh/skinning/material/shadow/floor visible path stays correct.** The normal-map AND-gate only changes one uniform's value; MSAA is a framebuffer/state concern; the FPS/load overlays are extra ImGui windows after the scene. Epic 2/3/4 + 6.5.1-6.5.4 rendering is otherwise unchanged.
- **Transport/playhead untouched** — every toggle is pure render/UI state (AC6).
- **Perf:** normal-maps-off and MSAA-off both *reduce* cost (the point). MSAA buffer allocated at context creation / resize only (cold path) → D2 zero-alloc per-frame preserved. The FPS overlay is a few verts; negligible.
- **Hide/show + reopen:** all new globals are session-only; GL state (MSAA enable) re-applies each frame; no persistence (no D9, `pcm_source_anim.cpp` untouched). Docked-close sends us nothing — don't destroy on hide; the timer parks while `!IsWindowVisible` ([viewer_window.cpp:332-338](../../src/viewer_window.cpp#L332)). [[project_reaper_docked_close]]
- **Boundary (AR15):** only the files in Task 7's audit; no `rec->Register`/`WANT_*`/CMake/plugin_main/reaper_api/pcm_source/asset_loader/scene.h.

### Validation = Antho's in-Reaper Windows gate (AR19)

Linux compiles the non-`_WIN32` TUs but can't build the `_WIN32` viewer/renderer/ImGui units or open Reaper, so this is **self-reviewed** and judged **in-Reaper on Windows**. Author gate **§8** **PENDING** — never pre-mark PASS. Validator hooks are **click-based** (double-click `build.bat`, click the hamburger → Performance, tick/untick each toggle, watch the top-right FPS). [[feedback_clickable_test_methods]]

Suggested §8 checks (click-based): (1) the menu shows a **Performance** section; (2) **Normal maps** off visibly flattens surface relief, on restores it, immediately; (3) **MSAA** off shows jagged edges, on smooths them, live, no flicker/crash (or: MSAA absent + noted, if descoped); (4) **FPS** on shows a live top-right readout that tracks load, off hides it — and the **console shows no FPS** in a normal build; (5) the existing **Ground** + **Shadow** controls still work (FR52 floor/shadow levers); (6) a deliberately-missing animation file shows a brief on-canvas "Failed to load…" and the rest keeps playing (AR17); (7) orbit/zoom/pan + Recenter + Light still work, no overlay steals the mouse; (8) **≥60 fps** (NFR-P1) holds with the expensive elements off; close→reopen returns to defaults, no ghost/leak, console silent (6.5.2).

### Project Structure Notes

- Flat `src/`. This story: `src/viewer_window.cpp` (Performance section + 3 toggle globals + `g_fps` + FPS overlay + load-failure overlay + MSAA bootstrap in `CreateGLContextFor` + optional `g_icon_perf` upload/free), `src/renderer.cpp` (normal-map AND-gate at line 753; MSAA `glEnable/disable` if the flag lives in the renderer), `src/renderer.h` (`SetNormalMapsEnabled` + `normal_maps_on_`; optional `SetMsaaEnabled` + `msaa_on_`), optionally `src/gl_loader.h` (guarded `GL_MULTISAMPLE` enum), optionally `src/overlay_icons.h` (regenerated) + `Icons/icon_perf.svg` + `tools/gen_icons.py`. Docs: gate §8, AR20 (if MSAA-ARB), deferred-work, sprint-status.
- Naming: members `snake_case_` (`normal_maps_on_`, `msaa_on_`); globals `g_` (`g_normal_maps_on`, `g_msaa_on`, `g_fps_overlay_on`, `g_fps`, `g_icon_perf`, `g_load_error`); methods PascalCase (`SetNormalMapsEnabled`, `SetMsaaEnabled`). Namespace `rav`; SPDX MIT header; WHY-only comments.

### References

- [Source: epics.md#Story-6.5.5 (lines 684-696) / FR52 (line 58), FR53 (line 59)] — the story ACs + the two requirements.
- [Source: epics.md:160-171] — AR15 (:163), AR17 (:165), AR18 (:166), AR19 (:170), AR20 (:171). [Source: architecture.md:93] — amended AR16 (silent-by-default + on-canvas signals); [Source: architecture.md:391,396] — MSAA 4× intent + "Render-quality toggles (FR52) gate normal-map sampling + MSAA".
- [Source: 6-5-4-floor-tool-solid-grid-show-hide.md] — the `Section` lambda, the Ground (`SetFloorVisible`/`g_floor_visible`) and Shadow (`SetShadowQuality`/`g_shadow_quality`) controls this story reuses; the icon add/regen/upload/free pattern; the gate/AR20/deferred-work rhythm.
- [Source: deferred-work.md:18, :54-55] — "Remaining FR52 levers + FR53 stay 6.5.5"; the on-canvas FPS + load-failure rehome targets explicitly assigned here.
- [Source: src/viewer_window.cpp:104-155] — globals + UI-state block; :209-237 `CreateGLContextFor` (MSAA target); :282 load-failure `LogError` + gate-advance; :309-317 FPS measurement; :401-509 `DrawToolUi` (Performance section + overlays); :601-650 icon upload/free; :664-666 `ImGuiWantsMouse`; :720-721 WM_SIZE size update.
- [Source: src/renderer.h:81,108,113,174-177,192,201] — inline-setter precedent, `SetShadowQuality`, `SetFloorVisible`, normal-map uniform locations + `normal_strength_`, `floor_visible_`. [Source: src/renderer.cpp:88,110-124,304,750-753] — `u_hasNormalMap` decl, the gated TBN block + NaN guard, location fetch, the per-material set site to AND-gate.
- [Source: src/console_log.cpp:18-25] — the silent `Emit()` funnel + the comment naming this story as the FPS/load-failure rehome.
- [Source: tools/gen_icons.py:84] — `names` list to extend with `'perf'`; [Source: src/overlay_icons.h] — regenerated icon data (do not hand-edit).

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Opus 4.8, 1M context) — dev-story 2026-06-28.

### Debug Log References

- Scope/boundary audit: `git diff -- src/ | grep -E 'rec->Register|REAPERAPI_WANT_'` → empty (AR15 intact). No `scene.h`/`CMakeLists.txt`/`plugin_main`/`reaper_api`/`pcm_source`/`asset_loader` touched.
- Host-stubbed CMake configure on Linux: `cmake -S . -B …` → exit 0 ("non-Windows platform detected — Phase 0 only supports Windows. Build target stubbed."). The `_WIN32` viewer/renderer/ImGui units do not build on Linux and Reaper can't open here → self-review + Antho's in-Reaper Windows gate §8 (AR19), the established 6.5.1–6.5.4 pattern.

### Completion Notes List

- **Task 1 (AC1):** Added a **Performance** `CollapsingHeader` after the Shadow section in `DrawToolUi`, with **Normal maps** / **MSAA** / **FPS** checkboxes (indent 8.0f, matching the Ground/Shadow rhythm). UI-state globals `g_normal_maps_on=true`, `g_msaa_on=true`, `g_fps_overlay_on=false` added next to `g_floor_visible`; the two render-quality toggles are pushed to the renderer in `StartRendering` so UI/render agree from frame 1.
- **Task 2 (AC2):** `Renderer::SetNormalMapsEnabled` + `normal_maps_on_` (mirror `SetFloorVisible`); the per-material set site is AND-gated `(normal_maps_on_ && nmap != 0)`. **No GLSL change** — the shader already keeps the geometric normal when the flag is 0.
- **Task 3 (AC3):** MSAA **implemented, not descoped.** `CreateGLContextFor` now tries a 4× (then 2×) multisample pixel format via the standard `wglChoosePixelFormatARB` dummy-context bootstrap (`ChooseMultisamplePixelFormat` helper), with the **verbatim legacy `ChoosePixelFormat` path** as a non-fatal fallback — context creation never fails over MSAA (AR17). Live toggle = `glEnable/glDisable(GL_MULTISAMPLE)` once per frame in `RenderFrame` (`Renderer::SetMsaaEnabled` + `msaa_on_`). `GL_MULTISAMPLE` added (guarded) to `gl_loader.h`; WGL constants/typedef are local to `viewer_window.cpp`. `g_msaa_available` tracks whether a multisample format was obtained → the checkbox is shown **disabled `(unavailable)`** if not.
- **Task 4 (AC4):** `g_fps` stored at the 1-Hz roll-up (the silent `LogInfo` kept; no console FPS re-added). A second frameless `NoInputs` ImGui overlay draws "`%.0f FPS`" **top-right** (right-edge pivot), gated on `g_fps_overlay_on`, in the same `NewFrame`/`Render` pair.
- **Task 5 (AC7):** The load-failure `LogError` site also stores `g_load_error` + a ~4 s expiry; a third frameless `NoInputs` overlay shows "Failed to load: <category>" top-centre while fresh. Gate-advance-on-failure logic unchanged.
- **Task 6 (AC1, optional asset):** `Icons/icon_perf.svg` is **not present** → text-only `CollapsingHeader` per Out-of-scope; **no placeholder icon invented**, no `gen_icons.py`/`overlay_icons.h` change.
- **Task 7 (AC6 + AR19/AR20):** Scope audit clean (only `viewer_window.cpp`, `renderer.{h,cpp}`, `gl_loader.h` in `src/`). AR20 Spec Change Log entry added (MSAA bootstrap + on-canvas signals completing the AR16 amendment); gate §8 appended **PENDING** (never pre-marked PASS); deferred-work targets marked DONE. Build judged at Antho's in-Reaper Windows gate (AR19).
- **AC5 (no code):** the floor (Ground) + shadow (Off/Low/Mid/High) levers from 6.5.4 are reused, not duplicated — together with Normal maps + MSAA they form the complete FR52 set.
- **AC6 (boundaries):** every toggle is pure render/UI state — no transport/playhead/persistence touched (`pcm_source_anim.cpp` byte-for-byte unchanged); D2 zero-alloc per-frame preserved (multisample buffer allocated only on the context-creation cold path); AR15 register-symmetry intact.

### File List

- `src/viewer_window.cpp` — Performance section + toggle globals + `g_fps` store + FPS overlay + load-failure overlay + WGL multisample bootstrap in `CreateGLContextFor` (`ChooseMultisamplePixelFormat` helper) + startup toggle sync; `<cstdio>` include.
- `src/renderer.h` — `SetNormalMapsEnabled`/`SetMsaaEnabled` + `normal_maps_on_`/`msaa_on_` members.
- `src/renderer.cpp` — normal-map AND-gate at the per-material set site; `glEnable/glDisable(GL_MULTISAMPLE)` per frame in `RenderFrame`.
- `src/gl_loader.h` — guarded `GL_MULTISAMPLE` (0x809D) enum.
- `src/overlay_icons.h` — regenerated with `kIcon_performance` (Antho's icon).
- `tools/gen_icons.py` — `'performance'` added to the `names` list.
- `Icons/icon_performance.svg` — Antho's Performance-section icon (new).
- `docs/PHASE4.5_VALIDATOR_GATE.md` — gate §8 (PENDING).
- `_bmad-output/planning-artifacts/architecture.md` — AR20 Spec Change Log entry (2026-06-28, Story 6.5.5).
- `_bmad-output/implementation-artifacts/deferred-work.md` — FR52/FR53 + FPS + load-failure rehome targets marked DONE.
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — 6-5-5 → in-progress → review.

### Change Log

- 2026-06-28 — Post-impl tweaks (Antho feedback, same day): (1) **Performance icon** wired — Antho provided `Icons/icon_performance.svg`; added to `gen_icons.py`, regenerated `overlay_icons.h` (`kIcon_performance`), uploaded/freed `g_icon_performance`, header now `Section(g_icon_performance, "Performance")` (no longer text-only). (2) **All Performance toggles default ON** — `g_fps_overlay_on` flipped to `true` (Normal maps + MSAA were already on). (3) **MSAA "no visible difference" diagnosis** — availability now decided from the **actual** default-framebuffer sample count (`glGetIntegerv(GL_SAMPLES)` once the context is current), not from format selection; the checkbox label shows the real count (e.g. **"MSAA (4x)"**) or is disabled **"(unavailable)"** if the GPU gave no multisample format — a click-based way to confirm whether MSAA is genuinely live. Added `GL_SAMPLES`/`GL_SAMPLE_BUFFERS` enums locally + a silent (debug-build) `LogInfo` of the sample counts.
- 2026-06-28 — Story 6.5.5 implemented (→ review). **Performance** section in the 6.5.3 Dear ImGui menu (text-only header — no `icon_perf.svg` yet) with **Normal maps** (AND-gate of the existing `u_hasNormalMap` per-material flag — one-line renderer change, no GLSL), **MSAA** (one-time `wglChoosePixelFormatARB` 4×/2× multisample format + verbatim legacy `ChoosePixelFormat` fallback + live `glEnable/glDisable(GL_MULTISAMPLE)`; **implemented, not descoped**; checkbox disabled if the driver offers no multisample format), and **FPS** toggles. On-canvas **FPS** readout top-right (second frameless `NoInputs` overlay reading the still-running 6.5.2 measurement via new `g_fps`; silent `LogInfo` kept, no console FPS re-added) + minimal on-canvas **load-failure** line (third overlay, ~4 s, rehoming the other 6.5.2-silenced signal — amended AR16). Floor + shadow levers reused from 6.5.4 (AC5). Session-only (no D9); D2 zero-alloc per-frame; AR15 register-symmetry intact; AR17 non-fatal MSAA fallback. Files: `src/viewer_window.cpp` + `src/renderer.{h,cpp}` + `src/gl_loader.h` (guarded `GL_MULTISAMPLE`). Docs: gate §8 PENDING + AR20 Spec Change Log + deferred-work DONE markers. Scope audit clean (no Register/WANT_/CMake/plugin_main/reaper_api/pcm_source/asset_loader/scene.h). Host-stubbed CMake configure exit 0; `_WIN32`/ImGui/Reaper units self-reviewed → Antho's in-Reaper Windows gate §8 (AR19).
- 2026-06-28 — Story 6.5.5 created (ready-for-dev). Closes Epic 6.5: a **Performance** section in the 6.5.3 Dear ImGui menu with **Normal maps** (AND-gate the existing `u_hasNormalMap` per-material flag — one-line renderer change, no GLSL) + **MSAA** (one-time `wglChoosePixelFormatARB` multisample format with legacy fallback + live `glEnable/glDisable(GL_MULTISAMPLE)` — riskiest task, descopable) toggles; an on-canvas **FPS** readout top-right (second ImGui overlay, reading the still-running 6.5.2 measurement via a new `g_fps`) replacing the removed console FPS log; and a **minimal on-canvas load-failure indication** rehoming the other 6.5.2-silenced signal (amended AR16). Floor + shadow levers already shipped in 6.5.4 — reused, not duplicated (the complete FR52 set). Session-only (no D9, `pcm_source_anim.cpp` untouched); D2 zero-alloc per-frame; AR15 register-symmetry intact (no Register/WANT_/CMake/plugin_main/reaper_api/pcm_source/asset_loader/scene.h); AR17 non-fatal MSAA fallback. Gate §8 PENDING. Realises FR52 (remaining levers) + FR53. Depends 6.5.3 (menu) + 6.5.4 (floor/shadow + icon pattern).
